"""Large real X11 INCR clipboard -> native address editor -> clipboard round trip."""
import hashlib,json,os,shutil,signal,subprocess,time
from dependencies import ROOT
from smoke_browser import wait_until
from smoke_window import key_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/clipboard-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('check_updates=false\nfilter_updates=false\nconfirm_quit=false\n')
    report=out/'report.json';report.unlink(missing_ok=True)
    with (out/'process.log').open('w') as log:
        p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
        clip=None
        try:
            def state():
                assert p.poll() is None,'Clipboard browser exited'
                try:return json.loads(report.read_text())
                except (OSError,ValueError):return {}
            wait_until(lambda:state().get('presentation_frames',0)>0,'No clipboard window',timeout=30)
            w=state()['window']
            for label,payload in [('small','ASCII Русский 🙂'.encode()),('incr',('Я🙂'*350000+'-TAIL').encode())]:
                clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-quiet','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                clip.stdin.write(payload);clip.stdin.close();time.sleep(.2)
                key_at(w,'l',4);key_at(w,'v',4)
                wait_until(lambda:state().get('address_input_bytes')==len(payload),f'{label} paste was empty or truncated',timeout=25)
                key_at(w,'a',4);key_at(w,'c',4)
                actual=subprocess.check_output(['xclip','-selection','clipboard','-out'],timeout=10)
                assert actual==payload,(label,len(actual),len(payload))
                clip.wait(timeout=10);clip=None
                (out/f'{label}-PASS.txt').write_text(f'PASS: {len(payload)} bytes, sha256 {hashlib.sha256(actual).hexdigest()}\n')
                key_at(w,'Escape')
            key_at(w,'q',4);p.wait(timeout=30)
            assert p.returncode==0
        finally:
            if clip is not None and clip.poll() is None:clip.terminate();clip.wait(timeout=5)
            if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
    print('PASS: small and 2.1 MB Unicode X11 clipboard transfers round trip without truncation')
if __name__=='__main__':main()
