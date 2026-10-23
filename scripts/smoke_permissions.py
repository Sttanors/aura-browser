"""Native permission changes and revocation, using synthetic devices on an isolated display."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from contextlib import ExitStack
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/permissions-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n')
    with ExitStack() as stack:
        origins=[]
        for _ in range(3):
            server=stack.enter_context(ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))))
            threading.Thread(target=server.serve_forever,daemon=True).start();stack.callback(server.shutdown)
            origins.append(f'http://127.0.0.1:{server.server_port}')
        a,b,c=origins;url=a+'/permissions.html'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}','--use-fake-device-for-media-stream','--proxy-server=http://127.0.0.1:9']+([f'--url={url}'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Permissions browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def current():
                        s=state();ts=s.get('tabs',[]);return ts[s.get('active_tab',0)] if ts else {}
                    def doc():
                        try:return json.loads(current().get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:current().get('loaded') and doc().get('token'),'No permissions fixture',timeout=30);w=state()['window']
                    def click(r):click_at(w,int(r[0]+r[2]/2),int(r[1]+r[3]/2))
                    def button(name):
                        r=doc()['buttons'][name];click_at(w,int(252+r[0]+r[2]/2),int(88+r[1]+r[3]/2))
                    def manager():
                        key_at(w,'i',12);wait_until(lambda:state().get('permissions_ui') is not None,'No permission center')
                    def row(id):return next((r for r in state()['permissions_ui']['rows'] if r['id']==id),None)
                    def choose(id,value):
                        while not row(id):key_at(w,'Next');time.sleep(.2)
                        click(row(id)['choice']);wait_until(lambda:state().get('menu_ui') is not None,'No permission choices')
                        item=next(x for x in state()['menu_ui'] if x['id']==['default','ask','allow','block'].index(value));assert item['enabled'],item
                        click(item['bounds']);wait_until(lambda:row(id)['value']==value and not state().get('menu_ui'),'Permission choice not applied')
                    def expect(name,value):wait_until(lambda:doc().get('states',{}).get(name)==value,f'{name} should be {value}, got {doc()}')
                    def navigate(address):
                        key_at(w,'l',4);type_text(w,address);key_at(w,'Return');wait_until(lambda:current().get('url')==address and current().get('loaded') and doc().get('token'),'Permission fixture navigation failed')
                    def tracks(kind,state_name):return [t for t in doc().get('tracks',[]) if t['kind']==kind and t['state']==state_name]
                    if launch==0:
                        expect('camera','denied');manager();assert state()['permissions_ui']['origin']==a
                        click(row('camera')['choice']);wait_until(lambda:state().get('menu_ui') is not None,'No camera menu');key_at(w,'Home');key_at(w,'Down');key_at(w,'Down');key_at(w,'Return')
                        wait_until(lambda:row('camera')['value']=='allow','Keyboard choice failed');expect('camera','granted');key_at(w,'Escape');token=doc()['token']
                        button('camera');wait_until(lambda:tracks('video','live') and current().get('media_video'),'Synthetic camera did not start',timeout=25)
                        assert all('fake' in t['label'].lower() for t in tracks('video','live')),doc()
                        button('protect');navigate_attempt=b+'/permissions.html'
                        key_at(w,'l',4);type_text(w,navigate_attempt);key_at(w,'Return');wait_until(lambda:state()['dialog_count']>0,'No beforeunload');key_at(w,'Escape')
                        wait_until(lambda:state()['dialog_count']==0,'Unload cancel failed');assert doc()['token']==token and current()['media_video']
                        manager();click(row('camera')['revoke']);expect('camera','denied')
                        wait_until(lambda:tracks('video','ended') and not current().get('media_video'),'Revoking permission did not stop real capture',timeout=20)
                        assert doc()['token']==token and doc()['form']=='retained form';key_at(w,'Escape');button('protect')
                        manager();choose('camera','ask');key_at(w,'Escape');button('camera')
                        wait_until(lambda:state()['dialog_count']>0,'No native camera prompt');key_at(w,'Return')
                        wait_until(lambda:tracks('video','live') and current()['media_video'],'Camera prompt allow failed');manager();assert row('camera')['value']=='allow'
                        choose('microphone','allow');key_at(w,'Escape');button('microphone')
                        wait_until(lambda:tracks('audio','live') and current()['media_audio'],'Synthetic microphone did not start',timeout=25)
                        manager();click(row('microphone')['revoke']);expect('microphone','denied')
                        wait_until(lambda:tracks('audio','ended') and not current()['media_audio'],'Microphone revoke did not stop capture')
                        assert tracks('video','live') and current()['media_video'];key_at(w,'Escape');button('stop')
                        wait_until(lambda:not current()['media_video'],'Stopped camera still indicated')
                        manager();choose('camera','ask');key_at(w,'Escape');button('expire')
                        wait_until(lambda:state()['dialog_count']>0,'No expiring media prompt')
                        wait_until(lambda:current().get('url','').endswith('?expired=1') and doc().get('token')!=token and state()['dialog_count']==0,'Navigation did not cancel pending prompt')
                        manager();assert row('camera')['value']=='ask';choose('camera','allow');key_at(w,'Escape')
                        manager();choose('notifications','ask');key_at(w,'Escape');button('notify')
                        wait_until(lambda:state()['dialog_count']>0,'No notification prompt');key_at(w,'Escape');expect('notifications','denied')
                        manager();assert row('notifications')['value']=='block';choose('geolocation','allow');key_at(w,'Escape');expect('geolocation','granted')
                        # A camera grant must not imply PTZ control. Other origins/ports remain blocked.
                        manager();key_at(w,'End');wait_until(lambda:row('camera_controls') is not None,'No PTZ row');assert row('camera_controls')['value']=='default';key_at(w,'Escape')
                        navigate(b+'/permissions.html');expect('camera','denied');expect('geolocation','denied')
                        navigate(url+'?child='+c+'/permissions.html');expect('geolocation','granted')
                        wait_until(lambda:doc().get('children',{}).get(c,{}).get('states',{}).get('geolocation')=='granted','Iframe did not use top-origin grant')
                        navigate(b+'/permissions.html?child='+c+'/permissions.html');expect('geolocation','denied')
                        wait_until(lambda:doc().get('children',{}).get(c,{}).get('states',{}).get('geolocation')=='denied','Iframe grant leaked across top origins')
                        key_at(w,'c',12);wait_until(lambda:state().get('containers_ui') is not None,'No containers');click(state()['containers_ui']['input']);type_text(w,'Permissions');key_at(w,'Return')
                        wait_until(lambda:len(state()['containers_ui']['items'])==2,'No permission container');container=state()['containers_ui']['items'][1]['id'];click(state()['containers_ui']['items'][1]['new_tab']);navigate(url);expect('camera','denied')
                        manager();choose('geolocation','allow');key_at(w,'Escape');expect('geolocation','granted');key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Container close failed')
                        key_at(w,'n',12);navigate(c+'/permissions.html?PRIVATE_PERMISSION_SENTINEL');expect('camera','denied');manager();assert state()['permissions_ui']['private']
                        choose('camera','allow');expect('camera','granted');click(row('notifications')['choice']);wait_until(lambda:state().get('menu_ui') is not None,'No private notification choices');assert all(not x['enabled'] for x in state()['menu_ui'] if x['id'] in (1,2));key_at(w,'Escape')
                        click(state()['permissions_ui']['sites_button']);wait_until(lambda:state()['permissions_ui']['picker'],'No private picker');assert all(x['origin']==c for x in state()['permissions_ui']['sites'])
                        key_at(w,'Escape');key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Private close failed');navigate(url);expect('camera','granted');manager()
                        subprocess.run(['import','-window',str(w),str(out/'en-permissions.png')],check=True,timeout=10);key_at(w,'Escape')
                    else:
                        navigate(url);expect('camera','granted');expect('geolocation','granted');manager();assert row('camera')['value']=='allow' and row('camera')['native']=='allow'
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3)
                        subprocess.run(['import','-window',str(w),str(out/'ru-permissions-small.png')],check=True,timeout=10)
                        click(state()['permissions_ui']['sites_button']);wait_until(lambda:state()['permissions_ui']['picker'],'No site picker');click(state()['permissions_ui']['search']);type_text(w,'no-such-permission-site')
                        wait_until(lambda:state()['permissions_ui']['sites']==[],'Site search empty state failed');key_at(w,'a',4);type_text(w,a);key_at(w,'Return')
                        wait_until(lambda:not state()['permissions_ui']['picker'],'Keyboard site select failed');click(state()['permissions_ui']['reset']);expect('camera','denied');expect('geolocation','denied');key_at(w,'Escape')
                        key_at(w,'n',12);navigate(c+'/permissions.html?PRIVATE_PERMISSION_SENTINEL');expect('camera','denied');key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Restart private close failed')
                    key_at(w,'q',4);p.wait(timeout=30);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_PERMISSION_SENTINEL' not in path.read_text() and not any(r['origin']==c for r in tomllib.loads(path.read_text()).get('site_permissions',[]))
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
    print('PASS: real synthetic camera/microphone revoke, prompt allow/deny, beforeunload cancellation, origin/port/iframe/container/private isolation, restart/reset, RU small UI')


if __name__=='__main__':main()
