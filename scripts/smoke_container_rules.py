"""Container rules intercept real GET/redirect/POST before crossing cookie contexts."""
import json
import os
import re
import shutil
import signal
import subprocess
import threading
import time
import tomllib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at


class Handler(BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        self.requests.append(('POST', self.path, self.headers.get('Cookie', '')))
        self.reply('<title>POST was sent</title>')

    def reply(self, body):
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        if self.path.startswith('/work'):
            self.send_header('Set-Cookie', 'context=work; Path=/')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.requests.append(('GET', self.path, self.headers.get('Cookie', '')))
        target = f'http://localhost:{self.server.server_port}/work'
        if self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', target + '?redirect')
            self.end_headers()
            return
        if self.path == '/source':
            self.reply(f'''<title>Source preserved</title>
                <a style="position:absolute;left:20px;top:20px" href="{target}?link">Open work</a>
                <form method="post" action="{target}?post"><button style="position:absolute;left:20px;top:100px;width:180px;height:40px">Send form</button></form>''')
        else:
            self.reply('<title>Work page</title>')


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT / 'artifacts/container-rules-smoke'
    out.mkdir(parents=True, exist_ok=True)
    profile = out / 'profile'
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    (profile / 'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = f'http://127.0.0.1:{server.server_port}'
        work = f'http://localhost:{server.server_port}/work'
        for launch in range(2):
            if launch:
                path = profile / 'profile.toml'
                path.write_text(re.sub(r'(?m)^language\s*=.*$', 'language="ru"', path.read_text()))
            report = out / f'report-{launch}.json'
            report.unlink(missing_ok=True)
            with (out / f'process-{launch}.log').open('w') as log:
                proc = subprocess.Popen([str(ROOT / 'build/linux-x64-release/aura_browser'), f'--data-dir={profile}',
                                         f'--report={report}', f'--url={base}/source'], stdout=log, stderr=log,
                                        start_new_session=True, env={**os.environ, 'AURA_BACKGROUND_TEST': '1'})
                try:
                    def state():
                        if proc.poll() is not None:
                            raise RuntimeError('Rule browser exited')
                        try:
                            return json.loads(report.read_text())
                        except (OSError, ValueError):
                            return {}
                    def current():
                        s = state()
                        return s.get('tabs', [{}])[s.get('active_tab', 0)]
                    wait_until(lambda: current().get('title') == 'Source preserved' and current()['loaded'], 'No source')
                    w = state()['window']
                    def click(r):
                        click_at(w, r[0] + r[2] // 2, r[1] + r[3] // 2)
                    def navigate(url):
                        key_at(w, 'l', 4)
                        type_text(w, url)
                        key_at(w, 'Return')
                    def manager():
                        key_at(w, 'c', 12)
                        wait_until(lambda: state().get('containers_ui'), 'No manager')
                    def rules():
                        click(state()['containers_ui']['rules_button'])
                        wait_until(lambda: state()['containers_ui'].get('rules_mode'), 'No rules editor')
                    def enter(text):
                        click(state()['containers_ui']['input'])
                        key_at(w, 'a', 4)
                        type_text(w, text)
                    if not launch:
                        manager()
                        enter('Work')
                        key_at(w, 'Return')
                        wait_until(lambda: len(state()['containers_ui']['items']) == 2, 'No Work container')
                        identity = state()['containers_ui']['items'][1]['id']
                        rules()
                        click(state()['containers_ui']['selector'])
                        key_at(w, 'End')
                        key_at(w, 'Return')
                        wait_until(lambda: state()['containers_ui']['target'] == identity, 'Wrong target')
                        enter('localhost/path')
                        key_at(w, 'Return')
                        wait_until(lambda: state()['containers_ui']['error'], 'Invalid host accepted')
                        enter('LOCALHOST')
                        key_at(w, 'Return')
                        wait_until(lambda: len(state()['containers_ui']['items']) == 1, 'No rule')
                        assert state()['containers_ui']['items'][0]['host'] == 'localhost'
                        click(state()['containers_ui']['items'][0]['edit'])
                        click(state()['containers_ui']['subdomains_button'])
                        click(state()['containers_ui']['save'])
                        wait_until(lambda: state()['containers_ui']['items'][0]['subdomains'], 'Rule edit failed')
                        subprocess.run(['import', '-window', str(w), str(out / 'en-rules.png')], check=True, timeout=10)
                        key_at(w, 'Escape')
                        # A native address-bar navigation is stopped before network/CEF mutation.
                        before = len(Handler.requests)
                        navigate(work + '?cancel')
                        wait_until(lambda: state()['dialog_count'] == 1, 'No confirmation')
                        assert len(Handler.requests) == before
                        subprocess.run(['import', '-window', str(w), str(out / 'en-confirm.png')], check=True, timeout=10)
                        key_at(w, 'Escape')
                        wait_until(lambda: state()['dialog_count'] == 0, 'Cancel failed')
                        assert current()['title'] == 'Source preserved' and len(state()['tabs']) == 1
                        # Actual page link is also intercepted; accept creates a separate context.
                        click_at(w, 300, 118)
                        wait_until(lambda: state()['dialog_count'] == 1, 'Link bypassed rule')
                        assert not any('?link' in x[1] for x in Handler.requests)
                        key_at(w, 'Return')
                        wait_until(lambda: len(state()['tabs']) == 2 and current()['title'] == 'Work page' and current()['loaded'], 'No routed tab')
                        assert current()['container'] == identity
                        key_at(w, 'l', 4)
                        type_text(w, work + '?same')
                        key_at(w, 'Return')
                        wait_until(lambda: any('?same' in x[1] for x in Handler.requests), 'Same context failed')
                        assert any('?same' in x[1] and 'context=work' in x[2] for x in Handler.requests)
                        assert state()['dialog_count'] == 0
                        key_at(w, '1', 4)
                        # POST must neither cross contexts nor be changed into a GET.
                        click_at(w, 330, 208)
                        time.sleep(1)
                        assert not any('?post' in x[1] for x in Handler.requests)
                        assert current()['title'] == 'Source preserved'
                        # Redirects cannot silently move a normal context onto a work host.
                        navigate(base + '/redirect')
                        wait_until(lambda: state()['dialog_count'] == 1, 'Redirect bypassed rule')
                        assert not any('?redirect' in x[1] for x in Handler.requests)
                        key_at(w, 'Escape')
                        # Private navigation ignores persistent routing and does not receive work cookies.
                        key_at(w, 'n', 12)
                        navigate(work + '?private')
                        wait_until(lambda: current().get('private') and current()['title'] == 'Work page', 'Private routed out')
                        assert state()['dialog_count'] == 0 and current()['container'] == 0
                        assert any('?private' in x[1] and 'context=work' not in x[2] for x in Handler.requests)
                        key_at(w, 'w', 4)
                    else:
                        manager()
                        rules()
                        assert state()['containers_ui']['items'][0]['container'] == identity
                        subprocess.run(['import', '-window', str(w), str(out / 'ru-rules.png')], check=True, timeout=10)
                        subprocess.run(['xdotool', 'windowsize', str(w), '1000', '740'], check=True)
                        wait_until(lambda: state()['interface']['width'] == 1000, 'Resize failed')
                        time.sleep(.3)
                        subprocess.run(['import', '-window', str(w), str(out / 'ru-rules-small.png')], check=True, timeout=10)
                        click(state()['containers_ui']['items'][0]['remove'])
                        wait_until(lambda: not state()['containers_ui']['items'], 'Remove failed')
                        key_at(w, 'Escape')
                        navigate(work + '?removed')
                        wait_until(lambda: current()['title'] == 'Work page', 'Deleted rule still intercepts')
                        assert current()['container'] == 0 and state()['dialog_count'] == 0
                    key_at(w, 'q', 4)
                    proc.wait(timeout=30)
                    assert proc.returncode == 0
                finally:
                    if proc.poll() is None:
                        os.killpg(proc.pid, signal.SIGTERM)
                        proc.wait(timeout=5)
            saved = tomllib.loads((profile / 'profile.toml').read_text())
            assert len(saved['container_rules']) == (0 if launch else 1)
            assert saved['language'] == ('ru' if launch else 'en')
        server.shutdown()
    (out / 'requests.json').write_text(json.dumps(Handler.requests, indent=2))
    print('PASS: rule UI/persistence/RU/EN, cancel, native navigation, links, redirects, POST block, container cookies and private isolation')


if __name__ == '__main__':
    main()
