"""Exercise real Chromium freezing, forms, wake and sleep exclusions."""
import json
import os
import shutil
import signal
import subprocess
import threading
import time
from functools import partial
from http.server import ThreadingHTTPServer
from dependencies import ROOT
from smoke import QuietHandler
from smoke_browser import wait_until, type_text
from smoke_window import key_at, click_at


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT / 'artifacts/sleep-smoke'
    out.mkdir(parents=True, exist_ok=True)
    with ThreadingHTTPServer(('127.0.0.1', 0), partial(QuietHandler, directory=str(ROOT / 'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f'http://127.0.0.1:{server.server_port}/sleep.html'
        for mode in (('manual', 'exceptions') if os.environ.get('AURA_SLEEP_MANUAL_ONLY') else ('manual', 'exceptions', 'auto')):
            profile = out / mode
            shutil.rmtree(profile, ignore_errors=True)
            profile.mkdir()
            prefs = 'language="ru"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n'
            if mode == 'exceptions':
                prefs += 'tab_sleep_exceptions="127.0.0.1"\n'
            if mode == 'auto':
                prefs += 'tab_sleep_minutes=5\n'
            (profile / 'profile.toml').write_text(prefs)
            report = out / f'{mode}.json'
            report.unlink(missing_ok=True)
            with (out / f'{mode}.log').open('w') as log:
                p = subprocess.Popen([str(ROOT / 'build/linux-x64-release/aura_browser'), f'--data-dir={profile}', f'--report={report}', f'--url={url}'], stdout=log, stderr=log, start_new_session=True, env={**os.environ, 'AURA_BACKGROUND_TEST': '1', 'XMODIFIERS': '@im=none'})
                try:
                    def state():
                        if p.poll() is not None:
                            raise RuntimeError('Sleep browser exited')
                        try:
                            return json.loads(report.read_text())
                        except (OSError, ValueError):
                            return {}
                    def first():
                        return state().get('tabs', [{}])[0]
                    def doc():
                        try:
                            return json.loads(first().get('title', ''))
                        except ValueError:
                            return {}
                    wait_until(lambda: first().get('loaded') and doc().get('ticks', 0) > 2, 'No lifecycle document', timeout=30)
                    w = state()['window']
                    token = doc()['token']
                    click_at(w, 352, 245)
                    type_text(w, 'Unsaved_data')
                    wait_until(lambda: doc().get('value') == 'Unsaved_data', 'No form input')
                    click_at(w, 352, 300)
                    wait_until(lambda: doc().get('protected'), 'No beforeunload handler')
                    if mode == 'auto':
                        key_at(w, 't', 4)
                        started = time.monotonic()
                        time.sleep(2)
                        assert not first()['sleeping'], 'Auto sleep fired before timeout'
                        print('Waiting for the actual 5-minute auto-sleep timeout', flush=True)
                        wait_until(lambda: first()['sleeping'], 'Automatic timeout did not freeze the tab', timeout=325)
                        assert time.monotonic() - started >= 298
                    else:
                        key_at(w, 's', 12)
                        if mode == 'exceptions':
                            time.sleep(2)
                            assert not first()['sleeping'] and first()['buffer_bytes'] > 0
                            key_at(w, '1', 4)
                            assert doc()['value'] == 'Unsaved_data'
                        else:
                            wait_until(lambda: first()['sleeping'], 'Manual sleep failed')
                    if mode != 'exceptions':
                        assert first()['buffer_bytes'] == 0, 'Sleeping tab retains native bitmap'
                        time.sleep(1)
                        frozen = doc()['ticks']
                        time.sleep(2)
                        assert doc()['ticks'] == frozen, 'Frozen timer keeps running'
                        assert state()['dialog_count'] == 0 and state()['cef_created'] == 1
                        subprocess.run(['import', '-window', str(w), str(out / f'{mode}.png')], check=True, timeout=10)
                        key_at(w, '1', 4)
                        wait_until(lambda: not first()['sleeping'] and first()['buffer_bytes'] > 0 and doc().get('ticks', 0) > frozen, 'Wake/repaint failed')
                        assert doc()['token'] == token and doc()['value'] == 'Unsaved_data' and doc()['protected'], 'Wake reloaded/lost the document'
                    if mode == 'manual':
                        key_at(w, 's', 12)
                        wait_until(lambda: first()['sleeping'], 'Second sleep failed')
                        key_at(w, 'q', 4)
                        wait_until(lambda: state()['dialog_count'] == 1, 'Frozen beforeunload was skipped')
                        key_at(w, 'Escape')
                        wait_until(lambda: state()['dialog_count'] == 0 and not first()['sleeping'], 'Cancel close did not restore frozen page')
                        key_at(w, '1', 4)
                        assert doc()['value'] == 'Unsaved_data' and doc()['protected']
                        # Pinned, private and active Web Audio tabs must not freeze.
                        click_at(w, 90, 200, button=3)
                        time.sleep(.2)
                        click_at(w, 140, 218)
                        wait_until(lambda: first()['pinned'], 'Pin menu did not pin the tab')
                        key_at(w, 's', 12)
                        time.sleep(1)
                        assert not first()['sleeping'], 'Pinned tab slept'
                        click_at(w, 90, 200, button=3)
                        time.sleep(.2)
                        click_at(w, 140, 218)
                        wait_until(lambda: not first()['pinned'], 'Unpin failed')
                        click_at(w, 352, 352)
                        wait_until(lambda: doc().get('audio') == 'running', 'Web Audio not started')
                        key_at(w, 's', 12)
                        time.sleep(2)
                        assert not first()['sleeping'], 'Web Audio tab slept'
                        key_at(w, 'n', 12)
                        key_at(w, 'l', 4)
                        type_text(w, url + '?PRIVATE_SLEEP_SENTINEL')
                        key_at(w, 'Return')
                        wait_until(lambda: len(state()['tabs']) == 3 and state()['tabs'][-1]['loaded'], 'No private tab')
                        key_at(w, 's', 12)
                        time.sleep(1)
                        assert state()['tabs'][-1]['private'] and not state()['tabs'][-1]['sleeping']
                    key_at(w, 'q', 4)
                    end = time.monotonic() + 30
                    while p.poll() is None and time.monotonic() < end:
                        if json.loads(report.read_text()).get('dialog_count'):
                            key_at(w, 'Return')
                        time.sleep(.1)
                    p.wait(timeout=2)
                    final = json.loads(report.read_text())
                    assert p.returncode == 0 and final['cef_created_total'] == final['cef_closed_total']
                    for path in profile.glob('profile.toml*'):
                        assert 'PRIVATE_SLEEP_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:
                        os.killpg(p.pid, signal.SIGTERM)
                        p.wait(timeout=5)
        server.shutdown()
    print('PASS: real freeze/resume, retained forms/history, freed bitmaps, pinned/private/media/domain exclusions' + ('' if os.environ.get('AURA_SLEEP_MANUAL_ONLY') else ' and actual five-minute automatic sleep'))


if __name__ == '__main__':
    main()
