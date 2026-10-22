"""Opt-in live verification using synthetic Hello and public example.com."""
import json,os,shutil,signal,socket,subprocess,threading,time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at,drag_between


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    if not shutil.which('node'):raise RuntimeError('Live provider verification requires Node.js with WebSocket support')
    out=ROOT/'artifacts/translation-online';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    with socket.socket() as available:available.bind(('127.0.0.1',0));port=available.getsockname()[1]
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        report=out/'report.json';report.unlink(missing_ok=True)
        with (out/'process.log').open('w') as log:
            p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--remote-debugging-port={port}','--remote-debugging-address=127.0.0.1',f'--url=http://127.0.0.1:{server.server_port}/translation.html?online=1'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
            findings={}
            try:
                def state():
                    if p.poll() is not None:raise RuntimeError('Live translation browser exited')
                    try:return json.loads(report.read_text())
                    except (ValueError,OSError):return {}
                def current():
                    s=state();ts=s.get('tabs',[]);return ts[s.get('active_tab',0)] if ts else {}
                def doc():
                    try:return json.loads(current().get('title',''))
                    except ValueError:return {}
                wait_until(lambda:current().get('loaded') and doc().get('text'),'No Hello fixture',timeout=30);w=state()['window']
                def click(r):click_at(w,r[0]+r[2]//2,r[1]+r[3]//2)
                def option(group,value):return next(o['bounds'] for o in state()['translation_ui']['options'] if o['group']==group and o['value']==value)
                for provider,host in [('google','translate.google.com'),('bing','www.bing.com')]:
                    if findings:
                        old=doc()['token'];key_at(w,'r',4);wait_until(lambda:doc().get('token') and doc()['token']!=old,'Fresh Hello document did not load')
                    r=doc()['text'];drag_between(w,252+r[0]+1,88+r[1]+16,252+r[0]+70,88+r[1]+16)
                    wait_until(lambda:current()['selection_bytes']>0,'Hello not selected')
                    key_at(w,'t',12);wait_until(lambda:state().get('translation_ui') is not None,'No translation controls')
                    click(option(0,provider));click(option(1,'ru'));click(option(2,'text'));click(state()['translation_ui']['send'])
                    wait_until(lambda:len(state()['tabs'])==2,'Provider tab was not created')
                    result={};deadline=time.monotonic()+45
                    while time.monotonic()<deadline:
                        read=subprocess.run(['node',str(ROOT/'scripts/probe_translation_page.mjs'),str(port),host],capture_output=True,text=True,timeout=12)
                        if read.returncode==0:
                            result=json.loads(read.stdout)
                            if result.get('translated') or result.get('requires_verification'):break
                        elif not result:result={'error':read.stderr[-500:]}
                        time.sleep(2)
                    findings[provider]=result
                    (out/'results.json').write_text(json.dumps(findings,ensure_ascii=False,indent=2)+'\n')
                    subprocess.run(['import','-window',str(w),str(out/f'{provider}.png')],check=True,timeout=10)
                    key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Provider close failed')
                # Verify the documented Google website route against a public page, not a local/private URL.
                key_at(w,'l',4)
                type_text(w,'https://example.com/');key_at(w,'Return')
                wait_until(lambda:current()['url']=='https://example.com/' and (current()['loaded'] or current()['failed']),'Example website did not settle',timeout=30)
                key_at(w,'t',12);wait_until(lambda:state().get('translation_ui') is not None,'No website translation controls')
                click(option(0,'google'));click(option(1,'ru'));click(option(2,'page'));click(state()['translation_ui']['send'])
                wait_until(lambda:len(state()['tabs'])==2,'Website provider did not open')
                result={};deadline=time.monotonic()+40
                while time.monotonic()<deadline:
                    from urllib.parse import urlsplit
                    host=urlsplit(current()['url']).hostname or 'translate.google.com'
                    read=subprocess.run(['node',str(ROOT/'scripts/probe_translation_page.mjs'),str(port),host,'page'],capture_output=True,text=True,timeout=12)
                    if read.returncode==0:
                        result=json.loads(read.stdout)
                        if result.get('translated') or result.get('requires_verification'):break
                    time.sleep(2)
                findings['google_page']=result
                subprocess.run(['import','-window',str(w),str(out/'google-page.png')],check=True,timeout=10)
                (out/'results.json').write_text(json.dumps(findings,ensure_ascii=False,indent=2)+'\n')
                key_at(w,'q',4);p.wait(timeout=30)
                assert findings['google'].get('translated'),'Google text response not verified'
                assert findings['google_page'].get('translated'),'Google website response not verified'
                assert findings['bing'].get('translated'),'Microsoft Translator text response not verified'
            finally:
                if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: Google and Microsoft translate synthetic Hello to Привет; Google translates a public website; all requests use explicit native controls')


if __name__=='__main__':main()
