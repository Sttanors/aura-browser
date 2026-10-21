"""Native window/profile sharing, private storage lifetime and session recovery."""
import json,os,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=windows')
    out=ROOT/'artifacts/windows-session-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile with spaces';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="ru"\nrestore=false\ncheck_updates=false\nconfirm_quit=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}/window-state.html'
        for launch in range(3):
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            args=[str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']
            args += [f'--url={base}?name=root&set=normal'] if launch==0 else (['--resume-session'] if launch==1 else [])
            with (out/f'process-{launch}.log').open('w') as log:
                proc=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'},start_new_session=True)
                try:
                    def state():
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def windows():return state().get('windows',[])
                    def current(w):return next((x for x in windows() if x['window']==w),{})
                    def navigate(w,url):
                        key_at(w,'Escape');key_at(w,'l',4);type_text(w,url);key_at(w,'Return')
                        wait_until(lambda:any(t['url']==url and t['loaded'] for t in current(w).get('tabs',[])),'Window navigation failed')
                    def document(w):
                        s=current(w)
                        try:return json.loads(s['tabs'][s['active_tab']]['title'])
                        except (KeyError,IndexError,ValueError):return {}
                    def new(w,private=False):
                        old={x['window'] for x in windows()};key_at(w,'n',5 if private else 4)
                        wait_until(lambda:any(x['window'] not in old for x in windows()),'No new native window')
                        added=next(x for x in windows() if x['window'] not in old)
                        assert added['private_window']==private
                        assert len({x['pid'] for x in windows()})==1
                        return added['window']
                    def close(w):key_at(w,'w',5);wait_until(lambda:not current(w),'Closed window remains active')
                    def shot(w,name):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);time.sleep(.4);subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
                    wait_until(lambda:windows() and all(w['presentation_frames'] for w in windows()),'No window group',timeout=30)
                    root=windows()[0]['window']
                    if launch==0:
                        wait_until(lambda:document(root).get('storage')=='normal','No normal storage')
                        normal=new(root);navigate(normal,base+'?name=normal2')
                        wait_until(lambda:document(normal).get('storage')=='normal','Normal windows did not share storage')
                        assert 'aura_window=normal' in document(normal)['cookie']
                        key_at(normal,'d',4);wait_until(lambda:current(root)['bookmark_count']==1,'Bookmarks were not shared')
                        click_at(normal,352,208);wait_until(lambda:document(normal).get('protected'),'Protect-page click did not reach the page');key_at(normal,'w',5)
                        wait_until(lambda:current(normal).get('dialog_count')==1,'Beforeunload confirmation was not visible')
                        shot(normal,'close-beforeunload.png');key_at(normal,'Escape')
                        wait_until(lambda:current(normal) and current(normal)['dialog_count']==0,'Close cancellation lost the window')
                        click_at(normal,352,208);wait_until(lambda:document(normal).get('protected') is False,'Page protection did not turn off')
                        private1=new(root,True);navigate(private1,base+'?name=PRIVATE_SENTINEL&set=private')
                        wait_until(lambda:document(private1).get('storage')=='private','No private storage')
                        private2=new(root,True);navigate(private2,base+'?name=PRIVATE_SENTINEL_2')
                        wait_until(lambda:document(private2).get('storage')=='private','Private windows did not share temporary storage')
                        assert document(normal)['storage']=='normal' and 'aura_window=normal' in document(normal)['cookie']
                        shot(private2,'private-window.png');close(private1)
                        assert document(private2)['storage']=='private'
                        close(private2);time.sleep(.7)
                        private3=new(root,True);navigate(private3,base+'?name=PRIVATE_SENTINEL_3')
                        wait_until(lambda:document(private3).get('name')=='PRIVATE_SENTINEL_3','Private page did not report')
                        assert document(private3)['storage'] is None and 'aura_window=' not in document(private3)['cookie']
                        close(private3)
                        close(normal);assert current(root),'Closing another window closed the root'
                        normal=new(root);navigate(normal,base+'?name=keep-second')
                        key_at(root,'t',4);navigate(root,base+'?name=keep-tab')
                        click_at(root,90,200,3);time.sleep(.2);click_at(root,120,215)
                        wait_until(lambda:len(current(root)['tabs'])==2,'Root tabs changed unexpectedly')
                        click_at(root,90,252);time.sleep(.4)
                        click_at(root,80,35);shot(root,'window-menu-ru.png');key_at(root,'Escape')
                        for value in ('en','ru'):
                            click_at(root,1307,55);wait_until(lambda:current(root).get('settings_ui'),'No language settings')
                            key_at(root,'f',4);type_text(root,'language');key_at(root,'Return')
                            wait_until(lambda:current(root)['settings_ui'].get('search')=='language','No language search')
                            control=next(c for c in current(root)['settings_ui']['controls'] if c['id']=='language')
                            rect=next(o['bounds'] for o in control['options'] if o['value']==value)
                            click_at(root,rect[0]+rect[2]//2,rect[1]+rect[3]//2)
                            wait_until(lambda:current(normal)['settings']['language']==value,'Appearance settings did not propagate to another window')
                            key_at(root,'Escape');key_at(root,'Escape')
                            subprocess.run(['xdotool','windowsize','--sync',str(root),'900','620'],check=True);time.sleep(.3)
                            click_at(root,80,35);shot(root,'window-menu-'+value+'-small.png');key_at(root,'Escape')
                            subprocess.run(['xdotool','windowsize','--sync',str(root),'1360','900'],check=True);time.sleep(.3)
                        key_at(root,'q',4);proc.wait(timeout=30)
                    else:
                        if launch==2:
                            wait_until(lambda:current(root).get('dialog_count')==1,'No interrupted-session recovery prompt')
                            shot(root,'restore-interrupted.png');key_at(root,'Return')
                        wait_until(lambda:len(windows())==2 and len(current(root).get('tabs',[]))==2,'Multiple windows were not restored')
                        assert all(not w['private_window'] and not any(t['private'] for t in w['tabs']) for w in windows())
                        assert current(root)['active_tab']==1
                        assert current(root)['settings']['restore'] is False
                        saved=tomllib.loads((profile/'profile.toml').read_text())
                        assert len(saved['windows'])==2 and saved['windows'][0]['tabs'][0]['pinned']
                        assert 'PRIVATE_SENTINEL' not in (profile/'profile.toml').read_text()
                        assert len(saved['bookmarks'])==1
                        if launch==1:
                            private=new(root,True);navigate(private,base+'?name=PRIVATE_SENTINEL_CRASH&set=crash')
                            time.sleep(.8);os.killpg(proc.pid,signal.SIGKILL);proc.wait(timeout=10)
                        else:
                            # Closing the original window must leave a child usable.
                            child=next(w['window'] for w in windows() if w['window']!=root)
                            close(root);navigate(child,base+'?name=after-root-close')
                            key_at(child,'q',4);proc.wait(timeout=30)
                    if launch!=1:
                        final=state();assert final['cef_created_total']==final['cef_closed_total'],final
                    assert 'PRIVATE_SENTINEL' not in (profile/'profile.toml').read_text()
                    assert tomllib.loads((profile/'profile.toml').read_text())['session_clean'] == (launch!=1)
                finally:
                    try:os.killpg(proc.pid,signal.SIGTERM)
                    except ProcessLookupError:pass
                    try:proc.wait(timeout=4)
                    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    (out/'PASS.txt').write_text('PASS: native windows, normal sharing, private lifetime, session restore and crash recovery\n')
    print('PASS: native windows and session recovery')
if __name__=='__main__':main()
