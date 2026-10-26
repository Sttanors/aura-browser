"""Real normal/safe restarts, script/extension/filter isolation and sanitized export."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at
from smoke_extension_access import seed
class Handler(QuietHandler):
    def do_GET(self):
        if self.path.startswith('/ads/cbr.js'):
            body=b'window.diagnosticAd=true;';self.send_response(200);self.send_header('Content-Type','application/javascript');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        else:super().do_GET()
def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/safe-mode-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile-PRIVATE_PATH';shutil.rmtree(profile,ignore_errors=True);profile.mkdir();eid=seed(profile)
    registry=profile/'extensions/registry.toml';registry.write_text(registry.read_text()+'pinned=true\n')
    (profile/'profile.toml').write_text('language="en"\ncheck_updates=false\nfilter_updates=false\nrestore=false\nconfirm_quit=false\ndarkreader_mode="on"\nadblock=true\n')
    for name in ('aura.log','cef.log'):(profile/name).write_text('OLD_SECRET_URL https://private.invalid/?token=SECRET')
    binary=ROOT/'build/linux-x64-release/aura_browser';report=out/'report.json';clean=out/'diagnostics.json';p=None;pid=0
    with ThreadingHTTPServer(('127.0.0.1',0),partial(Handler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();url=f'http://127.0.0.1:{server.server_port}/diagnostics.html?token=PRIVATE_URL_SENTINEL'
        def state():
            try:return json.loads(report.read_text())
            except(OSError,ValueError):return {}
        def doc(w=None):
            s=state() if w is None else next((x for x in state().get('windows',[]) if x['window']==w),{})
            try:return json.loads(s['tabs'][s['active_tab']]['title'])
            except(KeyError,IndexError,ValueError):return {}
        def click(r,w=None):
            s=state();f=s['interface']['device_scale']*s['interface']['applied']/100;click_at(w or s['window'],round((r[0]+r[2]/2)*f),round((r[1]+r[3]/2)*f))
        def command(text,w=None):
            w=w or state()['window'];key_at(w,'Escape');key_at(w,'k',4);type_text(w,text);key_at(w,'Return')
        def diagnostics():
            command('diagnostics');wait_until(lambda:state().get('diagnostics_ui'),'No diagnostics panel')
        def shot(name):
            w=state()['window'];subprocess.run(['xdotool','windowraise',str(w)],check=True);time.sleep(.3);subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
        def prefs():
            s=tomllib.loads((profile/'profile.toml').read_text());assert s['darkreader_mode']=='on' and s['adblock']
            e=tomllib.loads(registry.read_text())['extensions'][0];assert e['enabled'] and e['pinned']
        def private_report(path):
            text=path.read_text();s=json.loads(text);assert s['schema']==1 and s['product']=='Aura'
            for word in ('http:','PRIVATE_','OLD_SECRET','SECRET','127.0.0.1','diagnostics.html',str(profile),'tabs_ui','password'):
                assert word not in text,(path,word,text)
            return s
        try:
            for launch in range(2):
                if launch:
                    text=(profile/'profile.toml').read_text()
                    for k,v in [('language','"ru"'),('ui_zoom','200'),('theme','1')]:text=re.sub(r'(?m)^'+k+r'\s*=.*$',k+'='+v,text)
                    (profile/'profile.toml').write_text(text)
                report.unlink(missing_ok=True);clean.unlink(missing_ok=True)
                args=[str(binary),f'--data-dir={profile}',f'--report={report}',f'--diagnostic-report={clean}',f'--url={url}']
                args+=['--safe-mode','--gpu',f'--load-extension={profile/"extensions"/(eid+"-123")}'] if launch else ['--software-rendering']
                with (out/f'process-{launch}.log').open('w') as log:
                    p=subprocess.Popen(args,stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                    wait_until(lambda:state().get('presentation_frames',0)>0 and doc().get('token'),'No diagnostics fixture',timeout=35);pid=state()['pid'];w=state()['window']
                    if not launch:
                        wait_until(lambda:doc().get('extension') and doc().get('dark') and not doc().get('ad'),'Normal baseline scripts/filter failed')
                        original=doc()['token'];diagnostics();shot('en-normal-dark.png');click(state()['diagnostics_ui']['restart']);wait_until(lambda:state()['dialog_count']==1,'No restart confirmation');key_at(w,'Escape')
                        assert not state()['safe_mode'] and doc()['token']==original
                        # A real beforeunload rejection must preserve the running mode and DOM.
                        key_at(w,'Escape');click_at(w,352,208);wait_until(lambda:doc().get('protected'),'No beforeunload protection')
                        diagnostics();click(state()['diagnostics_ui']['restart']);wait_until(lambda:state()['dialog_count']==1,'No restart confirmation');key_at(w,'Return');time.sleep(.4)
                        wait_until(lambda:state()['dialog_count']==1,'No native beforeunload dialog');key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Beforeunload rejection failed')
                        assert state()['pid']==pid and not state()['safe_mode'] and doc()['token']==original
                        key_at(w,'Escape');click_at(w,352,208);wait_until(lambda:not doc().get('protected'),'Unprotect failed')
                        # Incoming --safe-mode must request a restart, not open an ordinary window.
                        with (out/'forwarded.log').open('w') as forwarding:
                            forwarded=subprocess.run([str(binary),f'--data-dir={profile}','--safe-mode'],stdout=forwarding,stderr=forwarding,timeout=25)
                        assert forwarded.returncode==0;wait_until(lambda:state()['dialog_count']==1,'Safe mode relaunch was not forwarded');assert len(state()['windows'])==1;key_at(w,'Escape')
                        # Private tabs must not be restored by the mode switch.
                        key_at(w,'n',12);wait_until(lambda:any(t['private'] for t in state()['tabs']),'No private tab');key_at(w,'1',4)
                        diagnostics();click(state()['diagnostics_ui']['restart']);wait_until(lambda:state()['dialog_count']==1,'No safe restart confirmation');key_at(w,'Return')
                        wait_until(lambda:state().get('pid',pid)!=pid and state().get('safe_mode') and doc().get('token'),'Safe restart did not finish',timeout=40);p.wait(timeout=15);pid=state()['pid'];w=state()['window'];prefs()
                        assert not any(t['private'] for t in state()['tabs'])
                    else:
                        assert state()['safe_mode'];subprocess.run(['xdotool','windowsize','--sync',str(w),'1800','1240'],check=True)
                    wait_until(lambda:doc().get('ad') and not doc().get('extension') and not doc().get('dark'),'Safe mode still runs extension, injection or ad filter')
                    assert all(t['injection_scripts']==0 for t in state()['tabs'])
                    item=state()['extensions']['items'][0];assert item['enabled'] and item['pinned'] and not item['load_requested'] and not item['loaded'],item
                    assert not state()['extension_toolbar']
                    assert private_report(clean)['renderer']=='raster'
                    command('reader');wait_until(lambda:state().get('commands_ui') and state()['commands_ui']['items'],'No paused reader command')
                    assert not state()['commands_ui']['items'][0]['enabled'];key_at(w,'Escape')
                    diagnostics();shot('ru-200-safe-light.png' if launch else 'en-safe-dark.png')
                    # Keyboard can activate Save without accidentally switching mode.
                    key_at(w,'F10');key_at(w,'Tab');key_at(w,'Tab');key_at(w,'Return')
                    wait_until(lambda:state()['diagnostics_ui']['picker_pid']>0,'Keyboard Save did not open native chooser')
                    target=out/f'export-{launch}.json';target.unlink(missing_ok=True)
                    def picker():
                        r=subprocess.run(['xdotool','search','--onlyvisible','--pid',str(state()['diagnostics_ui']['picker_pid'])],capture_output=True,text=True);return r.stdout.strip().splitlines()[-1] if r.returncode==0 and r.stdout.strip() else None
                    wait_until(picker,'No native chooser window');subprocess.run(['xdotool','windowfocus','--sync',picker()],check=True);subprocess.run(['xdotool','key','--clearmodifiers','ctrl+l'],check=True);time.sleep(.2)
                    clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(str(target).encode(),timeout=5)
                    subprocess.run(['xdotool','key','--clearmodifiers','ctrl+a','ctrl+v'],check=True);time.sleep(.2);subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True);time.sleep(.7)
                    if picker():subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True)
                    wait_until(lambda:target.exists() and state()['diagnostics_ui']['last_export']==str(target),'Diagnostics export failed');assert private_report(target)['safe_mode']
                    key_at(w,'Escape');key_at(w,'n',5);wait_until(lambda:len(state()['windows'])==2,'No private safe window');child=next(x for x in state()['windows'] if x['window']!=w);cw=child['window'];assert child['safe_mode'] and child['private_window']
                    key_at(cw,'l',4);type_text(cw,url);key_at(cw,'Return');wait_until(lambda:doc(cw).get('ad'),'Private safe page did not load');assert not doc(cw)['extension'] and not doc(cw)['dark'];key_at(cw,'w',5);wait_until(lambda:len(state()['windows'])==1,'Private safe window did not close')
                    if not launch:
                        diagnostics();click(state()['diagnostics_ui']['restart']);wait_until(lambda:state()['dialog_count']==1,'No normal restart confirmation');key_at(w,'Return')
                        wait_until(lambda:state().get('pid',pid)!=pid and not state().get('safe_mode') and doc().get('extension') and doc().get('dark') and not doc().get('ad'),'Normal restart did not restore features',timeout=40);pid=state()['pid'];w=state()['window'];prefs()
                    key_at(w,'Escape');key_at(w,'q',4);wait_until(lambda:state().get('cef_created_total',0)>0 and state()['cef_created_total']==state()['cef_closed_total'],'Unbalanced safe mode shutdown',timeout=35)
                    if p.poll() is None:p.wait(timeout=15)
                    prefs();private_report(clean)
                    assert not (profile/'diagnostics/verbose.log').exists() and not (profile/'diagnostics/cef-verbose.log').exists()
                    logtext=(profile/'diagnostics/events.log').read_text();assert 'PRIVATE_' not in logtext and 'http:' not in logtext
                    for name in ('aura.log','cef.log'):assert 'OLD_SECRET_URL' in (profile/name).read_text()
                    pid=0;p=None
        finally:
            if pid:
                try:os.kill(pid,signal.SIGTERM)
                except ProcessLookupError:pass
            if p and p.poll() is None:
                try:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
                except ProcessLookupError:pass
            server.shutdown()
    print('PASS: normal/safe/normal native restart, cancel/beforeunload, single-instance forwarding, preserved preferences, real extension/Dark Reader/adblock isolation, forced CPU, private windows, RU/EN UI200%, keyboard/native sanitized export, default log privacy, balanced CEF shutdown')
if __name__=='__main__':main()
