"""Compare full native repaint with repeated page-only repaints at rounded edges."""
import json,os,shutil,signal,subprocess,threading,time
from functools import partial
from http.server import ThreadingHTTPServer
from PIL import Image,ImageChops
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until
from smoke_window import key_at

def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=rendering')
    out=ROOT/'artifacts/rendering-smoke';out.mkdir(parents=True,exist_ok=True)
    results=[]
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        cases=[(z,2200 if z>=150 else 1360,1500 if z>=150 else 900,i%3,1) for i,z in enumerate((75,80,90,100,110,125,150,175,200))]
        cases += [(90,1360,900,1,1.25),(125,2200,1500,0,1.25)]
        for zoom,w,h,theme,base in cases:
            case=f'{zoom}-{base}'
            profile=out/f'profile-{case}';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
            (profile/'profile.toml').write_text(f'ui_zoom={zoom}\ntheme={theme}\nfilter_updates=false\ncheck_updates=false\nrestore=false\n')
            report=out/f'report-{case}.json';report.unlink(missing_ok=True)
            with (out/f'process-{case}.log').open('w') as log:
                process=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--scale={base}',f'--url=http://127.0.0.1:{server.server_port}/render-edges.html'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if process.poll() is not None:raise RuntimeError('Rendering browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    wait_until(lambda:state().get('tabs') and state()['tabs'][0]['loaded'] and state()['tabs'][0]['frames']>5,'No animated page')
                    window=str(state()['window']);subprocess.run(['xdotool','windowsize','--sync',window,str(w),str(h)],check=True)
                    wait_until(lambda:state()['interface']['applied']==zoom,'Rendering scale did not apply')
                    def shot(name):
                        target=out/name;subprocess.run(['import','-window',window,str(target)],check=True,timeout=10);return Image.open(target).convert('RGB')
                    # F10 focuses the native toolbar, keeping the full painter active.
                    key_at(window,'F10');time.sleep(.6)
                    focused=state();content_before=focused['presentation']['content_frames'];frames_before=focused['tabs'][0]['frames']
                    wait_until(lambda:state()['tabs'][0]['frames']>=frames_before+10,'Full painter did not receive animated frames')
                    assert state()['presentation']['content_frames']==content_before,'Native focus did not force the full painter'
                    full=shot(f'full-{case}.png')
                    initial=state()['presentation']['content_frames'];key_at(window,'Escape')
                    wait_until(lambda:state()['presentation']['content_frames']>=initial+30,'Page-only repaint was not exercised',timeout=25)
                    fast=shot(f'page-{case}.png');assert full.size==fast.size
                    logical=state()['interface'];scale=logical['device_scale']*logical['applied']/100
                    x,y=round(252*scale),round(88*scale);right=round((logical['width']-12)*scale);bottom=round((logical['height']-12)*scale)
                    radius=round(20*scale);rects=[(x,y,x+radius,y+radius),(right-radius,y,right,y+radius),(x,bottom-radius,x+radius,bottom),(right-radius,bottom-radius,right,bottom)]
                    maximum=0
                    for i,rect in enumerate(rects):
                        difference=ImageChops.difference(full.crop(rect),fast.crop(rect));value=max(high for low,high in difference.getextrema());maximum=max(maximum,value)
                        difference.save(out/f'corner-diff-{case}-{i}.png')
                    results.append({'zoom':zoom,'device_scale':base,'theme':theme,'maximum_channel_difference':maximum})
                    assert maximum<=1,('Rounded edge changed after repeated page-only repaint',results[-1])
                    key_at(window,'q',4);process.wait(timeout=25)
                    final=json.loads(report.read_text());assert final['cef_created_total']==final['cef_closed_total']
                finally:
                    if process.poll() is None:os.killpg(process.pid,signal.SIGTERM);process.wait(timeout=5)
        server.shutdown()
    (out/'verification.json').write_text(json.dumps(results,indent=2));print('Rendering PASS: full/page-only corner equality, dark/light themes, 75–200%, device scale 1/1.25, balanced CEF')
if __name__=='__main__':main()
