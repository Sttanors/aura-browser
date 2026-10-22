"""Two live Chromium panes: focus, input, scroll, history, resize and restore."""
import json, os, re, shutil, signal, subprocess, threading, time, tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out=ROOT/'artifacts/split-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="ru"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        url=f'http://127.0.0.1:{server.server_port}/split.html?name='
        for launch in range(3):
            if launch:
                path=profile/'profile.toml';prefs=path.read_text()
                prefs=re.sub(r'(?m)^ui_zoom\s*=.*$',f'ui_zoom={200 if launch==1 else 75}',prefs)
                prefs=re.sub(r'(?m)^language\s*=.*$',f'language="{"en" if launch==1 else "ru"}"',prefs)
                path.write_text(prefs)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={url}A'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Split browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def docs():
                        result={}
                        for t in state().get('tabs',[]):
                            try:
                                d=json.loads(t['title']);result[d['name']]=d
                            except (ValueError,KeyError):pass
                        return result
                    def pane(name):return next(x['bounds'] for x in state()['split_view']['panes'] if x['url'].endswith('name='+name))
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No split window',timeout=30);w=state()['window']
                    def navigate(value):
                        key_at(w,'l',4);type_text(w,value);key_at(w,'Return')
                    def local(name,x,y):
                        r=pane(name);factor=state()['interface']['applied']/100;base=state()['interface']['device_scale'];click_at(w,round(r[0]*factor+x*base),round(r[1]*factor+y*base))
                    def quit():
                        key_at(w,'q',4);end=time.monotonic()+30
                        while p.poll() is None and time.monotonic()<end:
                            if json.loads(report.read_text()).get('dialog_count'):key_at(w,'Return')
                            time.sleep(.1)
                        p.wait(timeout=2);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    if not launch:
                        wait_until(lambda:'A' in docs(),'A missing')
                        key_at(w,'t',4);navigate(url+'B');wait_until(lambda:'B' in docs(),'B missing')
                        tokens={k:v['token'] for k,v in docs().items()}
                        key_at(w,'v',12);wait_until(lambda:state().get('split_view') is not None,'No split view')
                        wait_until(lambda:all(d['width']<650 for d in docs().values()),'Panes not resized')
                        for name,value in [('A','Left_data'),('B','Right_data')]:
                            local(name,80,98);type_text(w,value)
                            wait_until(lambda:docs()[name]['value']==value,'Input went to wrong pane')
                            local(name,80,148);wait_until(lambda:docs()[name]['protected'],'Pane form not protected')
                        assert {k:v['token'] for k,v in docs().items()}==tokens
                        r=pane('A');click_at(w,r[0]+r[2]//2,r[1]+320,button=5)
                        wait_until(lambda:docs()['A']['scroll']>0,'Left pane did not scroll')
                        assert docs()['B']['scroll']==0,'Scrolling leaked into other pane'
                        local('B',70,192)
                        wait_until(lambda:state()['dialog_count'] or 'BNext' in docs(),'Right navigation not started')
                        if state()['dialog_count']:key_at(w,'Return')
                        wait_until(lambda:'BNext' in docs(),'Right navigation failed')
                        key_at(w,'Left',8);wait_until(lambda:'B' in docs(),'Right back failed')
                        assert docs()['A']['token']==tokens['A'] and docs()['A']['value']=='Left_data'
                        local('B',70,238)
                        wait_until(lambda:any(t.get('popup_visible') for t in state()['tabs']),'Select popup did not open')
                        subprocess.run(['import','-window',str(w),str(out/'split-select.png')],check=True,timeout=10)
                        key_at(w,'Down');key_at(w,'Return')
                        wait_until(lambda:docs()['B'].get('selection')==1,'Select popup input failed')
                        d=state()['split_view']['divider'];x=d[0]+4;y=d[1]+d[3]//2
                        # Native X11 button state, without moving the real desktop pointer.
                        from smoke_window import drag_between
                        drag_between(w,x,y,x+100,y)
                        wait_until(lambda:state()['split_view']['percent']!=50,'Divider did not resize')
                        assert docs()['A']['token']==tokens['A']
                        subprocess.run(['import','-window',str(w),str(out/'split.png')],check=True,timeout=10)
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True)
                        wait_until(lambda:all(d['width']<420 for d in docs().values()),'Small split did not resize')
                        subprocess.run(['import','-window',str(w),str(out/'split-small.png')],check=True,timeout=10)
                        assert state()['cef_created']==2 and state()['dialog_count']==0
                        quit()
                    else:
                        wait_until(lambda:state().get('split_view') is not None and 'A' in docs() and 'B' in docs(),'Split restore failed',timeout=30)
                        assert state()['split_view']['percent']!=50
                        if launch==1:
                            subprocess.run(['xdotool','windowsize',str(w),'2000','1400'],check=True)
                        wait_until(lambda:abs(state()['interface']['applied']-(200 if launch==1 else 75))<.1,'Requested split interface zoom not applied')
                        tokens={k:v['token'] for k,v in docs().items()}
                        for name in ('A','B'):
                            local(name,80,98);type_text(w,'Zoom'+str(launch))
                            wait_until(lambda:docs()[name]['value']=='Zoom'+str(launch),'Scaled pane input drifted')
                        assert {k:v['token'] for k,v in docs().items()}==tokens
                        subprocess.run(['import','-window',str(w),str(out/f'split-zoom-{launch}.png')],check=True,timeout=10)
                        if launch==1:
                            quit()
                            continue
                        key_at(w,'v',12);wait_until(lambda:state()['split_view'] is None,'Exit split failed')
                        wait_until(lambda:max(d['width'] for d in docs().values())>900,'Full page width not restored')
                        key_at(w,'v',12);wait_until(lambda:state()['split_view'] is not None,'Reopen split failed')
                        key_at(w,'t',4);wait_until(lambda:state()['split_view'] is None,'New tab did not exit split')
                        key_at(w,'w',4);key_at(w,'v',12);wait_until(lambda:state()['split_view'] is not None,'Split setup before close failed')
                        key_at(w,'w',4);wait_until(lambda:state()['split_view'] is None and len(state()['tabs'])==1,'Closing a pane lost other page')
                        key_at(w,'n',12);navigate(url+'PRIVATE_SPLIT_SENTINEL')
                        wait_until(lambda:'PRIVATE_SPLIT_SENTINEL' in docs(),'No private split page')
                        key_at(w,'v',12);wait_until(lambda:state()['split_view'] is not None,'Private split failed')
                        quit()
                        for path in profile.glob('profile.toml*'):assert 'PRIVATE_SPLIT_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: independent live panes, form focus, scroll, history, divider, small window, session restore and close/new-tab transitions')


if __name__=='__main__':main()
