"""Browser APIs, security boundaries, real media and public pages on a private display."""
import json,os,shutil,signal,ssl,subprocess,threading,time
from functools import partial
from http.server import ThreadingHTTPServer
from urllib.parse import urlparse
from dependencies import ROOT
from release_identity import read_identity
from smoke import QuietHandler
class Server(ThreadingHTTPServer):
    def handle_error(self,request,address):
        import sys
        if isinstance(sys.exc_info()[1],(ConnectionError,ssl.SSLError)):return
        super().handle_error(request,address)
class Handler(QuietHandler):
    reports=[]
    def end_headers(self):
        self.send_header('Cache-Control','no-store')
        if urlparse(self.path).path=='/csp.html':self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'")
        super().end_headers()
    def do_GET(self):
        path=urlparse(self.path).path
        if path in ('/cors-allowed','/cors-denied','/foreign'):
            self.send_response(200)
            if path=='/cors-allowed':self.send_header('Access-Control-Allow-Origin','*')
            self.send_header('Content-Type','text/html' if path=='/foreign' else 'text/plain');self.end_headers();self.wfile.write(b'<p>Foreign origin</p>' if path=='/foreign' else b'allowed')
        else:super().do_GET()
    def do_POST(self):
        if self.path!='/compat-report':self.send_error(404);return
        size=int(self.headers.get('Content-Length','0'));assert 0<size<65536
        self.reports.append(json.loads(self.rfile.read(size)));self.send_response(204);self.end_headers()

def main():
    from smoke_browser import wait_until,type_text
    from smoke_window import key_at
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=compatibility')
    out=ROOT/'artifacts/compatibility-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nfilter_updates=false\ncheck_updates=false\nrestore=false\nblock_popups=true\n')
    cert,key=out/'fixture-cert.pem',out/'fixture-key.pem'
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);key.chmod(0o600)
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
    handler=partial(Handler,directory=str(ROOT/'tests/fixtures/browser'));results=[];live=[]
    with Server(('127.0.0.1',0),handler) as server,Server(('127.0.0.1',0),handler) as remote,Server(('127.0.0.1',0),handler) as insecure_tls:
        insecure_tls.socket=context.wrap_socket(insecure_tls.socket,server_side=True)
        for source in (server,remote,insecure_tls):threading.Thread(target=source.serve_forever,daemon=True).start()
        url=f'http://127.0.0.1:{server.server_port}/compat.html?remote=http://127.0.0.1:{remote.server_port}'
        for launch in range(3):
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True);before=len(Handler.reports)
            with (out/f'process-{launch}.log').open('w') as log:
                process=subprocess.Popen([os.environ.get('AURA_TEST_BINARY',str(ROOT/'build/linux-x64-release/aura_browser')),f'--data-dir={profile}',f'--report={report}',f'--url={url}']+(['--private'] if launch==2 else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if process.poll() is not None:raise RuntimeError('Compatibility browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    wait_until(lambda:len(Handler.reports)>before,'API fixture did not finish',timeout=100)
                    actual=Handler.reports[-1];results.append({'launch':launch,'private':launch==2,**actual});(out/'api-results.json').write_text(json.dumps(results,indent=2,ensure_ascii=False))
                    assert actual['passed'],actual
                    assert actual['capabilities']['initial_indexeddb']==('fictional-persistent-marker' if launch==1 else ''),('IndexedDB persistence/private isolation',actual)
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No compatibility window')
                    assert state()['version']==read_identity().version
                    assert len(state()['tabs'])==1,'Blocked popup created a tab'
                    window=str(state()['window']);subprocess.run(['import','-window',window,str(out/f'api-{launch}.png')],check=True,timeout=10)
                    if launch==0:
                        def navigate(target):key_at(window,'l',4);type_text(window,target);key_at(window,'Return')
                        target=f'https://127.0.0.1:{insecure_tls.server_port}/foreign';navigate(target)
                        wait_until(lambda:state()['tabs'][0]['url']==target and state()['tabs'][0].get('failed'),'Untrusted TLS certificate was not blocked',timeout=25)
                        subprocess.run(['import','-window',window,str(out/'certificate-blocked.png')],check=True,timeout=10)
                        for target,expected in [('https://www.wikipedia.org/','Wikipedia'),('https://developer.mozilla.org/en-US/','MDN'),('https://github.com/Sttanors/aura-browser','aura-browser')]:
                            navigate(target)
                            try:wait_until(lambda:state()['tabs'][0]['url'].startswith(target) and (state()['tabs'][0]['loaded'] or state()['tabs'][0].get('failed')),'Public page did not finish',timeout=45)
                            except RuntimeError as error:live.append({'url':target,'verified':False,'reason':str(error)});continue
                            tab=state()['tabs'][0];verified=not tab.get('failed') and tab.get('http_status')==200 and expected.lower() in tab['title'].lower()
                            live.append({'url':target,'verified':verified,'http_status':tab.get('http_status'),'title':tab['title'],'failed':tab.get('failed')})
                            subprocess.run(['import','-window',window,str(out/f'live-{len(live)}.png')],check=True,timeout=10)
                        (out/'live-pages.json').write_text(json.dumps(live,indent=2,ensure_ascii=False))
                    key_at(window,'q',4);process.wait(timeout=30);final=json.loads(report.read_text());assert process.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                finally:
                    if process.poll() is None:os.killpg(process.pid,signal.SIGTERM);process.wait(timeout=5)
        for source in (server,remote,insecure_tls):source.shutdown()
    (out/'PASS.txt').write_text('PASS: browser APIs, IndexedDB restart/private isolation, CSP/CORS/origin/TLS boundaries, real WebM decoding, bounded popup policy; public-page results recorded separately.\n')
    print('Compatibility API/security/media PASS; public pages verified:',sum(item['verified'] for item in live),'/',len(live))
if __name__=='__main__':main()
