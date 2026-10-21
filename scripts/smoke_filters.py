"""Filter list UI, real rules, cache, public updates and private auto-check policy."""
import json,os,shutil,signal,subprocess,threading,time,tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from urllib.parse import urlparse,parse_qs
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at,click_at
class Server(ThreadingHTTPServer):
    def handle_error(self,request,address):
        import sys
        if isinstance(sys.exc_info()[1],ConnectionError):return
        super().handle_error(request,address)
class Handler(QuietHandler):
    probes=[];requests=[]
    def end_headers(self):self.send_header('Cache-Control','no-store');super().end_headers()
    def do_GET(self):
        parsed=urlparse(self.path);self.requests.append(self.path)
        if parsed.path=='/advanced-noop.js':
            self.send_response(200);self.send_header('Content-Type','application/javascript');self.end_headers();self.wfile.write(b'window.noopOriginal=true;')
        elif parsed.path=='/advanced-noop.css':
            self.send_response(200);self.send_header('Content-Type','text/css');self.end_headers();self.wfile.write(b'#aura-redirect-target{color:red}')
        elif parsed.path=='/advanced-pixel.gif':
            self.send_response(200);self.send_header('Content-Type','image/gif');self.end_headers();self.wfile.write(bytes([71,73,70,56,57,97,1,0,1,0,128,0,0,255,0,0,0,0,0,44,0,0,0,0,1,0,1,0,0,2,1,76,0,59]))
        elif parsed.path=='/advanced-echo':
            self.send_response(200);self.send_header('Content-Type','application/json');self.end_headers();self.wfile.write(json.dumps({'query':parsed.query}).encode())
        elif parsed.path=='/custom-block.js':
            self.send_response(200);self.send_header('Content-Type','application/javascript');self.end_headers();self.wfile.write(b'window.customRan=true;')
        elif parsed.path=='/filter-probe':
            self.probes.append(json.loads(parse_qs(parsed.query)['data'][0]));self.send_response(204);self.end_headers()
        else:super().do_GET()

