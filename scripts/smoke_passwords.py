"""Fictional credentials only. Private software display and a pinned local TLS fixture."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import ssl
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit,parse_qs
from dependencies import ROOT
from smoke_browser import wait_until,type_text
from smoke_window import click_at,key_at

class Server(ThreadingHTTPServer):
    def handle_error(self,request,address):
        import sys
        if isinstance(sys.exc_info()[1],(ConnectionError,ssl.SSLError)): return
        super().handle_error(request,address)

class Handler(BaseHTTPRequestHandler):
    probes=[]
    def log_message(self,*args): pass
    def do_GET(self):
        parts=urlsplit(self.path)
        if parts.path=='/probe':
            self.probes.append(json.loads(parse_qs(parts.query)['data'][0]));self.send_response(204);self.end_headers();return
        preset=parts.path=='/preset'
        user='captured-user' if preset else ''
        password='fictional-captured-password' if preset else ''
        body=f'''<!doctype html><title>Password fixture</title><style>body{{background:white;color:black;padding:30px;font:18px sans-serif}}input{{display:block;margin:12px;padding:10px}}</style>
<form action="/login"><input id=u autocomplete=username value="{user}"><input id=p type=password autocomplete=current-password value="{password}"><button>Sign in</button></form>
<script>setInterval(()=>fetch('/probe?data='+encodeURIComponent(JSON.stringify({{path:location.pathname,user:u.value,password:p.value}}))),200);</script>'''
        if parts.path=='/iframe': body='<!doctype html><title>Frame fixture</title><iframe src="/login"></iframe>'
        self.send_response(200);self.send_header('Content-Type','text/html');self.end_headers();self.wfile.write(body.encode())

def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=passwords')
    out=ROOT/'artifacts/passwords-smoke';out.mkdir(parents=True,exist_ok=True)
    profiles=[out/'profile',out/'target']
    for profile in profiles:
        if profile.exists():shutil.rmtree(profile)
        profile.mkdir();(profile/'profile.toml').write_text('language="ru"\nrestore=false\ncheck_updates=false\n')
    master='fictional master phrase'
    transfer='fictional transfer phrase'
    exported=out/'transfer.aura';exported.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='aura-test-tls-') as temporary:
        tls=Path(temporary)
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(tls/'key.pem'),'-out',str(tls/'cert.pem'),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        public=subprocess.check_output(['openssl','x509','-in',str(tls/'cert.pem'),'-pubkey','-noout'])
        der=subprocess.check_output(['openssl','pkey','-pubin','-outform','DER'],input=public)
        pin=base64.b64encode(hashlib.sha256(der).digest()).decode()
        https=Server(('127.0.0.1',0),Handler)
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(tls/'cert.pem',tls/'key.pem');https.socket=context.wrap_socket(https.socket,server_side=True)
        http=Server(('127.0.0.1',0),Handler)
        for server in (https,http):threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'https://127.0.0.1:{https.server_port}'
        try:
            for launch in range(3):
                profile=profiles[launch==2]
                report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
                binary=Path(os.environ.get('AURA_TEST_BINARY',str(ROOT/'build/linux-x64-release/aura_browser')))
                with (out/f'process-{launch}.log').open('w') as log:
                    proc=subprocess.Popen([str(binary),f'--data-dir={profile}',f'--report={report}',f'--url={base}/login',f'--ignore-certificate-errors-spki-list={pin}'],env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'},stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                    try:
                        def state():
                            if proc.poll() is not None:raise RuntimeError('Password test browser exited')
                            try:return json.loads(report.read_text())
                            except (FileNotFoundError,json.JSONDecodeError):return {}
                        wait_until(lambda:state().get('presentation_frames',0)>0,'No password test window')
                        window=str(state()['window'])
                        def ui():return state().get('passwords_ui') or {}
                        def click(rect):
                            x,y,w,h=rect;factor=state()['interface']['applied']/100
                            click_at(window,round((x+w/2)*factor),round((y+h/2)*factor))
                        def button(name):click(ui()['controls'][name]);time.sleep(.35)
                        def text(index,value):
                            button('field'+str(index));key_at(window,'a',4)
                            clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(value.encode(),timeout=5);key_at(window,'v',4);time.sleep(.2)
                        def done(count=None):wait_until(lambda:ui() and not ui().get('busy') and not ui().get('locked') and (count is None or ui()['count']==count),'Vault operation failed: '+str(ui()),timeout=20)
                        def open_panel():
                            key_at(window,'Escape');key_at(window,'Escape');wait_until(lambda:state().get('panel')==0,'Panel did not close')
                            factor=state()['interface']['applied']/100;click_at(window,round((state()['interface']['width']-53)*factor),round(55*factor))
                            wait_until(lambda:state().get('settings_ui'),'No settings')
                            key_at(window,'f',4);type_text(window,'Passwords');key_at(window,'Return')
                            wait_until(lambda:any(c['id']=='passwords' for c in (state().get('settings_ui') or {}).get('controls',[])),'No passwords action')
                            row=next(c for c in state()['settings_ui']['controls'] if c['id']=='passwords')['bounds'];click([row[0],row[1]+66,min(300,row[2]),32]);wait_until(lambda:ui(),'No passwords panel')
                        def navigate(url):
                            key_at(window,'Escape');key_at(window,'Escape');key_at(window,'l',4);type_text(window,url);key_at(window,'Return')
                            wait_until(lambda:any(t['url']==url and t['loaded'] for t in state().get('tabs',[])),'No login test page',timeout=25)
                        def shot(name):time.sleep(.3);subprocess.run(['import','-window',window,str(out/name)],check=True,timeout=10)
                        navigate(base+'/login')
                        open_panel()
                        if launch==1:
                            text(0,'wrong test password');button('unlock');wait_until(lambda:not ui().get('busy'),'Unlock stuck');assert ui()['locked']
                        text(0,master)
                        if launch!=1:text(1,master)
                        button('unlock');done(2 if launch==1 else 0)
                        if launch==0:
                            button('add');text(0,base);text(1,'fixture-user');text(2,'fictional-filled-password');shot('ru-edit.png');button('save');done(1)
                            click(ui()['rows'][0]);button('fill')
                            wait_until(lambda:Handler.probes and Handler.probes[-1]['password']=='fictional-filled-password','Same-origin fill failed')
                            assert Handler.probes[-1]['user']=='fixture-user'
                            navigate(f'http://127.0.0.1:{http.server_port}/login');open_panel();click(ui()['rows'][0]);button('fill');time.sleep(.6);assert Handler.probes[-1]['password']==''
                            navigate(f'https://localhost:{https.server_port}/login');open_panel();click(ui()['rows'][0]);button('fill');time.sleep(.6);assert Handler.probes[-1]['password']==''
                            navigate(base+'/iframe');open_panel();click(ui()['rows'][0]);button('fill');time.sleep(.6);assert Handler.probes[-1]['password']==''
                            key_at(window,'Escape');key_at(window,'Escape');key_at(window,'n',12);navigate(base+'/login');open_panel();click(ui()['rows'][0]);button('fill');time.sleep(.6);assert Handler.probes[-1]['password']==''
                            key_at(window,'Escape');key_at(window,'Escape');key_at(window,'w',4);navigate(base+'/preset');open_panel();button('capture');wait_until(lambda:state()['dialog_count']==1,'No save confirmation');key_at(window,'Return');done(2)
                            shot('ru-list.png');button('lock');wait_until(lambda:ui()['locked'],'Vault did not lock');assert ui()['count']==0
                        elif launch==1:
                            button('transfer');text(0,str(exported));text(1,transfer);button('export');done();wait_until(exported.exists,'No encrypted export');assert b'fictional' not in exported.read_bytes()
                        else:
                            button('transfer');text(0,str(exported));text(1,transfer);button('import');done(2);button('back');shot('ru-imported.png')
                            subprocess.run(['xdotool','windowsize','--sync',window,'900','620'],check=True);time.sleep(.4);shot('ru-small.png')
                            subprocess.run(['xdotool','windowsize','--sync',window,'1360','900'],check=True);time.sleep(.3)
                            def settings(search):
                                key_at(window,'Escape');key_at(window,'Escape');wait_until(lambda:state()['panel']==0,'Panel stayed open')
                                click_at(window,1307,55);wait_until(lambda:state().get('settings_ui'),'No settings')
                                key_at(window,'f',4);type_text(window,search);key_at(window,'Return')
                                wait_until(lambda:(state().get('settings_ui') or {}).get('search')==search,'No settings search')
                                return state()['settings_ui']['controls']
                            control=next(c for c in settings('language') if c['id']=='language')
                            click(next(o['bounds'] for o in control['options'] if o['value']=='en'))
                            wait_until(lambda:state()['settings']['language']=='en','English did not apply')
                            control=next(c for c in settings('Interface zoom') if c['id']=='ui_zoom')
                            click(next(o['bounds'] for o in control['options'] if o['value']=='125'))
                            wait_until(lambda:state()['interface']['applied']==125,'Zoom did not apply')
                            key_at(window,'Escape');key_at(window,'Escape');key_at(window,'y',5)
                            wait_until(lambda:ui(),'Password shortcut did not open panel');shot('en-125.png')
                        # Reports and ordinary profile/log files must never contain fixture passwords.
                        assert not state()['audit'].get('password_preference_errors',0)
                        secrets=('fictional-filled-password','fictional-captured-password',master,transfer)
                        for item in (profile/'profile.toml',profile/'aura.log',report):
                            data=item.read_text();assert not any(secret in data for secret in secrets),item
                        key_at(window,'Escape');key_at(window,'Escape');key_at(window,'q',4);proc.wait(timeout=15);assert proc.returncode==0
                    finally:
                        if proc.poll() is None:
                            os.killpg(proc.pid,signal.SIGTERM)
                            try:proc.wait(timeout=4)
                            except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
        finally:
            for server in (https,http):server.shutdown();server.server_close()
    (out/'PASS.txt').write_text('PASS: encrypted vault, restart, transfer, explicit HTTPS fill, HTTP/origin/iframe/private rejection, save confirmation, no password reports/logs\n')
    print('PASS: native password vault and guarded form filling')

if __name__=='__main__':main()
