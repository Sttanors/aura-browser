"""Actual native Wayland + CPU compositor protocol input, with no X server."""
import argparse,json,os,shutil,signal,socket,subprocess,tempfile,time,threading
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until
from smoke_extension_access import seed as seed_extension

KEYS={c:code for row,codes in [('qwertyuiop',range(16,26)),('asdfghjkl',range(30,39)),('zxcvbnm',range(44,51)),('1234567890',range(2,12))] for c,code in zip(row,codes)}
KEYS.update({'/':53,'.':52,'-':12,':':39,' ':57})
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--scale',type=int,choices=(1,2),default=1);args=parser.parse_args()
    out=ROOT/'artifacts/wayland-smoke';out.mkdir(parents=True,exist_ok=True)
    module=out/'input.so';flags=subprocess.check_output(['pkg-config','--cflags','--libs','libweston-15'],text=True).split()
    subprocess.run(['cc','-shared','-fPIC','-Wno-attributes',str(ROOT/'tests/fixtures/wayland_input.c'),'-o',str(module),*flags],check=True)
    profile=out/f'profile-{args.scale}';shutil.rmtree(profile,ignore_errors=True);profile.mkdir();(profile/'profile.toml').write_text('language="en"\nfilter_updates=false\ncheck_updates=false\nrestore=false\nconfirm_quit=false\n')
    extension_id=seed_extension(profile)
    registry=profile/'extensions/registry.toml';registry.write_text(registry.read_text()+'pinned=true\n')
    extension=profile/'extensions'/(extension_id+'-123')
    (extension/'popup.html').write_text('<!doctype html><meta charset="utf-8"><style>body{width:360px;height:300px;margin:0;background:#edf3ff}input,textarea{position:absolute;left:20px;width:300px;height:40px}#text{top:20px}#password{top:80px}#area{top:140px}</style><input id="text"><input id="password" type="password" value="POPUP_PASSWORD"><textarea id="area"></textarea><button id="selfclose" style="position:absolute;left:20px;top:220px;height:40px">Close popup later</button><script src="popup.js"></script>')
    (extension/'popup.js').write_text("setInterval(()=>document.title=JSON.stringify(Object.fromEntries(['text','password','area'].map(id=>[id,document.getElementById(id).value]))),100);document.getElementById('selfclose').onclick=()=>setTimeout(()=>window.close(),1500)")
    binary=ROOT/'build/linux-x64-release/aura_browser';report=out/f'report-{args.scale}.json';report.unlink(missing_ok=True)
    weston=browser=peer=None
    with tempfile.TemporaryDirectory(prefix='aura-native-wayland-') as d,ThreadingHTTPServer(('127.0.0.1',0),partial(QuietHandler,directory=str(ROOT/'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();runtime=Path(d)
        env={**os.environ,'XDG_RUNTIME_DIR':d,'WAYLAND_DISPLAY':'wayland-aura-native','XDG_SESSION_TYPE':'wayland','AURA_WINDOW_SYSTEM':'wayland','AURA_TEST_ISOLATED_DISPLAY':'1','AURA_BACKGROUND_TEST':'1','AURA_TEST_CONTROL_SOCKET':str(runtime/'control'),'LIBGL_ALWAYS_SOFTWARE':'1','DBUS_SESSION_BUS_ADDRESS':'unix:path=/nonexistent/aura-test-bus'};env.pop('DISPLAY',None);env.pop('WAYLAND_SOCKET',None)
        ctl=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);ctl.bind(str(runtime/'client'));ctl.settimeout(3)
        def command(text):
            if weston and weston.poll() is not None:raise RuntimeError('Private Weston exited '+str(weston.returncode)+'; see Weston log')
            ctl.sendto(text.encode(),env['AURA_TEST_CONTROL_SOCKET'])
            try:return ctl.recv(64).decode()
            except socket.timeout:
                if weston and weston.poll() is not None:raise RuntimeError('Private Weston exited '+str(weston.returncode)+'; see Weston log')
                raise
        def key(code,mods=()):
            for m in mods:command(f'key {m} 1')
            command(f'key {code} 1');command(f'key {code} 0')
            for m in reversed(mods):command(f'key {m} 0')
            time.sleep(.08)
        def type_ascii(text):
            for c in text:key(KEYS[c.lower()],(42,) if c.isupper() or c==':' else ())
        def click(x,y,button=272):
            assert command(f'move {x} {y}')=='ok';command(f'button {button} 1');command(f'button {button} 0');time.sleep(.15)
        def doc():
            try:return json.loads(state()['tabs'][state()['active_tab']]['title'])
            except (KeyError,IndexError,ValueError):return {}
        def state():
            if browser and browser.poll() is not None:raise RuntimeError('Native browser exited '+str(browser.returncode))
            try:return json.loads(report.read_text())
            except(OSError,ValueError):return {}
        try:
            with (out/f'weston-{args.scale}.log').open('w') as wl,(out/f'process-{args.scale}.log').open('w') as log:
                weston=subprocess.Popen(['weston','--backend=headless','--renderer=pixman','--shell=desktop-shell.so','--width=1800','--height=1240',f'--scale={args.scale}','--socket=wayland-aura-native','--idle-time=0','--no-config',f'--modules={module}'],env=env,stdout=wl,stderr=wl,start_new_session=True)
                wait_until(lambda:(runtime/'control').exists() and (runtime/'wayland-aura-native').exists(),'No private native compositor')
                browser=subprocess.Popen([str(binary),f'--data-dir={profile}',f'--report={report}','--software-rendering','--window-system=wayland',f'--url=http://127.0.0.1:{server.server_port}/a.html'],env=env,stdout=log,stderr=log,start_new_session=True)
                def loaded():
                    if browser.poll() is not None:raise RuntimeError('Native browser exited; see '+str(out/f'process-{args.scale}.log'))
                    return state().get('presentation_frames',0)>0 and any(t.get('title')=='Alpha' and t.get('loaded') for t in state().get('tabs',[]))
                wait_until(loaded,'No native Wayland CEF paint',timeout=40)
                s=state();assert s['window_system']=='wayland' and 'native Wayland' in s['compositor'],s
                assert command(f'focus {s["pid"]} 0')=='ok';time.sleep(.3)
                assert command('capture '+str(out/f'native-{args.scale}.ppm'))=='ok'
                key(20,(29,));wait_until(lambda:len(state()['tabs'])==2,'Ctrl+T native key failed')
                key(17,(29,));wait_until(lambda:len(state()['tabs'])==1,'Ctrl+W native key failed')
                # Actual native text input, selection and clipboard bridging to CEF.
                key(38,(29,));type_ascii(f'http://127.0.0.1:{server.server_port}/wayland.html');key(28)
                wait_until(lambda:'text' in doc(),'No input fixture')
                s=state();tab=s['tabs'][s['active_tab']];bounds=tab.get('bounds',[252,88]);factor=s['interface']['applied']/100
                bx,by=(bounds['x'],bounds['y']) if isinstance(bounds,dict) else bounds[:2]
                def web(x,y,button=272):click((bx+x/factor)*factor,(by+y/factor)*factor,button)
                def menu_item(label,enabled=True):
                    wait_until(lambda:any(i['label']==label and i['enabled']==enabled for i in state().get('menu_ui') or []),'Missing menu item '+label)
                    return next(i for i in state()['menu_ui'] if i['label']==label)
                def activate(label,keyboard=False):
                    item=menu_item(label)
                    if keyboard:
                        key(102)
                        for i in [i for i in state()['menu_ui'] if i['enabled'] and i['label']]:
                            if i['id']==item['id']:break
                            key(108)
                        key(28)
                    else:
                        x,y,w,h=item['bounds'];click((x+w/2)*factor,(y+h/2)*factor)
                    wait_until(lambda:not state().get('menu_ui'),'Menu did not close')
                web(60,120);type_ascii('Native');wait_until(lambda:doc().get('text')=='Native','Native text input failed')
                key(30,(29,));key(46,(29,));wait_until(lambda:state().get('audit',{}).get('native_clipboard_queries',0)>=1,'Native copy context query failed');key(38,(29,));key(47,(29,));time.sleep(.4)
                assert state()['address_input_bytes']==6,'Web copy to native address clipboard failed'
                key(1);web(60,280);key(47,(29,));wait_until(lambda:doc().get('area')=='Native','Native clipboard paste into textarea failed')
                # Password selection must not replace the native clipboard.
                web(60,200);key(30,(29,));key(46,(29,));wait_until(lambda:state().get('audit',{}).get('native_clipboard_queries',0)>=2,'No password copy validation');web(60,280);key(30,(29,));type_ascii('Clear');wait_until(lambda:doc().get('area')=='Clear','Textarea preparation failed');key(30,(29,));key(47,(29,));time.sleep(.6);wait_until(lambda:doc().get('area')=='Native','Password selection leaked into clipboard')
                # A separate native client owns and reads the real protocol selection.
                source=out/f'source-{args.scale}.txt';source.with_suffix('.txt.status').unlink(missing_ok=True);source.write_text('Привет 🌍\nWayland',encoding='utf-8')
                peer=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_wayland_peer'),'source',str(source)],env=env,stdout=log,stderr=log,start_new_session=True)
                wait_until(lambda:command(f'focus {peer.pid} 0')=='ok','Native clipboard peer did not map');time.sleep(.3);key(60);time.sleep(.4);assert peer.poll() is None, ('peer exited',peer.returncode)
                assert command(f'focus {browser.pid} 0')=='ok', 'Root window disappeared';time.sleep(.3);key(30,(29,));key(47,(29,));wait_until(lambda:doc().get('area')==source.read_text(),'External Unicode clipboard transfer failed')
                # Reject oversized external text without deleting selected form contents.
                command(f'focus {peer.pid} 0');source.write_text('X'*(17*1024*1024));key(60);time.sleep(.4);assert command(f'focus {browser.pid} 0')=='ok', 'Root window disappeared';time.sleep(.3);key(30,(29,));key(47,(29,));wait_until(lambda:state()['presentation']['clipboard']['failures']>=1,'Oversized clipboard was not rejected',timeout=8)
                assert doc().get('area')=='Привет 🌍\nWayland','Oversized clipboard changed the form'
                # Large but allowed clipboard round-trip uses the native pipe send path.
                key(46,(29,));wait_until(lambda:state().get('audit',{}).get('native_clipboard_queries',0)>=3,'No native copy query');time.sleep(.3)
                source.unlink();command(f'focus {peer.pid} 0');time.sleep(.3);key(61);wait_until(source.exists,'External client did not receive Aura clipboard');assert source.read_text()=='Привет 🌍\nWayland'
                command('close');peer.wait(timeout=10);peer=None;command(f'focus {browser.pid} 0');time.sleep(.5);assert doc().get('area')=='Привет 🌍\nWayland', 'Focus change altered the form'
                command('capture '+str(out/f'input-{args.scale}.ppm'))
                key(20,(29,));wait_until(lambda:len(state()['tabs'])==2,'No second tab');key(17,(29,));wait_until(lambda:len(state()['tabs'])==1,'Second tab not closed');time.sleep(.4);assert doc().get('area')=='Привет 🌍\nWayland', 'Tab switching altered the form'
                # Page context menus bridge the same native clipboard as shortcuts.
                web(60,120);key(30,(29,));web(60,120,273);activate('Copy',keyboard=True)
                web(60,280);key(30,(29,));web(60,280,273);activate('Paste');wait_until(lambda:doc().get('area')=='Native','Page menu paste failed')
                key(30,(29,));web(60,280,273);activate('Cut');wait_until(lambda:doc().get('area')=='','Page menu cut failed')
                web(60,280,273);activate('Paste');wait_until(lambda:doc().get('area')=='Native','Page cut/paste round trip failed')
                web(60,200);key(30,(29,));web(60,200,273);menu_item('Copy',False);menu_item('Cut',False);key(1)
                assert doc().get('password')=='PRIVATE_PASSWORD','Page password cut altered field'
                # Actual extension popup overlay: native keys and context-menu callbacks.
                wait_until(lambda:state().get('extension_toolbar'),'Pinned extension did not load')
                x,y,w,h=state()['extension_toolbar'][0]['bounds'];click((x+w/2)*factor,(y+h/2)*factor)
                def popup_doc():
                    try:return json.loads(state()['extension_popup']['title'])
                    except(KeyError,TypeError,ValueError):return {}
                wait_until(lambda:'text' in popup_doc(),'Extension clipboard fixture did not load')
                def popup_web(x,y,button=272):
                    px,py=state()['extension_popup']['content'][:2];click(px*factor+x,py*factor+y,button)
                popup_web(60,40);type_ascii('Popup');wait_until(lambda:popup_doc().get('text')=='Popup','Popup native typing failed')
                queries=state().get('audit',{}).get('native_clipboard_queries',0)
                key(30,(29,));key(46,(29,));wait_until(lambda:state().get('audit',{}).get('native_clipboard_queries',0)>queries,'Popup copy query failed')
                popup_web(60,160);key(47,(29,));wait_until(lambda:popup_doc().get('area')=='Popup','Popup native paste failed')
                key(30,(29,));key(45,(29,));wait_until(lambda:popup_doc().get('area')=='','Popup native cut failed');key(47,(29,));wait_until(lambda:popup_doc().get('area')=='Popup','Popup native cut/paste failed')
                popup_web(60,100);key(30,(29,));key(45,(29,));time.sleep(.3);assert popup_doc().get('password')=='POPUP_PASSWORD','Popup password cut leaked/changed text'
                popup_web(60,100,273);menu_item('Copy',False);menu_item('Cut',False);key(1)
                popup_web(60,160);key(30,(29,));type_ascii('Clear');wait_until(lambda:popup_doc().get('area')=='Clear','Popup preparation failed');key(30,(29,));popup_web(60,160,273);activate('Paste',keyboard=True);wait_until(lambda:popup_doc().get('area')=='Popup','Popup password protection or menu paste failed')
                key(30,(29,));popup_web(60,160,273);activate('Cut');wait_until(lambda:popup_doc().get('area')=='','Popup menu cut failed')
                popup_web(60,160,273);activate('Paste');wait_until(lambda:popup_doc().get('area')=='Popup','Popup menu cut/paste round trip failed')
                key(30,(29,));popup_web(60,160,273);activate('Copy',keyboard=True)
                command('capture '+str(out/f'popup-{args.scale}.ppm'))
                # Escape closes a menu first, then the overlay; never the underlying tab.
                popup_web(60,160,273);menu_item('Paste');key(1);assert state().get('extension_popup'),'Escape closed popup with its menu'
                key(1);wait_until(lambda:not state().get('extension_popup'),'Popup did not close')
                web(60,280);key(30,(29,));key(47,(29,));wait_until(lambda:doc().get('area')=='Popup','Popup to page clipboard failed')
                # A genuinely pending external paste must not outlive its popup or document.
                source.write_text('Late clipboard must be discarded',encoding='utf-8')
                peer=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_wayland_peer'),'source',str(source)],env=env,stdout=log,stderr=log,start_new_session=True)
                wait_until(lambda:command(f'focus {peer.pid} 0')=='ok','No delayed clipboard peer');time.sleep(.2)
                command(f'focus {browser.pid} 0');time.sleep(.2)
                x,y,w,h=state()['extension_toolbar'][0]['bounds'];click((x+w/2)*factor,(y+h/2)*factor);wait_until(lambda:'text' in popup_doc(),'Popup did not reopen')
                popup_web(60,160);type_ascii('Keep');wait_until(lambda:popup_doc().get('area')=='Keep','Delayed target was not prepared')
                def begin_delayed_paste():
                    pause=source.with_suffix('.txt.status.pause');pause.write_text('hold')
                    command(f'focus {peer.pid} 0');time.sleep(.15);key(60)
                    command(f'focus {browser.pid} 0');time.sleep(.15)
                    return pause
                # Losing native focus closes an extension popup; reopen before pasting.
                pause=begin_delayed_paste()
                x,y,w,h=state()['extension_toolbar'][0]['bounds'];click((x+w/2)*factor,(y+h/2)*factor);wait_until(lambda:'text' in popup_doc(),'Delayed popup did not reopen');popup_web(60,160)
                reads=state()['presentation']['clipboard']['reads'];key(47,(29,))
                wait_until(lambda:state()['presentation']['clipboard']['read_pending'],'Popup paste was not actually pending')
                key(1);wait_until(lambda:not state().get('extension_popup'),'Pending popup did not close');pause.unlink()
                wait_until(lambda:state()['presentation']['clipboard']['reads']>reads,'Delayed popup read did not finish')
                assert doc().get('area')=='Popup','Late popup clipboard changed the page'
                pause=begin_delayed_paste();web(60,280);reads=state()['presentation']['clipboard']['reads'];key(47,(29,))
                wait_until(lambda:state()['presentation']['clipboard']['read_pending'],'Page paste was not actually pending')
                key(20,(29,));wait_until(lambda:len(state()['tabs'])==2,'No new tab for late-paste guard');pause.unlink()
                wait_until(lambda:state()['presentation']['clipboard']['reads']>reads,'Delayed page read did not finish')
                assert state()['address_input_bytes']==0,'Late page clipboard changed the new-tab address'
                key(17,(29,));wait_until(lambda:len(state()['tabs'])==1,'Late-paste test tab not closed');assert doc().get('area')=='Popup','Late clipboard changed background form'
                command(f'focus {peer.pid} 0');command('close');peer.wait(timeout=10);peer=None;command(f'focus {browser.pid} 0');time.sleep(.4)
                wait_until(lambda:state()['presentation']['input_focus'],'Root focus was not acknowledged')
                x,y,w,h=state()['extension_toolbar'][0]['bounds'];click((x+w/2)*factor,(y+h/2)*factor);wait_until(lambda:'text' in popup_doc(),'Self-closing popup did not open')
                popup_web(60,240);popup_web(60,160,273);menu_item('Paste')
                wait_until(lambda:not state().get('extension_popup'),'Extension window.close did not close popup')
                assert not state().get('menu_ui'),'Self-closed popup left a stale context menu'
                command('close');browser.wait(timeout=30);assert browser.returncode==0
                final=json.loads(report.read_text());assert final['cef_created_total']==final['cef_closed_total'],final
        finally:
            for p in (peer,browser,weston):
                if p and p.poll() is None:
                    os.killpg(p.pid,signal.SIGTERM)
                    try:p.wait(timeout=5)
                    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
            ctl.close();server.shutdown()
    print('PASS native Wayland: actual CEF paint, protocol pointer/keys, password copy protection, independent SDL clipboard peer, Unicode/multiline round-trip, 17 MiB refusal without form loss, focus/tab preservation, page and extension popup clipboard keys/menus, protected password copy/cut, pending-paste popup/tab guards and balanced shutdown; no DISPLAY, XWayland or GPU')
if __name__=='__main__':main()