def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=filters')
    out=ROOT/'artifacts/filters-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    (profile/'profile.toml').write_text('language="ru"\nfilter_updates=false\ncheck_updates=false\nrestore=false\n')
    custom=out/'Свои правила.txt';custom.write_text('! fictional import\n||127.0.0.1^$script\n127.0.0.1##.aura-custom-ad\n')
    with Server(('127.0.0.1',0),partial(Handler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();url=f'http://127.0.0.1:{server.server_port}/filter-lists.html'
        for launch in range(4):
            active=profile if launch<2 else out/('private-profile' if launch==2 else 'auto-profile')
            if launch>=2:
                shutil.rmtree(active,ignore_errors=True);active.mkdir();(active/'profile.toml').write_text('language="ru"\nfilter_updates=true\ncheck_updates=false\nrestore=false\n')
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True);Handler.probes=[];started=time.monotonic()
            with (out/f'process-{launch}.log').open('w') as log:
                proc=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={active}',f'--report={report}',f'--url={url}']+(['--private'] if launch==2 else []),stdout=log,stderr=subprocess.STDOUT,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'},start_new_session=True)
                try:
                    def state():
                        if proc.poll() is not None:raise RuntimeError('Filter test browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No filter test window',timeout=25);window=str(state()['window'])
                    def ui():return state().get('filters_ui') or {}
                    def click(rect):x,y,w,h=rect;click_at(window,x+w//2,y+h//2)
                    def shot(name):time.sleep(.3);subprocess.run(['import','-window',window,str(out/name)],check=True,timeout=10)
                    def paste(value,target):
                        click(target);key_at(window,'a',4);clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(value.encode(),timeout=5);key_at(window,'v',4);time.sleep(.25)
                    def seek(text):
                        key_at(window,'Escape');key_at(window,'Escape');click_at(window,state()['interface']['width']-53,55)
                        wait_until(lambda:state().get('settings_ui'),'No filter settings');key_at(window,'f',4);type_text(window,text);key_at(window,'Return')
                        wait_until(lambda:state()['settings_ui'].get('search')==text,'No settings search')
                        return state()['settings_ui']['controls']
                    def manager():
                        row=next(c for c in seek('Filter lists and custom rules') if c['id']=='filters');r=row['bounds'];click([r[0],r[1]+66,min(300,r[2]),32]);wait_until(lambda:ui(),'No filter list panel')
                    def done(revision):wait_until(lambda:not state()['filter_lists']['busy'] and state()['filter_lists']['revision']>=revision,'Rules did not apply: '+state()['filter_lists']['status'],timeout=35)
                    def rule(value):
                        revision=state()['filter_lists']['revision'];paste(value,ui()['input']);click(ui()['save']);done(revision+1)
                    def probe(predicate):wait_until(lambda:Handler.probes and predicate(Handler.probes[-1]),'Filter result was not observed',timeout=20)
                    if launch==0:
                        probe(lambda p:p['network'] and not p['hidden']);manager();rule('127.0.0.1##.aura-custom-ad');probe(lambda p:p['hidden'])
                        rule('||127.0.0.1^$script');key_at(window,'r',4);probe(lambda p:not p['network'] and p['hidden'])
                        before=state()['filter_lists']['revision'];paste('||bad.test^$unknown-option-for-aura',ui()['input']);click(ui()['save'])
                        wait_until(lambda:not state()['filter_lists']['busy'] and 'invalid or unsupported' in state()['filter_lists']['status'],'Invalid rule was not rejected')
                        assert state()['filter_lists']['revision']==before and ui()['input_length']>0;shot('ru-error.png')
                        click(ui()['rows'][0]['edit']);rule('127.0.0.1##.other-ad');probe(lambda p:p['otherHidden'] and not p['hidden'])
                        click(ui()['rows'][0]['remove']);done(before+2);probe(lambda p:not p['otherHidden'])
                        click(ui()['import']);wait_until(lambda:ui().get('picker_pid',-1)>0,'No native filter chooser')
                        def picker_window():
                            result=subprocess.run(['xdotool','search','--onlyvisible','--pid',str(ui()['picker_pid'])],text=True,capture_output=True)
                            return result.stdout.strip().splitlines()[-1] if result.returncode==0 and result.stdout.strip() else None
                        wait_until(lambda:picker_window(),'Native filter chooser did not become visible')
                        found=picker_window()
                        subprocess.run(['xdotool','windowfocus','--sync',found],check=True);subprocess.run(['xdotool','key','--clearmodifiers','ctrl+l'],check=True);time.sleep(.2)
                        clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);clip.communicate(str(custom).encode(),timeout=5)
                        subprocess.run(['xdotool','key','--clearmodifiers','ctrl+a','ctrl+v'],check=True);time.sleep(.3);subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True);time.sleep(.7)
                        if ui()['picker_pid']>0:subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True)
                        wait_until(lambda:ui()['picker_pid']==-1,'Filter chooser did not close');done(before+3);probe(lambda p:p['hidden'])
                        assert ui()['custom_count']==3
                        click(ui()['easyprivacy_toggle']);done(before+4);assert not state()['filter_lists']['easyprivacy']
                        while time.monotonic()-started<33:time.sleep(.25)
                        assert not (active/'filter-lists/check.toml').exists(),'Automatic updates ran despite opt-out'
                        revision=state()['filter_lists']['revision'];click(ui()['check']);click(ui()['check'])
                        wait_until(lambda:not state()['filter_lists']['busy'] and 'cancelled' in state()['filter_lists']['status'],'Real filter update cancellation failed');assert state()['filter_lists']['revision']==revision
                        click(ui()['check']);wait_until(lambda:not state()['filter_lists']['busy'] and state()['filter_lists']['updated_easylist']>0,'Real official EasyList update failed: '+state()['filter_lists']['status'],timeout=90)
                        assert ui()['custom_count']==3 and not state()['filter_lists']['easyprivacy'];shot('ru-updated.png')
                        for advanced_rule in (
                            '/advanced-noop.js$script,redirect=noop.js',
                            '/advanced-noop.css$stylesheet,redirect=noop.css',
                            '/advanced-pixel.gif$image,redirect=1x1.gif',
                            '/advanced-echo$removeparam=utm_source',
                            '127.0.0.1##.aura-procedural-card:has-text(Sponsored)',
                            '127.0.0.1##.aura-style-target:style(color: rgb(12, 34, 56) !important)',
                            '127.0.0.1##.aura-sponsored-label:has-text(Sponsored):upward(1)',
                        ):rule(advanced_rule)
                        Handler.requests.clear();key_at(window,'r',4)
                        probe(lambda p:p['noopLoaded'] and not p['noopOriginal'] and p['noopCssLoaded'] and p.get('pixelAlpha')==0 and p.get('echoQuery')=='keep=1' and p['procHidden'] and p['parentHidden'] and p['styleColor']=='rgb(12, 34, 56)' and p['redirectColor']=='rgb(0, 0, 0)')
                        assert not any(urlparse(path).path in ('/advanced-noop.js','/advanced-noop.css','/advanced-pixel.gif') for path in Handler.requests),'Replacement contacted the original server'
                        assert state()['rewritten_requests']>0;shot('ru-advanced.png')
                        key_at(window,'Escape');key_at(window,'Escape');click_at(window,392,508);probe(lambda p:not p['procHidden']);click_at(window,392,508);probe(lambda p:p['procHidden'])
                        def protection_toggle():
                            key_at(window,'Escape');key_at(window,'Escape');click_at(window,state()['interface']['width']-213,55);time.sleep(.15);click_at(window,state()['interface']['width']-190,135)
                        protection_toggle();probe(lambda p:not p['procHidden'] and not p['parentHidden'] and p['styleColor']=='rgb(0, 0, 0)')
                        key_at(window,'r',4);probe(lambda p:p['network'] and p['noopOriginal'] and p.get('pixelAlpha')==255 and 'utm_source' in p.get('echoQuery',''))
                        protection_toggle();probe(lambda p:p['procHidden'] and p.get('pixelAlpha')==0 and p['noopLoaded'] and not p['noopOriginal'])
                        manager()
                        rows=seek('language');control=next(c for c in rows if c['id']=='language');click(next(o['bounds'] for o in control['options'] if o['value']=='en'));wait_until(lambda:state()['settings']['language']=='en','No English');manager();shot('en-lists.png')
                        subprocess.run(['xdotool','windowsize','--sync',window,'900','620'],check=True);time.sleep(.4);shot('en-small.png');subprocess.run(['xdotool','windowsize','--sync',window,'1360','900'],check=True);time.sleep(.3)
                    elif launch==1:
                        probe(lambda p:not p['network'] and p['hidden']);manager();assert ui()['custom_count']==10 and not state()['filter_lists']['easyprivacy'];assert state()['filter_lists']['updated_easylist']>0
                        revision=state()['filter_lists']['revision'];click(ui()['bundled']);done(revision+1);assert state()['filter_lists']['updated_easylist']==0;assert ui()['custom_count']==10
                    elif launch==2:
                        # A stale, enabled cache would trigger the automatic path in normal mode.
                        wait_until(lambda:state()['tabs'][0]['private'] and Handler.probes,'No private fixture')
                        end=time.monotonic()+34
                        while time.monotonic()<end:time.sleep(.5)
                        assert state()['filter_lists']['revision']==0 and not (active/'filter-lists/check.toml').exists()
                        assert state()['settings']['filter_updates'] is True
                    else:
                        wait_until(lambda:state()['filter_lists']['updated_easylist']>0 and state()['filter_lists']['updated_easyprivacy']>0,'Normal automatic filter update failed: '+state()['filter_lists']['status'],timeout=100)
                        assert (active/'filter-lists/check.toml').exists()
                    key_at(window,'Escape');key_at(window,'Escape');key_at(window,'q',4);time.sleep(.3)
                    if proc.poll() is None and json.loads(report.read_text()).get('dialog_count'):key_at(window,'Return')
                    proc.wait(timeout=30);final=json.loads(report.read_text());assert final['cef_created_total']==final['cef_closed_total']
                finally:
                    (out/f'probes-{launch}.json').write_text(json.dumps(Handler.probes[-20:],indent=2))
                    try:os.killpg(proc.pid,signal.SIGTERM)
                    except ProcessLookupError:pass
                    try:proc.wait(timeout=4)
                    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    (out/'PASS.txt').write_text('PASS: custom rules, real network/cosmetics, cache, official list update/cancel, native file import, RU/EN/small, private auto-check exclusion\n')
    print('PASS: native filter lists and custom rules')
if __name__=='__main__':main()
