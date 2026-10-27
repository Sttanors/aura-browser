"""Verify actual CEF document disposal/restoration and conservative unload guards."""
import json
import os
from pathlib import Path
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


class Handler(QuietHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', '0')))
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(b'<title>POST result</title><p>This response must not be replayed.</p>')


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT / 'artifacts/discard-smoke'
    out.mkdir(parents=True, exist_ok=True)
    binary = Path(os.environ.get('AURA_TEST_BINARY', str(ROOT / 'build/linux-x64-release/aura_browser')))
    with ThreadingHTTPServer(('127.0.0.1', 0), partial(Handler, directory=str(ROOT / 'tests/fixtures/browser'))) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = f'http://127.0.0.1:{server.server_port}'
        for mode in (() if os.environ.get('AURA_DISCARD_UI_ONLY') else ('manual',) if os.environ.get('AURA_DISCARD_MANUAL_ONLY') else ('manual', 'auto')):
            profile = out / mode
            shutil.rmtree(profile, ignore_errors=True)
            profile.mkdir()
            (profile / 'profile.toml').write_text('language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\n'
                'tab_sleep_mode="unload"\ntab_sleep_minutes=' + ('5' if mode == 'auto' else '0') + '\n')
            report = out / f'{mode}.json'
            report.unlink(missing_ok=True)
            with (out / f'{mode}.log').open('w') as log:
                p = subprocess.Popen([str(binary), f'--data-dir={profile}', f'--report={report}', f'--url={base}/discard.html'],
                    stdout=log, stderr=log, start_new_session=True,
                    env={**os.environ, 'AURA_BACKGROUND_TEST': '1', 'XMODIFIERS': '@im=none'})
                try:
                    def state():
                        if p.poll() is not None:
                            raise RuntimeError('Unload browser exited')
                        try:
                            return json.loads(report.read_text())
                        except (ValueError, OSError):
                            return {}
                    def first(): return state().get('tabs', [{}])[0]
                    def doc(tab=None):
                        try: return json.loads((tab or first()).get('title', ''))
                        except ValueError: return {}
                    wait_until(lambda: first().get('loaded') and doc().get('ticks', 0) > 2, 'No unload fixture', 30)
                    w = state()['window']
                    token = doc()['token']; cef_id = first()['cef_browser_id']
                    def command(name):
                        key_at(w, 'k', 4)
                        wait_until(lambda: state().get('commands_ui') is not None, 'No palette')
                        type_text(w, name)
                        wait_until(lambda: any(x['id'] == name for x in state()['commands_ui']['items']), 'No command ' + name)
                        key_at(w, 'Return')
                    def unload():
                        command('unload')
                        wait_until(lambda: state()['dialog_count'] == 1, 'No unload warning')
                        key_at(w, 'Return')
                        wait_until(lambda: state()['dialog_count'] == 0, 'Warning did not close')
                    def navigate(url):
                        key_at(w, 'l', 4); type_text(w, url); key_at(w, 'Return')
                    def new(url):
                        key_at(w, 't', 4); navigate(url)
                        wait_until(lambda: state()['tabs'][-1]['loaded'], 'No new document')
                    if mode == 'manual':
                        # Dirty form is retained while a clean sibling document really closes.
                        new(base + '/sleep.html?dirty')
                        click_at(w, 352, 245); type_text(w, 'KEEP_UNSAVED')
                        wait_until(lambda: doc(state()['tabs'][1]).get('value') == 'KEEP_UNSAVED', 'No dirty form')
                        key_at(w, 't', 4)
                        unload()
                    else:
                        key_at(w, 't', 4)
                        started = time.monotonic()
                        time.sleep(2)
                        assert not first()['discarded'], 'Timer fired early'
                        print('Waiting for the actual five-minute automatic unload timeout', flush=True)
                    wait_until(lambda: first().get('discarded'), 'Document did not unload', 325 if mode == 'auto' else 20)
                    if mode == 'auto': assert time.monotonic() - started >= 298
                    assert first()['cef_browser_id'] == 0 and first()['buffer_bytes'] == 0
                    assert not first()['sleeping'] and state()['cef_closed'] >= 1
                    if mode == 'manual':
                        assert not state()['tabs'][1]['discarded'] and doc(state()['tabs'][1])['value'] == 'KEEP_UNSAVED'
                    subprocess.run(['import', '-window', str(w), str(out / f'{mode}-unloaded.png')], check=True, timeout=10)
                    ticks = doc()['ticks']; time.sleep(1); assert doc()['ticks'] == ticks
                    key_at(w, '1', 4)
                    wait_until(lambda: not first()['discarded'] and first()['loaded'] and doc().get('token') != token, 'No fresh restored document', 30)
                    assert first()['cef_browser_id'] != cef_id
                    if mode == 'manual':
                        def tab_menu(command_id):
                            key_at(w, '3', 4)
                            click_at(w, 90, 200, button=3)
                            wait_until(lambda: bool(state().get('menu_ui')), 'No inactive tab menu')
                            assert state()['active_tab'] == 2, 'Right click selected the background tab'
                            item = next(x for x in state()['menu_ui'] if x['id'] == command_id)
                            assert item['enabled']
                            r = item['bounds']; click_at(w, r[0] + r[2] // 2, r[1] + r[3] // 2)
                        tab_menu(1)
                        wait_until(lambda: first()['pinned'], 'No pin')
                        key_at(w, '3', 4); unload(); time.sleep(1)
                        assert not first()['discarded'], 'Pinned document unloaded'
                        tab_menu(1)
                        wait_until(lambda: not first()['pinned'], 'No unpin')
                        tab_menu(14)
                        wait_until(lambda: state()['dialog_count'] == 1, 'No individual unload warning')
                        key_at(w, 'Return')
                        wait_until(lambda: first()['discarded'], 'Inactive tab menu did not unload')
                        key_at(w, '1', 4)
                        wait_until(lambda: first()['loaded'] and not first()['discarded'], 'No second restore', 30)
                        # addEventListener beforeunload isn't visible in an isolated world.
                        # Chromium's normal close path must refuse it without a background modal.
                        click_at(w, 352, 245)
                        wait_until(lambda: doc().get('protected'), 'No beforeunload listener')
                        key_at(w, '3', 4); closed = state()['cef_closed']
                        unload(); time.sleep(2)
                        assert not first()['discarded'] and not first()['discard_pending']
                        assert state()['dialog_count'] == 0 and state()['cef_closed'] == closed
                        # POST response, iframe and running Web Audio are never disposed.
                        new(base + '/discard.html?post')
                        click_at(w, 352, 303)
                        wait_until(lambda: state()['tabs'][-1]['title'] == 'POST result', 'No POST response')
                        post_id = state()['tabs'][-1]['id']
                        new(base + '/discard.html?frame')
                        frame_id = state()['tabs'][-1]['id']
                        new(base + '/sleep.html?audio')
                        click_at(w, 352, 352)
                        wait_until(lambda: doc(state()['tabs'][-1]).get('audio') == 'running', 'No Web Audio')
                        audio_id = state()['tabs'][-1]['id']
                        key_at(w, 't', 4); unload(); time.sleep(2)
                        for t in state()['tabs']:
                            if t['id'] in (post_id, frame_id, audio_id): assert not t['discarded'], t
                        # Existing pin/private eligibility guards are shared with freeze.
                        key_at(w, 'n', 12); navigate(base + '/discard.html?PRIVATE_UNLOAD_SENTINEL')
                        wait_until(lambda: state()['tabs'][-1]['loaded'] and state()['tabs'][-1]['private'], 'No private page')
                        private_id = state()['tabs'][-1]['id']; key_at(w, 't', 4); unload(); time.sleep(1)
                        assert not next(t for t in state()['tabs'] if t['id'] == private_id)['discarded']
                        # Mode is discoverable through exact settings search, with the common native layout.
                        command('tab_sleep_mode')
                        wait_until(lambda: (state().get('settings_ui') or {}).get('search') == 'tab_sleep_mode', 'No unload setting')
                        subprocess.run(['import', '-window', str(w), str(out / 'settings-en.png')], check=True, timeout=10)
                        key_at(w, 'l', 4); key_at(w, 'Escape')
                    key_at(w, 'q', 4)
                    deadline = time.monotonic() + 30
                    while p.poll() is None and time.monotonic() < deadline:
                        try:
                            if json.loads(report.read_text()).get('dialog_count'): key_at(w, 'Return')
                        except (OSError, ValueError): pass
                        time.sleep(.1)
                    p.wait(timeout=3)
                    final = json.loads(report.read_text())
                    assert p.returncode == 0 and final['cef_created_total'] == final['cef_closed_total']
                    saved = tomllib.loads((profile / 'profile.toml').read_text())
                    assert saved['tab_sleep_mode'] == 'unload'
                    for path in profile.glob('profile.toml*'):
                        assert 'PRIVATE_UNLOAD_SENTINEL' not in path.read_text()
                finally:
                    if p.poll() is None:
                        p.send_signal(signal.SIGTERM)
                        try: p.wait(timeout=5)
                        except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL); p.wait()
        # RU at 200% UI zoom: real choice controls, persistence and no overlap.
        profile = out / 'ui-ru'
        shutil.rmtree(profile, ignore_errors=True); profile.mkdir()
        (profile / 'profile.toml').write_text('language="ru"\nui_zoom=200\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
        report = out / 'ui-ru.json'; report.unlink(missing_ok=True)
        with (out / 'ui-ru.log').open('w') as log:
            p = subprocess.Popen([str(binary), f'--data-dir={profile}', f'--report={report}'],
                stdout=log, stderr=log, start_new_session=True,
                env={**os.environ, 'AURA_BACKGROUND_TEST': '1', 'XMODIFIERS': '@im=none'})
            try:
                def ui_state():
                    try: return json.loads(report.read_text())
                    except (OSError, ValueError): return {}
                wait_until(lambda: ui_state().get('presentation_frames', 0) > 0, 'No RU UI', 30)
                w = ui_state()['window']
                subprocess.run(['xdotool', 'windowsize', str(w), '2000', '1400'], check=True)
                wait_until(lambda: abs(ui_state()['interface']['applied'] - 200) < .1, 'No full 200% UI')
                key_at(w, 'k', 4); type_text(w, 'tab_sleep_mode'); key_at(w, 'Return')
                wait_until(lambda: (ui_state().get('settings_ui') or {}).get('search') == 'tab_sleep_mode', 'No RU mode setting')
                control = next(x for x in ui_state()['settings_ui']['controls'] if x['id'] == 'tab_sleep_mode')
                r = next(x for x in control['options'] if x['value'] == 'unload')['bounds']
                factor = ui_state()['interface']['applied'] / 100
                click_at(w, round((r[0] + r[2] / 2) * factor), round((r[1] + r[3] / 2) * factor))
                wait_until(lambda: tomllib.loads((profile / 'profile.toml').read_text()).get('tab_sleep_mode') == 'unload', 'RU choice did not persist')
                subprocess.run(['import', '-window', str(w), str(out / 'settings-ru-200.png')], check=True, timeout=10)
                key_at(w, 'l', 4); key_at(w, 'Escape'); key_at(w, 'q', 4); p.wait(timeout=20)
                assert p.returncode == 0
            finally:
                if p.poll() is None:
                    p.send_signal(signal.SIGTERM)
                    try: p.wait(timeout=5)
                    except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL); p.wait()
        server.shutdown()
    if os.environ.get('AURA_DISCARD_UI_ONLY'):
        print('PASS RU settings choice at actual UI 200%, persistence and clean shutdown')
    else:
        print('PASS actual document close, stopped JS, fresh CEF/DOM restore, dirty forms, beforeunload refusal, POST/frame/audio/private guards, mode persistence, RU UI 200%' + ('' if os.environ.get('AURA_DISCARD_MANUAL_ONLY') else ', actual five-minute automatic unload'))


if __name__ == '__main__': main()
