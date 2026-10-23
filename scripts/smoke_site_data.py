"""Actual per-origin storage and scoped cookie deletion, isolated containers and private data."""
import json,os,re,shutil,signal,subprocess,threading,time
from contextlib import ExitStack
from functools import partial
from http.server import ThreadingHTTPServer
from urllib.parse import urlsplit,parse_qs
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

class Handler(QuietHandler):
    def end_headers(self):
        seed=parse_qs(urlsplit(self.path).query).get('set',[])
        if seed:self.send_header('Set-Cookie',f'aura_http={seed[0]}; Path=/; HttpOnly; SameSite=Lax')
        super().end_headers()

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/site-data-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n')
    with ExitStack() as stack:
        origins=[]
        for _ in range(2):
            server=stack.enter_context(ThreadingHTTPServer(('127.0.0.1',0),partial(Handler,directory=str(ROOT/'tests/fixtures/browser'))));threading.Thread(target=server.serve_forever,daemon=True).start();stack.callback(server.shutdown);origins.append(f'http://127.0.0.1:{server.server_port}')
        a,b=origins;c=a.replace('127.0.0.1','localhost');url=a+'/site-data.html'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}','--proxy-server=http://127.0.0.1:9']+([f'--url={url}?set=alpha'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Site data browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def current():
                        s=state();ts=s.get('tabs',[]);return ts[s.get('active_tab',0)] if ts else {}
                    def doc(t=None):
                        try:return json.loads((t or current()).get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:doc().get('ready'),'No storage fixture',timeout=30);w=state()['window']
                    def click(r):click_at(w,int(r[0]+r[2]/2),int(r[1]+r[3]/2))
                    def manager():
                        key_at(w,'d',12);wait_until(lambda:state().get('site_data_ui') is not None and not state()['site_data_ui']['busy'],'Site data panel unavailable');assert not state()['site_data_ui']['error'],state()['site_data_ui']
                    def switch(number):
                        key_at(w,str(number),4);wait_until(lambda:state().get('active_tab')==number-1,'Tab switch not reported')
                    def navigate(address):
                        address+=('&' if '?' in address else '?')+'qa='+str(time.monotonic_ns())
                        old=doc().get('token');key_at(w,'l',4);type_text(w,address);key_at(w,'Return');wait_until(lambda:current().get('url')==address and current().get('loaded') and doc().get('ready') and doc().get('token')!=old,'Storage fixture navigation failed')
                    def expect(identity):assert doc()['idb']==identity and doc()['cached']==identity and doc()['local']==identity and doc()['workers']==(0 if identity is None else 1),doc()
                    def remove(kind,accept=True):
                        click(state()['site_data_ui'][kind+'_delete']);wait_until(lambda:state()['dialog_count']>0,'No scoped delete confirmation');key_at(w,'Return' if accept else 'Escape')
                        wait_until(lambda:state()['dialog_count']==0 and not state()['site_data_ui']['busy'],'Site data deletion did not complete');assert not state()['site_data_ui']['error'],state()['site_data_ui']
                    if not launch:
                        expect('alpha');token=doc()['token'];manager();d=state()['site_data_ui'];assert d['origin']==a and d['usage']>600000 and d['cookies']==3,d
                        remove('storage',False);remove('cookies',False);assert doc()['token']==token and state()['site_data_ui']['usage']>600000 and state()['site_data_ui']['cookies']==3
                        subprocess.run(['import','-window',str(w),str(out/'en-site-data.png')],check=True,timeout=10);key_at(w,'Escape')
                        key_at(w,'t',4);navigate(b+'/site-data.html?set=beta');expect('beta');key_at(w,'t',4);navigate(c+'/site-data.html?set=gamma');expect('gamma')
                        switch(1);navigate(url);expect('alpha');manager();token=doc()['token'];remove('storage');assert doc()['token']==token
                        key_at(w,'Escape');navigate(url);expect(None);assert 'aura_data=beta' in doc()['cookie']
                        switch(2);navigate(b+'/site-data.html');expect('beta');manager();assert state()['site_data_ui']['usage']>600000;key_at(w,'Escape')
                        switch(1);manager();remove('cookies');wait_until(lambda:state()['site_data_ui']['cookies']==0,'Cookie paths or HttpOnly were not cleared');key_at(w,'Escape')
                        switch(2);navigate(b+'/site-data.html');expect('beta');assert 'aura_data=' not in doc()['cookie']
                        switch(3);navigate(c+'/site-data.html');expect('gamma');assert 'aura_data=gamma' in doc()['cookie']
                        # Scoped origin picker never navigates the source document.
                        manager();token=doc()['token'];click(state()['site_data_ui']['choose']);wait_until(lambda:state()['site_data_ui']['picker'],'No data picker');click(state()['site_data_ui']['search']);type_text(w,a);key_at(w,'Return')
                        wait_until(lambda:not state()['site_data_ui']['picker'] and not state()['site_data_ui']['busy'],'Data picker query failed');assert state()['site_data_ui']['origin']==a and state()['site_data_ui']['cookies']==0 and doc()['token']==token
                        key_at(w,'Escape');key_at(w,'c',12);wait_until(lambda:state().get('containers_ui') is not None,'No container manager');click(state()['containers_ui']['input']);type_text(w,'Storage');key_at(w,'Return');wait_until(lambda:len(state()['containers_ui']['items'])==2,'No storage container');container=state()['containers_ui']['items'][1]['id'];click(state()['containers_ui']['items'][1]['new_tab'])
                        navigate(url+'?set=work');expect('work');manager();assert state()['site_data_ui']['container']==container and state()['site_data_ui']['cookies']==3
                        switch(1);click(state()['site_data_ui']['refresh']);wait_until(lambda:not state()['site_data_ui']['busy'],'Pinned context refresh failed');assert state()['site_data_ui']['container']==container and state()['site_data_ui']['cookies']==3;key_at(w,'Escape');switch(4)
                        key_at(w,'n',12);navigate(url+'?PRIVATE_SITE_DATA_SENTINEL');expect(None);assert 'aura_data=' not in doc()['cookie'];navigate(url+'?set=private&PRIVATE_SITE_DATA_SENTINEL');expect('private');manager();assert state()['site_data_ui']['private'];remove('storage');remove('cookies');key_at(w,'Escape');navigate(url+'?PRIVATE_SITE_DATA_SENTINEL');expect(None);assert 'aura_data=' not in doc()['cookie'];key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==4,'Private close failed')
                        navigate(url);expect('work');assert 'aura_data=work' in doc()['cookie'];switch(1);navigate(url);expect(None);assert 'aura_data=' not in doc()['cookie']
                    else:
                        wait_until(lambda:len(state()['tabs'])==4 and all(doc(t).get('ready') for t in state()['tabs']),'Site data restore incomplete',timeout=30)
                        switch(4);navigate(url);expect('work');manager();assert state()['site_data_ui']['usage']>600000 and state()['site_data_ui']['cookies']==3
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3);subprocess.run(['import','-window',str(w),str(out/'ru-site-data-small.png')],check=True,timeout=10)
                        click(state()['site_data_ui']['storage_delete']);wait_until(lambda:state()['dialog_count']>0,'No RU confirmation');subprocess.run(['import','-window',str(w),str(out/'ru-delete-confirmation.png')],check=True,timeout=10);key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'RU cancel failed')
                        click(state()['site_data_ui']['choose']);wait_until(lambda:state()['site_data_ui']['picker'],'No RU picker');click(state()['site_data_ui']['search']);type_text(w,'no-such-storage-site');wait_until(lambda:not state()['site_data_ui']['sites'],'Storage empty state failed');key_at(w,'Escape');key_at(w,'Escape')
                        switch(1);navigate(url);expect(None)
                    key_at(w,'q',4);p.wait(timeout=30);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_SITE_DATA_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
    print('PASS: native quota estimate, IDB/cache/localStorage deletion, cancelled confirmation, host cookies across ports/paths/HttpOnly, other host/container/private isolation, read-only site picker, restart and RU small UI')

if __name__=='__main__':main()
