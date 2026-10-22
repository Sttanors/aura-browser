"""Real Chromium popup WindowProxy/opener exchange in embedded Aura tabs."""
import json,os,shutil,signal,subprocess,threading
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until
from smoke_window import key_at,click_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/popup-smoke';out.mkdir(parents=True,exist_ok=True)
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        for private in (False,True):
            profile=out/('private' if private else 'normal');shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
            (profile/'profile.toml').write_text('filter_updates=false\ncheck_updates=false\nconfirm_quit=false\nforeground_links=false\nblock_popups=true\n')
            report=profile/'report.json'
            with (profile/'process.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url=http://127.0.0.1:{server.server_port}/popup-parent.html']+(['--private'] if private else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        assert p.poll() is None,'Browser exited'
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def parent():
                        for t in state().get('tabs',[]):
                            if t['url'].endswith('/popup-parent.html'):
                                try:return json.loads(t['title'])
                                except ValueError:pass
                        return {}
                    wait_until(lambda:parent().get('parent'),'No parent fixture',timeout=30)
                    w=state()['window'];click_at(w,352,208)
                    wait_until(lambda:parent().get('exchange'),'WindowProxy/opener postMessage exchange failed',timeout=25)
                    assert parent()['returned']
                    assert len(state()['tabs'])==2
                    assert all(t['private']==private for t in state()['tabs'])
                    # Reuse the named browsing context rather than creating another tab.
                    click_at(w,352,208)
                    wait_until(lambda:parent().get('exchange'),'Named popup failed')
                    assert len(state()['tabs'])==2
                    click_at(w,532,208)
                    wait_until(lambda:len(state()['tabs'])==1 and parent().get('closed'),'window.close did not close the child')
                    key_at(w,'q',4);p.wait(timeout=30)
                    final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: actual popup WindowProxy, opener/source/origin postMessage, named reuse, script close, normal/private and balanced CEF shutdown')
if __name__=='__main__':main()
