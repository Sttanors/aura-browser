"""Explicit translation controls and provider routing, with all external traffic blocked."""
import json,os,re,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from urllib.parse import urlsplit,parse_qs
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at,drag_between


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/translation-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        url=f'http://127.0.0.1:{server.server_port}/translation.html'
        for launch in range(2):
            if launch:
                path=profile/'profile.toml';path.write_text(re.sub(r'(?m)^language\s*=.*$','language="ru"',path.read_text()))
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                # Chromium bypasses its proxy for loopback fixtures; external providers cannot connect.
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}','--proxy-server=http://127.0.0.1:9']+([f'--url={url}'] if not launch else []),stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def state():
                        if p.poll() is not None:raise RuntimeError('Translation browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def current():
                        s=state();ts=s.get('tabs',[]);return ts[s.get('active_tab',0)] if ts else {}
                    def doc():
                        try:return json.loads(current().get('title',''))
                        except ValueError:return {}
                    wait_until(lambda:current().get('loaded') and doc().get('token'),'No translation fixture',timeout=30);w=state()['window']
                    def click(r):click_at(w,r[0]+r[2]//2,r[1]+r[3]//2)
                    def option(group,value):return next(o['bounds'] for o in state()['translation_ui']['options'] if o['group']==group and o['value']==value)
                    def manager():key_at(w,'t',12);wait_until(lambda:state().get('translation_ui') is not None,'No translation panel')
                    def select():
                        r=doc()['text'];drag_between(w,252+r[0]+1,88+r[1]+16,252+r[0]+600,88+r[1]+16)
                        wait_until(lambda:current().get('selection_bytes',0)>0,'CEF selection not captured')
                    def navigate(address):key_at(w,'l',4);type_text(w,address);key_at(w,'Return');wait_until(lambda:current().get('url')==address and current().get('loaded'),'Translation fixture navigation failed')
                    def requests():return state()['audit'].get('translation_requests',0)
                    def close_provider(count):key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==count,'Provider tab close failed')
                    def context(label):
                        r=doc()['text'];click_at(w,252+r[0]+80,88+r[1]+16,button=3)
                        wait_until(lambda:state().get('menu_ui') is not None,'No native context menu')
                        click(next(x['bounds'] for x in state()['menu_ui'] if x['label']==label))
                    if launch==0:
                        token=doc()['token'];created=state()['cef_created'];manager()
                        click(option(1,'ru'));click(option(0,'bing'));wait_until(lambda:state()['translation_ui']['problem'],'Unsupported page/provider was silently changed')
                        click(state()['translation_ui']['send']);time.sleep(.3);assert requests()==0 and state()['cef_created']==created
                        click(option(2,'text'));assert state()['translation_ui']['text_bytes']==0
                        click(state()['translation_ui']['cancel']);assert requests()==0
                        select();manager();wait_until(lambda:state()['translation_ui']['selection'] and state()['translation_ui']['text_bytes']>0,'Selection snapshot absent')
                        click(option(0,'google'));click(option(1,'ru'))
                        wait_until(lambda:not state()['translation_ui']['problem'],'Valid selection disabled')
                        wait_until(lambda:state()['settings']['translation_provider']=='google' and state()['settings']['translation_language']=='ru','Selection options not applied');time.sleep(.2)
                        subprocess.run(['import','-window',str(w),str(out/'en-translation.png')],check=True,timeout=10)
                        assert requests()==0 and state()['cef_created']==created
                        click(state()['translation_ui']['send']);wait_until(lambda:requests()==1 and len(state()['tabs'])==2 and urlsplit(current()['url']).hostname=='translate.google.com','Google selection route failed')
                        query=parse_qs(urlsplit(current()['url']).query);assert query['tl']==['ru'] and query['text'][0].startswith('Hello') and 'привет' in query['text'][0]
                        wait_until(lambda:current()['failed'],'Offline provider did not fail')
                        failed_url=current()['url'];errors=state()['audit']['load_errors'];key_at(w,'r',4)
                        wait_until(lambda:state()['audit']['load_errors']>errors and current()['failed'],'Failed navigation retry did not retry destination')
                        assert current()['url']==failed_url
                        close_provider(1);assert doc()['token']==token
                        # The context-menu action must capture this selection and source tab.
                        context('Translate selection');wait_until(lambda:state().get('translation_ui') is not None,'Context translate selection failed')
                        click(option(0,'bing'));click(option(1,'de'));click(state()['translation_ui']['send'])
                        wait_until(lambda:requests()==2 and len(state()['tabs'])==2 and urlsplit(current()['url']).hostname=='www.bing.com','Microsoft Translator route failed')
                        assert parse_qs(urlsplit(current()['url']).query)['to']==['de'] and parse_qs(urlsplit(current()['url']).query)['text'][0].startswith('Hello')
                        close_provider(1)
                        manager();click(option(0,'google'));click(option(2,'page'));key_at(w,'r',4)
                        wait_until(lambda:state()['translation_ui']['problem'] and 'changed' in state()['translation_ui']['problem'],'Stale source was not rejected')
                        click(state()['translation_ui']['send']);assert requests()==2;key_at(w,'Escape')
                        manager();click(option(0,'google'));click(option(2,'page'));source=current()['url'];click(state()['translation_ui']['send'])
                        wait_until(lambda:requests()==3 and len(state()['tabs'])==2 and urlsplit(current()['url']).hostname=='translate.google.com','Website route failed')
                        query=parse_qs(urlsplit(current()['url']).query);assert query['u']==[source]
                        close_provider(1)
                        navigate(url+'?long=1');context('Select all');wait_until(lambda:current()['selection_bytes']>5000,'Long selection not captured')
                        manager();click(option(2,'text'));wait_until(lambda:'shorter' in state()['translation_ui']['problem'],'Long selection silently truncated')
                        click(state()['translation_ui']['send']);assert requests()==3;key_at(w,'Escape');navigate(url)
                        key_at(w,'c',12);wait_until(lambda:state().get('containers_ui') is not None,'No container manager')
                        click(state()['containers_ui']['input']);type_text(w,'Translator');key_at(w,'Return')
                        wait_until(lambda:len(state()['containers_ui']['items'])==2,'Translation container create failed')
                        container=state()['containers_ui']['items'][1]['id'];click(state()['containers_ui']['items'][1]['new_tab'])
                        navigate(url);select();manager();click(option(0,'google'));click(state()['translation_ui']['send'])
                        wait_until(lambda:requests()==4 and len(state()['tabs'])==3 and current()['container']==container,'Translation lost source container')
                        close_provider(2);key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1,'Container fixture close failed')
                    else:
                        assert state()['settings']['translation_provider']=='google' and state()['settings']['translation_language']=='de'
                        subprocess.run(['xdotool','windowsize',str(w),'900','620'],check=True);time.sleep(.3)
                        manager();click(option(0,'google'));click(option(1,'interface'));click(option(2,'page'))
                        wait_until(lambda:state()['settings']['translation_provider']=='google' and state()['settings']['translation_language']=='interface','RU options not applied');time.sleep(.2)
                        subprocess.run(['import','-window',str(w),str(out/'ru-translation-small.png')],check=True,timeout=10)
                        key_at(w,'Escape');key_at(w,'n',12);navigate(url+'?private=1');select();manager();click(option(0,'bing'));click(option(2,'text'));click(state()['translation_ui']['send'])
                        wait_until(lambda:requests()==1 and current()['private'] and urlsplit(current()['url']).hostname=='www.bing.com','Translation lost private state')
                        assert parse_qs(urlsplit(current()['url']).query)['to']==['ru'] and parse_qs(urlsplit(current()['url']).query)['text'][0].startswith('PRIVATE_TRANSLATION_SENTINEL')
                        close_provider(2);key_at(w,'w',4);wait_until(lambda:len(state()['tabs'])==1 and not current()['private'],'Private fixture close failed')
                    key_at(w,'q',4);p.wait(timeout=30);s=json.loads(report.read_text());assert p.returncode==0 and s['cef_created_total']==s['cef_closed_total']
                    for path in profile.glob('profile.toml*'):assert 'PRIVATE_TRANSLATION_SENTINEL' not in path.read_text() and 'private=1' not in path.read_text()
                finally:
                    if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
        server.shutdown()
    print('PASS: no sends before command/cancel, Google text/page and Microsoft Translator text routing, UTF-8 encoding, source/selection/context menu, stale/empty/long errors, private/container inheritance, persisted language/provider, RU small UI; provider traffic blocked')


if __name__=='__main__':main()
