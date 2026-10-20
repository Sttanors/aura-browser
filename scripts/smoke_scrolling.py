"""Measure actual Chromium wheel animation and CPU presentation on an isolated display."""
from pathlib import Path
import ctypes, json, os, shutil, signal, subprocess, threading, time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until
from smoke_window import click_at, key_at, Event, ButtonEvent

class Handler(QuietHandler):
    results=[]
    starts=0
    ready=0
    def do_GET(self):
        if self.path=='/scroll-ready':
            Handler.ready+=1;self.send_response(204);self.end_headers()
        elif self.path=='/scroll-start':
            Handler.starts+=1;self.send_response(204);self.end_headers()
        else:super().do_GET()
    def do_POST(self):
        self.results.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
        self.send_response(204);self.end_headers()

def wheel(window,x,y,button):
    x11=ctypes.CDLL('libX11.so.6');x11.XOpenDisplay.restype=ctypes.c_void_p
    x11.XSendEvent.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_int,ctypes.c_long,ctypes.POINTER(Event)]
    x11.XSync.argtypes=[ctypes.c_void_p,ctypes.c_int];x11.XCloseDisplay.argtypes=[ctypes.c_void_p]
    display=x11.XOpenDisplay(None)
    assert display
    try:
        for kind,mask in ((4,4),(5,8)):
            event=Event();event.button=ButtonEvent(type=kind,display=display,window=int(window),x=x,y=y,button=button,same_screen=True)
            assert x11.XSendEvent(display,int(window),False,mask,ctypes.byref(event))
        x11.XSync(display,False)
    finally:x11.XCloseDisplay(display)

