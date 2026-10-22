"""Native start page history and beforeunload/reopen regression checks."""
import json,os,shutil,signal,subprocess,threading,time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/navigation-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('filter_updates=false\ncheck_updates=false\nconfirm_quit=false\nrestore=false\n')
    report=out/'report.json';report.unlink(missing_ok=True)
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}/window-state.html'
        with (out/'process.log').open('w') as log:
            p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={base}?name=A'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
            try:
                def state():
                    assert p.poll() is None,'Browser exited'
                    try:return json.loads(report.read_text())
                    except (OSError,ValueError):return {}
                def tab():
                    s=state()
                    return s.get('tabs',[{}])[s.get('active_tab',0)]
                def named(name):
                    try:return json.loads(tab().get('title','')).get('name')==name
                    except ValueError:return False
                wait_until(lambda:named('A'),'No first page',timeout=30);w=state()['window']
                def navigate(url):key_at(w,'l',4);type_text(w,url);key_at(w,'Return')
                navigate(base+'?name=B');wait_until(lambda:named('B') and tab()['loaded'],'No second page')
                navigate('aura://newtab');wait_until(lambda:tab().get('url')=='aura://newtab' and tab()['loaded'],'No committed start page')
                key_at(w,'Left',8);wait_until(lambda:named('B') and tab()['loaded'],'Back skipped the last website')
                assert tab()['audio_muted'] is False
                key_at(w,'Right',8);wait_until(lambda:tab().get('url')=='aura://newtab','Forward did not restore start page')
                key_at(w,'Left',8);wait_until(lambda:named('B'),'No website after second Back')
                click_at(w,352,208)
                wait_until(lambda:'true' in tab().get('title',''),'Beforeunload fixture not armed')
                navigate('aura://newtab');wait_until(lambda:state().get('dialog_count')==1,'Start-page navigation bypassed beforeunload')
                key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0 and named('B'),'Cancelled start navigation lost the old document')
                key_at(w,'w',4)
                wait_until(lambda:state().get('dialog_count')==1,'Close tab bypassed beforeunload')
                key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Tab close cancellation failed')
                key_at(w,'t',5);time.sleep(.6)
                assert len(state()['tabs'])==1,'Cancelled close polluted recently closed tabs'
                click_at(w,352,208)
                wait_until(lambda:'false' in tab().get('title',''),'Beforeunload fixture not disarmed')
                key_at(w,'w',4)
                wait_until(lambda:len(state()['tabs'])==1 and tab()['url']=='aura://newtab','Confirmed close did not finish')
                key_at(w,'t',5)
                wait_until(lambda:len(state()['tabs'])==2 and named('B'),'Confirmed closed tab was not restorable')
                key_at(w,'q',4);p.wait(timeout=30)
                final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
            finally:
                if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: A -> B -> native start -> Back B -> Forward start; audio state and cancelled beforeunload navigation preserved')
if __name__=='__main__':main()
