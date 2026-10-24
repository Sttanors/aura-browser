"""Real page zoom: per host/container, private isolation, reset, disable and restart."""
import json
import os
import re
import shutil
import signal
import subprocess
import threading
import time
import tomllib
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT / 'artifacts/site-zoom-smoke'
    out.mkdir(parents=True, exist_ok=True)
    profile = out / 'profile'
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    config = profile / 'profile.toml'
    config.write_text('language="ru"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n'
                      '[[containers]]\nid=7\nname="Work"\ncolor=1\n')
    with ThreadingHTTPServer(('127.0.0.1', 0), partial(QuietHandler, directory=str(ROOT / 'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f'http://127.0.0.1:{server.server_port}/a.html'
        for launch in range(3):
            if launch == 2:
                config.write_text(re.sub(r'(?m)^remember_site_zoom\s*=.*$', 'remember_site_zoom = false', config.read_text()))
            report = out / f'report-{launch}.json'
            report.unlink(missing_ok=True)
            with (out / f'process-{launch}.log').open('w') as log:
                proc = subprocess.Popen([str(ROOT / 'build/linux-x64-release/aura_browser'),
                                         f'--data-dir={profile}', f'--report={report}'] +
                                        ([f'--url={url}'] if launch == 0 else []),
                                        stdout=log, stderr=log, start_new_session=True,
                                        env={**os.environ, 'AURA_BACKGROUND_TEST': '1'})
                try:
                    def state():
                        if proc.poll() is not None:
                            raise RuntimeError('Site zoom browser exited')
                        try:
                            return json.loads(report.read_text())
                        except (OSError, ValueError):
                            return {}
                    def current():
                        s = state()
                        return s['tabs'][s['active_tab']]
                    wait_until(lambda: state().get('tabs') and all(t['loaded'] for t in state()['tabs']), 'No loaded zoom tabs', timeout=35)
                    w = state()['window']
                    def zoom(value):
                        wait_until(lambda: current()['zoom_percent'] == value, f'Expected native zoom {value}: ' + str(current()))
                    def navigate(address):
                        key_at(w, 'l', 4)
                        type_text(w, address)
                        key_at(w, 'Return')
                        wait_until(lambda: current()['loaded'] and current()['url'] == address, 'Zoom navigation incomplete')
                    def click(bounds):
                        click_at(w, bounds[0] + bounds[2] // 2, bounds[1] + bounds[3] // 2)
                    if launch == 0:
                        zoom(100)
                        key_at(w, 'equal', 4)
                        zoom(120)
                        key_at(w, 't', 4)
                        navigate(url + '?second')
                        zoom(120)
                        navigate(url.replace('127.0.0.1', 'localhost'))
                        zoom(100)
                        key_at(w, 'c', 12)
                        wait_until(lambda: state().get('containers_ui'), 'No zoom container manager')
                        click(next(c['new_tab'] for c in state()['containers_ui']['items'] if c['id'] == 7))
                        navigate(url)
                        assert current()['container'] == 7
                        zoom(100)
                        key_at(w, 'minus', 4)
                        zoom(83)
                        key_at(w, 'n', 12)
                        navigate(url + '?PRIVATE_ZOOM_SENTINEL')
                        zoom(100)
                        key_at(w, 'equal', 4)
                        key_at(w, 'equal', 4)
                        zoom(144)
                        assert state()['tabs'][0]['zoom_percent'] == 120
                        assert next(t for t in state()['tabs'] if t['container'] == 7)['zoom_percent'] == 83
                    elif launch == 1:
                        wait_until(lambda: state()['tabs'][0]['zoom_percent'] == 120 and
                                   next(t for t in state()['tabs'] if t['container'] == 7)['zoom_percent'] == 83,
                                   'Site zoom did not survive restart')
                        key_at(w, 'n', 12)
                        navigate(url + '?PRIVATE_ZOOM_SENTINEL')
                        zoom(100)
                        key_at(w, 'w', 4)
                        key_at(w, '1', 4)
                        key_at(w, '0', 4)
                        zoom(100)
                        assert next(t for t in state()['tabs'] if t['container'] == 7)['zoom_percent'] == 83
                    else:
                        wait_until(lambda: all(t['zoom_percent'] == 100 for t in state()['tabs']), 'Disabled remembered zoom still applies')
                        key_at(w, 'equal', 4)
                        zoom(120)
                        click_at(w, state()['interface']['width'] - 53, 55)
                        wait_until(lambda: state().get('settings_ui'), 'No zoom settings')
                        key_at(w, 'f', 4)
                        type_text(w, 'remember_site_zoom')
                        key_at(w, 'Return')
                        wait_until(lambda: any(c['id'] == 'remember_site_zoom' for c in state()['settings_ui']['controls']), 'No remembered zoom toggle')
                        row = next(c['bounds'] for c in state()['settings_ui']['controls'] if c['id'] == 'remember_site_zoom')
                        click_at(w, row[0] + row[2] - 22, row[1] + 26)
                        wait_until(lambda: state()['settings']['remember_site_zoom'], 'Zoom toggle did not enable')
                        wait_until(lambda: next(t for t in state()['tabs'] if t['container'] == 7)['zoom_percent'] == 83, 'Toggle did not apply saved container zoom')
                        click_at(w, row[0] + row[2] - 22, row[1] + 26)
                        wait_until(lambda: not state()['settings']['remember_site_zoom'] and all(t['zoom_percent'] == 100 for t in state()['tabs']), 'Toggle did not disable remembered zoom')
                        subprocess.run(['import', '-window', str(w), str(out / 'ru-setting.png')], check=True, timeout=10)
                        key_at(w, 'Escape')
                    key_at(w, 'q', 4)
                    proc.wait(timeout=25)
                    assert proc.returncode == 0
                finally:
                    if proc.poll() is None:
                        os.killpg(proc.pid, signal.SIGTERM)
                        proc.wait(timeout=5)
            saved = tomllib.loads(config.read_text())
            rules = saved.get('site_zooms', [])
            assert len(rules) == (2 if launch == 0 else 1), rules
            assert all(rule['level'] != 2 for rule in rules), 'Private zoom was persisted'
            if launch:
                assert rules[0]['container'] == 7 and rules[0]['level'] == -1, rules
            for p in profile.glob('profile.toml*'):
                assert 'PRIVATE_ZOOM_SENTINEL' not in p.read_text()
        server.shutdown()
    print('PASS: native per-host zoom, new tabs/navigation, container isolation, private ephemeral zoom, reset, disable and restart')


if __name__ == '__main__':
    main()
