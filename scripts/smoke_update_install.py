"""Upgrade an isolated previous-version installation through the real signed channel."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
import tomllib
import zipfile
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Use verify_browser.py --suite=update-install; no desktop test was started')
    out = ROOT/'artifacts/updates-journey'
    previous = out/'source/build/linux-x64-release/aura_browser'
    if not previous.is_file():
        raise RuntimeError('Prepare the isolated previous-sequence test build first')
    task_home = out/'test-home'
    if task_home.exists(): shutil.rmtree(task_home)
    task_data = task_home/'data'
    app = task_data/'aura-browser'
    app.mkdir(parents=True)
    archive = ROOT/'artifacts/aura2-linux64.zip'
    manifest = tomllib.loads((ROOT/'artifacts/channel-current/aura-update.toml').read_text())
    expected_zip = next(a['sha256'] for a in manifest['artifacts'] if a['filename'] == archive.name)
    with archive.open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected_zip
    with zipfile.ZipFile(archive) as source:
        expected_binary = hashlib.sha256(source.read('Aura2/aura_browser')).hexdigest()
        for entry in source.infolist():
            parts = Path(entry.filename).parts
            if parts[0] != 'Aura2' or '..' in parts:
                raise ValueError('Invalid runtime path')
            if entry.is_dir(): continue
            target = app.joinpath(*parts[1:])
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(entry) as incoming, target.open('wb') as output:
                shutil.copyfileobj(incoming, output)
    shutil.copy2(previous, app/'aura_browser')
    (app/'aura_browser').chmod(0o755)
    (app/'.aura-installed').touch()
    profile = task_data/'aura-browser-profile'
    profile.mkdir()
    (profile/'profile.toml').write_text('language="en"\nfilter_updates=false\nrestore=false\ncheck_updates=false\n'
        '[[bookmarks]]\nurl="https://example.com/keep-me"\ntitle="Keep this bookmark"\ntime=1\n')
    report = out/'install-report.json'
    report.unlink(missing_ok=True)
    env = {**os.environ, 'HOME': str(task_home), 'XDG_DATA_HOME': str(task_data),
           'XMODIFIERS': '@im=none', 'AURA_BACKGROUND_TEST': '1'}
    with (out/'install-process.log').open('w') as log:
        proc = subprocess.Popen([str(app/'aura_browser'), f'--data-dir={profile}', f'--report={report}'],
                                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            def state():
                if proc.poll() is not None: raise RuntimeError(f'Previous browser exited {proc.returncode}')
                try: return json.loads(report.read_text())
                except (FileNotFoundError, json.JSONDecodeError): return {}
            wait_until(lambda: state().get('presentation_frames', 0) > 0, 'No previous-version window')
            window = str(state()['window'])
            def click(r):
                x, y, w, h = r
                click_at(window, round(x+w/2), round(y+h/2))
            assert state()['updates']['installation'] == 'run'
            click_at(window, state()['interface']['width']-53, 55)
            wait_until(lambda: state().get('settings_ui'), 'No settings')
            key_at(window, 'f', 4)
            type_text(window, 'About Aura')
            key_at(window, 'Return')
            wait_until(lambda: (state().get('settings_ui') or {}).get('search') == 'About Aura', 'No update action')
            row = next(c for c in state()['settings_ui']['controls'] if c['id'] == 'updates')
            x, y, w, h = row['bounds']
            click([x, y+66, min(300, w), 32])
            wait_until(lambda: state().get('updates_ui'), 'No updates panel')
            click(state()['updates_ui']['buttons'][0])
            wait_until(lambda: state()['updates']['busy'], 'No channel request')
            wait_until(lambda: not state()['updates']['busy'], 'Channel timed out', timeout=150)
            assert state()['updates']['status'] == 'A new Aura release is available.', state()['updates']
            click(state()['updates_ui']['buttons'][1])
            wait_until(lambda: state()['updates']['busy'], 'Download did not start')
            wait_until(lambda: not state()['updates']['busy'], 'Download timed out', timeout=360)
            assert state()['updates']['ready'], state()['updates']
            subprocess.run(['import', '-window', window, str(out/'verified-before-install.png')], check=True, timeout=10)
            click(state()['updates_ui']['buttons'][2])
            wait_until(lambda: state()['dialog_count'] == 1, 'No installation confirmation')
            width, height = state()['interface']['width'], state()['interface']['height']
            x, y = max(260, (width-490)//2), max(110, (height-230)//2)
            click([x+490-114, y+230-60, 90, 35])
            proc.wait(timeout=30)
            assert proc.returncode == 0
            def installed():
                try:
                    return hashlib.sha256((app/'aura_browser').read_bytes()).hexdigest() == expected_binary and \
                           (task_home/'.local/bin/aura-browser').is_file() and \
                           (task_data/'applications/aura-browser.desktop').is_file()
                except FileNotFoundError: return False
            wait_until(installed, 'Verified installer did not replace the isolated installation', timeout=90)
            saved = tomllib.loads((profile/'profile.toml').read_text())
            assert any(b['url'] == 'https://example.com/keep-me' for b in saved['bookmarks'])
            result = subprocess.run([str(task_home/'.local/bin/aura-browser'), '--help'], env=env,
                                    text=True, capture_output=True, timeout=20)
            assert result.returncode == 0 and 'Aura browser' in result.stdout
            (out/'install-PASS.txt').write_text('PASS: synthetic previous-sequence installation -> real GitHub signature -> actual .run download/hash -> user confirmation -> CEF shutdown -> installer -> exact published binary, launcher/menu and preserved profile.\n')
            print('PASS: signed installed-browser upgrade and profile retention')
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=5)


if __name__ == '__main__':
    main()
