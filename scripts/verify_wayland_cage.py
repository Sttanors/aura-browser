"""Second compositor smoke: wlroots/Cage headless Pixman, two outputs, fractional client scale."""
import json, os, shutil, signal, subprocess, tempfile, threading, time
from pathlib import Path
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until

def main():
    if not shutil.which('cage'):
        raise RuntimeError('Cage is required; no desktop compositor is used')
    out=ROOT/'artifacts/cage-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\ncheck_updates=false\nfilter_updates=false\nrestore=false\nconfirm_quit=false\n')
    report=out/'report.json';report.unlink(missing_ok=True)
    process=None
    with tempfile.TemporaryDirectory(prefix='aura-cage-pixman-') as temporary, ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        runtime=Path(temporary);runtime.chmod(0o700)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        env={**os.environ,'XDG_RUNTIME_DIR':temporary,'WLR_BACKENDS':'headless','WLR_RENDERER':'pixman','WLR_HEADLESS_OUTPUTS':'2','LIBGL_ALWAYS_SOFTWARE':'1','AURA_BACKGROUND_TEST':'1','AURA_TEST_ISOLATED_DISPLAY':'1','DBUS_SESSION_BUS_ADDRESS':'unix:path=/nonexistent/aura-test-bus'}
        for key in ('DISPLAY','WAYLAND_DISPLAY','WAYLAND_SOCKET'):env.pop(key,None)
        for key in ('XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_CACHE_HOME'):
            env[key]=str(runtime/key.lower());Path(env[key]).mkdir()
        try:
            with (out/'process.log').open('w') as log:
                process=subprocess.Popen(['cage','-m','extend','--',str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}','--window-system=wayland','--software-rendering','--scale=1.25',f'--url=http://127.0.0.1:{server.server_port}/a.html'],env=env,stdout=log,stderr=log,start_new_session=True)
                def state():
                    if process.poll() is not None:raise RuntimeError('Cage/browser exited; see process.log')
                    try:return json.loads(report.read_text())
                    except (OSError,ValueError):return {}
                wait_until(lambda:state().get('presentation_frames',0)>0 and any(t.get('title')=='Alpha' and t.get('loaded') and t.get('frames',0)>0 for t in state().get('tabs',[])),'No real CEF/SHM paint under Cage',timeout=45)
                s=state();assert s['window_system']=='wayland' and s['interface']['device_scale']==1.25,s
                assert s['interface']['width']>900,s['interface']
                os.kill(s['pid'],signal.SIGTERM)
                process.wait(timeout=30);assert process.returncode==0,process.returncode
                final=json.loads(report.read_text());assert final['cef_created_total']==final['cef_closed_total'],final
        finally:
            if process and process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
            server.shutdown()
    print('PASS Cage/wlroots: headless Pixman, two outputs, client scale 1.25, actual CEF/SHM frames and graceful SIGTERM with balanced CEF shutdown; no DISPLAY or GPU')
if __name__=='__main__':main()
