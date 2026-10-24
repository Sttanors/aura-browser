"""A long download protects its source tab, while another tab can freeze."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/slow.bin':
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Disposition', 'attachment; filename="slow.bin"')
            self.send_header('Content-Length', str(16 * 1024 * 1024))
            self.end_headers()
            try:
                for _ in range(1024):
                    self.wfile.write(b'x' * 16384)
                    self.wfile.flush()
                    time.sleep(.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        body = ('<!doctype html><title>Download source</title><a href="/slow.bin" download '
                'style="position:absolute;left:20px;top:20px;width:300px;height:120px">Download</a>'
                if self.path == '/source' else '<!doctype html><title>Sleep target</title>Target')
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT / 'artifacts/download-sleep-smoke'
    out.mkdir(parents=True, exist_ok=True)
    profile = out / 'profile'
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    (profile / 'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\n'
                                        'filter_updates=false\nopen_downloads=false\n')
    report = out / 'report.json'
    report.unlink(missing_ok=True)
    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = f'http://127.0.0.1:{server.server_port}'
        with (out / 'process.log').open('w') as log:
            proc = subprocess.Popen([str(ROOT / 'build/linux-x64-release/aura_browser'),
                                     f'--data-dir={profile}', f'--report={report}',
                                     f'--url={base}/source'], stdout=log, stderr=log,
                                    env={**os.environ, 'AURA_BACKGROUND_TEST': '1'}, start_new_session=True)
            try:
                def state():
                    if proc.poll() is not None:
                        raise RuntimeError('Download/sleep browser exited')
                    try:
                        return json.loads(report.read_text())
                    except (OSError, ValueError):
                        return {}
                wait_until(lambda: state().get('tabs') and state()['tabs'][0]['loaded'], 'No source')
                w = state()['window']
                click_at(w, 330, 180)
                wait_until(lambda: any(d['received'] > 0 and not d['complete'] for d in state()['downloads']), 'No active download')
                key_at(w, 't', 4)
                key_at(w, 'l', 4)
                type_text(w, base + '/target')
                key_at(w, 'Return')
                wait_until(lambda: len(state()['tabs']) == 2 and state()['tabs'][1]['loaded'], 'No target')
                time.sleep(2)
                key_at(w, 's', 12)
                try:
                    wait_until(lambda: state()['tabs'][1]['sleeping'], 'Unrelated tab cannot sleep during another tab download', timeout=12)
                finally:
                    evidence = {'tabs': [(t['id'], t['sleeping'], t['url']) for t in state()['tabs']],
                                'downloads': state()['downloads']}
                    (out / 'evidence.json').write_text(json.dumps(evidence, indent=2))
                    print(json.dumps(evidence), flush=True)
                assert not state()['tabs'][0]['sleeping']
                assert any(d['received'] > 0 and not d['complete'] for d in state()['downloads'])
                key_at(w, '1', 4)
                before = state()['active_tab']
                key_at(w, 's', 12)
                time.sleep(1)
                assert not state()['tabs'][0]['sleeping'] and state()['active_tab'] == before
                key_at(w, 'q', 4)
                proc.wait(timeout=20)
                assert proc.returncode == 0
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=5)
        server.shutdown()
    print('PASS: active download protects only its source; unrelated page freezes; source rejection keeps focus')


if __name__ == '__main__':
    main()
