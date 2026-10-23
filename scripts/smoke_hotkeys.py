"""User key capture, conflicts, actual dispatch, persistence and shared windows."""
import json,os,re,shutil,signal,subprocess,threading,time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/hotkeys-smoke';out.mkdir(parents=True,exist_ok=True);profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();url=f'http://127.0.0.1:{server.server_port}/split.html?name=A-hotkeys'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={url}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def root():
                        if p.poll() is not None:raise RuntimeError('Hotkeys browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    selected=None
                    def state():return next((v for v in root().get('windows',[]) if v['window']==selected),{}) if selected else root()
                    def ui():return state().get('hotkeys_ui') or {}
                    def click(r):click_at(w,int(r[0]+r[2]/2),int(r[1]+r[3]/2))
                    def manager():
                        if launch:key_at(w,'k',5)
                        else:
                            key_at(w,'k',4);wait_until(lambda:state().get('commands_ui') is not None,'No palette');type_text(w,'Keyboard shortcuts');wait_until(lambda:any(x['id']=='hotkeys' for x in state()['commands_ui']['items']),'No hotkeys action');click(next(x['bounds'] for x in state()['commands_ui']['items'] if x['id']=='hotkeys'))
                        wait_until(lambda:bool(ui()),'No hotkey manager')
                    def query(text):click(ui()['search']);key_at(w,'a',4);key_at(w,'BackSpace');type_text(w,text);wait_until(lambda:ui()['query']==text,'Hotkey query failed')
                    def row(id):return next(x for x in ui()['items'] if x['id']==id)
                    def bind(id,key,mods):click(row(id)['bind']);wait_until(lambda:ui()['recording']==id,'No recorder');key_at(w,key,mods)
                    def shot(name):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);subprocess.run(['xdotool','windowfocus','--sync',str(w)],check=True);time.sleep(.3);subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
                    wait_until(lambda:root().get('presentation_frames',0)>0,'No hotkeys window',timeout=30);w=root()['window']
                    wait_until(lambda:state()['tabs'][0]['url']==url and state()['tabs'][0]['loaded'],'Hotkeys source missing',timeout=30)
                    if not launch:
                        manager();query('new_tab');assert len(ui()['items'])==1
                        bind('new_tab','b',4);wait_until(lambda:ui()['error'] and ui()['recording']=='new_tab','Conflict not reported');assert row('new_tab')['key']==ord('T')
                        key_at(w,'c',4);wait_until(lambda:'reserved' in ui()['error'],'Clipboard key accepted');assert row('new_tab')['key']==ord('T');key_at(w,'Escape');wait_until(lambda:not ui()['recording'],'Recorder cancel failed')
                        bind('new_tab','m',5);wait_until(lambda:row('new_tab')['key']==ord('M') and not ui()['recording'],'Remap failed');shot('en-hotkeys.png');key_at(w,'Escape');count=len(state()['tabs']);key_at(w,'t',4);time.sleep(.4);assert len(state()['tabs'])==count;key_at(w,'m',5);wait_until(lambda:len(state()['tabs'])==count+1,'Custom new tab failed');key_at(w,'m',5|128);key_at(w,'m',5|64);time.sleep(.3);assert len(state()['tabs'])==count+1,'AltGr/Super must not dispatch browser commands'
                        manager();query('bookmarks');click(row('bookmarks')['disable']);wait_until(lambda:row('bookmarks')['key']==0,'Disable failed');key_at(w,'Escape');key_at(w,'b',4);time.sleep(.4);assert state()['panel']==0
                        manager();query('bookmarks');bind('bookmarks','t',4);wait_until(lambda:row('bookmarks')['key']==ord('T'),'Reuse freed chord failed');query('new_tab');click(row('new_tab')['reset']);wait_until(lambda:ui()['error'],'Reset conflict missing');assert row('new_tab')['key']==ord('M');query('bookmarks');click(row('bookmarks')['disable']);wait_until(lambda:row('bookmarks')['key']==0,'Second disable failed');query('hotkeys');bind('hotkeys','k',5);wait_until(lambda:row('hotkeys')['key']==ord('K') and not ui()['recording'],'Manager shortcut assign failed');key_at(w,'Escape')
                        # A configured browser command cannot escape an owned beforeunload dialog.
                        key_at(w,'1',4);key_at(w,'l',4);type_text(w,'clipboard_value');key_at(w,'a',4);key_at(w,'c',4);key_at(w,'End');key_at(w,'v',4);wait_until(lambda:state()['address_input_bytes']==30,'Native clipboard editing was intercepted');key_at(w,'Escape');click_at(w,330,236)
                        wait_until(lambda:'"protected":true' in state()['tabs'][0]['title'],'Protected form missing');key_at(w,'w',4);wait_until(lambda:state()['dialog_count']>0,'No beforeunload dialog');count=len(state()['tabs']);key_at(w,'m',5);time.sleep(.3);assert len(state()['tabs'])==count and state()['dialog_count']>0;key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Dialog cancel failed')
                    else:
                        manager();query('new_tab');assert row('new_tab')['key']==ord('M') and row('new_tab')['modifiers']==3;query('bookmarks');assert row('bookmarks')['key']==0
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3);query('');shot('ru-hotkeys-small.png')
                        click(ui()['search']);key_at(w,'a',4)
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-quiet','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                        try:
                            clip.stdin.write('АДРЕСНАЯ'.encode());clip.stdin.close();time.sleep(.15);key_at(w,'v',4);wait_until(lambda:ui()['query']=='АДРЕСНАЯ' and len(ui()['items'])==1,'RU search paste failed');assert row('focus_address')['key']==ord('L')
                        finally:
                            if clip.poll() is None:clip.terminate();clip.wait(timeout=3)
                        key_at(w,'Tab');key_at(w,'Return');wait_until(lambda:ui()['recording']=='focus_address','Keyboard-only recorder failed');key_at(w,'Escape');key_at(w,'Escape')
                        original=w;key_at(w,'n',4);wait_until(lambda:len(root().get('windows',[]))==2,'Second window missing');w=next(v['window'] for v in root()['windows'] if v['window']!=original);selected=w
                        key_at(w,'m',5);wait_until(lambda:len(state()['tabs'])==2,'Other window did not inherit hotkeys');selected=None;w=original;manager();query('new_tab');click(row('new_tab')['reset']);wait_until(lambda:row('new_tab')['key']==ord('T'),'Reset key failed');key_at(w,'Escape');selected=next(v['window'] for v in root()['windows'] if v['window']!=original);w=selected;count=len(state()['tabs']);key_at(w,'t',4);wait_until(lambda:len(state()['tabs'])==count+1,'Other window did not update live');key_at(w,'m',5);time.sleep(.4);assert len(state()['tabs'])==count+1
                        manager();click(ui()['reset_all']);wait_until(lambda:not ui()['recording'] and not ui()['error'],'Reset all failed');key_at(w,'Escape');key_at(w,'b',4);wait_until(lambda:state()['panel']==1,'Default bookmark shortcut not restored');key_at(w,'Escape');selected=None;w=original
                    key_at(w,'q',4);end=time.monotonic()+30
                    while p.poll() is None and time.monotonic()<end:
                        for win in root().get('windows',[]):
                            if win['dialog_count']:key_at(win['window'],'Return')
                        time.sleep(.1)
                    p.wait(timeout=5);final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=10)
        server.shutdown()
    print('PASS hotkeys: actual remap/disable/old shortcut off, conflict/reserved/cancel, reset collision, persistence, owned dialog, shared windows, RU small panel, reset all')
if __name__=='__main__':main()
