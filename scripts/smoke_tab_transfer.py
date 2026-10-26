"""Move real OSR documents across native windows, without navigation or privacy changes."""
import ctypes, json, os, re, shutil, signal, subprocess, threading, time
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke_horizontal_tabs import Handler as ProbeHandler, probes
class Handler(ProbeHandler):
    download_started=threading.Event()
    download_release=threading.Event()
    download_data=b"Aura transfer download\n"*40000
    def do_GET(self):
        if self.path=='/transfer.bin':
            data=self.download_data
            self.send_response(200);self.send_header('Content-Type','application/octet-stream');self.send_header('Content-Disposition','attachment; filename="transfer.bin"');self.send_header('Content-Length',str(len(data)));self.end_headers()
            try:
                self.wfile.write(data[:32768]);self.wfile.flush();self.download_started.set();self.download_release.wait(20);self.wfile.write(data[32768:])
            except (BrokenPipeError,ConnectionResetError):pass
            return
        name=self.path.strip('/').split('?')[0]
        data=f'''<!doctype html><meta charset="utf-8"><title>Page{name}</title>
        <style>input,button{{position:absolute;left:24px;width:240px;height:40px}}input{{top:90px}}button{{top:150px}}a{{position:absolute;left:24px;top:220px}}</style>
        <h1>Page {name}</h1><input placeholder="Unsaved form"><button onclick="window.onbeforeunload=()=>true">Protect page</button>
        <a href="/Child{name}" target="_blank">Open child</a><a style="left:320px" href="/transfer.bin">Download</a>
        <script>const input=document.querySelector('input');const token=Math.random().toString(36);document.cookie='transfer={name};path=/;SameSite=Lax';
        input.oninput=()=>{{document.title='Page{name}: '+input.value;if(!history.state?.aura)history.pushState({{aura:true}},'',location.pathname+'?step=1')}};
        setInterval(()=>fetch('/probe',{{method:'POST',body:JSON.stringify({{name:{json.dumps(name)},token,value:input.value,width:innerWidth,protected:Boolean(window.onbeforeunload),cookie:document.cookie}})}}).catch(()=>{{}}),150)</script>'''.encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)

from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at, drag_between, Event, ButtonEvent

