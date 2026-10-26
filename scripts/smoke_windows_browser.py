"""Exercise the full Win32 browser on a disposable GitHub Actions Windows desktop."""
from __future__ import annotations
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from dependencies import ROOT
from release_identity import read_identity
from smoke_compatibility import Server as CompatServer, Handler as CompatHandler


def main():
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('This GUI smoke test runs only on the disposable Windows CI runner')
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.SendMessageW.restype = ctypes.c_ssize_t
    user.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    user.ClientToScreen.restype = wintypes.BOOL
    user.IsWindow.argtypes = [wintypes.HWND]
    user.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    output = ROOT/'artifacts/windows-smoke'
    output.mkdir(parents=True, exist_ok=True)
    profile = output/'profile-кириллица'
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    (profile/'profile.toml').write_text('language="ru"\nfilter_updates=false\ncheck_updates=false\nrestore=false\nblock_popups=true\n', encoding='utf-8')
    server = ThreadingHTTPServer(('127.0.0.1', 0), partial(SimpleHTTPRequestHandler, directory=str(ROOT/'tests/fixtures')))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    compat_servers=[CompatServer(('127.0.0.1',0),partial(CompatHandler,directory=str(ROOT/'tests/fixtures/browser'))) for _ in range(2)]
    for source in compat_servers:threading.Thread(target=source.serve_forever,daemon=True).start()
    compat_url=f'http://127.0.0.1:{compat_servers[0].server_port}/compat.html?remote=http://127.0.0.1:{compat_servers[1].server_port}'
    url = f'http://127.0.0.1:{server.server_port}/browser/a.html'
    binary = Path(os.environ.get('AURA_TEST_BINARY', str(ROOT/'build/windows-x64-release/aura_browser.exe')))
    process = None
    try:
        for launch in range(2):
            report_path = output/f'report-{launch}.json'
            report_path.unlink(missing_ok=True)
            last_report = {}
            def read():
                nonlocal last_report
                try:
                    last_report = json.loads(report_path.read_text(encoding='utf-8'))
                except (OSError, ValueError):
                    pass  # A Windows sharing violation during atomic replacement is transient.
                return last_report
            def wait(predicate, message, timeout=45):
                deadline = time.monotonic()+timeout
                while time.monotonic()<deadline:
                    state = read()
                    if state and predicate(state): return state
                    if process.poll() is not None: raise RuntimeError(f'Browser exited ({process.returncode}): {message}')
                    time.sleep(.1)
                raise RuntimeError(message)
            with (output/f'process-{launch}.log').open('w') as log:
                process = subprocess.Popen([str(binary), f'--data-dir={profile}', f'--report={report_path}', '--software-rendering', f'--url={url}'], stdout=log, stderr=subprocess.STDOUT)
                state = wait(lambda s: any(t.get('title') == 'Alpha' and t.get('loaded') and t.get('frames', 0) > 0 for t in s.get('tabs', [])), 'Alpha must load and paint')
                window = state['window']
                assert user.IsWindow(window), 'Report must identify the real native browser window'
                user.GetClassLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
                user.GetClassLongPtrW.restype = ctypes.c_size_t
                assert user.GetClassLongPtrW(window, -14), 'Window class must expose the Aura large icon'
                assert user.GetClassLongPtrW(window, -34), 'Window class must expose the Aura small icon'
                assert state.get('version')==read_identity().version, 'Running browser must match the source/release identity'
                assert state['adblock_ready'], 'Rust ad blocker must initialize'
                assert 'DIB' in state['renderer'], state['renderer']
                def click(x, y):
                    current = read()
                    scale = current['interface']['device_scale']*current['interface']['applied']/100
                    point = round(x*scale) | (round(y*scale)<<16)
                    user.SendMessageW(window, 0x201, 1, point)
                    user.SendMessageW(window, 0x202, 0, point)
                # Shared UI web bounds: sidebar 240 DIP, toolbar ends at 88 DIP.
                # Read the reported web rectangle instead of relying on window decorations.
                tab = next(t for t in state['tabs'] if t.get('title') == 'Alpha')
                bounds = tab.get('bounds', [252, 88, 1000, 700])
                x, y = (bounds['x'], bounds['y']) if isinstance(bounds, dict) else bounds[:2]
                def click_web(css_x, css_y):
                    factor = read()['interface']['applied']/100
                    click(x+css_x/factor, y+css_y/factor)
                click_web(120, 165)
                # Native SendMessage returns before Chromium acknowledges DOM focus.
                wait(lambda s: any(t.get('title') == 'Alpha input focused' for t in s.get('tabs', [])), 'The real web input must receive focus before typing')
                def type_text(value):
                    for char in value:
                        virtual = user.VkKeyScanW(ord(char)) & 255
                        if virtual == 255: virtual = ord('A')
                        user.SendMessageW(window, 0x100, virtual, 1)
                        user.SendMessageW(window, 0x102, ord(char), 1)
                        user.SendMessageW(window, 0x101, virtual, 1)
                type_text('Windows Привет')
                wait(lambda s: any(t.get('title') == 'Alpha input Windows Привет' for t in s.get('tabs', [])), 'Win32 Unicode typing must reach the real web input')
                click_web(100, 285)
                wait(lambda s: s.get('dialog_count') == 1, 'JavaScript dialog must open')
                user.SendMessageW(window, 0x100, 13, 1)
                user.SendMessageW(window, 0x101, 13, 1)
                wait(lambda s: any(t.get('title') == 'Alpha dialog accepted' for t in s.get('tabs', [])), 'Native dialog must accept Return')
                click_web(100, 108)
                wait(lambda s: any(t.get('title') == 'Beta' for t in s.get('tabs', [])), 'Native pointer input must navigate to Beta')
                if launch == 0:
                    click(read()['interface']['width']-53, 55)
                    settings = wait(lambda s: bool(s.get('settings_ui')), 'Settings panel must open')
                    def click_rect(rect):
                        click(rect[0]+rect[2]//2, rect[1]+rect[3]//2)
                    # The first category includes language; choose English through the native UI.
                    language = next(c for c in settings['settings_ui']['controls'] if c['id'] == 'language')
                    click_rect(next(o['bounds'] for o in language['options'] if o['value'] == 'en'))
                    wait(lambda s: s.get('settings', {}).get('language') == 'en', 'English selection must apply')
                    click_rect(read()['settings_ui']['search_bounds'])
                    type_text('ui_zoom')
                    settings = wait(lambda s: (s.get('settings_ui') or {}).get('search') == 'ui_zoom', 'Settings search must accept native text')
                    zoom = next(c for c in settings['settings_ui']['controls'] if c['id'] == 'ui_zoom')
                    click_rect(next(o['bounds'] for o in zoom['options'] if o['value'] == '125'))
                    wait(lambda s: s.get('settings', {}).get('ui_zoom') == 125, 'Interface zoom must apply')
                else:
                    assert state['settings']['language'] == 'en', 'Language must survive restart'
                    assert state['settings']['ui_zoom'] == 125, 'Interface scale must survive restart'
                # The filter manager uses the same native input and Unicode profile path.
                for _ in range(3):
                    user.SendMessageW(window, 0x100, 27, 1)
                    user.SendMessageW(window, 0x101, 27, 1)
                click(read()['interface']['width']-213, 55)
                click(read()['interface']['width']-190, 263)
                wait(lambda s: bool(s.get('filters_ui')), 'Native filter list panel must open')
                rect=read()['filters_ui']['input'];click(rect[0]+20,rect[1]+18)
                type_text('||ci-example.test^$script')
                wait(lambda s: (s.get('filters_ui') or {}).get('input_length') == len('||ci-example.test^$script'), 'Native typing must reach the custom filter editor')
                rect=read()['filters_ui']['save'];click(rect[0]+rect[2]//2,rect[1]+rect[3]//2)
                wait(lambda s: s['filter_lists']['revision'] > 0 and not s['filter_lists']['busy'] and (s.get('filters_ui') or {}).get('list_revision') == s['filter_lists']['revision'] and s['filters_ui']['custom_count'] == 1, 'Filter engine and painted rule list must finish applying on Windows')
                # Run the same real API/media/origin fixture as Linux, through native navigation.
                for _ in range(3):
                    user.SendMessageW(window, 0x100, 27, 1)
                    user.SendMessageW(window, 0x101, 27, 1)
                previous_reports=len(CompatHandler.reports)
                click(480,55);type_text(compat_url)
                user.SendMessageW(window,0x100,13,1);user.SendMessageW(window,0x101,13,1)
                wait(lambda s:len(CompatHandler.reports)>previous_reports,'Compatibility API fixture must finish on Windows',timeout=100)
                api=CompatHandler.reports[-1]
                (output/f'api-result-{launch}.json').write_text(json.dumps(api,indent=2,ensure_ascii=False),encoding='utf-8')
                assert api['passed'],api
                assert api['capabilities']['initial_indexeddb']==('fictional-persistent-marker' if launch else ''),api
                wait(lambda s:any(t.get('title')=='Compatibility PASS' and not t.get('failed') for t in s.get('tabs',[])),'Native report must acknowledge compatibility page')
                # Real native spelling markers and replacement, with bundled dictionaries.
                assert read()['spelling_resources_ready'] and not read()['spelling']['remote']
                spelling_url=f'http://127.0.0.1:{server.server_port}/browser/spelling.html'
                click(480,55);type_text(spelling_url)
                user.SendMessageW(window,0x100,13,1);user.SendMessageW(window,0x101,13,1)
                wait(lambda s:any(t.get('title')=='Spelling ready' and t.get('spelling_supported') for t in s.get('tabs',[])),'Spelling document API must initialize')
                click_web(52,42);type_text('helllo ')
                wait(lambda s:any(t.get('title')=='Spelling [helllo ]' for t in s.get('tabs',[])),'Spelling fixture must receive native input')
                def spelling_context():
                    current=read();factor=current['interface']['applied']/100
                    scale=factor*current['interface']['device_scale']
                    point=round((x+52/factor)*scale)|(round((y+42/factor)*scale)<<16)
                    user.SendMessageW(window,0x204,2,point);user.SendMessageW(window,0x205,0,point)
                # The marker arrives asynchronously after the input event.
                deadline=time.monotonic()+30
                while True:
                    spelling_context();time.sleep(.2)
                    suggestions=read().get('menu_ui') or []
                    correction=next((i for i in suggestions if 200<=i['id']<=204 and i['label'].lower()=='hello'),None)
                    if correction:break
                    user.SendMessageW(window,0x100,27,1);user.SendMessageW(window,0x101,27,1)
                    if time.monotonic()>deadline:raise RuntimeError('Native English spelling suggestions missing on Windows')
                    time.sleep(.3)
                rect=correction['bounds'];click(rect[0]+rect[2]/2,rect[1]+rect[3]/2)
                wait(lambda s:any(t.get('title')=='Spelling [hello ]' for t in s.get('tabs',[])),'Native spelling replacement must update the field')
                popup_url=f'http://127.0.0.1:{server.server_port}/browser/popup-parent.html'
                click(480,55);type_text(popup_url)
                user.SendMessageW(window,0x100,13,1);user.SendMessageW(window,0x101,13,1)
                def popup_parent(s):
                    for t in s.get('tabs',[]):
                        if t['url']==popup_url:
                            try:return json.loads(t['title'])
                            except ValueError:pass
                    return {}
                wait(lambda s:popup_parent(s).get('parent'),'Popup fixture must load')
                tab_count=len(read()['tabs'])
                click_web(100,120)
                wait(lambda s:popup_parent(s).get('exchange'),'Windows popup must retain WindowProxy/opener/postMessage')
                assert len(read()['tabs'])==tab_count+1 and popup_parent(read())['returned']
                click(100,200) # Return to the original parent tab in the native sidebar.
                wait(lambda s:s['active_tab']==0,'Popup parent must activate')
                click_web(280,120)
                wait(lambda s:len(s.get('tabs',[]))==tab_count and popup_parent(s).get('closed'),'Script window.close must close the embedded popup')
                # Exercise real additional Win32 windows with the same profile/process.
                for _ in range(3):
                    user.SendMessageW(window, 0x100, 27, 1)
                    user.SendMessageW(window, 0x101, 27, 1)
                wait(lambda s: s.get('panel') == 0, 'Settings must close before opening the window menu')
                click(80, 35)
                menu = wait(lambda s: any(item['id'] == 1 for item in (s.get('menu_ui') or [])), 'Aura menu must expose New window')
                bounds = next(item['bounds'] for item in menu['menu_ui'] if item['id'] == 1)
                click(bounds[0]+bounds[2]//2, bounds[1]+bounds[3]//2)
                group = wait(lambda s: len(s.get('windows', [])) == 2, 'New window must be a second native window')
                child = next(w for w in group['windows'] if w['window'] != window)
                assert user.IsWindow(child['window']) and not child['private_window']
                assert child['pid'] == group['pid'] and child['settings']['language'] == 'en'
                assert child['settings']['ui_zoom'] == 125
                if launch == 0:
                    moved_browser = read()['tabs'][read()['active_tab']]['cef_browser_id']
                    assert moved_browser
                    created_total = read()['cef_created_total']
                    active_id = read()['tabs'][read()['active_tab']]['id']
                    row = next(r['bounds'] for r in read()['sidebar'] if r['tab_index'] >= 0 and read()['tabs'][r['tab_index']]['id'] == active_id)
                    scale = read()['interface']['device_scale']*read()['interface']['applied']/100
                    point = round((row[0]+row[2]/2)*scale) | (round((row[1]+row[3]/2)*scale)<<16)
                    user.SendMessageW(window, 0x204, 2, point)
                    user.SendMessageW(window, 0x205, 0, point)
                    menu = wait(lambda s: any(i['id'] == 13 for i in (s.get('menu_ui') or [])), 'Tab menu must expose Move to window')
                    rect = next(i['bounds'] for i in menu['menu_ui'] if i['id'] == 13)
                    click(rect[0]+rect[2]//2, rect[1]+rect[3]//2)
                    chooser = wait(lambda s: any(i['kind'] == 'window_move' for i in (s.get('commands_ui') or {}).get('items', [])), 'Window chooser must open')
                    item = next(i for i in chooser['commands_ui']['items'] if i['id'] == str(child['window_uid']))
                    assert item['enabled']
                    rect=item['bounds'];click(rect[0]+rect[2]//2, rect[1]+rect[3]//2)
                    group = wait(lambda s: any(w['window'] == child['window'] and any(t['cef_browser_id'] == moved_browser for t in w['tabs']) for w in s.get('windows', [])), 'Win32 transfer must retain the CEF browser')
                    assert all(t['cef_browser_id'] != moved_browser for t in group['tabs'])
                    assert group['cef_created_total'] == created_total, 'Transfer must not recreate a page'
                user.SendMessageW(child['window'], 0x10, 0, 0)
                wait(lambda s: len(s.get('windows', [])) == 1, 'Closing a child must keep the root open')
                forwarded = subprocess.run([str(binary), f'--data-dir={profile}', f'--url={url}', '--software-rendering'], capture_output=True, timeout=45)
                assert forwarded.returncode == 0, forwarded.stderr.decode(errors='replace')
                group = wait(lambda s: len(s.get('windows', [])) == 2 and any(w['window'] != window and any(t['url'] == url and t['loaded'] for t in w['tabs']) for w in s['windows']), 'A second process must forward its URL to Aura')
                child = next(w for w in group['windows'] if w['window'] != window)
                assert child['pid'] == group['pid']
                user.SendMessageW(child['window'], 0x10, 0, 0)
                wait(lambda s: len(s.get('windows', [])) == 1, 'Forwarded window must close independently')
                if launch == 1:
                    # Test the new caption callback through Win32 itself, not just synthetic clicks.
                    before_created = read()['cef_created_total']
                    def set_layout(value):
                        click(read()['interface']['width']-53, 55)
                        panel = wait(lambda s: bool(s.get('settings_ui')), 'Layout settings must open')
                        search = panel['settings_ui']['search_bounds']
                        click(search[0]+search[2]-20, search[1]+search[3]//2)
                        type_text('tab_layout')
                        panel = wait(lambda s: (s.get('settings_ui') or {}).get('search') == 'tab_layout', 'Layout search must accept text')
                        control = next(c for c in panel['settings_ui']['controls'] if c['id'] == 'tab_layout')
                        rect = next(o['bounds'] for o in control['options'] if o['value'] == value)
                        click(rect[0]+rect[2]//2, rect[1]+rect[3]//2)
                        wait(lambda s: bool(s.get('tab_strip')) == (value == 'horizontal'), 'Layout must reflow the native window')
                        user.SendMessageW(window, 0x100, 27, 1)
                        user.SendMessageW(window, 0x101, 27, 1)
                        wait(lambda s: s['panel'] == 0, 'Settings must close')
                    set_layout('horizontal')
                    def native_hit(rect):
                        info = read()['interface']
                        factor = info['device_scale']*info['applied']/100
                        point = wintypes.POINT(round((rect[0]+rect[2]/2)*factor), round((rect[1]+rect[3]/2)*factor))
                        assert user.ClientToScreen(window, ctypes.byref(point))
                        return user.SendMessageW(window, 0x84, 0, (point.x & 0xffff) | ((point.y & 0xffff) << 16))
                    strip = read()['tab_strip']
                    assert native_hit(strip['caption']) == 2, 'Blank strip must be HTCAPTION'
                    assert native_hit(strip['add']) == 1, 'New-tab button must remain HTCLIENT'
                    for item in strip['items']:
                        assert native_hit(item['bounds']) == 1, 'Tabs must not become draggable window captions'
                    target = next(item for item in strip['items'] if item['id'])
                    rect = target['bounds'];click(rect[0]+rect[2]//2, rect[1]+rect[3]//2)
                    wait(lambda s: s['tabs'][s['active_tab']]['id'] == target['id'], 'Native top-tab click must select its page')
                    assert read()['cef_created_total'] == before_created, 'Layout changes must not recreate pages'
                    set_layout('vertical')
                click(80, 35)
                menu = wait(lambda s: any(item['id'] == 2 for item in (s.get('menu_ui') or [])), 'Aura menu must expose New private window')
                bounds = next(item['bounds'] for item in menu['menu_ui'] if item['id'] == 2)
                click(bounds[0]+bounds[2]//2, bounds[1]+bounds[3]//2)
                group = wait(lambda s: len(s.get('windows', [])) == 2, 'Private native window must open')
                private = next(w for w in group['windows'] if w['window'] != window)
                assert private['private_window'] and all(t['private'] for t in private['tabs'])
                user.SendMessageW(window, 0x10, 0, 0)
                wait(lambda s: len(s.get('windows', [])) == 1 and s['windows'][0]['window'] == private['window'], 'Private child must survive closing the root')
                user.SendMessageW(private['window'], 0x10, 0, 0)
                process.wait(timeout=30)
                assert process.returncode == 0, process.returncode
                final = read()
                assert final['cef_created_total'] == final['cef_closed_total'], final
                assert (profile/'profile.toml').is_file(), 'Profile must persist'
                process = None
        (output/'PASS.txt').write_text('PASS: full Win32 browser, Chromium page, Unicode path/input, pointer navigation, native dialog, English, interface scaling, adblock, real API/media/CSP/CORS/origin and IndexedDB persistence, profile replacement, horizontal/vertical reflow, Win32 HTCLIENT/HTCAPTION separation, live cross-window transfer and balanced CEF shutdown.\n')
    finally:
        if process and process.poll() is None:
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], check=False)
        server.shutdown()
        for source in compat_servers:source.shutdown();source.server_close()

if __name__ == '__main__':
    main()
