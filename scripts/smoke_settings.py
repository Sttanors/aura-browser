"""Exercise the native settings catalog and verify its effects in real CEF pages."""
from __future__ import annotations
import json
import os
import shutil
import signal
import subprocess
import threading
import time
import tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import type_text, wait_until
from smoke_window import click_at, key_at


class Handler(QuietHandler):
    probes = []
    requests = []
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        if self.path.startswith('/browser/preferences.html'):
            self.send_header('Set-Cookie', 'aura_settings=1; SameSite=Lax; Path=/')
        super().end_headers()
    def do_GET(self):
        self.requests.append({"path":self.path,"headers":dict(self.headers)})
        parsed = urlparse(self.path)
        if parsed.path == '/browser/probe':
            data = json.loads(parse_qs(parsed.query)['data'][0])
            self.probes.append({'page':data, 'headers':dict(self.headers)})
            self.send_response(204); self.end_headers()
        else:
            super().do_GET()


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Use python3 scripts/verify_browser.py --suite=settings; no desktop test was started')
    output = ROOT/'artifacts/settings-smoke'
    output.mkdir(parents=True, exist_ok=True)
    profile = output/'profile'
    if profile.exists(): shutil.rmtree(profile)
    profile.mkdir();(profile/'profile.toml').write_text('filter_updates=false\n')
    downloads = output/'custom-downloads'
    if downloads.exists(): shutil.rmtree(downloads)
    server = ThreadingHTTPServer(('127.0.0.1',0), partial(Handler,directory=str(ROOT/'tests/fixtures')))
    worker = threading.Thread(target=server.serve_forever,daemon=True); worker.start()
    base = f'http://127.0.0.1:{server.server_port}/browser/'
    reports=[]; persisted=None; validated=[]
    try:
        for launch in range(2):
            report_path=output/f'report-{launch}.json'; report_path.unlink(missing_ok=True)
            request_start = len(Handler.requests)
            with (output/f'process-{launch}.log').open('w') as log:
                process=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report_path}'],
                    cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,start_new_session=True,
                    env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'})
                window=None; width,height=1360,900
                try:
                    def state():
                        if process.poll() is not None: raise RuntimeError(f'Browser exited {process.returncode}; see {output}')
                        return json.loads(report_path.read_text()) if report_path.exists() else {}
                    wait_until(lambda:state().get('presentation_frames',0)>0 and state().get('tabs'),'No settings test window')
                    window=str(state()['window'])
                    def title():return subprocess.check_output(['xdotool','getwindowname',window],text=True,timeout=5)
                    def expect_title(text):wait_until(lambda:text in title(),f'Expected title {text!r}; report {report_path}')
                    def expect(id,value):wait_until(lambda:state()['settings'].get(id)==value,f'{id} did not become {value!r}')
                    def click(rect):x,y,w,h=rect;click_at(window,int(x+w/2),int(y+h/2))
                    def navigate(url):key_at(window,'l',4);type_text(window,url);key_at(window,'Return')
                    def screenshot(name):
                        time.sleep(.2);subprocess.run(['import','-window',window,str(output/name)],check=True,timeout=10)
                    def open_settings():
                        time.sleep(.3) # Let the 250 ms state report catch up with prior native input.
                        if state().get('panel')!=4:click_at(window,width-53,55)
                        wait_until(lambda:state().get('settings_ui') is not None,'Settings did not open')
                    def seek(id):
                        open_settings();key_at(window,'f',4);type_text(window,id);key_at(window,'Return')
                        wait_until(lambda:(state().get('settings_ui') or {}).get('search')==id and any(c['id']==id for c in (state().get('settings_ui') or {}).get('controls',[])),f'No setting control {id}')
                        return next(c for c in state()['settings_ui']['controls'] if c['id']==id)
                    def toggle(id,value):
                        if state()['settings'][id]==value:return
                        c=seek(id);x,y,w,h=c['bounds'];click_at(window,x+w-22,y+26);expect(id,value)
                    def choose(id,value):
                        c=seek(id);click(next(o['bounds'] for o in c['options'] if o['value']==str(value)));expect(id,value)
                    def number(id,value):
                        c=seek(id);x,y,w,h=c['bounds']
                        while state()['settings'][id]!=value:
                            before=state()['settings'][id]
                            click_at(window,x+w-(18 if before<value else 122),y+32)
                            expect(id,before+(1 if before<value else -1))
                    def text(id,value):
                        c=seek(id);x,y,w,h=c['bounds'];click_at(window,x+min(100,w//3),y+82)
                        key_at(window,'a',4)
                        if any(ord(c)>127 for c in value):
                            # Exercise real UTF-8 paste without changing any keyboard map.
                            clipboard=subprocess.Popen(['xclip','-selection','clipboard','-in','-quiet','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                            try:
                                clipboard.stdin.write(value.encode());clipboard.stdin.close();time.sleep(.12)
                                key_at(window,'v',4);clipboard.wait(timeout=5);time.sleep(.15)
                            finally:
                                if clipboard.poll() is None:clipboard.terminate();clipboard.wait()
                        else:type_text(window,value)
                        key_at(window,'Return');expect(id,value)
                    def category(name):
                        open_settings();click(next(c['bounds'] for c in state()['settings_ui']['categories'] if c['name']==name));time.sleep(.2)
                    def action(id,query):
                        open_settings();key_at(window,'f',4);type_text(window,query);key_at(window,'Return')
                        wait_until(lambda:state()['settings_ui']['search']==query and any(c['id']==id for c in state()['settings_ui']['controls']),f'No action {id}')
                        c=next(c for c in state()['settings_ui']['controls'] if c['id']==id)
                        x,y,w,h=c['bounds'];click_at(window,x+min(150,w//2),y+82)
                        wait_until(lambda:state()['dialog_count']>0,f'No confirmation for {id}')
                        key_at(window,'Return')
                    time.sleep(.3)
                    if launch==0:
                        navigate(base+'a.html');expect_title('Alpha');key_at(window,'d',4)
                        key_at(window,'t',4);expect_title('New tab');open_settings();screenshot('general-en.png')
                        # Native settings can be operated without a pointing device.
                        seek('restore');before=state()['settings']['restore']
                        key_at(window,'F10')
                        # Close, Search engines and search input precede the category list.
                        for _ in range(len(state()['settings_ui']['categories'])+3):key_at(window,'Tab')
                        screenshot('keyboard-focus.png')
                        key_at(window,'space');expect('restore',not before)
                        key_at(window,'space');expect('restore',before)
                        key_at(window,'Tab',1);key_at(window,'Return')
                        wait_until(lambda:state()['settings_ui']['search']=='','Keyboard category activation failed')
                        validated.append('native Tab / Shift+Tab focus and Space / Enter activation')
                        choose('language','ru');expect_title('Новая вкладка')
                        choose('search_engine','yandex');choose('default_zoom',125)
                        wait_until(lambda:all(t['zoom_percent']==125 for t in state()['tabs']),'Open CEF pages did not zoom')
                        toggle('restore',False)
                        previous_home=state()['settings']['home_page']
                        c=seek('home_page');x,y,w,h=c['bounds'];click_at(window,x+80,y+82)
                        key_at(window,'a',4);type_text(window,'invalid-address');key_at(window,'Return')
                        expect('home_page',previous_home)
                        wait_until(lambda:state()['settings_ui'].get('validation_error')=='home_page','No inline validation error')
                        screenshot('invalid-home-page.png')
                        text('home_page',base+'preferences.html')
                        wait_until(lambda:not state()['settings_ui'].get('validation_error'),'Validation error did not clear')
                        validated.append('invalid home address preserves value and shows inline error')
                        choose('startup_page','home');choose('site_language','ru-RU,ru,en-US,en')
                        for id,value in {'foreground_links':False,'tabs_at_end':False,'confirm_quit':True,
                            'middle_close':False,'wheel_switch':True,'mute_new_tabs':True,
                            'quick_links':False,'show_tab_host':False,'reduce_motion':True,
                            'show_favicons':False,'show_status':False,'ntp_clock':True,'clock_24':False,
                            'ntp_greeting':False}.items():toggle(id,value)
                        c=seek('theme');x,y,w,h=c['bounds'];tw=(w-16)//3
                        click_at(window,x+tw//2,y+66+84+37);expect('theme',3)
                        category('Appearance');screenshot('appearance-ru.png')
                        category('General');screenshot('general-ru.png')
                        number('default_font_size',20);number('minimum_font_size',8)
                        choose('font_family','mono')
                        for id in ('javascript','images','remote_fonts','local_storage','resize_textareas','tab_to_links','image_fit'):toggle(id,False)
                        key_at(window,'Escape');key_at(window,'t',4);navigate(base+'preferences.html');expect_title('Scripts disabled')
                        wait_until(lambda:state()['tabs'][state().get('active',len(state()['tabs'])-1)]['loaded'] if 'active' in state() else any(t['loaded'] and t['title']=='Scripts disabled' for t in state()['tabs']),'Script-disabled page did not load')
                        toggle('javascript',True)
                        for id,value in {'record_history':False,'suggest_history':False,'suggest_bookmarks':False,
                            'address_suggestions':False,'clear_history_exit':True,'clear_cookies_exit':True,
                            'do_not_track':True,'send_referrer':False,'block_popups':True,'allow_dialogs':False}.items():toggle(id,value)
                        choose('history_limit',100);choose('history_days',7);number('suggestion_limit',3)
                        choose('notifications','ask');choose('geolocation','ask')
                        category('Permissions');screenshot('permissions-ru.png')
                        key_at(window,'Escape');key_at(window,'t',4)
                        prior=len(Handler.probes);navigate(base+'preferences.html?popup');expect_title('Scripts enabled')
                        wait_until(lambda:len(Handler.probes)>prior,'No live web preferences probe')
                        probe=Handler.probes[-1];page=probe['page'];headers={k.lower():v for k,v in probe['headers'].items()}
                        assert page['font']=='20px' and page['storage'] is False and page['image']==0 and page['tiny']>=8,probe
                        assert headers.get('dnt')=='1' and not headers.get('referer'),probe
                        assert len(state()['tabs'])==4,state() # No unsolicited popup tab.
                        # The probe page is unstyled: controls remain near the top at zoom 125%.
                        key_at(window,'n')
                        wait_until(lambda:state()['dialog_count']>0,'No native permission prompt')
                        screenshot('permission-prompt.png');key_at(window,'Escape');expect_title('Notification denied')
                        key_at(window,'a');expect_title('Alert returned')
                        wait_until(lambda:state()['audit'].get('dialogs_blocked',0)>0,'Dialog suppression was not recorded')
                        assert state()['dialog_count']==0,state()
                        print('Settings phase: content, headers and permission prompt passed',flush=True)
                        validated.extend(['CEF JavaScript off/on','CEF font/minimum font/images/local storage','DNT/referrer','popup blocking','permission prompt','dialog suppression'])
                        text('download_directory',str(downloads))
                        for id,value in {'ask_download_name':True,'date_downloads':True,'open_downloads':True,'notify_downloads':True,'close_downloads':True}.items():toggle(id,value)
                        key_at(window,'Escape');navigate(base+'sample.bin')
                        wait_until(lambda:state()['dialog_count']>0,'No filename dialog')
                        key_at(window,'a',4);type_text(window,'chosen.bin');key_at(window,'Return')
                        wait_until(lambda:any(d['complete'] for d in state().get('downloads',[])),'Download did not finish',timeout=35)
                        payload=(ROOT/'tests/fixtures/browser/sample.bin').read_bytes()
                        files=list(downloads.glob('*_chosen.bin'));assert len(files)==1 and files[0].read_bytes()==payload,files
                        assert state()['panel']==0,state()
                        validated.append('custom download path/name/date and auto-close')
                        action('cache','Clear cached files');wait_until(lambda:state()['audit'].get('cleared_cache',0)>0,'Cache callback incomplete')
                        action('cookies','Clear cookies');wait_until(lambda:state()['audit'].get('cleared_cookies',0)>0,'Cookie callback incomplete')
                        key_at(window,'Escape');navigate(base+'cookie-check.html');expect_title('Cookie absent')
                        action('history','Clear Aura history');wait_until(lambda:state()['history_count']==0,'History clear failed');assert state()['bookmark_count']==1
                        category('Privacy');screenshot('privacy-ru.png')
                        subprocess.run(['xdotool','windowsize','--sync',window,'900','620'],check=True,timeout=5)
                        width,height=900,620;time.sleep(.3);category('Privacy');key_at(window,'End');screenshot('settings-small-ru.png')
                        # Search remains available even at the bottom of a long category.
                        toggle('send_referrer',True);toggle('send_referrer',False)
                        choose('new_tab_page','newtab');key_at(window,'Escape');key_at(window,'t',4);expect_title('Новая вкладка')
                        screenshot('new-tab-small-ru.png')
                        key_at(window,'F7');screenshot('new-tab-small-dark-ru.png');key_at(window,'F7')
                        key_at(window,'w',4)
                        validated.append('new tab at 900 x 620 in light and dark themes')
                        choose('new_tab_page','home');key_at(window,'Escape');key_at(window,'t',4);expect_title('Scripts enabled')
                        choose('new_tab_page','blank');key_at(window,'Escape');key_at(window,'t',4)
                        wait_until(lambda:any(t['url']=='about:blank' for t in state()['tabs']),'Blank new-tab preference ignored')
                        toggle('record_history',True);key_at(window,'Escape')
                        key_at(window,'Home',8);expect_title('Scripts enabled')
                        wait_until(lambda:state()['history_count']>0,'No history seeded for exit cleanup')
                        toggle('record_history',False);key_at(window,'Escape')
                        persisted=state()['settings'].copy()
                        print('Settings phase: downloads, data actions and small window passed',flush=True)
                    else:
                        expect_title('Scripts enabled')
                        assert len(state()['tabs'])==1,state() # Startup homepage, no session restore.
                        assert state()['settings']==persisted,(state()['settings'],persisted)
                        wait_until(lambda:bool(Handler.probes) and Handler.probes[-1]['page']['languages'][0].startswith('ru'),'Website language did not persist')
                        key_at(window,'l',4);type_text(window,base+'cookie-check.html');key_at(window,'Return');expect_title('Cookie present')
                        # Home page set a new cookie on this launch. Clear it and verify exit cleanup via the first request headers.
                        initial_requests=[p for p in Handler.requests[request_start:] if p['path'].startswith('/browser/preferences.html')]
                        assert initial_requests and not initial_requests[0]['headers'].get('Cookie'),initial_requests
                        choose('language','en');category('General');screenshot('reloaded-en.png');choose('language','ru')
                        action('reset','Reset settings');expect('language','en');expect('default_font_size',16);expect('restore',True)
                        assert state()['bookmark_count']==1,state()
                        validated.extend(['all configured preferences survived restart','startup home page and site language','cookies/history cleared on normal exit','reset keeps bookmarks'])
                    click_at(window,width-28,15)
                    time.sleep(.3)
                    if process.poll() is None and report_path.exists() and json.loads(report_path.read_text()).get('dialog_count',0):key_at(window,'Return')
                    process.wait(timeout=20);assert process.returncode==0,process.returncode
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid,signal.SIGTERM)
                        try:process.wait(timeout=3)
                        except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
            report=json.loads(report_path.read_text());reports.append(report)
            assert report['cef_created']==report['cef_closed'],report
            assert report['audit'].get('renderer_crashes',0)==0 and report['compositor']=='Skia raster / X11',report
        result={'passed':True,'preference_count':len(persisted),'persisted_settings':persisted,'verified':validated,'probes':Handler.probes,'launches':reports}
        (output/'verification.json').write_text(json.dumps(result,indent=2,ensure_ascii=False))
        print(json.dumps(result,indent=2,ensure_ascii=False))
    finally:server.shutdown();worker.join();server.server_close()


if __name__=='__main__':main()
