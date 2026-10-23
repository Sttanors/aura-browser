"""Native bookmark folders, Unicode search, editing, bulk/undo and actual HTML Save dialog."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/bookmarks-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();url=f'http://127.0.0.1:{server.server_port}/translation.html'
        seed='language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n'
        for i in range(20):seed+='\n[[bookmarks]]\nurl='+json.dumps(url+'?bookmark='+str(i))+'\ntitle='+json.dumps('Русская статья' if not i else f'Record {i}',ensure_ascii=False)+'\n'
        (profile/'profile.toml').write_text(seed)
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}']+([f'--url={url}'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Bookmarks browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No bookmarks window',timeout=30);w=state()['window']
                    def ui():return state().get('bookmarks_ui') or {}
                    def click(r):click_at(w,int(r[0]+r[2]/2),int(r[1]+r[3]/2))
                    def manager():key_at(w,'b',4);wait_until(lambda:bool(ui()),'No bookmark manager')
                    def paste(value):
                        if not value:key_at(w,'a',4);key_at(w,'BackSpace');return
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-quiet','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                        try:
                            clip.stdin.write(value.encode());clip.stdin.close();time.sleep(.15);key_at(w,'a',4);key_at(w,'v',4);time.sleep(.25)
                        finally:
                            if clip.poll() is None:clip.terminate();clip.wait(timeout=3)
                    def query(value):click(ui()['search']);paste(value);wait_until(lambda:ui()['query']==value,'Bookmark search paste failed')
                    def field(name,value):click(ui()[name+'_input']);paste(value);wait_until(lambda:ui()['draft'][name]==value,'Bookmark field paste failed: '+name)
                    def save():click(ui()['save']);wait_until(lambda:ui()['mode']==0,'Bookmark save failed: '+str(ui()))
                    def action(id):click(next(x['bounds'] for x in ui()['actions'] if x['id']==id))
                    def folder(name):
                        query(name);wait_until(lambda:len(ui()['folders'])==1,'Folder search failed');key_at(w,'Return');wait_until(lambda:ui()['mode']!=5,'Folder select failed')
                    def picker():
                        pid=ui().get('picker_pid',-1)
                        if pid<0:return None
                        r=subprocess.run(['xdotool','search','--onlyvisible','--pid',str(pid)],capture_output=True,text=True,timeout=5)
                        return r.stdout.strip().splitlines()[-1] if r.returncode==0 and r.stdout.strip() else None
                    manager()
                    if not launch:
                        query('РУССКАЯ');wait_until(lambda:len(ui()['items'])==1,'Cyrillic case search failed');click(ui()['items'][0]['edit']);wait_until(lambda:ui()['mode']==1,'No bookmark editor')
                        field('title','Русская статья <A & B>');field('tags','Research, Работа');field('url','javascript:alert(1)');click(ui()['save']);wait_until(lambda:ui()['error'],'Invalid URL accepted')
                        field('url',url+'?bookmark=ru&x=1');save();assert ui()['items'][0]['title']=='Русская статья <A & B>'
                        query('');click(ui()['new_folder']);wait_until(lambda:ui()['mode']==2,'No folder editor');field('name','Work');save();work=ui()['filter'];assert work>0 and not ui()['items']
                        click(ui()['folder']);wait_until(lambda:ui()['mode']==5,'No folder picker');key_at(w,'Down');key_at(w,'Down');wait_until(lambda:ui()['folder_selected']==2,'Folder keyboard selection failed');key_at(w,'Return');wait_until(lambda:ui()['mode']==0 and ui()['filter']==work,'Folder keyboard activate failed')
                        click(ui()['folder']);wait_until(lambda:ui()['mode']==5,'No all folders picker');folder('All folders');query('record');action('select_all');wait_until(lambda:ui()['selected']==19,'Select all missed hidden rows')
                        action('move');wait_until(lambda:ui()['mode']==5,'No move picker');folder('Work');assert all(x['folder']==work for x in ui()['items'])
                        action('select_all');action('tags');wait_until(lambda:ui()['mode']==4,'No bulk tags editor');field('tags','shared, TEST');save()
                        action('select_all');action('delete');wait_until(lambda:state()['bookmark_count']==1 and ui()['undo'],'Bulk deletion failed');action('undo');wait_until(lambda:state()['bookmark_count']==20,'Undo lost bookmarks')
                        query('');click(ui()['folder']);wait_until(lambda:ui()['mode']==5,'No folder picker');folder('Work');click(ui()['rename']);wait_until(lambda:ui()['mode']==3,'No folder rename');field('name','Projects');save();assert ui()['filter']==work
                        click(ui()['new_folder']);wait_until(lambda:ui()['mode']==2,'No second folder editor');field('name','Temporary');save();click(ui()['remove_folder']);wait_until(lambda:ui()['filter']==0,'Folder remove failed');assert state()['bookmark_count']==20
                        click(ui()['folder']);wait_until(lambda:ui()['mode']==5,'No all picker');folder('All folders');subprocess.run(['import','-window',str(w),str(out/'en-bookmarks.png')],check=True,timeout=10)
                        # Cancelling Save must not write a file or discard metadata.
                        click(ui()['export']);wait_until(lambda:picker(),'No native HTML save chooser');chooser=picker();subprocess.run(['xdotool','windowfocus','--sync',chooser],check=True);subprocess.run(['xdotool','key','--clearmodifiers','Escape'],check=True);wait_until(lambda:ui()['picker_pid']<0,'Save cancel failed');assert ui()['last_export']==''
                        target=out/'Закладки.html';target.unlink(missing_ok=True);click(ui()['export']);wait_until(lambda:picker(),'No second save chooser');chooser=picker();subprocess.run(['xdotool','windowfocus','--sync',chooser],check=True);subprocess.run(['xdotool','key','--clearmodifiers','ctrl+l'],check=True);time.sleep(.2)
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(str(target).encode(),timeout=5)
                        subprocess.run(['xdotool','key','--clearmodifiers','ctrl+a','ctrl+v'],check=True);time.sleep(.3);subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True);time.sleep(1)
                        if ui()['picker_pid']>0:subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True)
                        wait_until(lambda:ui()['last_export']==str(target) and target.exists(),'Native HTML export failed',timeout=20)
                        html=target.read_text();assert html.count('<DT><A ')==20 and '<H3>Projects</H3>' in html and 'TAGS="shared,TEST"' in html and '&lt;A &amp; B&gt;' in html and '<A & B>' not in html
                    else:
                        assert state()['bookmark_count']==20;subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3)
                        query('shared');wait_until(lambda:len(ui()['items'])>0,'Tags were not restored');assert all(x['folder']>0 for x in ui()['items'])
                        subprocess.run(['import','-window',str(w),str(out/'ru-bookmarks-small.png')],check=True,timeout=10)
                        query('no-such-bookmark');wait_until(lambda:not ui()['items'],'No empty search state');query('');key_at(w,'Escape');key_at(w,'n',12)
                        key_at(w,'l',4);type_text(w,url+'?PRIVATE_BOOKMARK_SENTINEL');key_at(w,'Return');wait_until(lambda:any(t['private'] and t['loaded'] for t in state()['tabs']),'No private fixture');manager();assert state()['bookmark_count']==20;key_at(w,'Escape');key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Private close failed')
                    key_at(w,'Escape');key_at(w,'q',4);p.wait(timeout=30);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    saved=tomllib.loads((profile/'profile.toml').read_text());assert len(saved['bookmarks'])==20 and len(saved['bookmark_folders'])==1
                    assert len([b for b in saved['bookmarks'] if b.get('tags')==['shared','TEST']])==19
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_BOOKMARK_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: Unicode/URL/tag search, validation, folder create/rename/remove, bulk selection/move/tag/delete/undo, native Save cancel and UTF-8 HTML export, escaped markup, restart and RU small UI, no implicit private bookmarks')

if __name__=='__main__':main()
