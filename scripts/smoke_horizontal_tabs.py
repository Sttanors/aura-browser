"""Native horizontal tabs, reflow of live CEF pages and alternate-layout persistence."""
import json, os, re, shutil, signal, subprocess, threading, time, tomllib
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from dependencies import ROOT
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at,drag_between
probes={}
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_POST(self):
        data=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        probes[data['name']]=data;self.send_response(204);self.end_headers()
    def do_GET(self):
        name=self.path.strip('/').split('?')[0]
        body=f'''<!doctype html><meta charset="utf-8"><title>Page{name}</title>
        <style>body{{font:18px sans-serif}} input,button{{position:absolute;left:24px;width:240px;height:40px}} input{{top:90px}}button{{top:150px}}</style>
        <h1>Page {name}</h1><input placeholder="Unsaved form"><button onclick="window.onbeforeunload=()=>true">Protect page</button>
        <script>let ups=0,held=0,seq=0;document.addEventListener('mouseup',()=>ups++);document.addEventListener('mousemove',e=>{{if(e.buttons&1)held++}});const token=Math.random().toString(36);const input=document.querySelector('input');document.cookie='probe={name}; path=/; SameSite=Lax';function probe(){{fetch('/probe',{{method:'POST',body:JSON.stringify({{name:{json.dumps(name)},token,ups,held,seq:seq++,value:input.value,width:innerWidth,protected:Boolean(window.onbeforeunload),cookie:document.cookie}})}}).catch(()=>{{}})}}setInterval(probe,150);probe()</script>'''.encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/horizontal-tabs-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();base=f'http://127.0.0.1:{server.server_port}'
        seed='language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\ntree_tabs=true\n'
        seed+='[[groups]]\nid=1\nworkspace=0\nname="Research"\ncolor=2\ncollapsed=false\n'
        seed+='[[containers]]\nid=0\nname="Default"\n[[containers]]\nid=1\nname="Work"\ncolor=1\n'
        seed+='[[windows]]\nactive=1\nworkspace=0\n'
        for name,parent,group,container in [('A',-1,1,1),('B',0,1,1),('C',-1,0,0)]:
            seed+='[[windows.tabs]]\nurl='+json.dumps(base+'/'+name)+f'\nparent={parent}\ngroup={group}\ncontainer={container}\n'
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
                        if p.poll() is not None:raise RuntimeError('Horizontal browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    wait_until(lambda:root().get('windows') and sum(t['url'] in [base+'/A',base+'/B',base+'/C'] for t in root()['windows'][0]['tabs'])==3 and all(t['ntp'] or t['loaded'] for s in root()['windows'] for t in s['tabs']),'Restore failed',timeout=35)
                    w=root()['window']
                    def state(win=None):return next(s for s in root()['windows'] if s['window']==(win or w))
                    def current():s=state();return s['tabs'][s['active_tab']]
                    def tab(id):return next(t for t in state()['tabs'] if t['id']==id)
                    def strip():return state().get('tab_strip') or {}
                    def factor():return state()['interface']['applied']/100
                    def click(r,button=1):click_at(w,round((r[0]+r[2]/2)*factor()),round((r[1]+r[3]/2)*factor()),button)
                    def palette(query):
                        key_at(w,'k',4);type_text(w,query)
                        wait_until(lambda:(state().get('commands_ui') or {}).get('query')==query and state()['commands_ui'].get('items'),'Palette failed: '+query)
                    def select(id):
                        palette(tab(id)['title']);key_at(w,'Return')
                        wait_until(lambda:current()['id']==id and not state().get('commands_ui') and state()['panel']==0 and (not strip() or any(r['id']==id for r in strip()['items'])),'Selection failed')
                    def layout(value):
                        palette('tab_layout');key_at(w,'Return');wait_until(lambda:bool(state().get('settings_ui')),'No layout setting')
                        control=next(c for c in state()['settings_ui']['controls'] if c['id']=='tab_layout')
                        click(next(o['bounds'] for o in control['options'] if o['value']==value))
                        wait_until(lambda:bool(state().get('tab_strip'))==(value=='horizontal'),'Layout did not change')
                        key_at(w,'Escape')
                    def toggle_setting(id):
                        palette(id);key_at(w,'Return');wait_until(lambda:bool(state().get('settings_ui')),'No toggle setting')
                        control=next(c for c in state()['settings_ui']['controls'] if c['id']==id)
                        click(control['bounds']);time.sleep(.2);key_at(w,'Escape')
                    def item(id):return next(r for r in strip()['items'] if r['id']==id)
                    def context(id):click(item(id)['bounds'],3);wait_until(lambda:bool(state().get('menu_ui')),'No context menu')
                    def menu(id):
                        wait_until(lambda:any(i['id']==id for i in (state().get('menu_ui') or [])),'Menu item missing: '+str(id))
                        click(next(i['bounds'] for i in state()['menu_ui'] if i['id']==id))
                    def shot(name):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);time.sleep(.35)
                        subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
                    a=next(t['id'] for t in state()['tabs'] if t['url']==base+'/A');b=next(t['id'] for t in state()['tabs'] if t['url']==base+'/B');c=next(t['id'] for t in state()['tabs'] if t['url']==base+'/C')
                    wait_until(lambda:'B' in probes,'No live page probe')
                    if not launch:
                        assert not strip() and current()['id']==b
                        click_at(w,340,200);type_text(w,'keep-live-form');click_at(w,340,258)
                        wait_until(lambda:probes['B']['value']=='keep-live-form' and probes['B']['protected'],'Form not protected')
                        token=probes['B']['token'];before=probes['B']['width'];cookies=probes['B']['cookie'];created=state()['cef_created']
                        layout('horizontal')
                        wait_until(lambda:probes['B']['width']==before+240,'CEF viewport did not widen')
                        assert probes['B']['value']=='keep-live-form' and probes['B']['token']==token and probes['B']['cookie']==cookies and state()['cef_created']==created
                        assert not state()['sidebar'] and strip()['web'][0]==12
                        click_at(w,100,200);key_at(w,'End');type_text(w,'-wide')
                        wait_until(lambda:probes['B']['value']=='keep-live-form-wide','Input in widened page was intercepted by old sidebar')
                        click(item(a)['bounds']);wait_until(lambda:current()['id']==a,'Tab click was captured as native window drag')
                        click(item(b)['bounds']);wait_until(lambda:current()['id']==b,'Child selection failed')
                        click(item(a)['disclosure']);wait_until(lambda:tab(a)['tree_collapsed'],'No horizontal collapse')
                        assert current()['id']==b and probes['B']['value']=='keep-live-form-wide'
                        key_at(w,'2',4);wait_until(lambda:not tab(a)['tree_collapsed'] and current()['id']==b,'Numeric selection did not reveal ancestors')
                        group=next(r for r in strip()['items'] if r['id']==0)
                        click(group['bounds']);wait_until(lambda:all(r['id']!=b for r in strip()['items']),'Group collapse failed')
                        key_at(w,'2',4);wait_until(lambda:any(r['id']==b for r in strip()['items']),'Collapsed group did not reveal')
                        context(b);menu(1);wait_until(lambda:tab(b)['pinned'],'Pin failed')
                        click(item(b)['close']);assert tab(b)['pinned']
                        shot('en-tabs.png')
                        # Ordinary CEF closure must still ask about the live form.
                        key_at(w,'w',4);wait_until(lambda:state()['dialog_count']==1,'No beforeunload in horizontal mode')
                        key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0 and any(t['id']==b for t in state()['tabs']),'Declining close lost the child')
                        assert tab(b)['parent']==a and probes['B']['token']==token
                        for _ in range(10):key_at(w,'t',4)
                        wait_until(lambda:len(state()['tabs'])==13 and current()['id']==13 and strip().get('overflow') and any(r['id']==13 for r in strip()['items']),'Overflow did not reveal active tab')
                        last=strip()['first'];key_at(w,'F10');key_at(w,'Return')
                        wait_until(lambda:strip()['first']<last,'Keyboard previous did not scroll')
                        last=strip()['first'];key_at(w,'Return');wait_until(lambda:strip()['first']<last,'Repeated keyboard scroll lost focus');key_at(w,'Escape')
                        last=strip()['first'];click(strip()['previous']);wait_until(lambda:strip()['first']<last,'Previous button did not scroll')
                        at=strip()['first'];click(strip()['next']);wait_until(lambda:strip()['first']>at,'Next button did not scroll')
                        select(c);wait_until(lambda:any(r['id']==c for r in strip()['items']),'Selecting an offscreen tab did not reveal it')
                        key_at(w,'1',4);wait_until(lambda:current()['id']==a,'First numeric tab failed')
                        # Drag an unrelated root before the parent; the parent's live subtree stays intact.
                        rows=strip()['items'];source=next(r for r in rows if r['id'] and r['id'] not in (a,b,c));target=item(a)
                        sr,tr=source['bounds'],target['bounds'];drag_between(w,sr[0]+sr[2]//2,16,tr[0]+tr[2]//2,16)
                        wait_until(lambda:[t['id'] for t in state()['tabs']].index(source['id']) < [t['id'] for t in state()['tabs']].index(a),'Root did not move')
                        sr,tr=item(a)['bounds'],item(source['id'])['bounds']
                        drag_between(w,sr[0]+sr[2]//2,16,tr[0]+tr[2]//2,16)
                        wait_until(lambda:[t['id'] for t in state()['tabs']].index(a) < [t['id'] for t in state()['tabs']].index(source['id']),'Branch did not move')
                        assert tab(b)['parent']==a and probes['B']['token']==token
                        # A drag that ends over the document must not deliver mouse-up/move to CEF.
                        select(a);before_ups=probes['A']['ups'];before_held=probes['A']['held'];r=item(a)['bounds']
                        drag_between(w,r[0]+r[2]//2,16,400,400)
                        seq=probes['A']['seq'];wait_until(lambda:probes['A']['seq']>seq,'No post-drag page probe')
                        assert probes['A']['ups']==before_ups and probes['A']['held']==before_held,'Tab gesture leaked into the page'
                        assert tab(b)['parent']==a and probes['B']['token']==token
                        # Every sidebar destination is reachable in the compact Aura menu.
                        click(strip()['menu']);menu(21);wait_until(lambda:state()['panel']==1,'Bookmarks missing from compact menu');key_at(w,'Escape')
                        click(strip()['menu']);menu(26);wait_until(lambda:any(i['id']==19 for i in state()['menu_ui']),'More tools missing');key_at(w,'Escape')
                        select(b);time.sleep(.25)
                        key_at(w,'F10');wait_until(lambda:strip().get('focus')==0,'Native focus did not start')
                        for _ in range(32):
                            if strip().get('hint')==b:break
                            previous=strip()['focus'];key_at(w,'Tab')
                            wait_until(lambda:strip().get('focus')!=previous,'Native focus did not advance')
                        wait_until(lambda:strip().get('hint')==b,'Keyboard context hint missing');shot('en-keyboard-hint.png');key_at(w,'Escape')
                        r=item(b)['bounds'];subprocess.run(['xdotool','mousemove','--window',str(w),str(r[0]+r[2]//2),'16'],check=True)
                        wait_until(lambda:strip().get('hint')==b,'Hover context hint missing');time.sleep(.7);assert strip()['hint']==b
                        shot('en-hover-hint.png');subprocess.run(['xdotool','mousemove','--window',str(w),'500','120'],check=True)
                        layout('vertical');wait_until(lambda:probes['B']['width']==before,'CEF viewport did not narrow')
                        assert probes['B']['token']==token and probes['B']['value']=='keep-live-form-wide'
                        layout('horizontal')
                        toggle_setting('tree_tabs');wait_until(lambda:not state()['settings']['tree_tabs'],'Flat tab mode did not apply')
                        key_at(w,'9',4);wait_until(lambda:current()['id']==c and strip()['first']>2,'Flat overflow reveal failed');time.sleep(.2)
                        last=strip()['first'];key_at(w,'F10');key_at(w,'Return');wait_until(lambda:strip()['first']<last,'Flat keyboard scroll failed')
                        last=strip()['first'];key_at(w,'Return');wait_until(lambda:strip()['first']<last,'Flat repeated scroll lost focus');key_at(w,'Escape')
                        toggle_setting('tree_tabs');wait_until(lambda:state()['settings']['tree_tabs'],'Tree mode did not return');select(b)
                        # Settings apply to all windows of the profile, including an existing second window.
                        key_at(w,'n',4);wait_until(lambda:len(root()['windows'])==2,'New window missing')
                        other=next(s['window'] for s in root()['windows'] if s['window']!=w)
                        assert state(other)['tab_strip']
                        layout('vertical');wait_until(lambda:all(s['tab_strip'] is None for s in root()['windows']),'Second window did not reflow')
                        layout('horizontal');wait_until(lambda:all(s['tab_strip'] for s in root()['windows']),'Second window did not restore top layout')
                        key_at(other,'w',5);wait_until(lambda:len(root()['windows'])==1,'Second window did not close')
                        select(a);select(b);click(item(a)['disclosure']);wait_until(lambda:tab(a)['tree_collapsed'],'Collapse before restart failed')
                    else:
                        assert strip() and tab(a)['tree_collapsed'] and tab(b)['parent']==a and tab(b)['pinned']
                        subprocess.run(['xdotool','windowsize',str(w),'1800','1240'],check=True);time.sleep(.4)
                        wait_until(lambda:state()['interface']['applied']==200,'200% scale failed')
                        key_at(w,'2',4);wait_until(lambda:current()['id']==b and not tab(a)['tree_collapsed'],'Restored child did not reveal')
                        shot('ru-200-tabs.png');context(b);shot('ru-200-context.png');key_at(w,'Escape')
                        click(strip()['menu']);menu(26);shot('ru-200-tools.png');key_at(w,'Escape')
                        key_at(w,'n',12);key_at(w,'l',4);type_text(w,base+'/PRIVATE_SENTINEL');key_at(w,'Return')
                        wait_until(lambda:current()['private'] and current()['loaded'] and 'PRIVATE_SENTINEL' in probes,'Private page missing')
                        closed=state()['cef_closed'];key_at(w,'w',4);wait_until(lambda:state()['cef_closed']>closed,'Private page did not close')
                        layout('vertical');shot('ru-200-sidebar.png');layout('horizontal')
                    key_at(w,'q',4);end=time.monotonic()+30
                    while p.poll() is None and time.monotonic()<end:
                        try:s=json.loads(report.read_text())
                        except (OSError,ValueError):s={}
                        for win in s.get('windows',[]):
                            if win.get('dialog_count'):key_at(win['window'],'Return')
                        time.sleep(.1)
                    p.wait(timeout=5);final=json.loads(report.read_text())
                    assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                    saved=tomllib.loads((profile/'profile.toml').read_text());assert saved['tab_layout']=='horizontal'
                    for path in [profile/'profile.toml',profile/'profile.toml.bak']:
                        if path.exists():assert 'PRIVATE_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:
                        os.killpg(p.pid,signal.SIGTERM)
                        try:p.wait(timeout=5)
                        except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
        server.shutdown()
    print('PASS: native horizontal selection/hit testing, live reflow/form/cookie preservation, tree/group collapse/reveal, pin, beforeunload decline, overflow controls and active reveal, branch drag and input isolation, menu destinations, shared settings in two windows, restart, private exclusion, RU/EN light UI 200%, balanced CEF lifecycle')
if __name__=='__main__':main()
