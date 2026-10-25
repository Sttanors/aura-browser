"""Real opener trees, live forms, beforeunload, branch moves and persistent structure."""
import json, os, re, shutil, signal, subprocess, threading, time, tomllib
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at, drag_between

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        name=self.path.split('?')[0].strip('/') or 'root'
        target={'root':'child','child':'leaf','branch':'deep','private':'PRIVATE_CHILD'}.get(name,'deep')
        body=f'''<!doctype html><meta charset="utf-8"><title>Tree {name}</title>
        <style>body{{font:18px sans-serif}} a,input,button{{position:absolute;left:20px;width:220px;height:40px;box-sizing:border-box}} a{{top:20px}} input{{top:100px}} button{{top:160px}}</style>
        <a target="_blank" href="/{target}">Open child</a>
        <input oninput="document.title='Tree {name}: '+this.value" placeholder="Unsaved form">
        <button onclick="window.onbeforeunload=()=>true;this.textContent='Protected';document.title+=' [protected]'">Protect page</button>'''.encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/tree-tabs-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ntree_tabs=true\ncheck_updates=false\nfilter_updates=false\nconfirm_quit=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        base=f'http://127.0.0.1:{server.server_port}'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';text=path.read_text()
                text=re.sub(r'(?m)^language\s*=.*$','language="ru"',text)
                text=re.sub(r'(?m)^ui_zoom\s*=.*$','ui_zoom=200',text);path.write_text(text)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={base}/root'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None: raise RuntimeError('Tree browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    wait_until(lambda:state().get('tabs') and state().get('presentation_frames',0)>0,'No tree window',timeout=35)
                    w=state()['window']
                    def current():return state()['tabs'][state()['active_tab']]
                    def tab(id):return next(t for t in state()['tabs'] if t['id']==id)
                    def by_path(path):return next(t for t in state()['tabs'] if t['url']==base+path)
                    def factor():return state()['interface']['applied']/100
                    def click(r):click_at(w,round((r[0]+r[2]/2)*factor()),round((r[1]+r[3]/2)*factor()))
                    def page(x,y):
                        wait_until(lambda:current()['loaded'] and current()['frames']>0,'No painted page')
                        time.sleep(.15)
                        click_at(w,round((252+x)*factor()),round((88+y)*factor()))
                    def palette(query):
                        key_at(w,'k',4);type_text(w,query)
                        wait_until(lambda:(state().get('commands_ui') or {}).get('query')==query and state()['commands_ui'].get('items'),'Palette failed: '+query)
                    def action(id):
                        palette(id);assert state()['commands_ui']['items'][0]['id']==id
                        key_at(w,'Return');wait_until(lambda:not state().get('commands_ui'),'Command stuck')
                    def select(id):
                        palette(tab(id)['title'].split(':')[0]);key_at(w,'Return');wait_until(lambda:current()['id']==id,'Activation failed')
                    def row(id):return next(r for r in state()['sidebar'] if r['tab_index']>=0 and state()['tabs'][r['tab_index']]['id']==id)
                    def toggle(id):click(row(id)['disclosure']);wait_until(lambda:tab(id)['tree_collapsed'],'Collapse failed')
                    def navigate(path):
                        key_at(w,'l',4);type_text(w,base+path);key_at(w,'Return')
                        wait_until(lambda:current()['url']==base+path and current()['loaded'],'Navigation failed')
                    def opener(path):
                        before=len(state()['tabs']);page(90,38)
                        wait_until(lambda:len(state()['tabs'])==before+1 and current()['url']==base+path and current()['loaded'],'Link did not open: '+path,timeout=25)
                        return current()['id']
                    def shot(name):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);time.sleep(.35)
                        subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
                    def setting(id):
                        action(id);wait_until(lambda:bool(state().get('settings_ui')),'No setting')
                        control=next(c for c in state()['settings_ui']['controls'] if c['id']==id)
                        click(control['bounds']);time.sleep(.3);key_at(w,'Escape')
                    if not launch:
                        wait_until(lambda:current()['loaded'] and current()['title']=='Tree root' and current()['frames']>0,'Root did not load')
                        root=current()['id'];child=opener('/child');leaf=opener('/leaf')
                        assert tab(child)['parent']==root and tab(leaf)['parent']==child
                        assert [r['depth'] for r in state()['sidebar']]==[0,1,2]
                        page(90,120);type_text(w,'kept-form');page(90,180)
                        wait_until(lambda:tab(leaf)['title']=='Tree leaf: kept-form [protected]','Form not edited/protected')
                        # The disclosure is reachable through native keyboard focus.
                        key_at(w,'F10');key_at(w,'Tab');key_at(w,'Tab');key_at(w,'Return')
                        wait_until(lambda:tab(root)['tree_collapsed'] and len(state()['sidebar'])==1,'Keyboard disclosure failed')
                        assert current()['id']==leaf
                        key_at(w,'Return');wait_until(lambda:not tab(root)['tree_collapsed'],'Keyboard disclosure did not expand')
                        key_at(w,'Escape')
                        created=state()['cef_created'];toggle(child)
                        assert current()['id']==leaf and 'kept-form' in tab(leaf)['title'] and state()['dialog_count']==0 and state()['cef_created']==created
                        assert len(state()['sidebar'])==2
                        shot('en-collapsed.png')
                        select(leaf);wait_until(lambda:not tab(child)['tree_collapsed'] and len(state()['sidebar'])==3,'Search did not reveal ancestors')
                        select(child);page(90,120);type_text(w,'parent-form');page(90,180)
                        wait_until(lambda:'parent-form [protected]' in tab(child)['title'],'Parent not edited/protected')
                        key_at(w,'w',4);wait_until(lambda:state()['dialog_count']==1,'No real parent beforeunload')
                        key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Decline stuck')
                        assert tab(leaf)['parent']==child and 'parent-form' in tab(child)['title']
                        key_at(w,'w',4);wait_until(lambda:state()['dialog_count']==1,'No second beforeunload');key_at(w,'Return')
                        wait_until(lambda:len(state()['tabs'])==2 and state()['dialog_count']==0 and tab(leaf)['parent']==root,'Parent did not close/promote')
                        assert tab(leaf)['parent']==root and 'kept-form' in tab(leaf)['title']
                        select(leaf);action('tree_detach');wait_until(lambda:tab(leaf)['parent']==0,'Detach failed')
                        select(root);action('tree_child');branch=current()['id'];navigate('/branch')
                        deep=opener('/deep');assert tab(branch)['parent']==root and tab(deep)['parent']==branch
                        shot('en-expanded.png')
                        # Move a subtree above an unrelated root, retaining descendants and DOM.
                        source=row(branch)['bounds'];target=row(leaf)['bounds']
                        drag_between(w,120,source[1]+23,120,target[1]+23)
                        wait_until(lambda:tab(branch)['parent']==0,'Branch did not detach on move')
                        assert tab(deep)['parent']==branch and 'kept-form' in tab(leaf)['title']
                        # Drop on own descendant must never create a cycle or lose a page.
                        source=row(branch)['bounds'];target=row(deep)['bounds']
                        drag_between(w,120,source[1]+23,120,target[1]+23)
                        assert tab(branch)['parent']==0 and tab(deep)['parent']==branch
                        # Pinned root and private opener tree are independent of normal saved edges.
                        select(root);r=row(root)['bounds'];click_at(w,120,r[1]+23,3)
                        wait_until(lambda:bool(state().get('menu_ui')),'No tab context menu')
                        click(next(i['bounds'] for i in state()['menu_ui'] if i['id']==1));wait_until(lambda:tab(root)['pinned'],'Pin failed')
                        key_at(w,'n',12);navigate('/private');private=current()['id'];private_child=opener('/PRIVATE_CHILD')
                        assert tab(private_child)['parent']==private and tab(private_child)['private']
                        closed=state()['cef_closed'];key_at(w,'w',4);wait_until(lambda:state()['cef_closed']>closed and all(t['id']!=private_child for t in state()['tabs']),'Private child not closed')
                        closed=state()['cef_closed'];key_at(w,'w',4);wait_until(lambda:state()['cef_closed']>closed and all(t['id']!=private for t in state()['tabs']),'Private root not closed')
                        select(deep);toggle(branch)
                        assert current()['id']==deep and tab(branch)['tree_collapsed']
                        # Snapshot captures the same tree without expanding it.
                        key_at(w,'e',12);wait_until(lambda:bool(state().get('sessions_ui')),'No snapshot manager')
                        click(state()['sessions_ui']['input']);type_text(w,'Tree snapshot');click(state()['sessions_ui']['save'])
                        wait_until(lambda:len(state()['sessions_ui']['items'])==1,'Snapshot save failed')
                        key_at(w,'Escape')
                    else:
                        wait_until(lambda:len(state()['tabs'])==4 and all(t['loaded'] for t in state()['tabs']),'Restore failed',timeout=35)
                        root=by_path('/root')['id'];leaf=by_path('/leaf')['id'];branch=by_path('/branch')['id'];deep=by_path('/deep')['id']
                        assert tab(root)['pinned'] and tab(branch)['parent']==0 and tab(deep)['parent']==branch
                        assert current()['id']==deep and tab(branch)['tree_collapsed']
                        assert len(state()['sidebar'])==3
                        subprocess.run(['xdotool','windowsize',str(w),'1800','1240'],check=True);time.sleep(.4)
                        wait_until(lambda:state()['interface']['applied']==200,'200% scale failed')
                        shot('ru-200-collapsed.png')
                        select(deep);wait_until(lambda:not tab(branch)['tree_collapsed'],'Restored ancestor not revealed')
                        select(branch);action('tree_toggle');wait_until(lambda:tab(branch)['tree_collapsed'],'Keyboard collapse failed')
                        setting('tree_tabs');wait_until(lambda:len(state()['sidebar'])==4,'Flat mode still hides children')
                        assert tab(branch)['tree_collapsed'] and tab(deep)['parent']==branch
                        shot('ru-200-flat.png')
                        setting('tree_tabs');wait_until(lambda:len(state()['sidebar'])==3,'Tree mode not restored')
                        action('tree_toggle');wait_until(lambda:len(state()['sidebar'])==4,'Keyboard expand failed')
                        shot('ru-200-expanded.png')
                        # Closing a branch keeps its descendants alive and promotes them to root.
                        key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==3 and tab(deep)['parent']==0,'Restored parent close failed')
                        assert tab(deep)['parent']==0
                        # Deep trees retain their logical level while indentation stays bounded.
                        for _ in range(6):action('tree_child')
                        wait_until(lambda:max(r['depth'] for r in state()['sidebar'])>=5,'Deep tree depth missing')
                        assert any(r['depth']>3 for r in state()['sidebar'])
                        shot('ru-200-deep.png')
                        key_at(w,'1',4);wait_until(lambda:current()['id']==root,'Visual numeric order failed')
                    key_at(w,'q',4);end=time.monotonic()+30
                    while p.poll() is None and time.monotonic()<end:
                        try:s=json.loads(report.read_text())
                        except (OSError,ValueError):s={}
                        if s.get('dialog_count'):key_at(w,'Return')
                        time.sleep(.1)
                    p.wait(timeout=5);final=json.loads(report.read_text())
                    assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
                    saved=tomllib.loads((profile/'profile.toml').read_text())
                    assert 'PRIVATE_CHILD' not in (profile/'profile.toml').read_text() and '/private' not in (profile/'profile.toml').read_text()
                    backup=profile/'profile.toml.bak'
                    if backup.exists():assert 'PRIVATE_CHILD' not in backup.read_text() and '/private' not in backup.read_text()
                    if not launch:
                        tabs=saved['windows'][0]['tabs'];parent=next(i for i,t in enumerate(tabs) if t['url']==base+'/branch')
                        assert next(t for t in tabs if t['url']==base+'/deep')['parent']==parent
                        snap=saved['named_sessions'][0]['windows'][0]['tabs'];assert any(t['parent']>=0 for t in snap)
                finally:
                    if p.poll() is None:
                        os.killpg(p.pid,signal.SIGTERM)
                        try:p.wait(timeout=5)
                        except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
        server.shutdown()
    print('PASS: real opener/popup trees, nesting, live forms, collapse/reveal, beforeunload decline/accept, promotion/detach, branch drag/cycle prevention, pin, private tree exclusion, snapshots, restart, RU/EN 200%, flat mode and keyboard commands, balanced CEF lifecycle')

if __name__=='__main__':main()
