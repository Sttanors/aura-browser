"""Named snapshots of real windows, layout and identity; original documents stay live."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/sessions-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();url=f'http://127.0.0.1:{server.server_port}/split.html?name='
        seed='language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n'
        seed+='[[workspaces]]\nid=0\nname="Personal space"\n[[workspaces]]\nid=1\nname="Study"\n'
        seed+='[[groups]]\nid=1\nworkspace=1\nname="Sources"\ncolor=2\ncollapsed=false\n'
        seed+='[[containers]]\nid=0\nname="Default"\n[[containers]]\nid=1\nname="Accounts"\ncolor=1\n'
        seed+='[[windows]]\nactive=1\nworkspace=1\nsplit_left=1\nsplit_right=2\n'
        for name,workspace,group,container,pinned in [('Root',0,0,0,False),('A1',1,1,0,True),('A2',1,1,1,False)]:seed+='[[windows.tabs]]\nurl='+json.dumps(url+name)+f'\nworkspace={workspace}\ngroup={group}\ncontainer={container}\npinned={str(pinned).lower()}\nworkspace_active=true\n'
        seed+='[[windows]]\nactive=0\nworkspace=0\n[[windows.tabs]]\nurl='+json.dumps(url+'Second')+'\nworkspace=0\ncontainer=0\n'
        (profile/'profile.toml').write_text(seed)
        for launch in range(3):
            if launch:
                path=profile/'profile.toml';text=re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text())
                if launch==2:text=re.sub(r'(?m)^restore\s*=.*$','restore=false',text)
                path.write_text(text)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+(['--private',f'--url={url}PRIVATE_SESSION_SENTINEL'] if launch==2 else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def root():
                        if p.poll() is not None:raise RuntimeError('Snapshot browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    selected=None
                    def state():
                        s=root();return next((v for v in s.get('windows',[]) if v['window']==selected),{}) if selected else s
                    def ui():return state().get('sessions_ui') or {}
                    def doc(t):
                        try:return json.loads(t.get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:root().get('presentation_frames',0)>0,'No snapshot window',timeout=30);original=root()['window'];w=original
                    def click(r):click_at(w,int(r[0]+r[2]/2),int(r[1]+r[3]/2))
                    def shot(filename):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);subprocess.run(['xdotool','windowfocus','--sync',str(w)],check=True);time.sleep(.4)
                        subprocess.run(['import','-window',str(w),str(out/filename)],check=True,timeout=10)
                    def manager():key_at(w,'e',12);wait_until(lambda:bool(ui()),'No snapshot manager')
                    def item():return ui()['items'][0]
                    def name(value):click(ui()['input']);key_at(w,'a',4);type_text(w,value);wait_until(lambda:ui()['name']==value,'Snapshot name input failed')
                    def save(value):name(value);click(ui()['save']);wait_until(lambda:len(ui()['items'])==1 and not ui()['name'],'Snapshot save failed: '+str(ui()))
                    def quit():
                        key_at(w,'q',4);end=time.monotonic()+30
                        while p.poll() is None and time.monotonic()<end:
                            for win in root().get('windows',[]):
                                if win.get('dialog_count'):key_at(win['window'],'Return')
                            time.sleep(.1)
                        p.wait(timeout=5);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    if not launch:
                        wait_until(lambda:len(root().get('windows',[]))==2 and all(all(t.get('loaded') and doc(t).get('token') for t in v['tabs']) for v in root()['windows']),'Seed windows not restored',timeout=35)
                        assert state()['workspace']==1 and state()['active_tab']==1 and state()['split_view']
                        pane=next(x['bounds'] for x in state()['split_view']['panes'] if x['url']==url+'A1')
                        click_at(w,pane[0]+80,pane[1]+98);type_text(w,'unsaved_token');click_at(w,pane[0]+80,pane[1]+148)
                        wait_until(lambda:any(doc(t).get('protected') and doc(t).get('value')=='unsaved_token' for t in state()['tabs']),'Source form not protected')
                        original_token=next(doc(t)['token'] for t in state()['tabs'] if doc(t).get('name')=='A1')
                        normal_ids={v['window'] for v in root()['windows']};key_at(w,'n',5);wait_until(lambda:len(root()['windows'])==3,'No private window')
                        private=next(v['window'] for v in root()['windows'] if v['window'] not in normal_ids);selected=private;w=private
                        key_at(w,'l',4);type_text(w,url+'PRIVATE_SESSION_SENTINEL');key_at(w,'Return');wait_until(lambda:any(doc(t).get('name')=='PRIVATE_SESSION_SENTINEL' for t in state()['tabs']),'Private fixture missing')
                        selected=None;w=original;manager();save('Research');assert item()['windows']==2 and item()['tabs']==4 and item()['available']
                        click(item()['rename']);wait_until(lambda:ui()['editing']>0,'No rename editor');name('Work session');click(ui()['save']);wait_until(lambda:item()['name']=='Work session' and not ui()['editing'],'Snapshot rename failed')
                        shot('en-sessions.png')
                        click(item()['restore']);wait_until(lambda:state()['dialog_count']>0,'No restore confirmation');key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Restore cancel failed');assert len(root()['windows'])==3
                        click(item()['restore']);wait_until(lambda:state()['dialog_count']>0,'No second restore confirmation');key_at(w,'Return')
                        wait_until(lambda:len(root()['windows'])==5 and all(all(t.get('loaded') and doc(t).get('token') for t in v['tabs']) for v in root()['windows']),'Snapshot windows did not load',timeout=40)
                        restored=[v for v in root()['windows'] if v['window'] not in normal_ids|{private}];assert sorted(len(v['tabs']) for v in restored)==[1,3]
                        primary=next(v for v in restored if len(v['tabs'])==3);assert primary['workspace']==1 and primary['active_tab']==1 and primary['split_view']
                        assert primary['tabs'][1]['pinned'] and primary['tabs'][1]['group']==1 and primary['tabs'][2]['container']==1
                        assert doc(primary['tabs'][1])['token']!=original_token and doc(primary['tabs'][1])['value']==''
                        old=next(doc(t) for t in state()['tabs'] if doc(t).get('name')=='A1');assert old['token']==original_token and old['value']=='unsaved_token' and old['protected']
                        key_at(w,'Escape');key_at(w,'n',12);key_at(w,'l',4);type_text(w,url+'PRIVATE_SESSION_SENTINEL_MIXED');key_at(w,'Return');wait_until(lambda:any(doc(t).get('name')=='PRIVATE_SESSION_SENTINEL_MIXED' for t in state()['tabs']),'No mixed private tab');manager()
                        click(item()['update']);wait_until(lambda:state()['dialog_count']>0,'No update confirmation');key_at(w,'Escape');wait_until(lambda:state()['dialog_count']==0,'Update cancel failed');assert item()['windows']==2
                        click(item()['update']);wait_until(lambda:state()['dialog_count']>0,'No update confirmation');key_at(w,'Return');wait_until(lambda:item()['windows']==4 and item()['tabs']==8,'Snapshot update failed')
                        click(item()['delete']);wait_until(lambda:not ui()['items'] and ui()['can_undo'],'Delete snapshot failed');click(ui()['undo']);wait_until(lambda:len(ui()['items'])==1 and item()['tabs']==8,'Undo snapshot failed')
                        key_at(w,'Escape');key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==3,'Mixed private tab did not close')
                        # Close every live tab of this identity; the saved references must remain blocked.
                        for win in list(root()['windows']):
                            if not any(t['container']==1 for t in win['tabs']):continue
                            selected=win['window'];w=selected;key_at(w,'2',4);wait_until(lambda:state()['tabs'][state()['active_tab']]['container']==1,'Container tab select failed');key_at(w,'w',4);wait_until(lambda:not any(t['container']==1 for t in state()['tabs']),'Container tab close failed')
                        selected=None;w=original;key_at(w,'c',12);wait_until(lambda:state().get('containers_ui') is not None,'No containers');entry=next(x for x in state()['containers_ui']['items'] if x['id']==1);assert not entry['in_use'];click(entry['remove']);wait_until(lambda:state()['dialog_count']>0,'No container removal confirmation');key_at(w,'Return');wait_until(lambda:len(state()['containers_ui']['items'])==1,'Container remove failed');key_at(w,'Escape');manager();assert not item()['available'];count=len(root()['windows']);click(item()['restore']);time.sleep(.3);assert not state()['dialog_count'] and len(root()['windows'])==count
                        key_at(w,'Escape');quit()
                    elif launch==1:
                        wait_until(lambda:len(root().get('windows',[]))==4,'Snapshot source windows not restored',timeout=35);manager();assert item()['name']=='Work session' and item()['windows']==4 and not item()['available']
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3);shot('ru-sessions-small.png')
                        key_at(w,'Escape');key_at(w,'c',12);wait_until(lambda:state().get('containers_ui') is not None,'No restarted containers');click(state()['containers_ui']['input']);type_text(w,'New account');key_at(w,'Return');wait_until(lambda:len(state()['containers_ui']['items'])==2,'New container failed');assert state()['containers_ui']['items'][1]['id']>1
                        key_at(w,'Escape');manager();assert not item()['available'];click(item()['update']);wait_until(lambda:state()['dialog_count']>0,'No RU update confirmation');key_at(w,'Return');wait_until(lambda:item()['available'] and item()['tabs']==6,'Explicit update did not replace missing identity references')
                        click(item()['restore']);wait_until(lambda:state()['dialog_count']>0,'No RU restore confirmation');shot('ru-restore-confirmation.png');key_at(w,'Escape');key_at(w,'Escape');quit()
                    else:
                        wait_until(lambda:state()['private_window'] and any(t['loaded'] for t in state()['tabs']),'Private-only launch failed',timeout=30);manager();assert item()['windows']==4 and item()['tabs']==6
                        name('Forbidden');click(ui()['save']);wait_until(lambda:ui()['error'],'Private-only save accepted');assert len(ui()['items'])==1
                        click(item()['update']);wait_until(lambda:state()['dialog_count']>0,'No private-only update prompt');key_at(w,'Return');wait_until(lambda:ui()['error'],'Private-only update accepted');assert item()['tabs']==6;key_at(w,'Escape');quit()
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_SESSION_SENTINEL' not in path.read_text()
                    saved=tomllib.loads((profile/'profile.toml').read_text());assert len(saved['named_sessions'])==1 and 'unsaved_token' not in json.dumps(saved['named_sessions'])
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: named snapshot save/rename/update/delete/undo, cancel and real new-window restore, active/pinned/group/workspace/container/split, original DOM/form kept, missing identity blocked and not reused, restart, RU small UI, private-only save/update rejected')

if __name__=='__main__':main()
