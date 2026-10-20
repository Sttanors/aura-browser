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
    server = ThreadingHTTPServer(('127.0.0.1', 0), partial(SimpleHTTPRequestHandler, directory=str(ROOT/'tests/fixtures')))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://127.0.0.1:{server.server_port}/browser/a.html'
    binary = ROOT/'build/windows-x64-release/aura_browser.exe'
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
                assert state['adblock_ready'], 'Rust ad blocker must initialize'
                assert 'DIB' in state['renderer'], state['renderer']
                def click(x, y):
                    scale = state['interface']['device_scale']*state['interface']['applied']/100
                    point = round(x*scale) | (round(y*scale)<<16)
                    user.SendMessageW(window, 0x201, 1, point)
                    user.SendMessageW(window, 0x202, 0, point)
                # Shared UI web bounds: sidebar 240 DIP, toolbar ends at 88 DIP.
                # Read the reported web rectangle instead of relying on window decorations.
                tab = next(t for t in state['tabs'] if t.get('title') == 'Alpha')
                bounds = tab.get('bounds', [252, 88, 1000, 700])
                x, y = (bounds['x'], bounds['y']) if isinstance(bounds, dict) else bounds[:2]
                click(x+120, y+165)
                for char in 'Windows Привет': user.SendMessageW(window, 0x102, ord(char), 1)
                wait(lambda s: any(t.get('title') == 'Alpha input Windows Привет' for t in s.get('tabs', [])), 'Win32 Unicode typing must reach the real web input')
                click(x+100, y+285)
                wait(lambda s: s.get('dialog_count') == 1, 'JavaScript dialog must open')
                user.SendMessageW(window, 0x100, 13, 1)
                user.SendMessageW(window, 0x101, 13, 1)
                wait(lambda s: any(t.get('title') == 'Alpha dialog accepted' for t in s.get('tabs', [])), 'Native dialog must accept Return')
                click(x+100, y+108)
                wait(lambda s: any(t.get('title') == 'Beta' for t in s.get('tabs', [])), 'Native pointer input must navigate to Beta')
                user.SendMessageW(window, 0x10, 0, 0)
                process.wait(timeout=30)
                assert process.returncode == 0, process.returncode
                final = read()
                assert final['cef_created'] == final['cef_closed'], final
                assert (profile/'profile.toml').is_file(), 'Profile must persist'
                process = None
        (output/'PASS.txt').write_text('PASS: full Win32 browser, Chromium page, Unicode path/input, pointer navigation, native dialog, adblock, profile replacement and balanced CEF shutdown.\n')
    finally:
        if process and process.poll() is None:
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], check=False)
        server.shutdown()

if __name__ == '__main__':
    main()
