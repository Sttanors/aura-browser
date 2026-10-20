"""Verify native start-page customization, zoom, scrolling and persistence."""
import json, os, shutil, signal, subprocess, threading, time, ctypes
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at, Event, ButtonEvent


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Use verify_browser.py --suite=startpage')
    out=ROOT/'artifacts/startpage-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile'
    if profile.exists():shutil.rmtree(profile)
    profile.mkdir();(profile/'profile.toml').write_text('language="ru"\nrestore=false\n')
    reports=[];saved=None
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures'))) as server:
        worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
        target=f'http://127.0.0.1:{server.server_port}/browser/a.html'
        for launch in range(2):
            path=out/f'report-{launch}.json';path.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                proc=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={path}'],cwd=ROOT,stdout=log,stderr=log,start_new_session=True,env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'})
                try:
                    def state():
                        if proc.poll() is not None:raise RuntimeError(f'Browser exited {proc.returncode}')
                        return json.loads(path.read_text()) if path.exists() else {}
                    wait_until(lambda:state().get('start_page') and state().get('presentation_frames',0)>0,'No native start page')
                    window=str(state()['window']);width=1360
                    def click(r):x,y,w,h=r;click_at(window,x+w//2,y+h//2)
                    def expect(id,value):wait_until(lambda:state()['settings'][id]==value,f'{id}: expected {value}')
                    def screenshot(name):time.sleep(.3);subprocess.run(['import','-window',window,str(out/name)],check=True,timeout=10)
                    def seek(id):
                        if state().get('panel')!=4:click_at(window,width-53,55)
                        wait_until(lambda:state().get('settings_ui') is not None,'No settings')
                        key_at(window,'f',4);type_text(window,id);key_at(window,'Return')
                        wait_until(lambda:(state().get('settings_ui') or {}).get('search')==id,'No setting search')
                        return next(c for c in state()['settings_ui']['controls'] if c['id']==id)
                    def choose(id,value):
                        c=seek(id);click(next(o['bounds'] for o in c['options'] if o['value']==value));expect(id,value)
                    def toggle(id,value):
                        if state()['settings'][id]!=value:
                            c=seek(id);x,y,w,h=c['bounds'];click_at(window,x+w-22,y+26);expect(id,value)
                    def text(id,value):
                        c=seek(id);x,y,w,h=c['bounds'];click_at(window,x+70,y+82)
                        key_at(window,'a',4)
                        if value:
                            clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                            clip.communicate(value.encode(),timeout=5);key_at(window,'v',4);time.sleep(.15)
                        else:key_at(window,'BackSpace')
                        key_at(window,'Return');expect(id,value)
                    def dismiss():key_at(window,'Escape');wait_until(lambda:state()['panel']==0,'Panel stayed open')
                    def zoom(value):
                        while state()['settings']['ntp_zoom']!=value:
                            before=state()['settings']['ntp_zoom']
                            click(state()['start_page']['zoom_in' if before<value else 'zoom_out'])
                            expect('ntp_zoom',before+(10 if before<value else -10))
                    def wheel(up=True,ctrl=False):
                        x11=ctypes.CDLL('libX11.so.6');x11.XOpenDisplay.restype=ctypes.c_void_p
                        x11.XSendEvent.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_int,ctypes.c_long,ctypes.POINTER(Event)]
                        x11.XSync.argtypes=[ctypes.c_void_p,ctypes.c_int];x11.XCloseDisplay.argtypes=[ctypes.c_void_p]
                        display=x11.XOpenDisplay(None);assert display
                        try:
                            for kind,mask in ((4,4),(5,8)):
                                event=Event();event.button=ButtonEvent(type=kind,display=display,window=int(window),x=600,y=300,state=4 if ctrl else 0,button=4 if up else 5,same_screen=True)
                                x11.XSendEvent(display,int(window),False,mask,ctypes.byref(event))
                            x11.XSync(display,False)
                        finally:x11.XCloseDisplay(display)
                    if launch==0:
                        screenshot('default-ru.png')
                        click(state()['start_page']['customize'])
                        wait_until(lambda:state().get('settings_ui') is not None,'Customize did not open settings')
                        screenshot('customization-settings-ru.png')
                        choose('accent','rose');choose('ntp_width','wide');choose('ntp_background','plain')
                        text('ntp_title','Мой уголок интернета');text('ntp_subtitle','Work, learn and explore at your own pace.')
                        toggle('ntp_clock',True);toggle('ntp_logo',False);toggle('ntp_link_icons',False)
                        choose('ntp_link_style','rows');choose('ntp_link_columns','2')
                        dismiss();click(state()['start_page']['manage'])
                        wait_until(lambda:state().get('shortcuts_ui') is not None,'No shortcut manager')
                        click(state()['shortcuts_ui']['items'][0]['edit']);click(state()['shortcuts_ui']['url'])
                        key_at(window,'a',4)
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                        clip.communicate(target.encode(),timeout=5);key_at(window,'v',4);time.sleep(.15)
                        click(state()['shortcuts_ui']['save'])
                        wait_until(lambda:state()['shortcuts_ui']['items'][0]['url']==target,'Shortcut did not update')
                        click(state()['shortcuts_ui']['items'][1]['remove'])
                        wait_until(lambda:len(state()['shortcuts_ui']['items'])==2,'Shortcut did not remove')
                        dismiss();assert len(state()['start_page']['links'])==2
                        key_at(window,'equal',4);expect('ntp_zoom',110)
                        key_at(window,'minus',4);expect('ntp_zoom',100)
                        wheel(True,True);expect('ntp_zoom',110)
                        key_at(window,'0',4);expect('ntp_zoom',100)
                        screenshot('customized-100.png')
                        zoom(200)
                        subprocess.run(['xdotool','windowsize','--sync',window,'900','620'],check=True);width=900;time.sleep(.4)
                        wait_until(lambda:state()['start_page']['maximum']>0,'Large start page did not become scrollable')
                        page=state()['start_page'];v=page['viewport'];assert v[0]+v[2]<=900
                        assert page['search'][0]>=v[0] and page['search'][0]+page['search'][2]<=v[0]+v[2]
                        screenshot('customized-200-top.png')
                        wheel(False);wait_until(lambda:state()['start_page']['scroll']>0,'Wheel did not scroll')
                        key_at(window,'End');wait_until(lambda:state()['start_page']['scroll']==state()['start_page']['maximum'],'End did not reach bottom')
                        screenshot('customized-200-bottom.png')
                        page=state()['start_page'];click(page['search'])
                        type_text(window,target);key_at(window,'Return')
                        wait_until(lambda:'Alpha' in subprocess.check_output(['xdotool','getwindowname',window],text=True),'Scaled search is not clickable')
                        assert all(t['zoom_percent']==100 for t in state()['tabs']),'Native zoom changed website zoom'
                        key_at(window,'t',4);wait_until(lambda:state().get('start_page') is not None,'No new start page')
                        key_at(window,'End');time.sleep(.3)
                        click(next(x['bounds'] for x in state()['start_page']['links'] if x['url']==target))
                        wait_until(lambda:'Alpha' in subprocess.check_output(['xdotool','getwindowname',window],text=True),'Scaled shortcut is not clickable')
                        key_at(window,'t',4);wait_until(lambda:state().get('start_page') is not None,'No final start page')
                        saved=state()['settings'].copy()
                    else:
                        assert state()['settings']==saved,'Customization did not persist'
                        subprocess.run(['xdotool','windowsize','--sync',window,'900','620'],check=True);width=900;time.sleep(.4)
                        screenshot('restored-200.png')
                        zoom(50);wait_until(lambda:state()['start_page']['maximum']==0,'Small zoom did not fit')
                        screenshot('customized-50.png')
                        click(state()['start_page']['zoom_reset']);expect('ntp_zoom',100)
                        choose('ntp_width','compact');choose('ntp_background','aura');choose('accent','blue')
                        choose('ntp_link_style','cards');choose('ntp_link_columns','auto');toggle('ntp_logo',True);toggle('ntp_link_icons',True)
                        toggle('ntp_greeting',False);dismiss();screenshot('compact-no-greeting.png')
                        toggle('ntp_greeting',True);choose('language','en');dismiss()
                        key_at(window,'F7');screenshot('customized-light-en.png')
                    click_at(window,width-28,15);proc.wait(timeout=20);assert proc.returncode==0
                    report=json.loads(path.read_text());assert report['cef_created']==report['cef_closed'];assert not report['audit'].get('renderer_crashes')
                    reports.append(report)
                finally:
                    if proc.poll() is None:
                        os.killpg(proc.pid,signal.SIGTERM)
                        try:proc.wait(timeout=4)
                        except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
        server.shutdown();worker.join()
    (out/'verification.json').write_text(json.dumps({'passed':True,'launches':reports},indent=2))
    print('Start page PASS: customization, 50–200% zoom, wheel/keyboard, scaled search/links, restart, website zoom isolation',flush=True)
if __name__=='__main__':main()
