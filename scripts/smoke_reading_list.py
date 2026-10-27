"""Real article extraction, offline opening after server shutdown, and library persistence."""
import json, os, re, shutil, signal, subprocess, threading, time, tomllib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at

class Handler(BaseHTTPRequestHandler):
    requests = []
    def log_message(self, *_): pass
    def do_GET(self):
        Handler.requests.append(self.path)
        title = 'PRIVATE_SENTINEL' if 'private' in self.path else 'Offline article title'
        paragraphs = ''.join(f'<p>Paragraph {i}. Секретный ЁЖ. This saved article explains offline reading with enough meaningful text for reliable extraction. No internet is needed to read this entire paragraph later.</p>' for i in range(48))
        data = f'''<!doctype html><meta charset=utf-8><title>{title}</title><main><article><h1>{title}</h1>{paragraphs}<p>&lt;script&gt;OFFLINE_ATTACK&lt;/script&gt;</p><h2>Final section</h2><p>END_OF_OFFLINE_ARTICLE</p></article></main>'''.encode()
        self.send_response(200); self.send_header('Content-Type','text/html; charset=utf-8')
        self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT/'artifacts/reading-list-smoke'; out.mkdir(parents=True,exist_ok=True)
    profile = out/'profile'; shutil.rmtree(profile,ignore_errors=True); profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
    thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
    url=f'http://127.0.0.1:{server.server_port}/article'
    try:
        for launch in range(3):
            if launch:
                config=profile/'profile.toml';config.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',config.read_text()))
                if launch==2:
                    config.write_text(re.sub(r'(?m)^ui_zoom\s*=.*$', 'ui_zoom=200', re.sub(r'(?m)^theme\s*=.*$', 'theme=1',config.read_text())))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={url if not launch else "aura://newtab"}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None: raise RuntimeError('Browser exited')
                        try:return json.loads(report.read_text())
                        except (ValueError,OSError):return {}
                    def current():
                        s=state(); tabs=s.get('tabs',[]); return tabs[s.get('active_tab',0)] if tabs else {}
                    wait_until(lambda:state().get('window') and current(),'No browser',timeout=30);w=state()['window']
                    def command(text):
                        key_at(w,'k',4);type_text(w,text)
                        wait_until(lambda:(state().get('commands_ui') or {}).get('items'),'No command')
                        key_at(w,'Return')
                    def library():
                        command('Read later');wait_until(lambda:state().get('reading_ui') is not None,'No library')
                    def ui():return state().get('reading_ui') or {}
                    def click(r):
                        scale=state().get('interface',{}).get('applied',100)/100
                        click_at(w,int((r[0]+r[2]//2)*scale),int((r[1]+r[3]//2)*scale))
                    def saved():return tomllib.loads((profile/'reading-list.toml').read_text())['articles']
                    def paste(text):
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-quiet','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                        try:
                            clip.stdin.write(text.encode());clip.stdin.close();time.sleep(.15);key_at(w,'v',4)
                            clip.wait(timeout=5)
                        finally:
                            if clip.poll() is None:clip.terminate();clip.wait(timeout=3)

                    if not launch:
                        wait_until(lambda:current().get('title')=='Offline article title' and current().get('loaded'),'Article not loaded',timeout=30)
                        library();click(ui()['save'])
                        wait_until(lambda:ui().get('count')==1,'Article not saved',timeout=20)
                        a=saved()[0];assert len(a['blocks'])>40 and 'END_OF_OFFLINE_ARTICLE' in str(a)
                        click(ui()['items'][0]['mark']);wait_until(lambda:ui()['items'][0]['read'],'Mark read failed')
                        click(ui()['save']);wait_until(lambda:state()['audit'].get('reading_articles_saved',0)==2,'Replacement failed')
                        assert ui()['count']==1 and ui()['items'][0]['read']
                        click(ui()['search']);paste('ёж');wait_until(lambda:ui().get('query')=='ёж' and ui()['count']==1,'Russian text search failed')
                        key_at(w,'a',4);paste('x'*1024);time.sleep(.2);assert ui()['query']=='ёж'
                        key_at(w,'a',4);type_text(w,'does-not-exist');wait_until(lambda:ui()['count']==0,'Search miss failed')
                        key_at(w,'a',4);key_at(w,'BackSpace');wait_until(lambda:ui()['count']==1,'Search clear failed')
                        # Reach the same library from the Bookmarks header.
                        key_at(w,'Escape');key_at(w,'b',4);key_at(w,'Tab');key_at(w,'Tab');key_at(w,'Return')
                        wait_until(lambda:state().get('reading_ui') is not None,'Bookmarks entry failed')
                        subprocess.run(['import','-window',str(w),str(out/'en-library.png')],check=True,timeout=10)
                        key_at(w,'Escape');key_at(w,'n',12);key_at(w,'l',4);type_text(w,url+'?private');key_at(w,'Return')
                        wait_until(lambda:current().get('private') and current().get('title')=='PRIVATE_SENTINEL' and current().get('loaded'),'Private page missing')
                        library();click(ui()['save']);time.sleep(.5);assert ui()['count']==1 and 'PRIVATE_SENTINEL' not in str(saved())
                        key_at(w,'Escape');key_at(w,'w',4);wait_until(lambda:not current().get('private'),'Private close failed')
                        server.shutdown();thread.join(timeout=5);library()
                    else:
                        subprocess.run(['xdotool','windowsize',str(w),str(2000 if launch==2 else 1000),str(1400 if launch==2 else 740)],check=True);time.sleep(.3)
                        library();assert ui()['count']==1 and ui()['items'][0]['read']
                        if launch==2:assert state()['interface']['applied']==200
                        subprocess.run(['import','-window',str(w),str(out/('ru-light-200.png' if launch==2 else 'ru-library-small.png'))],check=True,timeout=10)
                    requests=len(Handler.requests)
                    click(ui()['items'][0]['open'])
                    wait_until(lambda:current().get('url','').startswith('file://') and current().get('loaded') and current().get('title')=='Offline article title','Offline copy not opened',timeout=30)
                    assert len(Handler.requests)==requests
                    copy=next((profile/'reading-copies').glob('*.html'));html=copy.read_text()
                    assert 'END_OF_OFFLINE_ARTICLE' in html and '<script>' not in html and "default-src 'none'" in html
                    key_at(w,'End',4);time.sleep(.4)
                    subprocess.run(['import','-window',str(w),str(out/f'offline-{launch}.png')],check=True,timeout=10)
                    if launch==2:
                        library();click(ui()['items'][0]['delete']);wait_until(lambda:state()['dialog_count']==1,'No deletion prompt')
                        key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Cancel failed');assert ui()['count']==1
                        click(ui()['items'][0]['delete']);wait_until(lambda:state()['dialog_count']==1,'No deletion prompt')
                        key_at(w,'Return');wait_until(lambda:ui()['count']==0,'Deletion failed');assert not copy.exists()
                    key_at(w,'q',4);p.wait(timeout=30)
                    final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
    finally:
        if thread.is_alive():server.shutdown();thread.join(timeout=5)
        server.server_close()
    print('PASS: saved text, replacement/read state, RU full-text search, private exclusion, offline after server shutdown and restart, deletion, native library entry')

if __name__=='__main__':main()
