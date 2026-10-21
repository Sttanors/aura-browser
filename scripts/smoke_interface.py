"""Exercise live chrome scaling and CEF input independently of website zoom."""
import json, os, shutil, signal, subprocess, threading, time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Use verify_browser.py --suite=interface')
    out = ROOT / 'artifacts/interface-smoke'
    out.mkdir(parents=True, exist_ok=True)
    profile = out / 'profile'
    if profile.exists(): shutil.rmtree(profile)
    profile.mkdir()
    (profile / 'profile.toml').write_text('language="ru"\nfilter_updates=false\nrestore=false\n')
    with ThreadingHTTPServer(('127.0.0.1', 0), partial(QuietHandler, directory=str(ROOT/'tests/fixtures'))) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        target = f'http://127.0.0.1:{server.server_port}/browser/a.html'
        for launch in range(2):
            path = out / f'report-{launch}.json'
            path.unlink(missing_ok=True)
            base = 1 if launch == 0 else 1.25
            with (out / f'process-{launch}.log').open('w') as log:
                proc = subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'), f'--data-dir={profile}', f'--report={path}', f'--scale={base}'], cwd=ROOT, stdout=log, stderr=log, start_new_session=True, env={**os.environ, 'XMODIFIERS':'@im=none', 'AURA_BACKGROUND_TEST':'1'})
                try:
                    def state():
                        if proc.poll() is not None: raise RuntimeError(f'Browser exited {proc.returncode}')
                        try: return json.loads(path.read_text())
                        except (FileNotFoundError, json.JSONDecodeError): return {}
                    wait_until(lambda:state().get('start_page') and state().get('presentation_frames',0)>0, 'No start page')
                    window = str(state()['window'])
                    def factor(): return state()['interface']['applied'] / 100 * base
                    def point(x,y): click_at(window, round(x*factor()), round(y*factor()))
                    def click(r): x,y,w,h=r;point(x+w/2,y+h/2)
                    def screenshot(name):
                        time.sleep(.4)
                        subprocess.run(['import','-window',window,str(out/name)], check=True, timeout=10)
                    def seek(id):
                        if state()['panel'] != 4: point(state()['interface']['width']-53,55)
                        wait_until(lambda:state().get('settings_ui'), 'Settings did not open')
                        key_at(window,'f',4);type_text(window,id);key_at(window,'Return')
                        wait_until(lambda:(state().get('settings_ui') or {}).get('search')==id, 'No settings search')
                        return next(c for c in state()['settings_ui']['controls'] if c['id']==id)
                    def choose(id,value):
                        c=seek(id);click(next(o['bounds'] for o in c['options'] if o['value']==str(value)))
                        wait_until(lambda:str(state()['settings'][id])==str(value), f'Failed {id}={value}')
                    def verify_web(tag):
                        key_at(window,'Escape');key_at(window,'l',4);type_text(window,target);key_at(window,'Return')
                        wait_until(lambda:any(t['loaded'] and t['title']=='Alpha' for t in state()['tabs']), 'Alpha not loaded')
                        time.sleep(.3)
                        # Website CSS coordinates remain independent of chrome scaling.
                        click_at(window,round(252*factor()+100*base),round(88*factor()+164*base))
                        type_text(window,tag)
                        wait_until(lambda:any(t['title']=='Alpha input '+tag for t in state()['tabs']), 'CEF input coordinates drifted')
                        assert state()['tabs'][-1]['zoom_percent']==100
                        assert state()['settings']['ntp_zoom']==100
                        click_at(window,round(252*factor()+100*base),round(88*factor()+106*base))
                        wait_until(lambda:any(t['title']=='Beta' for t in state()['tabs']), 'CEF link coordinates drifted')
                    if launch==0:
                        for value in (75,125,200):
                            choose('ui_zoom',value)
                            wait_until(lambda:abs(state()['interface']['applied']-min(value,900/620*100))<.1, 'Wrong effective scale')
                            screenshot(f'ru-{value}.png')
                            verify_web('Scale'+str(value))
                        subprocess.run(['xdotool','windowsize',window,'2000','1400'],check=True)
                        wait_until(lambda:abs(state()['interface']['applied']-200)<.1,'Full 200% not applied in large window')
                        choose('language','en');seek('ui_zoom');screenshot('en-200-large.png');verify_web('Large')
                        subprocess.run(['xdotool','windowsize',window,'900','620'],check=True)
                        wait_until(lambda:abs(state()['interface']['applied']-100)<.1,'Small window did not fit')
                        seek('ui_zoom');screenshot('en-small-fit.png')
                        choose('ui_zoom',75)
                        wait_until(lambda:state()['interface']['applied']==75,'Small UI did not shrink')
                        subprocess.run(['xdotool','windowsize',window,'675','465'],check=True)
                        wait_until(lambda:state()['interface']['width']==900 and state()['interface']['height']==620,'Minimum window did not fit')
                        seek('ui_zoom');screenshot('minimum-675x465.png')
                        verify_web('Small')
                        key_at(window,'0',5)
                        wait_until(lambda:state()['settings']['ui_zoom']==100,'Emergency reset failed')
                        subprocess.run(['xdotool','windowsize',window,'1360','900'],check=True)
                        wait_until(lambda:state()['interface']['width']==1360,'Window not resized')
                        choose('ui_zoom',125)
                    else:
                        assert state()['settings']['ui_zoom']==125
                        assert abs(state()['interface']['applied']-125)<.1
                        verify_web('RestartDPI')
                        seek('ui_zoom');screenshot('restart-125-dpi.png')
                    key_at(window,'q',4);proc.wait(timeout=20)
                    assert proc.returncode==0
                    final=json.loads(path.read_text());assert final['cef_created']==final['cef_closed']
                finally:
                    if proc.poll() is None:
                        os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=10)
        server.shutdown()
    print('PASS: UI 75–200%, small-window fitting, live resize, RU/EN, CEF pointer/zoom isolation, reset, persistence and 1.25 device scale')


if __name__ == '__main__': main()
