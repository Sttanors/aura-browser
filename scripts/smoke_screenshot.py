"""Inspect actual CDP PNG pixels on an isolated CPU display, never the desktop."""
import json,os,re,shutil,signal,subprocess,threading,time
from functools import partial
from http.server import ThreadingHTTPServer
from PIL import Image
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at,drag_between

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/screenshot-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();url=f'http://127.0.0.1:{server.server_port}/screenshot.html'
        report=out/'report.json';report.unlink(missing_ok=True)
        with (out/'process.log').open('w') as log:
            p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={url}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
            try:
                def state():
                    if p.poll() is not None:raise RuntimeError('Screenshot browser exited')
                    try:return json.loads(report.read_text())
                    except (OSError,ValueError):return {}
                def ui():return state().get('screenshot_ui') or {}
                def doc():
                    s=state()
                    try:return json.loads(s['tabs'][s['active_tab']]['title'])
                    except (KeyError,ValueError):return {}
                wait_until(lambda:doc().get('token'),'Screenshot fixture missing',timeout=35);w=state()['window']
                def factor():return state()['interface']['applied']/100*state()['interface']['device_scale']
                def click(r):click_at(w,int((r[0]+r[2]/2)*factor()),int((r[1]+r[3]/2)*factor()))
                def open(mode=0):
                    key_at(w,'s',5);wait_until(lambda:bool(ui()),'No screenshot panel');click(ui()['options'][mode]['bounds']);wait_until(lambda:ui()['mode']==mode,'Screenshot mode failed')
                def picker():
                    pid=ui().get('picker_pid',-1)
                    if pid<0:return None
                    r=subprocess.run(['xdotool','search','--onlyvisible','--pid',str(pid)],capture_output=True,text=True,timeout=5)
                    return r.stdout.strip().splitlines()[-1] if r.returncode==0 and r.stdout.strip() else None
                def save(name):
                    wait_until(lambda:picker() or ui().get('error'),'No screenshot save dialog',timeout=30)
                    assert not ui().get('error'),ui()
                    target=out/name;target.unlink(missing_ok=True)
                    subprocess.run(['xdotool','windowfocus','--sync',picker()],check=True)
                    subprocess.run(['xdotool','key','--clearmodifiers','ctrl+l'],check=True);time.sleep(.2)
                    clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(str(target).encode(),timeout=5)
                    subprocess.run(['xdotool','key','--clearmodifiers','ctrl+a','ctrl+v'],check=True);time.sleep(.2);subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True);time.sleep(.8)
                    if ui().get('picker_pid',-1)>0:subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True)
                    wait_until(lambda:ui().get('last_export')==str(target) and target.exists(),'PNG save failed',timeout=20)
                    return Image.open(target).convert('RGB')
                def color(image,y,expected):
                    pixel=image.getpixel((image.width//2,y));assert all(abs(a-b)<4 for a,b in zip(pixel,expected)),(image.size,y,pixel,expected)
                original=doc()['token'];click_at(w,330,148);type_text(w,'unsaved_screenshot');wait_until(lambda:doc().get('value')=='unsaved_screenshot','Source form input failed');click_at(w,700,400);time.sleep(.2)
                # Scroll the actual CEF page by a script-free keyboard command.
                key_at(w,'End',4);wait_until(lambda:doc().get('y',0)>1000,'Page did not scroll')
                before=doc();open();click(ui()['capture']);image=save('visible.png')
                assert abs(image.width-before['clientWidth'])<=2 and abs(image.height-before['clientHeight'])<=2,(image.size,before)
                color(image,image.height-20,(187,34,136));assert doc()['token']==original and doc()['y']==before['y'] and doc()['value']=='unsaved_screenshot'
                key_at(w,'Escape');open(2);click(ui()['capture']);image=save('full.png')
                assert image.height==2600,image.size
                for y,c in [(200,(204,51,51)),(700,(51,170,85)),(1200,(51,102,204)),(1700,(238,204,51)),(2400,(187,34,136))]:color(image,y,c)
                assert doc()['token']==original and doc()['y']==before['y'] and doc()['value']=='unsaved_screenshot';key_at(w,'Escape')
                open(1);click(ui()['capture']);wait_until(lambda:ui().get('selecting'),'No area selector');area=ui()['area']
                drag_between(w,area[0]+40,area[1]+80,area[0]+240,area[1]+210);time.sleep(.3)
                key_at(w,'Return');image=save('Область.png');assert abs(image.width-200)<=2 and abs(image.height-130)<=2,image.size
                color(image,65,(238,204,51));key_at(w,'Escape')
                open(1);click(ui()['capture']);wait_until(lambda:ui().get('selecting'),'No second selector');key_at(w,'Escape');wait_until(lambda:not ui().get('selecting'),'Selector cancel failed');assert ui()['picker_pid']<0
                key_at(w,'Escape');open();click(ui()['capture']);wait_until(lambda:picker(),'No cancel chooser');subprocess.run(['xdotool','windowfocus','--sync',picker()],check=True);subprocess.run(['xdotool','key','--clearmodifiers','Escape'],check=True);wait_until(lambda:ui().get('picker_pid',0)<0,'Save cancel failed');assert not ui()['last_export']
                key_at(w,'Escape');key_at(w,'l',4);type_text(w,url+'?tall=1');key_at(w,'Return');wait_until(lambda:doc().get('full')==50000,'Tall document missing');open(2);click(ui()['capture']);image=save('tall.png');assert ui()['reduced'] and image.height<=32766 and image.width*image.height<=32*1024*1024; color(image,image.height-20,(187,34,136))
                key_at(w,'Escape');key_at(w,'t',4);key_at(w,'l',4);type_text(w,url+'?other=1');key_at(w,'Return');wait_until(lambda:doc().get('full')==1800,'Split source missing');key_at(w,'v',12);wait_until(lambda:state().get('split_view'),'No split view');open(2);click(ui()['capture']);image=save('split.png');assert image.height==1800 and image.width<before['width'];color(image,1200,(17,119,136));key_at(w,'Escape');key_at(w,'v',12)
                key_at(w,'q',4);p.wait(timeout=30);final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                config=profile/'profile.toml';text=config.read_text()
                for name,value in [('language','"ru"'),('ui_zoom','150'),('default_zoom','125')]:text=re.sub(r'(?m)^'+name+r'\s*=.*$',name+'='+value,text)
                config.write_text(text);report.unlink(missing_ok=True)
                with (out/'process-scaled.log').open('w') as scaled_log:
                    p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}','--scale=1.5','--private',f'--url={url}?PRIVATE_SCREENSHOT_SENTINEL'],stdout=scaled_log,stderr=scaled_log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                    wait_until(lambda:doc().get('token'),'Scaled private fixture missing',timeout=35);w=state()['window'];assert state()['private_window'] and state()['tabs'][state()['active_tab']]['zoom_percent']==125
                    before=doc();open();subprocess.run(['xdotool','windowraise',str(w)],check=True);subprocess.run(['xdotool','windowfocus','--sync',str(w)],check=True);time.sleep(.3);subprocess.run(['import','-window',str(w),str(out/'ru-scaled-panel.png')],check=True,timeout=10)
                    click(ui()['capture']);image=save('private-scaled.png');assert abs(image.width-before['clientWidth']*before['dpr'])<=3 and abs(image.height-before['clientHeight']*before['dpr'])<=3,(image.size,before)
                    key_at(w,'Escape');open(2);click(ui()['capture']);image=save('private-full-scaled.png');assert abs(image.height-2600*before['dpr'])<=3,(image.size,before);color(image,int(1200*before['dpr']),(51,102,204))
                    key_at(w,'Escape');open(1);click(ui()['capture']);wait_until(lambda:ui().get('selecting'),'Scaled selector missing');old=ui()['selection'];key_at(w,'Right');key_at(w,'Down',1);wait_until(lambda:ui()['selection'][0]==old[0]+10 and ui()['selection'][3]==old[3]+10,'Selector keyboard move/resize failed');subprocess.run(['import','-window',str(w),str(out/'ru-scaled-selector.png')],check=True,timeout=10)
                    key_at(w,'Tab');key_at(w,'Return');wait_until(lambda:not ui().get('selecting'),'Keyboard selector cancel failed');assert ui()['picker_pid']<0
                    # A source document reloaded after opening the tool must never be silently replaced.
                    key_at(w,'Escape');open();token=doc()['token'];key_at(w,'r',4);wait_until(lambda:doc().get('token') and doc()['token']!=token,'Reload did not change source');click(ui()['capture']);wait_until(lambda:ui().get('error'),'Stale screenshot source accepted');assert ui()['picker_pid']<0
                    click(ui()['refresh']);wait_until(lambda:not ui()['error'],'Refresh screenshot source failed');key_at(w,'Escape');key_at(w,'q',4);p.wait(timeout=30);final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                assert 'PRIVATE_SCREENSHOT_SENTINEL' not in config.read_text()
                if (profile/'profile.toml.bak').exists():assert 'PRIVATE_SCREENSHOT_SENTINEL' not in (profile/'profile.toml.bak').read_text()
                print('PASS screenshots: actual visible/full/region pixels, scroll/form preservation, native save/cancel/UTF-8, tall downscale, RU/UI150%/page125%/DPR1.5, private source, keyboard selector and stale navigation')
            finally:
                if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=10)
        server.shutdown()
if __name__=='__main__':main()
