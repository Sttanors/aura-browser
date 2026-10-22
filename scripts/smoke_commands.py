"""Native palette search, real tab/workspace activation, settings and privacy."""
import json,os,re,shutil,signal,subprocess,threading,time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/commands-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        url=f'http://127.0.0.1:{server.server_port}/window-state.html'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={url}?name=Alpha'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Palette browser exited')
                        try:return json.loads(report.read_text())
                        except (ValueError,OSError):return {}
                    def current():
                        s=state();tabs=s.get('tabs',[]);return tabs[s.get('active_tab',0)] if tabs else {}
                    def doc():
                        try:return json.loads(current().get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No palette window',timeout=30);w=state()['window']
                    def click(r):click_at(w,r[0]+r[2]//2,r[1]+r[3]//2)
                    def palette():
                        key_at(w,'k',4);wait_until(lambda:state().get('commands_ui') is not None and state()['commands_ui']['query']=='','No command palette')
                    def query(text):
                        key_at(w,'a',4);type_text(w,text);wait_until(lambda:state()['commands_ui']['query']==text,'Command query failed')
                    def paste(text):
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-quiet','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                        try:
                            clip.stdin.write(text.encode());clip.stdin.close();time.sleep(.15);key_at(w,'a',4);key_at(w,'v',4)
                            wait_until(lambda:state()['commands_ui']['query']==text,'Unicode query paste failed')
                        finally:
                            if clip.poll() is None:clip.terminate();clip.wait(timeout=3)
                    if launch==0:
                        wait_until(lambda:doc().get('name')=='Alpha','No Alpha page')
                        alpha=current()['id'];click_at(w,352,208);wait_until(lambda:doc().get('protected'),'Alpha page not protected')
                        key_at(w,'e',5);wait_until(lambda:state().get('workspaces_ui') is not None,'No workspace manager')
                        click(state()['workspaces_ui']['input']);type_text(w,'Work');key_at(w,'Return')
                        wait_until(lambda:state()['workspace']>0,'Workspace create failed');key_at(w,'Escape')
                        key_at(w,'l',4);type_text(w,url+'?name=Beta');key_at(w,'Return');wait_until(lambda:doc().get('name')=='Beta','No Beta page')
                        created=state()['cef_created'];palette();query('ALPHA')
                        wait_until(lambda:state()['commands_ui']['items'][0]['id']==str(alpha),'Title case folding failed')
                        key_at(w,'Return');wait_until(lambda:state()['workspace']==0 and current()['id']==alpha,'Palette did not activate hidden workspace tab')
                        assert doc()['protected'] and state()['cef_created']==created and state()['dialog_count']==0
                        palette();query('ui_zoom');key_at(w,'Return')
                        wait_until(lambda:(state().get('settings_ui') or {}).get('search')=='ui_zoom','Setting did not open exact editor')
                        key_at(w,'Escape');palette();query('new_tab');count=len(state()['tabs']);key_at(w,'Return')
                        wait_until(lambda:len(state()['tabs'])==count+1 and current()['ntp'],'New tab command failed')
                        palette();query('back');wait_until(lambda:state()['commands_ui']['items'][0]['id']=='back','Back result absent')
                        assert not state()['commands_ui']['items'][0]['enabled'];key_at(w,'Return');assert state().get('commands_ui')
                        query('NO_SUCH_COMMAND_739');wait_until(lambda:state()['commands_ui']['count']==0,'Empty result failed');key_at(w,'Return');assert state().get('commands_ui')
                        palette();key_at(w,'Down');wait_until(lambda:state()['commands_ui']['selected']==1,'Arrow selection failed')
                        key_at(w,'Tab');wait_until(lambda:state()['commands_ui']['selected']==2,'Tab selection failed')
                        key_at(w,'Next');wait_until(lambda:state()['commands_ui']['selected']>2,'PageDown failed')
                        subprocess.run(['import','-window',str(w),str(out/'en-commands.png')],check=True,timeout=10)
                        key_at(w,'Escape');key_at(w,'n',12);key_at(w,'l',4);type_text(w,url+'?name=PRIVATE_PALETTE_SENTINEL');key_at(w,'Return')
                        wait_until(lambda:doc().get('name')=='PRIVATE_PALETTE_SENTINEL','No private page')
                        palette();query('Alpha');wait_until(lambda:state()['commands_ui']['count']==0,'Normal tab exposed in private search')
                        key_at(w,'Escape');key_at(w,'w',4);wait_until(lambda:not current().get('private'),'Private close failed')
                        palette();query('PRIVATE_PALETTE_SENTINEL');wait_until(lambda:state()['commands_ui']['count']==0,'Private tab exposed in normal search');key_at(w,'Escape')
                        # The page with beforeunload still has the same live DOM.
                        palette();query('Alpha');key_at(w,'Return');wait_until(lambda:current()['id']==alpha,'Alpha return failed')
                        click_at(w,352,208);wait_until(lambda:not doc().get('protected'),'Could not unprotect fixture')
                    else:
                        palette();paste('МАСШТАБ ИНТЕРФЕЙСА')
                        wait_until(lambda:state()['commands_ui']['items'] and state()['commands_ui']['items'][0]['id']=='ui_zoom','Russian case folding failed')
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3)
                        subprocess.run(['import','-window',str(w),str(out/'ru-commands-small.png')],check=True,timeout=10)
                        key_at(w,'Return');wait_until(lambda:(state().get('settings_ui') or {}).get('search')=='ui_zoom','RU setting action failed')
                        key_at(w,'Escape');palette();paste('интерфейса масштаб')
                        assert any(x['id']=='ui_zoom' for x in state()['commands_ui']['items'])
                        # English ids remain available in Russian UI.
                        query('downloads');key_at(w,'Return');wait_until(lambda:state()['panel']==3,'English action id failed in RU')
                        key_at(w,'Escape')
                    key_at(w,'q',4);p.wait(timeout=30);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_PALETTE_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: title/RU/EN/token search, live cross-workspace activation, exact settings, actions, disabled/empty states, arrows/Tab/paging, privacy, RU small window')


if __name__=='__main__':main()
