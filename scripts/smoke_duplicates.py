"""Duplicate review/merge across windows, contexts and real CEF beforeunload."""
import json,os,shutil,signal,subprocess,threading,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from dependencies import ROOT
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

class Handler(BaseHTTPRequestHandler):
    serial=0
    finish_download=threading.Event()
    def log_message(self,*_):pass
    def do_GET(self):
        if self.path=='/slow.bin':
            self.send_response(200);self.send_header('Content-Type','application/octet-stream');self.send_header('Content-Length',str(2*1024*1024));self.end_headers()
            try:
                self.wfile.write(b'x'*65536);self.wfile.flush()
                Handler.finish_download.wait(timeout=45)
                self.wfile.write(b'x'*(2*1024*1024-65536))
            except (BrokenPipeError,ConnectionResetError):pass
            return
        Handler.serial+=1;token=Handler.serial
        frame=f'<iframe src="http://localhost:{self.server.server_port}/frame"></iframe>' if self.path.startswith('/opaque') else ''
        data=f'''<!doctype html><meta charset=utf-8><title>Duplicate page {token}</title>
<script>window.auraLeave=e=>{{e.preventDefault();e.returnValue='';}};</script>
<input style="position:absolute;left:20px;top:20px;width:180px;height:32px" oninput="document.title='Form: '+this.value">
<button style="position:absolute;left:20px;top:80px" onclick="window.protectedPage=!window.protectedPage;if(protectedPage)window.addEventListener('beforeunload',auraLeave);else window.removeEventListener('beforeunload',auraLeave)">Protect form</button>
<a href="/slow.bin" download="duplicate-test.bin" style="position:absolute;left:20px;top:200px;width:180px;height:34px">Download fixture</a>
<button style="position:absolute;left:20px;top:140px" onclick="window.audio=new AudioContext();audio.resume();document.title='Audio running'">Play audio</button>{frame}'''.encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/duplicates-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    report=out/'report.json';report.unlink(missing_ok=True)
    with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();base=f'http://127.0.0.1:{server.server_port}'
        config='language="en"\nrestore=true\nquit_last_tab=true\ncheck_updates=false\nfilter_updates=false\nopen_downloads=false\nnotify_downloads=false\n'
        config+='[[workspaces]]\nid=0\nname="Personal space"\n[[workspaces]]\nid=1\nname="Research"\n'
        config+='[[containers]]\nid=0\nname="Default"\n[[containers]]\nid=1\nname="Separate"\n'
        specs=[('/same',0,0,True),('/same',0,0,False),('/same',0,0,False),('/same#anchor',0,0,False),('/same?variant=1',0,0,False),('/same',0,1,False),('/same',1,0,True)]
        for tabs in (specs,[('/same',0,0,False)]):
            config+='[[windows]]\nactive=0\nworkspace=0\n'
            for path,space,container,pin in tabs:
                config+='[[windows.tabs]]\nurl='+json.dumps(base+path)+f'\nworkspace={space}\ncontainer={container}\npinned={str(pin).lower()}\n'
        (profile/'profile.toml').write_text(config)
        with (out/'process.log').open('w') as log:
            p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
            try:
                def root():
                    if p.poll() is not None:raise RuntimeError('Duplicate browser exited')
                    try:return json.loads(report.read_text())
                    except (ValueError,OSError):return {}
                def windows():return root().get('windows',[])
                def state(w):return next((s for s in windows() if s['window']==w),{})
                def current(w):
                    s=state(w);return s['tabs'][s['active_tab']]
                wait_until(lambda:len(windows())==2 and all(t['loaded'] for s in windows() for t in s['tabs']),'Restore fixture failed',timeout=40)
                first=next(s['window'] for s in windows() if len(s['tabs'])==7);second=next(s['window'] for s in windows() if s['window']!=first)
                def ui(w=first):return state(w).get('duplicates_ui') or {}
                def click(w,r):
                    factor=state(w).get('interface',{}).get('applied',100)/100
                    click_at(w,round((r[0]+r[2]/2)*factor),round((r[1]+r[3]/2)*factor))
                def palette(w,query):
                    key_at(w,'k',4);type_text(w,query)
                    wait_until(lambda:(state(w).get('commands_ui') or {}).get('query')==query and state(w)['commands_ui'].get('items'),'No palette result')
                def manager(w=first):
                    palette(w,'Duplicate tabs');key_at(w,'Return');wait_until(lambda:bool(ui(w)),'No duplicate manager')
                def review(path,w=first):
                    manager(w);key_at(w,'Home')
                    for _ in range(12):
                        row=next((r for r in ui(w).get('items',[]) if r.get('url')==base+path and r.get('container')==0),None)
                        if row:
                            click(w,row['review']);wait_until(lambda:ui(w).get('review') and ui(w).get('items') and 'window' in ui(w)['items'][0],'No review');return
                        key_at(w,'Next');time.sleep(.1)
                    raise RuntimeError('Group not reachable: '+path)
                def select(w,id):
                    title=next(t['title'] for t in state(w)['tabs'] if t['id']==id)
                    palette(w,title);key_at(w,'Return');wait_until(lambda:current(w)['id']==id,'Tab activation failed')
                def navigate(w,path):
                    key_at(w,'l',4);type_text(w,base+path);key_at(w,'Return')
                    wait_until(lambda:current(w)['url']==base+path and current(w)['loaded'],'Navigation failed')
                def shot(w,name):
                    subprocess.run(['xdotool','windowraise',str(w)],check=True)
                    time.sleep(.35)
                    subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
                def add(w,path,private=False):
                    key_at(w,'n' if private else 't',12 if private else 4);navigate(w,path);return current(w)['id']
                # The same address in another container or with query/fragment differences is separate.
                manager();wait_until(lambda:ui()['groups']==1,'Incorrect grouping')
                assert ui()['items'][0]['count']==5
                review('/same');wait_until(lambda:ui()['closable']==3,'Protected/loading rows not ready')
                assert ui()['keep_tab']==1
                # Form state and a genuine sticky-activation beforeunload handler.
                key_at(first,'Escape');select(first,2);click_at(first,310,124);type_text(first,'kept-form');click_at(first,330,182)
                wait_until(lambda:current(first)['title']=='Form: kept-form','Form not edited')
                review('/same');click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No merge confirmation')
                shot(first,'en-confirm.png');key_at(first,'Escape');wait_until(lambda:state(first)['dialog_count']==0,'Confirmation cancel failed')
                assert len(state(first)['tabs'])==7
                # Membership changes in another window invalidate the confirmation.
                review('/same');click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No stale confirmation')
                navigate(second,'/different');key_at(first,'Return');wait_until(lambda:state(first)['dialog_count']==0,'Stale prompt stuck')
                time.sleep(.4);assert len(state(first)['tabs'])==7 and not ui()['busy']
                navigate(second,'/same');review('/same');wait_until(lambda:ui()['closable']==3,'No close plan')
                shot(first,'en-review.png');click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No merge prompt');key_at(first,'Return')
                wait_until(lambda:state(first)['dialog_count']==1 and ui().get('busy'),'No real beforeunload',timeout=20)
                shot(first,'en-beforeunload.png');key_at(first,'Escape');wait_until(lambda:not ui()['busy'],'Declining did not stop the queue')
                assert len(state(first)['tabs'])==7 and len(state(second)['tabs'])==1
                assert next(t for t in state(first)['tabs'] if t['id']==2)['title']=='Form: kept-form'
                # Accepting closes three eligible copies serially, preserving two pinned copies.
                review('/same');wait_until(lambda:ui()['closable']==3,'Plan did not recover after cancel')
                click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No merge prompt');key_at(first,'Return')
                wait_until(lambda:state(first)['dialog_count']==1 and ui().get('busy'),'No second beforeunload');key_at(first,'Return')
                wait_until(lambda:not ui().get('busy') and ui().get('closed')==3,'Merge did not finish',timeout=30)
                assert {t['id'] for t in state(first)['tabs']}=={1,4,5,6,7}
                assert state(second) and len(state(second)['tabs'])==1 and state(second)['tabs'][0]['ntp']
                # Show a kept member in another workspace; the underlying page is not reloaded.
                review('/same');row=next(r for r in ui()['items'] if r['tab']==7);click(first,row['show'])
                wait_until(lambda:state(first)['workspace']==1 and current(first)['id']==7,'Cross-workspace Show failed')
                # Live Web Audio is kept even when another duplicate is selected to remain.
                media1=add(first,'/media');media2=add(first,'/media');media3=add(first,'/media')
                select(first,media2);click_at(first,330,248);wait_until(lambda:current(first)['title']=='Audio running','Audio context not running')
                review('/media');wait_until(lambda:any(r['tab']==media2 and r['protection']&4 for r in ui()['items']),'Audio not protected')
                click(first,next(r['keep'] for r in ui()['items'] if r['tab']==media1));wait_until(lambda:ui()['keep_tab']==media1 and ui()['closable']==1,'Keep choice failed')
                click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No media merge prompt');key_at(first,'Return')
                wait_until(lambda:not ui()['busy'] and ui()['closed']==1,'Media merge failed');assert any(t['id']==media2 for t in state(first)['tabs']) and all(t['id']!=media3 for t in state(first)['tabs'])
                # Inaccessible child frames conservatively keep the duplicate.
                opaque1=add(first,'/opaque');opaque2=add(first,'/opaque');review('/opaque')
                click(first,next(r['keep'] for r in ui()['items'] if r['tab']==opaque1));wait_until(lambda:ui()['closable']==1,'Opaque plan not ready')
                click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No opaque prompt');key_at(first,'Return')
                wait_until(lambda:state(first)['dialog_count']==0 and not ui()['busy'] and ui()['status'].startswith('Duplicate cleanup finished.') and ui()['closed']==0,'Opaque probe stuck',timeout=25)
                assert ui()['closed']==0 and all(any(t['id']==id for t in state(first)['tabs']) for id in (opaque1,opaque2))
                # Stop remaining work while the current beforeunload prompt belongs to another window.
                key_at(second,'Escape');navigate(second,'/stop');stop_tab=current(second)['id']
                click_at(second,330,182);add(second,'/stop')
                local_stop=add(first,'/stop');review('/stop');wait_until(lambda:ui()['closable']==2,'Stop plan missing')
                click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No stop merge confirmation');key_at(first,'Return')
                wait_until(lambda:state(second)['dialog_count']==1 and ui().get('busy'),'No remote beforeunload')
                click(first,ui()['stop']);wait_until(lambda:not ui()['busy'],'Stop did not end queue')
                assert state(second)['dialog_count']==1
                key_at(second,'Escape');wait_until(lambda:state(second)['dialog_count']==0,'Pending prompt cancel failed')
                assert len([t for t in state(second)['tabs'] if t['url']==base+'/stop'])==2
                select(second,stop_tab);click_at(second,330,182)
                # Changing the kept page while a remote leave prompt is pending stops the remainder.
                click_at(second,330,182);review('/stop');wait_until(lambda:ui()['closable']==2,'Keeper-change plan missing')
                click(first,ui()['close']);wait_until(lambda:state(first)['dialog_count']==1,'No keeper-change prompt');key_at(first,'Return')
                wait_until(lambda:state(second)['dialog_count']==1 and ui().get('busy'),'No pending keeper-change close')
                navigate(first,'/changed-keeper');manager();wait_until(lambda:not ui()['busy'] and 'kept tab changed' in ui()['status'],'Changed keeper did not stop queue')
                key_at(second,'Escape');wait_until(lambda:state(second)['dialog_count']==0,'Remote prompt cancel failed')
                assert len([t for t in state(second)['tabs'] if t['url']==base+'/stop'])==2
                select(second,stop_tab);click_at(second,330,182)
                # An active download protects its exact source window/tab.
                download_source=add(first,'/download');download_keep=add(first,'/download');select(first,download_source)
                click_at(first,330,304);wait_until(lambda:any(not d['complete'] and not d.get('cancelled') for d in state(first).get('downloads',[])),'Download not started')
                review('/download');wait_until(lambda:any(r['tab']==download_source and r['protection']&8 for r in ui()['items']),'Download source not protected')
                click(first,next(r['keep'] for r in ui()['items'] if r['tab']==download_keep));wait_until(lambda:ui()['keep_tab']==download_keep and ui()['closable']==0,'Download offered for closure')
                Handler.finish_download.set();wait_until(lambda:any(d['complete'] for d in state(first).get('downloads',[])),'Download did not finish',timeout=20)
                # Private duplicates are visible only to their owner window.
                add(first,'/PRIVATE_TAB_SENTINEL',True);add(first,'/PRIVATE_TAB_SENTINEL',True);manager()
                wait_until(lambda:any(r.get('private') for r in ui()['items']),'Own private group missing')
                manager(second);assert not any(r.get('private') for r in ui(second)['items'])
                old={s['window'] for s in windows()};key_at(first,'n',5);wait_until(lambda:any(s['window'] not in old for s in windows()),'No private window')
                private=next(s['window'] for s in windows() if s['window'] not in old)
                navigate(private,'/PRIVATE_WINDOW_SENTINEL');add(private,'/PRIVATE_WINDOW_SENTINEL',True);manager(private)
                assert ui(private)['groups']==1 and all(r.get('private') and 'PRIVATE_WINDOW_SENTINEL' in r['url'] for r in ui(private)['items'])
                key_at(private,'w',4);wait_until(lambda:state(private) and len(state(private)['tabs'])==1,'Private tab did not close')
                key_at(private,'w',4);wait_until(lambda:not state(private),'Private window not closed')
                # RU/EN and minimum logical viewport at 200%, including keyboard paging.
                def choose(id,value):
                    palette(first,id);key_at(first,'Return');wait_until(lambda:state(first).get('settings_ui'),'No settings')
                    c=next(c for c in state(first)['settings_ui']['controls'] if c['id']==id)
                    click(first,next(o['bounds'] for o in c['options'] if o['value']==str(value)))
                    wait_until(lambda:str(state(first)['settings'][id])==str(value),'Setting failed')
                choose('language','ru');subprocess.run(['xdotool','windowsize',str(first),'1800','1240'],check=True);choose('ui_zoom',200)
                manager();wait_until(lambda:state(first)['interface']['applied']==200,'Scale missing');shot(first,'ru-200-overview.png')
                review('/same');key_at(first,'End');shot(first,'ru-200-review.png');key_at(first,'Tab');key_at(first,'Tab');key_at(first,'Return')
                key_at(first,'q',4);p.wait(timeout=30);final=json.loads(report.read_text());assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                for path in profile.glob('profile.toml*'):assert 'PRIVATE_' not in path.read_text()
            finally:
                Handler.finish_download.set()
                if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: exact/context grouping across windows/workspaces, chosen keeper, stale/cancel/accept beforeunload, pinned/audio/opaque protections, last-window preservation, private isolation, RU UI 200%, lifecycle')

if __name__=='__main__':main()
