"""Live workspace switching, native management and session/private persistence."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/workspaces-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\nconfirm_quit=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}/window-state.html'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';s=path.read_text();s=re.sub(r"(?m)^language\s*=.*$",'language="ru"',s);path.write_text(s)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={base}?name=A'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Workspace browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def current():
                        s=state();tabs=s.get('tabs',[])
                        return tabs[s.get('active_tab',0)] if tabs else {}
                    def document():
                        try:return json.loads(current().get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No workspace window',timeout=30)
                    w=state()['window']
                    def click(rect):click_at(w,rect[0]+rect[2]//2,rect[1]+rect[3]//2)
                    def manager():
                        key_at(w,'Escape');key_at(w,'e',5)
                        wait_until(lambda:state().get('workspaces_ui') is not None,'No workspace manager')
                    def item(id):return next(x for x in state()['workspaces_ui']['items'] if x['id']==id)
                    def select(id):
                        manager();click(item(id)['open'])
                        wait_until(lambda:state()['workspace']==id and state()['panel']==0,'Workspace did not activate')
                    def navigate(url):
                        key_at(w,'l',4);type_text(w,url);key_at(w,'Return')
                        wait_until(lambda:current().get('url')==url and current().get('loaded'),'Workspace URL failed')
                    def name(value):
                        click(state()['workspaces_ui']['input']);key_at(w,'a',4);type_text(w,value)
                        wait_until(lambda:state()['workspaces_ui']['input_length']==len(value),'Workspace name input failed')
                    if launch==0:
                        wait_until(lambda:document().get('name')=='A','No original document')
                        click_at(w,352,208);wait_until(lambda:document().get('protected'),'Original document not armed')
                        manager();name('Work');key_at(w,'Return')
                        wait_until(lambda:state()['workspace']!=0 and len(state()['workspaces_ui']['items'])==2,'Workspace creation failed')
                        space=state()['workspace'];key_at(w,'Escape');navigate(base+'?name=B')
                        click_at(w,352,208);wait_until(lambda:document().get('protected'),'Second document not armed')
                        select(0);wait_until(lambda:document().get('name')=='A','Workspace did not restore A')
                        assert document()['protected'] and state()['dialog_count']==0
                        assert state()['cef_created']==2,'Switching reloaded/recreated a document'
                        key_at(w,'t',4);navigate(base+'?name=A2')
                        select(space);wait_until(lambda:document().get('name')=='B','Workspace did not restore B')
                        assert document()['protected']
                        manager();click(item(space)['rename']);name('Research');key_at(w,'Return')
                        wait_until(lambda:item(space)['name']=='Research','Rename did not apply')
                        click(item(space)['rename']);name('Personal space');key_at(w,'Return')
                        wait_until(lambda:bool(state()['workspaces_ui']['error']),'Duplicate workspace name accepted')
                        assert state()['workspaces_ui']['input_length']==len('Personal space')
                        key_at(w,'Escape');key_at(w,'Escape')
                        # Private tab belongs to the workspace but never to its persisted session.
                        key_at(w,'n',12);navigate(base+'?name=PRIVATE_WORKSPACE_SENTINEL')
                        assert current()['private']
                        key_at(w,'w',4)
                        wait_until(lambda:not any(t['private'] for t in state()['tabs']),'Private workspace tab did not close')
                        manager();subprocess.run(['import','-window',str(w),str(out/'en-workspaces.png')],check=True,timeout=10);key_at(w,'Escape')
                    else:
                        wait_until(lambda:document().get('name')=='B','Selected workspace did not survive restart',timeout=30)
                        assert state()['workspace']>0 and state()['workspace_name']=='Research'
                        assert len(state()['tabs'])==3 and all(not t['private'] for t in state()['tabs'])
                        space=state()['workspace'];select(0)
                        wait_until(lambda:document().get('name')=='A2','Inactive workspace selected tab did not survive restart')
                        key_at(w,'t',4);navigate(base+'?name=C')
                        key_at(w,'1',4);wait_until(lambda:document().get('name')=='A','Ctrl+1 wrong workspace')
                        key_at(w,'2',4);wait_until(lambda:document().get('name')=='A2','Ctrl+2 selected hidden workspace tab')
                        key_at(w,'3',4);wait_until(lambda:document().get('name')=='C','Ctrl+3 wrong visible tab')
                        key_at(w,'Tab',4);wait_until(lambda:document().get('name')=='A','Ctrl+Tab crossed workspace')
                        key_at(w,'3',4);key_at(w,'w',4)
                        wait_until(lambda:len(state()['tabs'])==3,'C did not close')
                        key_at(w,'2',4);key_at(w,'w',4)
                        wait_until(lambda:len(state()['tabs'])==2,'A2 did not close')
                        select(space);wait_until(lambda:document().get('name')=='B','Research lost B')
                        click_at(w,352,208);wait_until(lambda:document().get('protected'),'Deletion document not armed')
                        manager();subprocess.run(['import','-window',str(w),str(out/'ru-workspaces.png')],check=True,timeout=10)
                        click(item(space)['remove']);wait_until(lambda:state()['dialog_count']==1,'No explicit workspace removal confirmation')
                        key_at(w,'Return');wait_until(lambda:state()['workspace']==0 and all(t['workspace']==0 for t in state()['tabs']),'Removal lost or failed to move tabs')
                        assert len(state()['tabs'])==2
                        assert any(json.loads(t['title']).get('name')=='B' and json.loads(t['title']).get('protected') for t in state()['tabs'])
                        key_at(w,'Escape')
                    key_at(w,'q',4)
                    end=time.monotonic()+30
                    while p.poll() is None and time.monotonic()<end:
                        if json.loads(report.read_text()).get('dialog_count'):key_at(w,'Return')
                        time.sleep(.1)
                    p.wait(timeout=2);final=json.loads(report.read_text())
                    assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                    for path in profile.glob('profile.toml*'):
                        assert 'PRIVATE_WORKSPACE_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    saved=tomllib.loads((profile/'profile.toml').read_text());assert len(saved['workspaces'])==1
    print('PASS: native workspace create/rename/validation, live protected documents, visible keyboard tab order, restart, private exclusion and deletion without closing pages')
if __name__=='__main__':main()
