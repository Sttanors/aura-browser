"""Local article extraction, native lifecycle, selection, reader controls and CSP."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at,drag_between


class Handler(QuietHandler):
    def end_headers(self):
        if 'csp=1' in self.path:
            self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'nonce-aura'; style-src 'none'; img-src 'none'; require-trusted-types-for 'script'; trusted-types 'none'")
        super().end_headers()


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/reader-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(Handler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        url=f'http://127.0.0.1:{server.server_port}/reader.html'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={url}'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Reader browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def current():
                        s=state();tabs=s.get('tabs',[]);return tabs[s.get('active_tab',0)] if tabs else {}
                    def doc():
                        try:return json.loads(current().get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:current().get('loaded') and doc().get('token'),'No article page',timeout=30);w=state()['window']
                    def click(r,offset=False):click_at(w,(252 if offset else 0)+r[0]+r[2]//2,(88 if offset else 0)+r[1]+r[3]//2)
                    def open_reader():
                        key_at(w,'r',12);wait_until(lambda:current().get('reader_active') and current().get('reader_ui'),'Reader did not open',timeout=20)
                    def close_reader():
                        key_at(w,'Escape');wait_until(lambda:not current().get('reader_active') and doc().get('dialog')==0,'Reader did not close')
                    def navigate(address):key_at(w,'l',4);type_text(w,address);key_at(w,'Return');wait_until(lambda:current().get('loaded') and current().get('url')==address,'Reader navigation failed')
                    if launch==0:
                        token=doc()['token'];click(doc()['input'],True);key_at(w,'a',4);type_text(w,'kept-form')
                        wait_until(lambda:doc().get('form')=='kept-form','Original form edit failed')
                        # Protect original through the real beforeunload handler.
                        r=doc()['input'];click_at(w,252+r[0]+r[2]+90,88+r[1]+r[3]//2)
                        wait_until(lambda:doc().get('protected'),'Original page not protected')
                        created=state()['cef_created'];attacks=doc()['attacks'];original_scroll=doc()['scroll']
                        open_reader();assert current()['reader_ui']['characters']>10000 and current()['reader_ui']['blocks']>30
                        assert doc()['token']==token and doc()['form']=='kept-form' and doc()['protected'] and doc()['attacks']==attacks and state()['cef_created']==created
                        click(current()['reader_ui']['larger'],True);wait_until(lambda:state()['settings']['reader_font_size']==22,'Reader font size did not change')
                        click(current()['reader_ui']['font'],True);key_at(w,'Down');key_at(w,'Return');wait_until(lambda:state()['settings']['reader_font']=='sans','Reader font family did not change')
                        click(current()['reader_ui']['width'],True);key_at(w,'End');key_at(w,'Return');wait_until(lambda:state()['settings']['reader_width']==900,'Reader width did not change')
                        subprocess.run(['import','-window',str(w),str(out/'en-reader.png')],check=True,timeout=10)
                        text=current()['reader_ui']['text']
                        drag_between(w,252+text[0]+1,88+text[1]+16,252+text[0]+260,88+text[1]+16)
                        wait_until(lambda:current().get('selection_bytes',0)>0,'Reader text did not select')
                        key_at(w,'c',4)
                        copied=subprocess.run(['xclip','-selection','clipboard','-o'],capture_output=True,text=True,timeout=5)
                        assert copied.returncode==0 and 'This article' in copied.stdout,copied.stdout
                        # Focus text and page down: the overlay scrolls, original document does not.
                        body=current()['reader_ui']['body'];click_at(w,252+body[0]+80,88+min(420,body[1]+150));key_at(w,'Next')
                        wait_until(lambda:current()['reader_ui']['scroll']>0,'Reader body did not scroll')
                        assert doc()['scroll']==original_scroll and doc()['attacks']==attacks
                        close_reader();assert doc()['token']==token and doc()['form']=='kept-form' and doc()['protected']
                        # Reopen from command palette and exit using the reader button.
                        key_at(w,'k',4);type_text(w,'reader');wait_until(lambda:(state().get('commands_ui') or {}).get('items'),'No reader command');key_at(w,'Return')
                        wait_until(lambda:current()['reader_active'] and current()['reader_ui'],'Palette reader failed');click(current()['reader_ui']['exit'],True)
                        wait_until(lambda:not current()['reader_active'] and doc()['dialog']==0,'Exit button failed')
                        # Remove protection before navigating, avoiding a synthetic confirmation.
                        r=doc()['input'];click_at(w,252+r[0]+r[2]+90,88+r[1]+r[3]//2);wait_until(lambda:not doc()['protected'],'Cannot unprotect original')
                        navigate(url+'?csp=1');wait_until(lambda:doc().get('token')!=token,'CSP fixture not loaded');open_reader();assert state()['dialog_count']==0
                        close_reader()
                        navigate(url+'?empty=1');key_at(w,'r',12);wait_until(lambda:not current()['reader_pending'],'Empty article still pending');time.sleep(.3)
                        assert not current()['reader_active'] and doc()['dialog']==0
                        navigate(url+'?oversized=1');key_at(w,'r',12)
                        wait_until(lambda:state()['audit'].get('reader_large_articles',0)>0 and not current()['reader_active'] and not current()['reader_pending'],'Oversized article was silently truncated',timeout=25)
                        assert doc()['dialog']==0
                        navigate(url)
                    else:
                        assert state()['settings']['reader_font_size']==22 and state()['settings']['reader_font']=='sans' and state()['settings']['reader_width']==900
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3);open_reader()
                        subprocess.run(['import','-window',str(w),str(out/'ru-reader-small.png')],check=True,timeout=10)
                        for key in ('exit','larger','smaller','font','width'):
                            r=current()['reader_ui'][key];assert r[0]>=0 and r[0]+r[2]<=636 and r[1]>=0 and r[1]+r[3]<=520,(key,r)
                        close_reader();key_at(w,'n',12);navigate(url+'?private=1');open_reader();close_reader();key_at(w,'w',4)
                        wait_until(lambda:not current()['private'],'Private reader tab close failed')
                    key_at(w,'q',4);p.wait(timeout=30);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    for path in profile.glob('profile.toml*'):
                        data=path.read_text();assert 'private=1' not in data and 'READER_PAYLOAD' not in data and 'Paragraph 45.' not in data
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: real local reader, text-only article, preserved DOM/form/beforeunload/scroll, font/size/width persistence, CSP+Trusted Types, empty/private, RU small view')


if __name__=='__main__':main()