def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=scrolling')
    out=ROOT/'artifacts/scrolling-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile'
    if profile.exists():shutil.rmtree(profile)
    profile.mkdir();(profile/'profile.toml').write_text('restore=false\nadblock=false\n')
    measured=[]
    with ThreadingHTTPServer(('127.0.0.1',0),partial(Handler,directory=str(ROOT/'tests/fixtures'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        path=out/'report.json';path.unlink(missing_ok=True)
        with (out/'process.log').open('w') as log:
            proc=subprocess.Popen([os.environ.get('AURA_SCROLL_BINARY',str(ROOT/'build/linux-x64-release/aura_browser')),f'--data-dir={profile}',f'--report={path}',f'--url=http://127.0.0.1:{server.server_port}/browser/scroll.html'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'})
            try:
                def state():
                    if proc.poll() is not None:raise RuntimeError(f'Browser exited {proc.returncode}')
                    return json.loads(path.read_text()) if path.exists() else {}
                wait_until(lambda:state().get('tabs') and state()['tabs'][0]['loaded'] and state()['tabs'][0]['frames']>0,'No scroll page')
                wait_until(lambda:Handler.ready>0,'Scroll page handlers not ready')
                wait_until(lambda:state()['presentation']['display']['refresh_hz']==int(os.environ.get('AURA_TEST_REFRESH','60')),'Monitor refresh was not detected')
                assert state()['tabs'][0]['frame_rate']==int(os.environ.get('AURA_TEST_REFRESH','60')),'CEF frame rate did not follow monitor'
                window=str(state()['window']);time.sleep(.5)
                def measure(name,x,y,buttons,axis,sign,scale=1):
                    initial=len(Handler.results)
                    frame_start=state()['presentation_frames']
                    display_before=(state().get('presentation',{}).get('display') or {}).get('last_ust',0)
                    def cpu_ms():
                        fields=Path(f'/proc/{proc.pid}/stat').read_text().rsplit(')',1)[1].split()
                        return (int(fields[11])+int(fields[12]))*1000/os.sysconf('SC_CLK_TCK')
                    cpu_start=cpu_ms()
                    before_start=Handler.starts
                    click_at(window,round(252*scale+60),round(88*scale+30))
                    try:wait_until(lambda:Handler.starts>before_start,'Measure button click not dispatched',timeout=4)
                    except Exception:
                        subprocess.run(['import','-window',window,str(out/'failed-measure.png')],check=True,timeout=10)
                        raise
                    time.sleep(.15)
                    for button in buttons:
                        wheel(window,round(252*scale+x),round(88*scale+y),button);time.sleep(.04)
                    wait_until(lambda:len(Handler.results)>initial,'No scroll samples')
                    samples=Handler.results[-1]['samples'];values=[s[axis] for s in samples]
                    positions=sorted(set(values));distance=values[-1]-1000
                    result={'name':name,'distinct_positions':len(positions),'distance':distance,'native_frames':state()['presentation_frames']-frame_start,'main_cpu_ms':cpu_ms()-cpu_start,'presentation':state().get('presentation'),'samples':samples}
                    display=state().get('presentation',{}).get('display',{})
                    frames=[f for f in display.get('frames',[]) if f[1]>display_before]
                    gaps=[(b[1]-a[1])/1000 for a,b in zip(frames,frames[1:]) if 0<b[1]-a[1]<100000]
                    result['display_frames']=frames
                    result['display_intervals_ms']=gaps
                    if os.environ.get('AURA_X11_PRESENT')!='0' and os.environ.get('AURA_SCROLL_BASELINE')!='1':
                        assert display.get('synchronized'), 'XPresent path not exercised'
                        assert display['maximum_pending']<=2, 'Unbounded presentation queue'
                        assert len(frames)>=5,(name,'No completed presentations')
                        assert all(b[2]>a[2] for a,b in zip(frames,frames[1:])),(name,'Several frames share one refresh',frames)
                        if name=='large-wheel-stream':
                            assert gaps and sorted(gaps)[int((len(gaps)-1)*.95)]<25,(name,'Uneven actual presentations',gaps)
                    if os.environ.get('AURA_X11_SHM')!='0' and os.environ.get('AURA_SCROLL_BASELINE')!='1':
                        assert display.get('shared_memory') and display.get('shm_frames',0)>0, 'MIT-SHM path not exercised'
                    measured.append(result)
                    print(name,len(positions),'positions; distance',distance,'native frames',result['native_frames'],'CPU ms',result['main_cpu_ms'],flush=True)
                    if os.environ.get('AURA_SCROLL_BASELINE')!='1':
                        assert len(positions)>=5,(name,'No smooth animation',positions)
                        assert sign*distance>50,(name,'Wrong direction',distance)
                        assert all(sign*(b-a)>=-1 for a,b in zip(values,values[1:])),(name,'Scroll reversed')
                        for other in ('y','n','x'):
                            if other!=axis:assert max(abs(s[other]-1000) for s in samples)<=1,(name,'Wrong scroll target')
                measure('page-down',120,320,[5],'y',1)
                measure('page-up',120,320,[4],'y',-1)
                measure('wheel-burst',120,320,[5,5,5,5],'y',1)
                measure('nested',550,180,[5],'n',1)
                measure('horizontal',550,440,[7],'x',1)
                # The same wheel coordinates must remain valid after changing native chrome scale.
                click_at(window,1307,55);wait_until(lambda:state().get('settings_ui'),'No settings')
                from smoke_browser import type_text
                key_at(window,'f',4);type_text(window,'ui_zoom');key_at(window,'Return')
                wait_until(lambda:(state().get('settings_ui') or {}).get('search')=='ui_zoom','No zoom settings')
                control=next(c for c in state()['settings_ui']['controls'] if c['id']=='ui_zoom')
                r=next(o['bounds'] for o in control['options'] if o['value']=='125');click_at(window,r[0]+r[2]//2,r[1]+r[3]//2)
                wait_until(lambda:state()['interface']['applied']==125,'UI zoom not applied');key_at(window,'Escape');time.sleep(.3)
                measure('page-125',120,320,[5],'y',1,1.25)
                subprocess.run(['xdotool','windowsize',window,'2200','1500'],check=True)
                wait_until(lambda:state()['interface']['width']==1760,'Large window not resized');time.sleep(.3)
                measure('large-wheel-stream',120,320,[5]*16,'y',1,1.25)
                if os.environ.get('AURA_SCROLL_BASELINE')!='1':
                    assert state()['presentation']['raster_allocations']<=3,'Raster buffer allocated every frame'
                subprocess.run(['import','-window',window,str(out/'scroll-page.png')],check=True,timeout=10)
                key_at(window,'q',4);proc.wait(timeout=20)
                assert proc.returncode==0
                final=json.loads(path.read_text());assert final['cef_created']==final['cef_closed']
            finally:
                (out/'measurements.json').write_text(json.dumps(measured,indent=2))
                if proc.poll() is None:os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=10)
        server.shutdown()
    print('Scrolling PASS: native page/nested/horizontal wheel, direction, burst, chrome zoom, balanced CEF lifetime')

if __name__=='__main__':main()
