"""Signed CRX upgrade, restart, worker/storage continuity, disable/re-enable/remove."""
import json,os,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke_features import Handler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

class Server(ThreadingHTTPServer):
    def handle_error(self,request,address):
        import sys
        if isinstance(sys.exc_info()[1],ConnectionError):return
        super().handle_error(request,address)

def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=extensions')
    out=ROOT/'artifacts/extension-lifecycle-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile with spaces'
    if profile.exists():shutil.rmtree(profile)
    profile.mkdir();(profile/'profile.toml').write_text('language="ru"\nrestore=false\ncheck_updates=false\n[[bookmarks]]\nurl="https://kept.test/"\ntitle="Keep this"\ntime=1\n')
    report=out/'report.json';report.unlink(missing_ok=True)
    with Server(('127.0.0.2',0),partial(Handler,directory=str(ROOT/'tests/fixtures'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.2:{server.server_port}/browser/'
        with (out/'process.log').open('w') as log:
            proc=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={base}protection.html'],stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'},start_new_session=True)
            try:
                def state():
                    try:return json.loads(report.read_text())
                    except (FileNotFoundError,ValueError):return {}
                wait_until(lambda:state().get('presentation_frames',0)>0,'No lifecycle test window')
                window=str(state()['window'])
                def click(rect):x,y,w,h=rect;click_at(window,x+w//2,y+h//2)
                def manager():
                    key_at(window,'Escape');key_at(window,'Escape');wait_until(lambda:state().get('panel')==0,'Panel did not close')
                    click_at(window,1187,55);wait_until(lambda:state().get('extensions_ui'),'No extension manager')
                def paste(text):
                    ui=state()['extensions_ui'];click(ui['input']);key_at(window,'a',4)
                    clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(text.encode(),timeout=5);key_at(window,'v',4);time.sleep(.3)
                def download(name):
                    paste(str(ROOT/'tests/fixtures/extensions'/name));click(state()['extensions_ui']['download'])
                def preview(version):wait_until(lambda:state()['extensions']['candidate_version']==version,'No signed extension preview')
                def install():click(state()['extensions_ui']['install']);wait_until(lambda:state()['extensions']['candidate'] is None,'Install did not finish')
                def item_button(offset):
                    r=state()['extensions_ui']['bounds'];click_at(window,r[0]+offset,r[1]+287);time.sleep(.35)
                def screenshot(name):time.sleep(.3);subprocess.run(['import','-window',window,str(out/name)],check=True,timeout=10)
                def layouts(prefix):
                    def language(value):
                        key_at(window,'Escape');click_at(window,1307,55)
                        wait_until(lambda:state().get('settings_ui'),'No language settings')
                        key_at(window,'f',4);type_text(window,'language');key_at(window,'Return')
                        wait_until(lambda:state()['settings_ui'].get('search')=='language','No language search')
                        control=next(c for c in state()['settings_ui']['controls'] if c['id']=='language')
                        click(next(o['bounds'] for o in control['options'] if o['value']==value))
                        wait_until(lambda:state()['settings']['language']==value,'Language did not change')
                        manager()
                    for value in ('ru','en'):
                        if state()['settings']['language']!=value:language(value)
                        for w,h in ((1360,900),(900,620)):
                            subprocess.run(['xdotool','windowsize','--sync',window,str(w),str(h)],check=True)
                            time.sleep(.3);screenshot(prefix+'-'+value+'-'+str(w)+'.png')
                        subprocess.run(['xdotool','windowsize','--sync',window,'1360','900'],check=True)
                        time.sleep(.3)
                    language('ru')
                def restart(cancel=False):
                    nonlocal window
                    old=window;old_pid=state()['pid'];click(state()['extensions_ui']['restart']);wait_until(lambda:state()['dialog_count']==1,'No restart confirmation')
                    if cancel:
                        key_at(window,'Escape');wait_until(lambda:state()['dialog_count']==0,'Restart cancellation failed');assert str(state()['window'])==old;return
                    key_at(window,'Return')
                    wait_until(lambda:state().get('window') and state()['pid']!=old_pid and state()['presentation_frames']>0,'Restart did not create a new window',timeout=35)
                    window=str(state()['window']);Handler.probes.clear()
                    wait_until(lambda:len(state()['tabs'])==2 and not any(t['private'] for t in state()['tabs']),'Session was not restored')
                    assert state()['settings']['restore'] is False
                    assert state()['bookmark_count']==1
                    wait_until(lambda:Handler.probes,'Restored page did not run',timeout=25)
                def probe(predicate):wait_until(lambda:Handler.probes and predicate(Handler.probes[-1]),'No expected extension activity',timeout=25)
                wait_until(lambda:state()['tabs'][0]['url']==base+'protection.html' and state()['tabs'][0]['loaded'],'Initial URL lost during bootstrap')
                manager();download('valid.crx');preview('1.0');install()
                r=state()['extensions_ui']['bounds'];click_at(window,r[0]+r[2]-92,r[1]+234);time.sleep(.3)
                key_at(window,'Escape');key_at(window,'t',4);key_at(window,'l',4);type_text(window,base+'a.html');key_at(window,'Return')
                wait_until(lambda:len(state()['tabs'])==2 and state()['tabs'][1]['url']==base+'a.html','No second normal tab')
                key_at(window,'n',5);key_at(window,'l',4);type_text(window,base+'private.html');key_at(window,'Return');wait_until(lambda:any(t['private'] for t in state()['tabs']),'No private tab')
                manager();restart(cancel=True);restart()
                probe(lambda p:p['extension'] and p['extension'].get('worker'))
                assert state()['extensions']['items'][0]['pinned']
                manager();item_button(206);wait_until(lambda:state().get('extension_popup') and state()['extension_popup']['loaded'],'No extension popup')
                wait_until(lambda:state()['extensions']['items'][0]['observed'],'Popup activity was not recorded');key_at(window,'Escape');time.sleep(.3)
                manager();download('update.crx');preview('2.0');assert state()['extensions']['added_permissions']==['tabs'];screenshot('ru-update-permissions.png');layouts('update-buttons');install()
                assert state()['extensions']['items'][0]['version']=='2.0' and state()['extensions']['items'][0]['pending_update']
                restart();probe(lambda p:p['extension'] and p['extension'].get('version')=='2.0')
                assert Handler.probes[-1]['extension']['preserved']=='version-one','Extension local storage was lost during update'
                manager();download('module.crx');preview('3.0');assert state()['extensions']['added_permissions']==[];install();restart();probe(lambda p:p['extension'] and p['extension'].get('version')=='3.0');assert Handler.probes[-1]['extension']['preserved']=='version-one'
                manager();download('valid.crx');wait_until(lambda:state()['extensions']['status']=='An older extension version was rejected.','Downgrade was not rejected');assert not state()['extensions']['candidate'];assert state()['extensions']['items'][0]['version']=='3.0'
                item_button(90);assert state()['extensions']['items'][0]['enabled'] is False;screenshot('ru-pending-disable.png');layouts('pending-buttons');restart()
                probe(lambda p:not p['extension']);assert not state()['extensions']['items'][0]['loaded']
                manager();item_button(90);restart();probe(lambda p:p['extension'] and p['extension'].get('version')=='3.0');assert Handler.probes[-1]['extension']['preserved']=='version-one'
                manager();item_button(430);wait_until(lambda:state()['dialog_count']==1,'No remove confirmation');key_at(window,'Return');wait_until(lambda:not state()['extensions']['items'],'Extension was not removed');restart();probe(lambda p:not p['extension'])
                saved=tomllib.loads((profile/'profile.toml').read_text());assert all(not t['url'].endswith('private.html') for t in saved['session']+saved['history']);assert saved['bookmarks'][0]['title']=='Keep this'
                key_at(window,'q',4);time.sleep(.4)
                if state()['dialog_count']:key_at(window,'Return')
                wait_until(lambda:state()['cef_created']==state()['cef_closed'],'CEF did not close all browser instances')
                time.sleep(.5)
            finally:
                (out/'last-probes.json').write_text(json.dumps([p.get('extension') for p in Handler.probes[-10:]],indent=2))
                try:os.killpg(proc.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:proc.wait(timeout=4)
                except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    (out/'PASS.txt').write_text('PASS: verified upgrade + permission delta + real restart + session/private policy + storage + disable/enable/remove\n')
    print('PASS: extension upgrade and restart lifecycle')
if __name__=='__main__':main()
