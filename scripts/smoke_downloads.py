"""Shared native download manager: progress/control/retry/SHA-256/private boundaries."""
import hashlib
import json
import os
import shutil
import signal
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at

DATA = b'abc'


class Handler(BaseHTTPRequestHandler):
    fail = True

    def log_message(self, *args):
        pass

    def do_GET(self):
        if not self.path.endswith('.bin'):
            body = b'<title>Download fixture</title>Downloads'
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        slow = self.path == '/slow.bin'
        broken = self.path == '/broken.bin' and self.fail
        size = 32 * 1024 * 1024 if slow else 1024 * 1024 if broken else len(DATA)
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Disposition', 'attachment; filename="' + self.path[1:] + '"')
        self.send_header('Content-Length', str(size))
        self.end_headers()
        try:
            if slow:
                for _ in range(1024):
                    self.wfile.write(b'x' * 32768)
                    self.wfile.flush()
                    time.sleep(.1)
            elif broken:
                self.wfile.write(b'x' * 1024)
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
            else:
                self.wfile.write(DATA)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT / 'artifacts/downloads-smoke'
    out.mkdir(parents=True, exist_ok=True)
    profile = out / 'profile'
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    (profile / 'profile.toml').write_text('language="ru"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    report = out / 'report.json'
    report.unlink(missing_ok=True)
    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = f'http://127.0.0.1:{server.server_port}'
        with (out / 'process.log').open('w') as log:
            proc = subprocess.Popen([str(ROOT / 'build/linux-x64-release/aura_browser'), f'--data-dir={profile}',
                                     f'--report={report}', f'--url={base}/source'], stdout=log, stderr=log,
                                    start_new_session=True, env={**os.environ, 'AURA_BACKGROUND_TEST': '1'})
            try:
                def state():
                    if proc.poll() is not None:
                        raise RuntimeError('Downloads browser exited')
                    try:
                        return json.loads(report.read_text())
                    except (OSError, ValueError):
                        return {}
                def windows():
                    return state().get('windows', [])
                def window(w):
                    return next((s for s in windows() if s['window'] == w), {})
                wait_until(lambda: windows() and windows()[0]['tabs'][0]['loaded'], 'No source')
                first = windows()[0]['window']
                def navigate(w, path):
                    key_at(w, 'Escape')
                    key_at(w, 'l', 4)
                    type_text(w, base + path)
                    key_at(w, 'Return')
                def new_window(w, private=False):
                    old = {s['window'] for s in windows()}
                    key_at(w, 'n', 5 if private else 4)
                    wait_until(lambda: any(s['window'] not in old for s in windows()), 'No second window')
                    return next(s['window'] for s in windows() if s['window'] not in old)
                def records(w):
                    return window(w).get('downloads', [])
                def item(w, name):
                    return next((d for d in records(w) if d['name'] == name), {})
                def row(w, d):
                    return next(r for r in window(w)['downloads_ui']['items'] if r['id'] == d['id'] and r['source_window'] == d['source_window'])
                def click(w, r):
                    click_at(w, r[0]+r[2]//2, r[1]+r[3]//2)
                def panel(w):
                    key_at(w, 'j', 4)
                    wait_until(lambda: window(w).get('downloads_ui'), 'No download panel')
                def shot(w, name):
                    subprocess.run(['xdotool', 'windowraise', str(w)], check=True)
                    time.sleep(.3)
                    subprocess.run(['import', '-window', str(w), str(out/name)], check=True, timeout=10)
                # Start in one window, inspect and control from another.
                navigate(first, '/slow.bin')
                wait_until(lambda: item(first, 'slow.bin').get('speed', 0) > 0, 'No actual transfer speed', timeout=30)
                second = new_window(first)
                panel(second)
                wait_until(lambda: item(second, 'slow.bin').get('received', 0) > 0, 'Downloads not shared')
                navigate(second, '/sleep-target')
                wait_until(lambda: any(t['loaded'] and t['title'] == 'Download fixture' for t in window(second)['tabs']), 'No second-window page')
                time.sleep(2)
                key_at(second, 's', 12)
                wait_until(lambda: any(t['sleeping'] for t in window(second)['tabs']), 'Other-window download incorrectly blocks sleep')
                panel(second)
                shot(second, 'ru-speed.png')
                click(second, row(second, item(second, 'slow.bin'))['pause'])
                wait_until(lambda: item(second, 'slow.bin')['paused'], 'Cross-window pause failed')
                click(second, row(second, item(second, 'slow.bin'))['pause'])
                wait_until(lambda: not item(second, 'slow.bin')['paused'], 'Resume failed')
                click(second, row(second, item(second, 'slow.bin'))['cancel'])
                wait_until(lambda: item(second, 'slow.bin')['cancelled'], 'Cancel failed')
                navigate(first, '/broken.bin')
                wait_until(lambda: item(second, 'broken.bin').get('interrupted'), 'No interrupted download', timeout=40)
                Handler.fail = False
                panel(second)
                click(second, row(second, item(second, 'broken.bin'))['action'])
                wait_until(lambda: any(d['complete'] and d['name'].startswith('broken') for d in records(second)), 'Retry did not complete', timeout=30)
                completed = next(d for d in records(second) if d['complete'] and d['name'].startswith('broken'))
                assert completed['name'] == 'broken (1).bin', completed['name']
                # Compare a known file on a background CEF file thread.
                panel(second)
                click(second, row(second, completed)['action'])
                wait_until(lambda: window(second)['dialog_count'] == 1, 'No checksum prompt')
                type_text(second, 'invalid')
                key_at(second, 'Return')
                time.sleep(.4)
                assert window(second)['dialog_count'] == 1
                key_at(second, 'a', 4)
                type_text(second, hashlib.sha256(DATA).hexdigest().upper())
                key_at(second, 'Return')
                wait_until(lambda: any(d['checksum_status'] == 'match' for d in records(second)), 'Correct checksum did not match')
                wait_until(lambda: window(second)['dialog_count'] == 1, 'No checksum result')
                shot(second, 'ru-sha-match.png')
                key_at(second, 'Escape')
                click(second, row(second, completed)['action'])
                wait_until(lambda: window(second)['dialog_count'] == 1, 'No second checksum prompt')
                type_text(second, '0'*64)
                key_at(second, 'Return')
                wait_until(lambda: any(d['checksum_status'] == 'mismatch' for d in records(second)), 'Mismatch not detected')
                wait_until(lambda: window(second)['dialog_count'] == 1, 'No mismatch result')
                key_at(second, 'Escape')
                # A private window has neither the normal list nor leaked records in another window.
                private = new_window(second, True)
                panel(private)
                assert not records(private)
                navigate(private, '/private.bin')
                wait_until(lambda: item(private, 'private.bin').get('complete'), 'No private download')
                assert not item(second, 'private.bin') and not item(first, 'private.bin')
                key_at(private, 'w', 5)
                wait_until(lambda: not window(private), 'Private window did not close')
                # Public records outlive the original source window.
                key_at(first, 'w', 5)
                wait_until(lambda: not window(first), 'Source window did not close')
                assert any(d['complete'] and d['name'].startswith('broken') for d in records(second))
                panel(second)
                before = len(records(second))
                click(second, row(second, item(second, 'slow.bin'))['action'])
                wait_until(lambda: len(records(second)) > before and records(second)[0]['received'] > 0, 'Closed-source retry failed')
                assert records(second)[0]['source_window'] != completed['source_window']
                panel(second)
                click(second, row(second, records(second)[0])['cancel'])
                wait_until(lambda: records(second)[0]['cancelled'], 'Closed-source retry cancel failed')
                shot(second, 'ru-shared-final.png')
                key_at(second, 'q', 4)
                proc.wait(timeout=30)
                assert proc.returncode == 0
                final = json.loads(report.read_text())
                assert final['cef_created_total'] == final['cef_closed_total']
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=5)
        server.shutdown()
    print('PASS: shared progress/ETA controls, pause/resume/cancel, interrupted retry, SHA match/mismatch, private isolation and closed-source history')


if __name__ == '__main__':
    main()
