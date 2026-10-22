"""Real separate-process profiles: storage, singleton forwarding, catalog and restart."""
import json, os, re, shutil, signal, subprocess, threading, time, tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out=ROOT/'artifacts/profiles-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\nsearch_engine="yandex"\n')
    report=out/'parent.json';report.unlink(missing_ok=True)
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        url=f'http://127.0.0.1:{server.server_port}/container.html'
        with (out/'process.log').open('w') as log:
            p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={url}?name=Parent&set=parent'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
            try:
                def read(path):
                    try:return json.loads(path.read_text())
                    except (OSError,ValueError):return {}
                def state():
                    if p.poll() is not None:raise RuntimeError('Parent profile exited')
                    return read(report)
                def tab(s):
                    tabs=s.get('tabs',[]);return tabs[s.get('active_tab',0)] if tabs else {}
                def doc(s):
                    try:return json.loads(tab(s).get('title',''))
                    except ValueError:return {}
                def ready(s,value):
                    d=doc(s)
                    return tab(s).get('loaded') and d.get('idb')==value and d.get('storage')==value and ((f'aura_container={value}' in d.get('cookie','')) if value else 'aura_container=' not in d.get('cookie',''))
                wait_until(lambda:ready(state(),'parent'),'Parent storage failed',timeout=30)
                w=state()['window']
                def click(window,r):click_at(window,r[0]+r[2]//2,r[1]+r[3]//2)
                def manager(window,fn):
                    key_at(window,'Escape');key_at(window,'p',12)
                    wait_until(lambda:fn().get('profiles_ui') is not None,'No profile manager')
                def item(fn,id):return next(x for x in fn()['profiles_ui']['items'] if x['id']==id)
                def name(window,fn,text):
                    click(window,fn()['profiles_ui']['input']);key_at(window,'a',4);type_text(window,text)
                    wait_until(lambda:fn()['profiles_ui']['input_length']==len(text),'Name entry failed')
                def navigate(window,address):key_at(window,'l',4);type_text(window,address);key_at(window,'Return')
                key_at(w,'d',4);wait_until(lambda:tomllib.loads((profile/'profile.toml').read_text()).get('bookmarks'),'Parent bookmark missing')
                manager(w,state);name(w,state,'Work');click(w,state()['profiles_ui']['save'])
                wait_until(lambda:len(state()['profiles_ui']['items'])==2,'Create profile failed')
                child_dir=profile/'profiles/data/1';child_report=child_dir/'profile-window.json'
                child=lambda:read(child_report)
                wait_until(lambda:child().get('presentation_frames',0)>0,'Child profile did not launch',timeout=40)
                cw=child()['window'];assert child()['profile_name']=='Work' and child()['profile_directory']==str(child_dir)
                prefs=tomllib.loads((child_dir/'profile.toml').read_text())
                assert prefs['search_engine']=='duckduckgo' and not prefs.get('bookmarks') and not prefs.get('history')
                navigate(cw,url+'?name=ChildEmpty');wait_until(lambda:ready(child(),None),'Parent storage leaked into child')
                navigate(cw,url+'?name=Child&set=work');wait_until(lambda:ready(child(),'work'),'Child storage failed')
                key_at(cw,'d',4);wait_until(lambda:tomllib.loads((child_dir/'profile.toml').read_text()).get('bookmarks'),'Child bookmark missing')
                assert ready(state(),'parent') and len(state()['windows'])==1
                click(w,item(state,1)['rename']);name(w,state,'Team');key_at(w,'Return')
                wait_until(lambda:item(state,1)['name']=='Team','Rename failed')
                manager(cw,child);wait_until(lambda:item(child,1)['name']=='Team','Shared catalog did not refresh');key_at(cw,'Escape')
                name(w,state,'Team');key_at(w,'Return');wait_until(lambda:state()['profiles_ui']['error'],'Duplicate accepted')
                manager(w,state)
                subprocess.run(['import','-window',str(w),str(out/'en-profiles.png')],check=True,timeout=10)
                click(w,item(state,1)['open']);wait_until(lambda:len(child().get('windows',[]))==2,'Existing profile was not forwarded to its process',timeout=35)
                extra=next(x['window'] for x in child()['windows'] if x['window']!=cw)
                key_at(extra,'w',5);wait_until(lambda:len(child()['windows'])==1,'Extra profile window close failed')
                click(w,item(state,1)['archive']);wait_until(lambda:len(state()['profiles_ui']['items'])==1,'Archive failed')
                assert (child_dir/'profile.toml').exists() and ready(child(),'work')
                click(w,state()['profiles_ui']['toggle']);wait_until(lambda:len(state()['profiles_ui']['items'])==1 and state()['profiles_ui']['archived'],'Archived view failed')
                click(w,item(state,1)['archive']);wait_until(lambda:len(state()['profiles_ui']['items'])==0,'Restore failed')
                click(w,state()['profiles_ui']['toggle']);wait_until(lambda:len(state()['profiles_ui']['items'])==2,'Restored profile missing')
                key_at(cw,'q',4);wait_until(lambda:read(child_report).get('cef_created_total')==read(child_report).get('cef_closed_total'),'Child shutdown did not finish')
                time.sleep(.5);child_report.unlink(missing_ok=True)
                path=child_dir/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
                click(w,item(state,1)['open']);wait_until(lambda:child().get('presentation_frames',0)>0 and ready(child(),'work'),'Child restore lost storage/session',timeout=40)
                cw=child()['window'];assert child()['profile_name']=='Team'
                manager(cw,child)
                subprocess.run(['xdotool','windowsize',str(cw),'900','620'],check=True);time.sleep(.4)
                subprocess.run(['import','-window',str(cw),str(out/'ru-profiles-small.png')],check=True,timeout=10)
                assert ready(state(),'parent')
                parent_prefs=tomllib.loads((profile/'profile.toml').read_text());child_prefs=tomllib.loads(path.read_text())
                assert parent_prefs['language']=='en' and parent_prefs['search_engine']=='yandex'
                assert child_prefs['language']=='ru' and child_prefs['search_engine']=='duckduckgo'
                assert all('name=Child' not in b['url'] for b in parent_prefs['bookmarks'])
                assert all('name=Parent' not in b['url'] for b in child_prefs['bookmarks'])
                key_at(cw,'Escape');key_at(cw,'q',4)
                wait_until(lambda:read(child_report).get('cef_created_total')==read(child_report).get('cef_closed_total'),'Restored child did not shut down')
                key_at(w,'Escape');key_at(w,'q',4);p.wait(timeout=30)
                final=read(report);assert p.returncode==0 and final['cef_created_total']==final['cef_closed_total']
            finally:
                # The launched profile processes inherit this disposable process group.
                try:os.killpg(p.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                if p.poll() is None:p.wait(timeout=10)
        server.shutdown()
    print('PASS: independent process/cookies/localStorage/IndexedDB/history/bookmarks/settings, singleton reopening, rename/archive/restore/restart, RU/EN')


if __name__=='__main__':main()
