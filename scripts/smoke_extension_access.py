"""Native Chromium host permission enforcement, restart and isolated Aura UI."""
import base64,hashlib,json,os,shutil,signal,struct,subprocess,threading,time
from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
from dependencies import ROOT
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at
probes={}
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_POST(self):
        p=json.loads(self.rfile.read(int(self.headers['Content-Length'])));probes[p['name']]=p
        self.send_response(204);self.end_headers()
    def do_GET(self):
        name=self.path.strip('/')
        body=('''<!doctype html><meta charset="utf-8"><title>Access fixture</title><input value="Keep my form"><script>const token=Math.random().toString(36);setInterval(()=>fetch('/probe',{method:'POST',body:JSON.stringify({name:%s,token,extension:document.documentElement.dataset.extAccess==='yes'})}),100)</script>'''%json.dumps(name)).encode()
        self.send_response(200);self.send_header('Content-Type','text/html');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
def fields(data):
    pos=0
    def number():
        nonlocal pos
        n=shift=0
        while True:
            b=data[pos];pos+=1;n|=(b&127)<<shift
            if not b&128:return n
            shift+=7
    while pos<len(data):
        tag=number();size=number();value=data[pos:pos+size];pos+=size
        assert tag&7==2
        yield tag>>3,value

def seed(profile):
    b=(ROOT/'tests/fixtures/extensions/valid.crx').read_bytes();n=struct.unpack_from('<I',b,8)[0]
    pub=next(v for tag,proof in fields(b[12:12+n]) if tag==2 for k,v in fields(proof) if k==1)
    extension_id=''.join(chr(97+(v>>4))+chr(97+(v&15)) for v in hashlib.sha256(pub).digest()[:16])
    directory=extension_id+'-123';target=profile/'extensions'/directory;target.mkdir(parents=True)
    manifest={'manifest_version':3,'name':'Aura site access test','version':'1.0','key':base64.b64encode(pub).decode(),'permissions':['storage'],'action':{'default_popup':'popup.html'},'host_permissions':['http://127.0.0.1/*','http://127.0.0.2/*'],'content_scripts':[{'matches':['http://127.0.0.1/*','http://127.0.0.2/*'],'js':['content.js'],'run_at':'document_start'}]}
    (target/'manifest.json').write_text(json.dumps(manifest))
    (target/'popup.html').write_text('<!doctype html><title>Host fetch pending</title><script src="popup.js"></script>')
    (target/'popup.js').write_text("document.title='Host fetch pending'")
    (target/'content.js').write_text("document.documentElement.dataset.extAccess='yes';")
    (profile/'extensions/registry.toml').write_text('[[extensions]]\nid='+json.dumps(extension_id)+'\nname="Aura site access test"\nversion="1.0"\ndirectory='+json.dumps(directory)+'\nenabled=true\npopup="popup.html"\npermissions=["http://127.0.0.1/*","http://127.0.0.2/*"]\n')
    return extension_id

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/extension-access-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir();extension_id=seed(profile)
    (profile/'profile.toml').write_text('language="en"\ncheck_updates=false\nfilter_updates=false\nrestore=false\nconfirm_quit=false\n')
    servers=[ThreadingHTTPServer((host,0),Handler) for host in ('127.0.0.1','127.0.0.2')]
    for server in servers:threading.Thread(target=server.serve_forever,daemon=True).start()
    urls=[f'http://{server.server_address[0]}:{server.server_port}' for server in servers]
    popup=profile/'extensions'/(extension_id+'-123')/'popup.js'
    popup.write_text("fetch("+json.dumps(urls[1]+'/cors')+").then(r=>r.text()).then(()=>document.title='Host fetch allowed',()=>document.title='Host fetch blocked');")
    try:
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';s=path.read_text();import re
                s=re.sub(r'(?m)^language\s*=.*$','language="ru"',s);s=re.sub(r'(?m)^ui_zoom\s*=.*$','ui_zoom=200',s);s=re.sub(r'(?m)^theme\s*=.*$','theme=1',s);path.write_text(s)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={urls[0]}/initial-{launch}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Access browser exited')
                        try:return json.loads(report.read_text())
                        except(OSError,ValueError):return {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No access browser',timeout=30);w=state()['window']
                    def click(r):
                        f=state()['interface']['device_scale']*state()['interface']['applied']/100;click_at(w,round((r[0]+r[2]/2)*f),round((r[1]+r[3]/2)*f))
                    def access():return state()['extension_access']
                    def ui():return state().get('extension_access_ui') or {}
                    def opened():
                        revision=access()['revision']
                        key_at(w,'k',4);type_text(w,'extensions');key_at(w,'Return');wait_until(lambda:state().get('extensions_ui'),'No extension manager')
                        r=state()['extensions_ui']['bounds'];click([r[0]+r[2]-316,r[1]+218,152,32])
                        wait_until(lambda:ui() and not access()['busy'] and access()['revision']>=revision+2,'Site access did not finish',timeout=30)
                        assert access()['available'] and not access()['service_visible'],access()
                    def host_fetch(expected):
                        key_at(w,'Escape');key_at(w,'k',4);type_text(w,'extensions');key_at(w,'Return');wait_until(lambda:state().get('extensions_ui'),'No popup manager')
                        r=state()['extensions_ui']['bounds'];click([r[0]+152,r[1]+270,108,34])
                        title='Host fetch '+('allowed' if expected else 'blocked')
                        wait_until(lambda:state().get('extension_popup') and state()['extension_popup'].get('title')==title,'Host permission fetch did not match '+title)
                        key_at(w,'Escape')
                    def mode(i):
                        revision=access()['revision'];click(ui()['modes'][i]);wait_until(lambda:not access()['busy'] and access()['revision']>=revision+2,'Mode mutation stuck')
                        assert not access()['error'],access()
                    def page(host,name,expected):
                        key_at(w,'Escape');key_at(w,'l',4);type_text(w,urls[host]+'/'+name);key_at(w,'Return')
                        wait_until(lambda:name in probes,'No fixture probe '+name);time.sleep(.7)
                        assert probes[name]['extension']==expected,(name,probes[name],access())
                    def add(value):
                        revision=access()['revision'];click(ui()['input']);key_at(w,'a',4);type_text(w,value);key_at(w,'Return');time.sleep(.4)
                        if not ui()['error']:wait_until(lambda:not access()['busy'] and access()['revision']>=revision+2,'Add stuck')
                    def shot(name):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);time.sleep(.3);subprocess.run(['import','-window',str(w),str(out/name)],check=True)
                    if not launch:
                        page(0,'initial-allow-a',True);page(1,'initial-allow-b',True);host_fetch(True);opened();shot('en-initial.png')
                        mode(0);assert access()['mode']=='ON_CLICK';page(0,'blocked-a',False);page(1,'blocked-b',False);host_fetch(False)
                        opened();mode(1);add('http://127.0.0.1');assert 'http://127.0.0.1/*' in access()['hosts'],access()
                        page(0,'allowed-a',True);page(1,'still-blocked-b',False);host_fetch(False)
                        opened();add('javascript:evil');assert ui()['error'] and ui()['query']=='javascript:evil';shot('en-invalid.png')
                        add('http://127.0.0.1');assert ui()['error'],'Duplicate site accepted'
                        add('https://outside.test');assert access()['error'] and 'https://outside.test/*' not in access()['hosts'],access()
                        revision=access()['revision'];click(ui()['refresh']);wait_until(lambda:not access()['busy'] and access()['revision']>=revision+2,'Refresh stuck')
                        revision=access()['revision'];click(ui()['items'][0]['remove']);wait_until(lambda:not access()['busy'] and access()['revision']>=revision+2,'Revoke stuck');page(0,'removed-a',False)
                        opened();mode(2);page(0,'all-a',True);page(1,'all-b',True);host_fetch(True)
                        opened();mode(0);mode(1);add('http://127.0.0.1');assert not access()['error'],access()
                    else:
                        page(0,'restart-allowed-a',True);page(1,'restart-blocked-b',False)
                        subprocess.run(['xdotool','windowsize','--sync',str(w),'1800','1240'],check=True)
                        opened();assert access()['mode']=='ON_SPECIFIC_SITES';shot('ru-200-specific.png')
                        revision=access()['revision'];key_at(w,'F10');key_at(w,'Tab');key_at(w,'Return')
                        wait_until(lambda:not access()['busy'] and access()['revision']>=revision+2 and access()['mode']=='ON_CLICK','Keyboard host mode activation failed')
                        mode(1);add('http://127.0.0.1')
                        key_at(w,'Escape');key_at(w,'n',12);wait_until(lambda:any(t['private'] for t in state()['tabs']),'No private tab')
                        revision=access()['revision'];key_at(w,'k',4);type_text(w,'extensions');key_at(w,'Return');wait_until(lambda:state().get('extensions_ui'),'No private manager')
                        r=state()['extensions_ui']['bounds'];click([r[0]+r[2]-316,r[1]+218,152,32]);time.sleep(.4)
                        assert not ui() and access()['revision']==revision,'Private context changed ordinary host grants'
                        key_at(w,'Escape');key_at(w,'w',4);wait_until(lambda:not any(t['private'] for t in state()['tabs']),'Private tab not closed')
                    key_at(w,'q',4);p.wait(timeout=30)
                    final=json.loads(report.read_text());assert final['cef_created_total']==final['cef_closed_total'],final
                    assert final['extension_access']['created']==final['extension_access']['closed']==1
                finally:
                    try:os.killpg(p.pid,signal.SIGTERM)
                    except ProcessLookupError:pass
                    try:p.wait(timeout=4)
                    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
    finally:
        for server in servers:server.shutdown();server.server_close()
    print('PASS: native host permissions block/regrant two real origins and cross-origin popup reads, specific allowlist, revocation, requested-host boundary, invalid/duplicate drafts, keyboard activation, private guard, restart, RU/EN UI 200%, hidden WebUI lifecycle')
if __name__=='__main__':main()