def held_drag(window, x1, y1, x2, y2):
    x11=ctypes.CDLL("libX11.so.6");x11.XOpenDisplay.restype=ctypes.c_void_p
    x11.XSendEvent.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_int,ctypes.c_long,ctypes.POINTER(Event)]
    x11.XSync.argtypes=[ctypes.c_void_p,ctypes.c_int];x11.XCloseDisplay.argtypes=[ctypes.c_void_p]
    display=x11.XOpenDisplay(None)
    if not display:raise RuntimeError("No isolated drag display")
    def send(kind,mask,x,y,state):
        event=Event();event.button=ButtonEvent(type=kind,display=display,window=int(window),x=x,y=y,state=state,button=1 if kind!=6 else 0,same_screen=True)
        assert x11.XSendEvent(display,int(window),False,mask,ctypes.byref(event));x11.XSync(display,False)
    send(4,4,x1,y1,0)
    for i in range(1,11):send(6,64,x1+(x2-x1)*i//10,y1+(y2-y1)*i//10,256)
    def release():
        try:send(5,8,x2,y2,256)
        finally:x11.XCloseDisplay(display)
    return release


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out=ROOT/'artifacts/tab-transfer-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir();shutil.rmtree(out/'downloads',ignore_errors=True)
    with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}'
        seed='language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\nconfirm_quit=false\n'
        seed+='download_directory='+json.dumps(str(out/'downloads'))+'\n'
        seed+='[[groups]]\nid=1\nworkspace=0\nname="Research"\ncolor=1\n[[groups]]\nid=2\nworkspace=0\nname="Destination"\ncolor=2\n'
        seed+='[[containers]]\nid=0\nname="Default"\n[[containers]]\nid=1\nname="Work"\ncolor=1\n'
        seed+='[[windows]]\nactive=0\nworkspace=0\n'
        for name,parent in [('A',-1),('B',0),('C',-1)]:
            seed+='[[windows.tabs]]\nurl='+json.dumps(base+'/'+name)+f'\ncontainer=1\nparent={parent}\ngroup={1 if name in ("A","B") else 0}\n'
        seed+='[[windows]]\nactive=0\nworkspace=0\n[[windows.tabs]]\nurl='+json.dumps(base+'/D')+'\ngroup=2\n'
        (profile/'profile.toml').write_text(seed)
        for launch in range(2):
            probes.clear()
            if launch:
                path=profile/'profile.toml';s=path.read_text();s=re.sub(r'(?m)^language\s*=.*$','language="ru"',s);s=re.sub(r'(?m)^ui_zoom\s*=.*$','ui_zoom=200',s);s=re.sub(r'(?m)^theme\s*=.*$','theme=1',s);path.write_text(s)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def root():
                        if p.poll() is not None:raise RuntimeError('Transfer browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def windows():return root().get('windows',[])
                    def state(w):return next((s for s in windows() if s['window']==w),{})
                    def current(w):s=state(w);return s['tabs'][s['active_tab']]
                    def by_name(name):return next(( (s['window'],t) for s in windows() for t in s['tabs'] if t['url'].split('?')[0]==base+'/'+name),None)
                    def factor(w):s=state(w)['interface'];return s['applied']/100*s['device_scale']
                    def click(w,r,button=1):f=factor(w);click_at(w,round((r[0]+r[2]/2)*f),round((r[1]+r[3]/2)*f),button)
                    def palette(w,q):
                        key_at(w,'k',4);type_text(w,q)
                        wait_until(lambda:(state(w).get('commands_ui') or {}).get('query')==q and state(w)['commands_ui'].get('items'),'No command: '+q)
                    def select(w,t):
                        palette(w,t['title']);key_at(w,'Return');wait_until(lambda:current(w)['id']==t['id'] and state(w)['panel']==0,'No selection')
                    def setting(w,id,value):
                        palette(w,id);key_at(w,'Return');wait_until(lambda:bool(state(w).get('settings_ui')),'No settings')
                        c=next(c for c in state(w)['settings_ui']['controls'] if c['id']==id)
                        click(w,next(o['bounds'] for o in c['options'] if o['value']==value))
                        wait_until(lambda:state(w)['settings'][id]==value,'Setting did not apply');key_at(w,'Escape')
                    def row(w,id):
                        s=state(w)
                        if s.get('tab_strip'):return next(r['bounds'] for r in s['tab_strip']['items'] if r['id']==id)
                        return next(r['bounds'] for r in s['sidebar'] if r['tab_index']>=0 and s['tabs'][r['tab_index']]['id']==id)
                    def edit(w,name,text):
                        x=12 if state(w).get('tab_strip') else 252;f=factor(w)
                        ds=state(w)['interface']['device_scale'];click_at(w,round(x*f+80*ds),round(88*f+114*ds));key_at(w,'a',4);type_text(w,text)
                        wait_until(lambda:probes.get(name,{}).get('value')==text,'Form input not preserved/routed')
                    def protect(w,name):
                        x=12 if state(w).get('tab_strip') else 252;f=factor(w)
                        ds=state(w)['interface']['device_scale'];click_at(w,round(x*f+80*ds),round(88*f+170*ds))
                        wait_until(lambda:probes.get(name,{}).get('protected'),'No beforeunload guard')
                    def shot(w,name):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);time.sleep(.35)
                        subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
                    def new(w,private=False):
                        old={s['window'] for s in windows()};key_at(w,'n',5 if private else 4)
                        wait_until(lambda:any(s['window'] not in old for s in windows()),'No new window')
                        return next(s['window'] for s in windows() if s['window'] not in old)
                    def navigate(w,name):
                        key_at(w,'l',4);type_text(w,base+'/'+name);key_at(w,'Return')
                        wait_until(lambda:current(w)['url']==base+'/'+name and current(w)['loaded'] and current(w)['frames']>0,'No live page',timeout=30)
                    def move_menu(w,id,target):
                        select(w,next(t for t in state(w)['tabs'] if t['id']==id))
                        # Exercise the shared context menu as well as keyboard destination selection.
                        if launch:
                            palette(w,'move_window');assert state(w)['commands_ui']['items'][0]['id']=='move_window';key_at(w,'Return')
                        else:
                            click(w,row(w,id),3);wait_until(lambda:bool(state(w).get('menu_ui')),'No tab menu')
                            click(w,next(i['bounds'] for i in state(w)['menu_ui'] if i['id']==13))
                        wait_until(lambda:any(i['kind']=='window_move' for i in (state(w).get('commands_ui') or {}).get('items',[])),'No window chooser')
                        uid=str(state(target)['window_uid']);entry=next(i for i in state(w)['commands_ui']['items'] if i['id']==uid)
                        assert entry['enabled']
                        if launch:
                            shot(w,'ru-200-window-chooser.png');key_at(w,'Down');key_at(w,'Up');key_at(w,'Return')
                        else:click(w,entry['bounds'])
                        wait_until(lambda:state(target).get('audit',{}).get('tabs_transferred_in',0)>0 and all(t['id']!=id for t in state(w)['tabs']),'Menu transfer failed')
                    def close(w):
                        key_at(w,'w',5);wait_until(lambda:not state(w),'Window did not close')
                    def position(w,x):
                        subprocess.run(['xdotool','windowsize','--sync',str(w),'900','620'],check=True)
                        subprocess.run(['xdotool','windowmove','--sync',str(w),str(x),'100'],check=True)
                        wait_until(lambda:state(w)['interface']['width']==900,'Window did not resize')
                    def geometry(w):
                        raw=subprocess.check_output(['xdotool','getwindowgeometry','--shell',str(w)],text=True)
                        return dict((k,int(v)) for line in raw.splitlines() if '=' in line for k,v in [line.split('=',1)] if v.lstrip('-').isdigit())
                    def drag(w,id,target,end=None,hold=False):
                        r=row(w,id);f=factor(w);a=geometry(w);b=geometry(target)
                        tx,ty=end or ((60,16) if state(target).get('tab_strip') else (110,195))
                        tf=factor(target)
                        # The release stays addressed to the captured source, with native root coordinates.
                        return (held_drag if hold else drag_between)(w,round((r[0]+r[2]/2)*f),round((r[1]+r[3]/2)*f),b['X']-a['X']+round(tx*tf),b['Y']-a['Y']+round(ty*tf))
                    wait_until(lambda:len(windows())==2 and by_name('D') and (launch or all(by_name(n) for n in ('A','B','C'))) and all(t['ntp'] or t['loaded'] for s in windows() for t in s['tabs']),'Restore failed',timeout=40)
                    if not launch:
                        source,a=by_name('A');target,d=by_name('D');_,b=by_name('B')
                        select(source,a);edit(source,'A','live-A');protect(source,'A')
                        token=probes['A']['token'];browser=a['cef_browser_id'];created=root()['cef_created_total']
                        for command in (1,2):
                            click(source,row(source,a['id']),3);wait_until(lambda:bool(state(source).get('menu_ui')),'No pin/mute menu')
                            click(source,next(i['bounds'] for i in state(source)['menu_ui'] if i['id']==command))
                        wait_until(lambda:current(source)['pinned'] and current(source)['audio_muted'],'Pin/mute not set')
                        position(source,20);position(target,1000)
                        drag(source,a['id'],target,end=(400,350));time.sleep(.5)
                        assert by_name('A')[0]==source,'Page content accepted tab drop'
                        release=drag(source,a['id'],target,hold=True)
                        wait_until(lambda:state(source)['tab_drop_target']==state(target)['window_uid'],'No drop preview')
                        shot(target,'en-drop-preview.png');key_at(source,'Escape')
                        wait_until(lambda:state(source)['tab_drop_target']==0,'Escape did not cancel preview');release()
                        assert by_name('A')[0]==source,'Cancelled gesture transferred a tab'
                        drag(source,a['id'],target)
                        wait_until(lambda:by_name('A')[0]==target,'Native cross-window drag failed')
                        moved=by_name('A')[1]
                        assert moved['cef_browser_id']==browser and moved['container']==1 and moved['parent']==0 and moved['pinned'] and moved['audio_muted'] and moved['group']==2
                        assert by_name('B')[1]['group']==1 and by_name('B')[1]['parent']==0 and probes['A']['token']==token and probes['A']['value']=='live-A'
                        assert root()['cef_created_total']==created and state(target)['dialog_count']==0
                        shot(target,'en-sidebar-moved.png')
                        key_at(target,'w',4);wait_until(lambda:state(target)['dialog_count']==1,'Transferred beforeunload did not route to destination')
                        drag(source,b['id'],target);time.sleep(.4)
                        assert by_name('B')[0]==source,'Window with an open dialog accepted a tab'
                        key_at(target,'Escape');wait_until(lambda:state(target)['dialog_count']==0,'Cannot decline transferred close')
                        assert probes['A']['token']==token
                        # The origin is an additional window that will be deleted, not just hidden root.
                        d=by_name('D')[1];select(target,d);edit(target,'D','live-D');dt=probes['D']['token'];db=d['cef_browser_id']
                        click_at(target,612,317)
                        wait_until(lambda:Handler.download_started.is_set() and any(d['name']=='transfer.bin' and not d['complete'] for d in state(target)['downloads']),'No live source download')
                        move_menu(target,d['id'],source)
                        record=next(v for v in state(source)['downloads'] if v['name']=='transfer.bin')
                        assert record['source_window']==state(source)['window_uid'] and record['source_tab']==by_name('D')[1]['id'] and not record['complete']
                        assert by_name('D')[1]['cef_browser_id']==db and probes['D']['token']==dt
                        # Only A remains there. Accept its real leave prompt and retire that controller.
                        key_at(target,'w',5);wait_until(lambda:state(target)['dialog_count']==1,'No transferred leave prompt on close')
                        key_at(target,'Return');wait_until(lambda:not state(target),'Old native parent controller not retired')
                        Handler.download_release.set()
                        wait_until(lambda:any(v['name']=='transfer.bin' and v['complete'] for v in state(source)['downloads']),'Moved download did not survive closing its original window')
                        record=next(v for v in state(source)['downloads'] if v['name']=='transfer.bin')
                        assert (out/'downloads'/record['name']).read_bytes()==Handler.download_data
                        source,d=by_name('D');select(source,d);edit(source,'D','live-D-after-origin-close')
                        assert probes['D']['token']==dt and current(source)['cef_browser_id']==db
                        wait_until(lambda:'live-D-after-origin-close' in current(source)['title'],'Title callbacks retained the retired controller')
                        assert current(source)['url'].endswith('?step=1')
                        key_at(source,'Left',8);wait_until(lambda:current(source)['url']==base+'/D','Transferred history lost Back')
                        key_at(source,'Right',8);wait_until(lambda:current(source)['url'].endswith('?step=1'),'Transferred history lost Forward')
                        assert probes['D']['token']==dt
                        click_at(source,332,317)
                        wait_until(lambda:by_name('ChildD') and by_name('ChildD')[1]['loaded'],'Transferred popup callback failed')
                        assert by_name('ChildD')[0]==source and by_name('ChildD')[1]['parent']==d['id']
                        key_at(source,'w',4);wait_until(lambda:not by_name('ChildD'),'Child did not close')
                        select(source,by_name('D')[1])
                        # Private pages cannot enter an ordinary window, even via captured native drag.
                        private1=new(source,True);navigate(private1,'PRIVATE_TRANSFER')
                        private2=new(source,True);position(private1,20);position(source,1000)
                        pt=current(private1);ptoken=probes['PRIVATE_TRANSFER']['token'];pid=pt['cef_browser_id']
                        drag(private1,pt['id'],source);time.sleep(.5)
                        assert by_name('PRIVATE_TRANSFER')[0]==private1 and not any(t['private'] for t in state(source)['tabs'])
                        move_menu(private1,pt['id'],private2)
                        assert by_name('PRIVATE_TRANSFER')[1]['private'] and by_name('PRIVATE_TRANSFER')[1]['cef_browser_id']==pid
                        close(private1);edit(private2,'PRIVATE_TRANSFER','private-after-origin-close')
                        assert probes['PRIVATE_TRANSFER']['token']==ptoken
                        close(private2)
                        assert 'PRIVATE_TRANSFER' not in (profile/'profile.toml').read_text()
                        # Actual cross-window drag works in the horizontal layout too.
                        setting(source,'tab_layout','horizontal');other=new(source)
                        position(source,20);position(other,1000)
                        d=by_name('D')[1];select(source,d);drag(source,d['id'],other)
                        wait_until(lambda:by_name('D')[0]==other,'Horizontal cross-window drag failed')
                        assert by_name('D')[1]['cef_browser_id']==db and probes['D']['token']==dt
                        shot(other,'en-horizontal-moved.png')
                    else:
                        source,d=by_name('D');target=next(s['window'] for s in windows() if s['window']!=source)
                        for win in (source,target):
                            subprocess.run(['xdotool','windowsize','--sync',str(win),'1800','1240'],check=True)
                            wait_until(lambda:state(win)['interface']['applied']==200,'200% UI failed')
                        select(source,d);edit(source,'D','RU-200-live');token=probes['D']['token'];browser=d['cef_browser_id']
                        move_menu(source,d['id'],target)
                        wait_until(lambda:by_name('D')[0]==target,'Scaled menu move failed')
                        assert probes['D']['token']==token and by_name('D')[1]['cef_browser_id']==browser
                        shot(target,'ru-200-moved.png')
                    key_at(windows()[0]['window'],'q',4);p.wait(timeout=35)
                    final=root() if p.poll() is None else json.loads(report.read_text())
                    assert final['cef_created_total']==final['cef_closed_total'],final
                    for file in ('profile.toml','profile.toml.bak'):
                        path=profile/file
                        assert not path.exists() or 'PRIVATE_TRANSFER' not in path.read_text()
                finally:
                    try:os.killpg(p.pid,signal.SIGTERM)
                    except ProcessLookupError:pass
                    try:p.wait(timeout=4)
                    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
    print('PASS: live sidebar/top cross-window drag, preview/Escape/content/dialog rejection, form/cookie/container/CEF identity, history, pin/mute/groups, promotion, beforeunload, keyboard chooser, last-tab fallback, retired parent input/title/popup routing, active download continuation/bytes, private isolation, restart, RU/EN UI 200%, balanced CEF lifecycle')
if __name__=='__main__':main()
