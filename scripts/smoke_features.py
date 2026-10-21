"""Real CEF protection, signed extension installation and restart lifecycle."""
import json,os,subprocess,threading,time,signal,shutil
from functools import partial
from http.server import ThreadingHTTPServer
from urllib.parse import urlparse,parse_qs
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at
class Handler(QuietHandler):
    probes=[];requests=[]
    def end_headers(self):self.send_header('Cache-Control','no-store');super().end_headers()
    def do_GET(self):
        path=urlparse(self.path);self.requests.append(path.path)
        if path.path=='/features-probe':
            self.probes.append(json.loads(parse_qs(path.query)['data'][0]));self.send_response(204);self.end_headers()
        elif path.path in ['/ads/cbr.js','/extension-blocked.js']:
            self.send_response(200);self.send_header('Content-Type','application/javascript');self.end_headers();self.wfile.write(b'window.auraAdNetwork=true;' if path.path=='/ads/cbr.js' else b'window.auraExtensionNetwork=true;')
        else:super().do_GET()

def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=features')
    out=ROOT/'artifacts/features-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile'
    if profile.exists():shutil.rmtree(profile)
    profile.mkdir();(profile/'profile.toml').write_text('language="ru"\nfilter_updates=false\ndarkreader_mode="on"\nrestore=false\ndefault_font_size=20\nminimum_font_size=8\nfont_family="mono"\n',encoding='utf-8')
    results=[]
    with ThreadingHTTPServer(('127.0.0.2',0),partial(Handler,directory=str(ROOT/'tests/fixtures'))) as server:
        worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
        url=f'http://127.0.0.2:{server.server_port}/browser/protection.html'
        for launch in range(4):
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True);Handler.probes=[]
            with (out/f'process-{launch}.log').open('w') as log:
                process=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}',f'--url={url}'],stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'},start_new_session=True)
                try:
                    def state():
                        if process.poll() is not None:raise RuntimeError(f'Browser exited {process.returncode}')
                        return json.loads(report.read_text()) if report.exists() else {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No feature test window',timeout=25)
                    window=str(state()['window'])
                    def click(r):x,y,w,h=r;click_at(window,x+w//2,y+h//2)
                    def screenshot(name):time.sleep(.3);subprocess.run(['import','-window',window,str(out/name)],check=True,timeout=10)
                    def probe(predicate,label):wait_until(lambda:Handler.probes and predicate(Handler.probes[-1]),label+'; last probe: '+str(Handler.probes[-1] if Handler.probes else None),timeout=25);return Handler.probes[-1]
                    def manager():
                        if state().get('panel')!=5:click_at(window,1187,55)
                        wait_until(lambda:state().get('extensions_ui') is not None,'No extension manager')
                    def install_input(value):
                        manager();click(state()['extensions_ui']['input']);key_at(window,'a',4);clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(value.encode(),timeout=5);key_at(window,'v',4);time.sleep(.25);click(state()['extensions_ui']['download'])
                    def button_layouts(prefix):
                        def language(value):
                            if state()['settings']['language']==value:return
                            key_at(window,'Escape');click_at(window,1307,55)
                            wait_until(lambda:state().get('settings_ui') is not None,'No language settings')
                            key_at(window,'f',4);type_text(window,'language');key_at(window,'Return')
                            wait_until(lambda:(state().get('settings_ui') or {}).get('search')=='language','No language search')
                            control=next(c for c in state()['settings_ui']['controls'] if c['id']=='language')
                            click(next(o['bounds'] for o in control['options'] if o['value']==value))
                            wait_until(lambda:state()['settings']['language']==value,'Language did not change')
                            screenshot('button-settings-language-'+value+'.png')
                            key_at(window,'Escape');manager()
                        for value in ('ru','en'):
                            language(value)
                            for w,h in ((1360,900),(900,620)):
                                subprocess.run(['xdotool','windowsize','--sync',window,str(w),str(h)],check=True)
                                time.sleep(.3);screenshot(prefix+'-'+value+'-'+str(w)+'.png')
                            subprocess.run(['xdotool','windowsize','--sync',window,'1360','900'],check=True)
                            time.sleep(.3)
                        language('ru')
                    def protection_action(index):
                        key_at(window,'Escape');click_at(window,1147,55);time.sleep(.15);click_at(window,1080,82+6+index*32+15)
                    if launch==0:
                        evidence=probe(lambda p:p['dark'] and p['hidden'] and p['dynamicHidden'] and not p['adScript'],'Dark Reader or network/cosmetic ad blocking failed')
                        assert evidence['fonts']['size']=='20px' and evidence['fonts']['tiny']>=8,evidence;assert not state()['audit'].get('font_preference_errors');assert state()['blocked_requests']>=1 and state()['adblock_ready'];screenshot('protection-dark.png');print('Network/cosmetic/Dark Reader PASS',flush=True)
                        protection_action(1)
                        probe(lambda p:p['adScript'] and not p['hidden'],'Site allowlist did not restore ads')
                        protection_action(1)
                        probe(lambda p:not p['adScript'] and p['hidden'],'Site allowlist did not re-enable filtering')
                        protection_action(2)
                        probe(lambda p:not p['dark'] and p['background']=='rgb(255, 255, 255)','Dark Reader site exception failed')
                        protection_action(2);probe(lambda p:p['dark'],'Dark Reader live re-enable failed')
                        install_input(str(ROOT/'tests/fixtures/extensions/traversal.crx'))
                        wait_until(lambda:state()['extensions']['status']=='Unsafe extension archive entry','Unsafe archive not rejected')
                        assert not state()['extensions']['items']
                        install_input(str(ROOT/'tests/fixtures/extensions/valid.crx'))
                        wait_until(lambda:state()['extensions']['candidate'] is not None,'Signed CRX review missing')
                        assert not state()['extensions']['items'];screenshot('extension-permissions.png');button_layouts('permission-buttons');click(state()['extensions_ui']['install'])
                        wait_until(lambda:len(state()['extensions']['items'])==1,'CRX not installed')
                        assert state()['extensions']['restart'] and not state()['extensions']['items'][0]['loaded']
                        r=state()['extensions_ui']['bounds'];click_at(window,r[0]+r[2]-80,r[1]+234)
                        wait_until(lambda:state()['extensions']['items'][0]['pinned'] and len(state()['extension_toolbar'])==1,'Pin did not appear on toolbar')
                        assert state()['extension_toolbar'][0]['icon_loaded'];screenshot('extensions-pinned.png');button_layouts('extension-buttons')
                    elif launch==1:
                        expected=(ROOT/'tests/fixtures/extensions/id.txt').read_text().strip()
                        proof=probe(lambda p:p['extension'] and p['extension'].get('storage') and p['extension'].get('worker'),'Installed MV3 extension did not execute')
                        assert proof['extension']['id']==expected and proof['extension']['content'];time.sleep(1)
                        assert not Handler.probes[-1]['extensionRequestRan'],'DNR rule did not block request'
                        assert state()['extensions']['items'][0]['pinned'] and len(state()['extension_toolbar'])==1,'Pin did not survive restart'
                        parent_tabs=len(state()['tabs'])
                        def open_popup():
                            click(state()['extension_toolbar'][0]['bounds'])
                            wait_until(lambda:state().get('extension_popup') and state()['extension_popup']['loaded'] and state()['extension_popup']['frames']>0,'Popup overlay did not paint')
                            assert len(state()['tabs'])==parent_tabs,'Popup created a tab'
                            assert subprocess.check_output(['xdotool','search','--onlyvisible','--class','Aura2'],text=True).strip().splitlines()==[window],'Popup created a native window'
                        open_popup();key_at(window,'Shift_L',8);time.sleep(.3)
                        assert state().get('extension_popup'),'Alt+Shift dismissed popup instead of reaching extension input'
                        screenshot('extension-overlay.png')
                        content=state()['extension_popup']['content'];click_at(window,content[0]+50,content[1]+64);type_text(window,'aurapopup')
                        click_at(window,content[0]+50,content[1]+110)
                        wait_until(lambda:state()['dialog_count']>0,'Popup confirm did not use Aura dialog')
                        assert state().get('extension_popup');screenshot('popup-confirm-buttons.png');key_at(window,'Return')
                        wait_until(lambda:(state().get('extension_popup') or {}).get('title')=='Popup saved: aurapopup / true','Popup mouse, keyboard, storage or messaging failed')
                        before=state()['extension_popup']['content'];click_at(window,before[0]+150,before[1]+110)
                        wait_until(lambda:state()['extension_popup']['content'][2]>before[2] and state()['extension_popup']['content'][3]>before[3],'Popup did not resize to content')
                        screenshot('extension-overlay-grown.png')
                        key_at(window,'Escape');wait_until(lambda:state().get('extension_popup') is None,'Escape did not close overlay')
                        open_popup();wait_until(lambda:state()['extension_popup']['title']=='Popup restored: aurapopup','Popup storage did not persist')
                        click_at(window,500,700);wait_until(lambda:state().get('extension_popup') is None,'Outside click did not close overlay')
                        open_popup();click(state()['extension_toolbar'][0]['bounds']);wait_until(lambda:state().get('extension_popup') is None,'Toolbar toggle did not close popup')
                        open_popup();click(state()['extension_popup']['pin'])
                        wait_until(lambda:not state()['extension_toolbar'],'Header unpin failed')
                        assert state()['extension_popup'],'Unpin unexpectedly closed overlay'
                        click(state()['extension_popup']['pin']);wait_until(lambda:len(state()['extension_toolbar'])==1,'Header pin failed')
                        subprocess.run(['xdotool','windowsize',window,'900','620'],check=True);time.sleep(.4)
                        small=state()['extension_popup']['bounds'];assert small[0]>=252 and small[0]+small[2]<=900 and small[1]+small[3]<=620,small
                        screenshot('extension-overlay-small.png')
                        subprocess.run(['xdotool','windowsize',window,'1360','900'],check=True);time.sleep(.4)
                        click(state()['extension_popup']['close']);wait_until(lambda:state().get('extension_popup') is None,'Header close failed')
                        open_popup();content=state()['extension_popup']['content'];click_at(window,content[0]+250,content[1]+110)
                        wait_until(lambda:state().get('extension_popup') is None,'window.close did not close extension popup')
                        manager();r=state()['extensions_ui']['bounds'];click_at(window,r[0]+205,r[1]+287)
                        wait_until(lambda:state().get('extension_popup'),'Manager did not open overlay');key_at(window,'Escape')
                        manager();r=state()['extensions_ui']['bounds'];click_at(window,r[0]+320,r[1]+287)
                        wait_until(lambda:any(t['title']=='Aura extension options' for t in state()['tabs']),'Extension options did not open')
                        key_at(window,'w',4);time.sleep(.5)
                        key_at(window,'n',12);key_at(window,'l',4);type_text(window,url+'?private');key_at(window,'Return')
                        proof_private=probe(lambda p:p.get('url','').endswith('?private') and p['dark'] and p['hidden'],'Private protection failed')
                        assert proof_private['extension'] is None,'Extension ran in a private context without permission'
                        assert proof_private['fonts']['size']=='20px' and proof_private['fonts']['tiny']>=8,proof_private
                        assert not state()['audit'].get('font_preference_errors')
                        click(state()['extension_toolbar'][0]['bounds']);time.sleep(.3)
                        assert state().get('extension_popup') is None,'Popup opened in private context'
                        key_at(window,'w',4);time.sleep(.5)
                        # Extension content scales with native chrome, including its input coordinates.
                        click_at(window,1307,55)
                        wait_until(lambda:state().get('settings_ui'),'No UI scaling settings')
                        key_at(window,'f',4);type_text(window,'ui_zoom');key_at(window,'Return')
                        wait_until(lambda:(state().get('settings_ui') or {}).get('search')=='ui_zoom','No zoom search')
                        control=next(c for c in state()['settings_ui']['controls'] if c['id']=='ui_zoom')
                        click(next(o['bounds'] for o in control['options'] if o['value']=='125'))
                        wait_until(lambda:state()['interface']['applied']==125,'UI zoom not applied')
                        key_at(window,'Escape')
                        def scaled_click(r):
                            x,y,w,h=r;click_at(window,round((x+w/2)*1.25),round((y+h/2)*1.25))
                        scaled_click(state()['extension_toolbar'][0]['bounds'])
                        wait_until(lambda:state().get('extension_popup') and state()['extension_popup']['loaded'] and state()['extension_popup']['frames']>0,'Scaled popup did not paint')
                        content=state()['extension_popup']['content']
                        scaled_click([content[0]+16,content[1]+48,100,32]);key_at(window,'a',4);type_text(window,'scaled')
                        scaled_click([content[0]+16,content[1]+96,86,30])
                        wait_until(lambda:state()['dialog_count']>0,'Scaled popup input failed');key_at(window,'Return')
                        wait_until(lambda:(state().get('extension_popup') or {}).get('title')=='Popup saved: scaled / true','Scaled popup messaging failed')
                        screenshot('extension-overlay-scaled.png')
                        key_at(window,'Escape');key_at(window,'0',5)
                        wait_until(lambda:state()['interface']['applied']==100,'UI zoom reset failed')
                        open_popup() # Closing the parent must also close this live OSR popup.
                        print('Overlay/input/auto-size/pinning/private context PASS',flush=True)
                    elif launch==2:
                        probe(lambda p:p['extension'] is not None,'Enabled extension failed on restart')
                        assert state().get('extension_popup') is None,'Popup was restored as a tab/window'
                        assert state()['extensions']['items'][0]['pinned'] and len(state()['extension_toolbar'])==1
                        manager();r=state()['extensions_ui']['bounds'];click_at(window,r[0]+90,r[1]+287)
                        wait_until(lambda:not state()['extensions']['items'][0]['enabled'],'Disable failed')
                        assert state()['extensions']['items'][0]['pinned']
                        print('Pin persistence and disable PASS',flush=True)
                    else:
                        probe(lambda p:p['dark'] and p['hidden'],'Persisted protection failed')
                        assert not state()['extensions']['items'][0]['enabled'];time.sleep(1)
                        assert Handler.probes[-1]['extension'] is None,'Disabled extension still running after restart'
                        manager();r=state()['extensions_ui']['bounds'];click_at(window,r[0]+430,r[1]+287);time.sleep(.3);key_at(window,'Return')
                        wait_until(lambda:not state()['extensions']['items'],'Removal failed')
                        # Exercise the actual Chrome Web Store downloader, without installing another extension.
                        install_input('eimadpbcbfnmbkopoojfekhnkhdbieeh')
                        wait_until(lambda:state()['extensions']['candidate'] is not None,'Chrome Web Store download or signature verification failed',timeout=65)
                        screenshot('web-store-download.png');click(state()['extensions_ui']['cancel'])
                        print('Disable/removal/restart and real Chrome Web Store download PASS',flush=True)
                    if launch!=1:key_at(window,'Escape')
                    click_at(window,1332,15);process.wait(timeout=20)
                    final=json.loads(report.read_text());assert final['cef_created']==final['cef_closed'] and not final.get('audit',{}).get('renderer_crashes')
                    results.append(final)
                except Exception:
                    print("LAST PROBE",Handler.probes[-1] if Handler.probes else None,flush=True);raise
                finally:
                    if process.poll() is None:os.killpg(process.pid,signal.SIGTERM);process.wait(timeout=5)
        server.shutdown();worker.join()
    (out/'verification.json').write_text(json.dumps({'launches':results,'checks':['network filtering','cosmetic filtering','dynamic elements','site exceptions','Dark Reader live toggle','CRX3 signature','ZIP traversal rejection','permission review','private context isolation','font preferences with protection','MV3 worker','storage','messaging','content script','DNR','popup overlay','popup input and messaging','popup layout-switch keys','popup native JS dialog','popup autosize','popup escape/outside click','popup window.close','popup shutdown lifecycle','icon pin persistence','options','disable after restart','removal','Chrome Web Store download']},indent=2,ensure_ascii=False)+'\n')
    print('Features PASS: 4 launches; CPU only; balanced CEF lifetime',flush=True)
if __name__=='__main__':main()
