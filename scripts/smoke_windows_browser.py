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


def main():
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('This GUI smoke test runs only on the disposable Windows CI runner')
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.SendMessageW.restype = ctypes.c_ssize_t
    user.IsWindow.argtypes = [wintypes.HWND]
    user.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    output = ROOT/'artifacts/windows-smoke'
    output.mkdir(parents=True, exist_ok=True)
    profile = output/'profile-кириллица'
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    (profile/'profile.toml').write_text('language="ru"\nrestore=false\n', encoding='utf-8')
    server = ThreadingHTTPServer(('127.0.0.1', 0), partial(SimpleHTTPRequestHandler, directory=str(ROOT/'tests/fixtures')))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{server.server_port}/browser/a.html'
    binary = Path(os.environ.get('AURA_TEST_BINARY', str(ROOT/'build/windows-x64-release/aura_browser.exe')))
    process = None
    try:
        for launch in range(2):
            report_path = output/f'report-{launch}.json'
            report_path.unlink(missing_ok=True)
            def read():
                try: return json.loads(report_path.read_text(encoding='utf-8'))
                except (OSError, ValueError): return {}
            def wait(predicate, message, timeout=45):
                deadline = time.monotonic()+timeout
                while time.monotonic()<deadline:
                    state = read()
                    if predicate(state): return state
                    if process.poll() is not None: raise RuntimeError(f'Browser exited ({process.returncode}): {message}')
                    time.sleep(.1)
                raise RuntimeError(message)
            with (output/f'process-{launch}.log').open('w') as log:
                process = subprocess.Popen([str(binary), f'--data-dir={profile}', f'--report={report_path}', '--software-rendering', f'--url={url}'], stdout=log, stderr=subprocess.STDOUT)
                state = wait(lambda s: any(t.get('title') == 'Alpha' and t.get('frames', 0) > 0 for t in s.get('tabs', [])), 'Alpha must load and paint')
                window = state['window']
                assert user.IsWindow(window), 'Report must identify the real native browser window'
                user.GetClassLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
                user.GetClassLongPtrW.restype = ctypes.c_size_t
                assert user.GetClassLongPtrW(window, -14), 'Window class must expose the Aura large icon'
                assert user.GetClassLongPtrW(window, -34), 'Window class must expose the Aura small icon'
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
                user.SendMessageW(window, 0x10, 0, 0)
                process.wait(timeout=30)
                assert process.returncode == 0, process.returncode
                final = read()
                assert final['cef_created'] == final['cef_closed'], final
                assert (profile/'profile.toml').is_file(), 'Profile must persist'
                process = None
        (output/'PASS.txt').write_text('PASS: full Win32 browser, Chromium page, Unicode path/input, pointer navigation, native dialog, English, interface scaling, adblock, profile replacement and balanced CEF shutdown.\n')
    finally:
        if process and process.poll() is None:
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], check=False)
        server.shutdown()

if __name__ == '__main__':
    main()
