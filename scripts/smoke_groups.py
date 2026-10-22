"""Native tab grouping with live forms, collapse, visual order and restart."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/groups-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\nconfirm_quit=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}/window-state.html'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';s=re.sub(r"(?m)^language\s*=.*$",'language="ru"',path.read_text());path.write_text(s)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={base}?name=A'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Group browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def current():
                        s=state();tabs=s.get('tabs',[]);return tabs[s.get('active_tab',0)] if tabs else {}
                    def document():
                        try:return json.loads(current().get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No group window',timeout=30)
                    w=state()['window']
                    def click(rect):click_at(w,rect[0]+rect[2]//2,rect[1]+rect[3]//2)
                    def manager():
                        key_at(w,'Escape');key_at(w,'g',12)
                        wait_until(lambda:state().get('groups_ui') is not None,'No group manager')
                    def name(value):
                        click(state()['groups_ui']['input']);key_at(w,'a',4);type_text(w,value)
                        wait_until(lambda:state()['groups_ui']['input_length']==len(value),'Group name did not arrive')
                    def group():return state()['groups_ui']['items'][0]
                    def navigate(url):
                        key_at(w,'l',4);type_text(w,url);key_at(w,'Return')
                        wait_until(lambda:current().get('url')==url and current().get('loaded'),'Group URL failed')
                    def header():return next(x['bounds'] for x in state()['sidebar'] if x['group']>0)
                    if launch==0:
                        wait_until(lambda:document().get('name')=='A','No first document')
                        click_at(w,352,208);wait_until(lambda:document().get('protected'),'A was not armed')
                        manager();name('Sources');key_at(w,'Return')
                        wait_until(lambda:state()['groups_ui']['items'] and current()['group']>0,'Create/assign group failed')
                        id=group()['id'];click(group()['color_button'])
                        wait_until(lambda:group()['color']==1,'Color did not change')
                        click(group()['rename']);name('References');key_at(w,'Return')
                        wait_until(lambda:group()['name']=='References','Rename failed')
                        name('References');key_at(w,'Return')
                        wait_until(lambda:bool(state()['groups_ui']['error']),'Duplicate group name accepted')
                        key_at(w,'Escape');key_at(w,'Escape')
                        key_at(w,'t',4);navigate(base+'?name=B')
                        assert current()['group']==id,'New tab did not inherit group'
                        click_at(w,352,208);wait_until(lambda:document().get('protected'),'B was not armed')
                        click(header());wait_until(lambda:len(state()['sidebar'])==1,'Collapsed tabs are still shown')
                        assert document()['protected'] and state()['dialog_count']==0 and state()['cef_created']==2
                        key_at(w,'Tab',4);wait_until(lambda:document().get('name')=='A' and len(state()['sidebar'])==3,'Keyboard did not reveal collapsed group')
                        assert document()['protected']
                        key_at(w,'n',12);navigate(base+'?name=PRIVATE_GROUP_SENTINEL');assert current()['private'] and current()['group']==id
                        key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==2,'Private group tab did not close')
                        manager();subprocess.run(['import','-window',str(w),str(out/'en-groups.png')],check=True,timeout=10);key_at(w,'Escape')
                        click(header());wait_until(lambda:len(state()['sidebar'])==1,'Collapse before restart failed')
                    else:
                        wait_until(lambda:len(state().get('tabs',[]))==2 and all(t['loaded'] for t in state()['tabs']),'Session group tabs did not load',timeout=30)
                        assert all(t['group']>0 for t in state()['tabs'])
                        wait_until(lambda:len(state()['sidebar'])==1,'Collapse state did not survive restart')
                        manager();assert group()['name']=='References' and group()['color']==1 and group()['collapsed']
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.4)
                        subprocess.run(['import','-window',str(w),str(out/'ru-groups-small.png')],check=True,timeout=10)
                        click(state()['groups_ui']['ungroup'])
                        wait_until(lambda:current()['group']==0 and len(state()['sidebar'])==2,'Ungroup failed')
                        key_at(w,'Tab',4)
                        wait_until(lambda:current()['group']>0 and len(state()['sidebar'])==3,'Group did not expand for keyboard focus')
                        # Grouped tabs are now before the ungrouped tab in visual keyboard order.
                        key_at(w,'1',4);wait_until(lambda:current()['group']>0,'Ctrl+1 ignored visual group order')
                        click_at(w,352,208);wait_until(lambda:document().get('protected'),'Removal document was not armed')
                        manager();click(group()['remove']);wait_until(lambda:state()['dialog_count']==1,'No group removal confirmation')
                        key_at(w,'Return');wait_until(lambda:not state()['groups_ui']['items'] and all(t['group']==0 for t in state()['tabs']),'Group removal lost tabs')
                        assert len(state()['tabs'])==2 and document()['protected'];key_at(w,'Escape')
                    key_at(w,'q',4);end=time.monotonic()+30
                    while p.poll() is None and time.monotonic()<end:
                        if json.loads(report.read_text()).get('dialog_count'):key_at(w,'Return')
                        time.sleep(.1)
                    p.wait(timeout=2);final=json.loads(report.read_text())
                    assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_GROUP_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    assert not tomllib.loads((profile/'profile.toml').read_text()).get('groups')
    print('PASS: group create/rename/color, collapse/reveal without closing forms, visual keyboard order, ungroup/remove, restart, private exclusion and RU/EN small layout')
if __name__=='__main__':main()
