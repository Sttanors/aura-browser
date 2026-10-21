"""Exercise the updater panel and cancellation on the private software display."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Use verify_browser.py --suite=updates; no desktop test was started')
    out = ROOT/'artifacts/updates-smoke'
    out.mkdir(parents=True, exist_ok=True)
    profile = out/'profile'
    if profile.exists(): shutil.rmtree(profile)
    profile.mkdir()
    (profile/'profile.toml').write_text('language="ru"\nfilter_updates=false\nrestore=false\ncheck_updates=false\n')
    report = out/'report.json'
    report.unlink(missing_ok=True)
    binary = Path(os.environ.get('AURA_TEST_BINARY', str(ROOT/'build/linux-x64-release/aura_browser')))
    with (out/'process.log').open('w') as log:
        proc = subprocess.Popen([str(binary), f'--data-dir={profile}', f'--report={report}'],
                                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
                                env={**os.environ, 'XMODIFIERS': '@im=none', 'AURA_BACKGROUND_TEST': '1'})
        try:
            def state():
                if proc.poll() is not None: raise RuntimeError(f'Browser exited {proc.returncode}')
                try: return json.loads(report.read_text())
                except (FileNotFoundError, json.JSONDecodeError): return {}
            wait_until(lambda: state().get('presentation_frames', 0) > 0, 'No updater test window')
            window = str(state()['window'])
            def factor(): return state()['interface']['applied']/100
            def click(rect):
                x, y, w, h = rect
                click_at(window, round((x+w/2)*factor()), round((y+h/2)*factor()))
            def settings_search(text):
                click_at(window, round((state()['interface']['width']-53)*factor()), round(55*factor()))
                wait_until(lambda: state().get('settings_ui'), 'Settings did not open')
                key_at(window, 'f', 4)
                type_text(window, text)
                key_at(window, 'Return')
                wait_until(lambda: (state().get('settings_ui') or {}).get('search') == text, 'Settings search failed')
            def open_updates():
                settings_search('About Aura')
                row = next(c for c in state()['settings_ui']['controls'] if c['id'] == 'updates')
                x, y, w, h = row['bounds']
                click([x, y+66, min(300, w), 32])
                wait_until(lambda: state().get('updates_ui'), 'Updater panel did not open')
            assert not state()['updates']['busy']
            assert state()['updates']['status'] == 'Updates have not been checked yet.'
            for language in ('ru', 'en'):
                if language == 'en':
                    settings_search('language')
                    row = next(c for c in state()['settings_ui']['controls'] if c['id'] == 'language')
                    click(next(c['bounds'] for c in row['options'] if c['value'] == 'en'))
                    key_at(window, 'Escape')
                open_updates()
                time.sleep(.3)
                subprocess.run(['import', '-window', window, str(out/f'{language}.png')], check=True, timeout=10)
                # Start and cancel if the request is still running; immediate HTTP failures are valid too.
                before = state()['updates']['status']
                click(state()['updates_ui']['buttons'][0])
                wait_until(lambda: state()['updates']['busy'] or state()['updates']['status'] != before,
                           'Check button did not start the updater')
                if state()['updates']['busy']:
                    click(state()['updates_ui']['buttons'][0])
                    wait_until(lambda: not state()['updates']['busy'], 'Cancel did not stop the updater')
                assert not state()['updates']['ready']
                key_at(window, 'Escape')
            if os.environ.get('AURA_TEST_LIVE_CHANNEL') == '1':
                open_updates()
                click(state()['updates_ui']['buttons'][0])
                wait_until(lambda: state()['updates']['busy'] or
                           state()['updates']['status'] == 'Aura is up to date on this channel.',
                           'Live channel request did not start')
                wait_until(lambda: not state()['updates']['busy'], 'Live channel check timed out', timeout=150)
                assert state()['updates']['status'] == 'Aura is up to date on this channel.', state()['updates']
                (out/'live-channel-PASS.txt').write_text('PASS: real GitHub index, exact release manifest, pinned Ed25519 signature, current sequence.\n')
                key_at(window, 'Escape')
            key_at(window, 'q', 4)
            proc.wait(timeout=20)
            assert proc.returncode == 0
            private_profile = out/'private-profile'
            if private_profile.exists(): shutil.rmtree(private_profile)
            private_profile.mkdir()
            (private_profile/'profile.toml').write_text('check_updates=true\nfilter_updates=false\nrestore=false\n')
            report.unlink(missing_ok=True)
            proc = subprocess.Popen([str(binary), '--private', f'--data-dir={private_profile}', f'--report={report}'],
                                    cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
                                    env={**os.environ, 'XMODIFIERS': '@im=none', 'AURA_BACKGROUND_TEST': '1'})
            wait_until(lambda: state().get('presentation_frames', 0) > 0, 'No private startup window')
            time.sleep(1)
            assert not state()['updates']['busy']
            assert state()['updates']['status'] == 'Updates have not been checked yet.'
            assert not (private_profile/'updates.toml').exists()
            key_at(str(state()['window']), 'q', 4)
            proc.wait(timeout=20)
            assert proc.returncode == 0
            (out/'PASS.txt').write_text('PASS: RU/EN updater UI, disabled startup checks, explicit check, cancel/error, private startup without checks, clean shutdown.\n')
            print('PASS: updater UI and cancellation')
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=5)


if __name__ == '__main__':
    main()
