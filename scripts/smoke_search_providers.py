"""Native provider management and actual encoded HTTP searches; no external services."""
import json, os, re, shutil, signal, subprocess, threading, time, tomllib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at

class Handler(BaseHTTPRequestHandler):
    requests=[]
    def log_message(self,*_):pass
    def do_GET(self):
        Handler.requests.append((urlsplit(self.path).path,parse_qs(urlsplit(self.path).query)))
        data=b'<!doctype html><meta charset=utf-8><title>Search results fixture</title><h1>Search results</h1><p>Local search target.</p>'
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/search-providers-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}'
        for launch in range(2):
            if launch:
                config=profile/'profile.toml';text=config.read_text()
                for name,value in [('language','"ru"'),('ui_zoom','200'),('theme','1')]:
                    text=re.sub(rf'(?m)^{name}\s*=.*$',name+'='+value,text)
                config.write_text(text)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={"w startup query" if launch else "aura://newtab"}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Browser exited')
                        try:return json.loads(report.read_text())
                        except (ValueError,OSError):return {}
                    def current():
                        s=state();ts=s.get('tabs',[]);return ts[s.get('active_tab',0)] if ts else {}
                    wait_until(lambda:state().get('window') and current(),'No browser',timeout=30);w=state()['window']
                    def ui():return state().get('search_ui') or {}
                    def click(r):
                        scale=state().get('interface',{}).get('applied',100)/100
                        click_at(w,int((r[0]+r[2]//2)*scale),int((r[1]+r[3]//2)*scale))
                    def paste(text):
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-quiet','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                        try:
                            clip.stdin.write(text.encode());clip.stdin.close();time.sleep(.1);key_at(w,'v',4);clip.wait(timeout=5)
                        finally:
                            if clip.poll() is None:clip.terminate();clip.wait(timeout=3)
                    def manager():
                        key_at(w,'k',4);type_text(w,'Search engines');wait_until(lambda:(state().get('commands_ui') or {}).get('items'),'No command');key_at(w,'Return')
                        wait_until(lambda:bool(ui()),'No search manager')
                    def field(i,value):
                        click(ui()['fields'][i]['bounds']);key_at(w,'a',4)
                        if value:paste(value)
                        else:key_at(w,'BackSpace')
                        wait_until(lambda:ui()['fields'][i]['value']==value,'Field not edited')
                    def save():click(ui()['save']);wait_until(lambda:not ui().get('editing',True),'Search engine not saved: '+str(ui()))
                    def navigate(text):
                        key_at(w,'l',4);paste(text);key_at(w,'Return')
                        wait_until(lambda:current().get('loaded') and current().get('title')=='Search results fixture','Search not loaded',timeout=30)
                    def query(text,expected,path='/search'):
                        before=len(Handler.requests);navigate(text)
                        wait_until(lambda:any(p==path and q.get('q')==[expected] for p,q in Handler.requests[before:]),'Server received wrong query: '+str(Handler.requests[before:]))
                    if not launch:
                        # Discover the manager through the native Settings header using keyboard.
                        key_at(w,'k',4);type_text(w,'Settings');wait_until(lambda:state().get('commands_ui'),'No settings command');key_at(w,'Return');wait_until(lambda:state().get('panel')==4,'No settings')
                        key_at(w,'Tab');key_at(w,'Tab');key_at(w,'Return');wait_until(lambda:bool(ui()),'Settings entry failed')
                        click(ui()['add']);wait_until(lambda:ui().get('editing'),'Add did not open form');field(0,'Local search');field(1,'W');field(2,'https://{searchTerms}.example.org/')
                        click(ui()['save']);wait_until(lambda:ui().get('error'),'Invalid URL accepted');assert ui()['editing'] and ui()['fields'][0]['value']=='Local search'
                        field(2,base+'/search?q={searchTerms}&fixed=yes');save();assert ui()['items'][0]['keyword']=='w'
                        click(ui()['items'][0]['default']);wait_until(lambda:ui()['default']==ui()['items'][0]['id'],'Default not changed')
                        subprocess.run(['import','-window',str(w),str(out/'en-list.png')],check=True,timeout=10)
                        key_at(w,'Escape');query('plain a&b=#%/?ёж','plain a&b=#%/?ёж')
                        query('W explicit ёж &=%','explicit ёж &=%')
                        navigate(base+'/plain');wait_until(lambda:current()['url']==base+'/plain' and current().get('loaded'),'Normal URL changed into search')
                        manager();click(ui()['add']);wait_until(lambda:ui().get('editing'),'Add did not open form');field(0,'Duplicate');field(1,'w');field(2,base+'/code?q={searchTerms}')
                        click(ui()['save']);wait_until(lambda:ui().get('error'),'Duplicate keyword accepted')
                        field(1,'gh');save();assert len(ui()['items'])==2
                        key_at(w,'Escape');query('gh repository #one','repository #one','/code')
                        manager();click(ui()['items'][1]['edit']);field(0,'Code updated');field(1,'code');save()
                        key_at(w,'Escape');key_at(w,'t',4);query('code new tab','new tab','/code')
                        key_at(w,'n',12);query('w PRIVATE_SEARCH_SENTINEL','PRIVATE_SEARCH_SENTINEL')
                        assert current()['private'];key_at(w,'w',4);wait_until(lambda:not current()['private'],'Private close failed')
                        manager();assert ui()['name']=='Local search'
                        # Clear and restore the custom default without visiting an external engine.
                        click(ui()['builtin']);wait_until(lambda:ui()['default']==0,'Built-in fallback failed')
                        click(ui()['items'][0]['default']);wait_until(lambda:ui()['default']==ui()['items'][0]['id'],'Default restore failed')
                        key_at(w,'Escape');key_at(w,'k',4);type_text(w,'search_engine')
                        wait_until(lambda:(state().get('commands_ui') or {}).get('query')=='search_engine' and (state().get('commands_ui') or {}).get('items') and state()['commands_ui']['items'][0]['id']=='search_engine','Built-in setting not found')
                        key_at(w,'Return')
                        wait_until(lambda:state().get('settings_ui'),'No search settings')
                        control=next(c for c in state()['settings_ui']['controls'] if c['id']=='search_engine')
                        click(next(o['bounds'] for o in control['options'] if o['value']=='bing'))
                        manager();assert ui()['default']==0 and ui()['name']=='Bing'
                        click(ui()['items'][0]['default']);wait_until(lambda:ui()['name']=='Local search','Custom default restore failed')
                    else:
                        wait_until(lambda:any(q.get('q')==['startup query'] for _,q in Handler.requests),'Keyword failed during startup',timeout=30)
                        subprocess.run(['xdotool','windowsize',str(w),'1800','1240'],check=True);time.sleep(.3)
                        manager();assert state()['interface']['applied']==200 and ui()['name']=='Local search'
                        assert ui()['items'][1]['name']=='Code updated' and ui()['items'][1]['keyword']=='code'
                        subprocess.run(['import','-window',str(w),str(out/'ru-light-200-list.png')],check=True,timeout=10)
                        click(ui()['items'][1]['edit']);field(2,'javascript:{searchTerms}');click(ui()['save']);wait_until(lambda:ui().get('error'),'Unsafe scheme accepted')
                        subprocess.run(['import','-window',str(w),str(out/'ru-light-200-error.png')],check=True,timeout=10)
                        click(ui()['cancel']);assert ui()['items'][1]['keyword']=='code'
                        click(ui()['items'][0]['delete']);wait_until(lambda:state()['dialog_count']==1,'No delete prompt');key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Cancel failed');assert len(ui()['items'])==2
                        click(ui()['items'][0]['delete']);wait_until(lambda:state()['dialog_count']==1,'No delete prompt');key_at(w,'Return');wait_until(lambda:len(ui()['items'])==1,'Delete failed');assert ui()['default']==0
                    key_at(w,'q',4);p.wait(timeout=30)
                    final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                    assert 'PRIVATE_SEARCH_SENTINEL' not in (profile/'profile.toml').read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: native provider CRUD, validation, encoded RU queries at HTTP server, default/keyword routing, normal addresses, startup persistence, private history exclusion, keyboard, RU light UI 200%')

if __name__=='__main__':main()
