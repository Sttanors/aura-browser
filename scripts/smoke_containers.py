"""Real Chromium storage isolation, inheritance, restore and deferred container cleanup."""
import json, os, re, shutil, signal, subprocess, threading, time, tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/containers-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        url=f'http://127.0.0.1:{server.server_port}/container.html'
        identity=0;temporary_identity=0
        for launch in range(3):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={url}?name=Normal&set=normal'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    selected=None
                    def root():
                        if p.poll() is not None:raise RuntimeError('Container browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def state():
                        r=root()
                        if selected is None:return r
                        return next((w for w in r.get('windows',[]) if w['window']==selected),{})
                    def current():
                        s=state();ts=s.get('tabs',[]);return ts[s.get('active_tab',0)] if ts else {}
                    def doc(tab=None):
                        try:return json.loads((tab or current()).get('title',''))
                        except ValueError:return {}
                    def ready(value):return doc().get('storage')==value and doc().get('idb')==value and ((f'aura_container={value}' in doc().get('cookie','')) if value else 'aura_container=' not in doc().get('cookie',''))
                    wait_until(lambda:root().get('presentation_frames',0)>0,'No container window',timeout=30);w=root()['window'];original=w
                    def click(r):click_at(w,r[0]+r[2]//2,r[1]+r[3]//2)
                    def navigate(address):
                        key_at(w,'l',4);type_text(w,address);key_at(w,'Return')
                    def manager():
                        key_at(w,'Escape');key_at(w,'c',12);wait_until(lambda:state().get('containers_ui') is not None,'No container manager')
                    def item(id):return next(c for c in state()['containers_ui']['items'] if c['id']==id)
                    def name(text):
                        click(state()['containers_ui']['input']);key_at(w,'a',4);type_text(w,text)
                        wait_until(lambda:state()['containers_ui']['input_length']==len(text),'Name input failed')
                    def quit():
                        key_at(w,'q',4);p.wait(timeout=30);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    if launch==0:
                        wait_until(lambda:ready('normal'),'No default storage')
                        manager();name('Work')
                        r=state()['containers_ui']['input'];click_at(w,r[0]+221,r[1]+62)
                        key_at(w,'Return');wait_until(lambda:len(state()['containers_ui']['items'])==2,'Create failed')
                        identity=state()['containers_ui']['items'][1]['id'];assert item(identity)['color']==2
                        click(item(identity)['rename']);name('Team');key_at(w,'Return')
                        wait_until(lambda:item(identity)['name']=='Team','Rename failed')
                        name('Team');key_at(w,'Return');wait_until(lambda:state()['containers_ui']['error'],'Duplicate accepted')
                        key_at(w,'Escape');key_at(w,'Escape');manager();click(item(identity)['new_tab'])
                        navigate(url+'?name=Team&set=work');wait_until(lambda:ready('work'),'Container storage failed')
                        assert current()['container']==identity
                        key_at(w,'t',4);navigate(url+'?name=Inherited');wait_until(lambda:ready('work'),'New tab did not share container')
                        assert current()['container']==identity;key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==2,'Inherited close failed')
                        for y,label in ((112,'Child'),(166,'Popup')):
                            click_at(w,350,88+y);wait_until(lambda:doc().get('name')==label and ready('work'),'Link/popup container inheritance failed')
                            assert current()['container']==identity
                            key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==2,'Child/popup close failed')
                        manager();click(item(0)['new_tab']);navigate(url+'?name=DefaultRead');wait_until(lambda:ready('normal'),'Default cookies leaked')
                        assert current()['container']==0;key_at(w,'w',4)
                        key_at(w,'n',12);navigate(url+'?name=PRIVATE_CONTAINER_SENTINEL');wait_until(lambda:doc().get('name')=='PRIVATE_CONTAINER_SENTINEL' and ready(None),'Private storage leaked')
                        assert current()['private'] and current()['container']==0
                        key_at(w,'c',12);time.sleep(.3);assert state().get('containers_ui') is None
                        key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==2,'Private close failed')
                        # A second window reuses the same named container but not the default context.
                        key_at(w,'n',4);wait_until(lambda:len(root().get('windows',[]))==2,'No second window')
                        second=next(v['window'] for v in root()['windows'] if v['window']!=original)
                        selected=second;w=second;manager();click(item(identity)['new_tab']);navigate(url+'?name=OtherWindow');wait_until(lambda:ready('work'),'Cross-window context not shared')
                        selected=None;w=original;key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Original container tab close failed')
                        manager();assert item(identity)['in_use'];click(item(identity)['remove']);assert state()['dialog_count']==0
                        selected=second;w=second;key_at(w,'w',5);wait_until(lambda:len(root()['windows'])==1,'Other window did not close')
                        selected=None;w=original;manager();assert not item(identity)['in_use'];click(item(identity)['new_tab']);navigate(url+'?name=TeamRead');wait_until(lambda:ready('work'),'Persistent context was lost')
                        key_at(w,'1',4);navigate(url+'?name=NormalRead');wait_until(lambda:ready('normal'),'Default read failed');key_at(w,'2',4)
                        manager();subprocess.run(['import','-window',str(w),str(out/'en-containers.png')],check=True,timeout=10);key_at(w,'Escape');quit()
                    elif launch==1:
                        wait_until(lambda:len(state().get('tabs',[]))==2 and all(doc(t).get('idb')!='pending' and t['loaded'] for t in state()['tabs']),'Container restore failed',timeout=30)
                        for t in state()['tabs']:
                            expected='work' if t['container']==identity else 'normal';d=doc(t)
                            assert d['storage']==expected and d['idb']==expected and f'aura_container={expected}' in d['cookie']
                        manager();assert item(identity)['in_use']
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3)
                        subprocess.run(['import','-window',str(w),str(out/'ru-containers-small.png')],check=True,timeout=10)
                        # Close while a fresh persistent context is initializing, then delete it.
                        name('Temporary');key_at(w,'Return');key_at(w,'End')
                        wait_until(lambda:len(state()['containers_ui']['items'])>=2 and any(c['name']=='Temporary' for c in state()['containers_ui']['items']),'Temporary container missing')
                        temporary_identity=next(c['id'] for c in state()['containers_ui']['items'] if c['name']=='Temporary')
                        click(item(temporary_identity)['new_tab']);navigate(url+'?name=PendingClose');key_at(w,'w',4)
                        wait_until(lambda:len(state()['tabs'])==2,'Pending container tab leaked')
                        manager();key_at(w,'End');wait_until(lambda:any(c['id']==temporary_identity for c in state()['containers_ui']['items']),'Temporary removal row missing')
                        click(item(temporary_identity)['remove']);wait_until(lambda:state()['dialog_count']==1,'No temporary removal confirmation');key_at(w,'Return')
                        wait_until(lambda:not any(c['id']==temporary_identity for c in state()['containers_ui']['items']),'Temporary removal failed')
                        key_at(w,'Escape');key_at(w,'2',4);key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Container tab did not close')
                        manager();wait_until(lambda:not item(identity)['in_use'],'Closed container still in use')
                        click(item(identity)['remove']);wait_until(lambda:state()['dialog_count']==1,'No removal confirmation');key_at(w,'Return')
                        wait_until(lambda:len(state()['containers_ui']['items'])==1,'Container removal failed')
                        assert (profile/'cache'/f'AuraContainer-{identity}').exists(),'Live context files were removed unsafely'
                        key_at(w,'Escape');quit()
                    else:
                        wait_until(lambda:ready('normal'),'Default data changed by container cleanup',timeout=30)
                        assert not (profile/'cache'/f'AuraContainer-{identity}').exists(),'Deferred cleanup did not remove container cache'
                        assert not (profile/'cache'/f'AuraContainer-{temporary_identity}').exists(),'Pending context cleanup failed'
                        prefs=tomllib.loads((profile/'profile.toml').read_text());assert not prefs.get('container_trash') and not prefs.get('containers')
                        quit()
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_CONTAINER_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: separate cookies/localStorage/IndexedDB, tab/link/popup/window inheritance, private isolation, restart, deletion guard and deferred cleanup, RU/EN')


if __name__=='__main__':main()
