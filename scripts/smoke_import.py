"""Exercise profile discovery, explicit import, backups and native chooser on a private display."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import time
import tomllib
from dependencies import ROOT
from smoke_browser import wait_until, type_text
from smoke_window import click_at, key_at


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Use verify_browser.py --suite=import; no desktop test was started')
    out = ROOT/'artifacts/import-smoke'
    out.mkdir(parents=True, exist_ok=True)
    home = out/'test-home'
    if home.exists(): shutil.rmtree(home)
    chrome = home/'config/google-chrome/Default'
    chrome.mkdir(parents=True)
    now = int(time.time())
    bookmark_file = chrome/'Bookmarks'
    bookmark_file.write_text(json.dumps({'roots': {'other': {'children': [
        {'url': 'https://kept.test/', 'name': 'Imported title'},
        {'url': 'https://new.test/', 'name': 'Из Chrome'},
        {'url': 'javascript:alert(1)', 'name': 'Unsafe'}]}}}), encoding='utf-8')
    database = chrome/'History'
    with sqlite3.connect(database) as db:
        db.execute('CREATE TABLE urls(id INTEGER,url TEXT,title TEXT,visit_count INTEGER,last_visit_time INTEGER)')
        db.executemany('INSERT INTO urls VALUES(?,?,?,?,?)', [
            (1, 'https://one.test/', 'One', 1, (now+11644473600)*1000000),
            (2, 'https://two.test/', 'Two', 1, (now+11644473600-60)*1000000)])
    # Discovery must not parse unselected databases.
    firefox = home/'.mozilla/firefox/example.default'
    firefox.mkdir(parents=True)
    (firefox/'places.sqlite').write_text('Unselected fixture')
    source_hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (bookmark_file, database)}
    html = out/'Экспорт с пробелами.html'
    html.write_text('<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<A HREF="https://html.test/">Из HTML &amp; экспорт</A>', encoding='utf-8')
    invalid = out/'invalid.json'
    invalid.write_text('{not valid JSON}')
    profile = out/'profile'
    if profile.exists(): shutil.rmtree(profile)
    profile.mkdir()
    (profile/'profile.toml').write_text('language="ru"\nrestore=false\ncheck_updates=false\n'
        '[[bookmarks]]\nurl="https://kept.test/"\ntitle="My original title"\ntime=1\n')
    env = {**os.environ, 'HOME': str(home), 'XDG_CONFIG_HOME': str(home/'config'),
           'XDG_DATA_HOME': str(home/'data'), 'XMODIFIERS': '@im=none', 'AURA_BACKGROUND_TEST': '1'}
    binary = Path(os.environ.get('AURA_TEST_BINARY', str(ROOT/'build/linux-x64-release/aura_browser')))
    for launch in range(2):
        report = out/f'report-{launch}.json'
        report.unlink(missing_ok=True)
        with (out/f'process-{launch}.log').open('w') as log:
            proc = subprocess.Popen([str(binary), f'--data-dir={profile}', f'--report={report}'], cwd=ROOT,
                                    env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                def state():
                    if proc.poll() is not None: raise RuntimeError(f'Browser exited {proc.returncode}')
                    try: return json.loads(report.read_text())
                    except (FileNotFoundError, json.JSONDecodeError): return {}
                wait_until(lambda: state().get('presentation_frames', 0) > 0, 'No import test window')
                window = str(state()['window'])
                def factor(): return state()['interface']['applied']/100
                def click(r):
                    x, y, w, h = r
                    click_at(window, round((x+w/2)*factor()), round((y+h/2)*factor()))
                def shot(name):
                    time.sleep(.3)
                    subprocess.run(['import', '-window', window, str(out/name)], check=True, timeout=10)
                def paste(target, value):
                    key_at(target, 'a', 4)
                    clipboard = subprocess.Popen(['xclip', '-selection', 'clipboard', '-in', '-loops', '1'],
                                                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    clipboard.communicate(value.encode('utf-8'), timeout=5)
                    key_at(target, 'v', 4)
                def seek(text):
                    key_at(window, 'Escape')
                    key_at(window, 'Escape')
                    wait_until(lambda: state().get('panel') == 0, 'Panel did not close')
                    click_at(window, round((state()['interface']['width']-53)*factor()), round(55*factor()))
                    wait_until(lambda: state().get('settings_ui'), 'No settings panel')
                    key_at(window, 'f', 4)
                    type_text(window, text)
                    key_at(window, 'Return')
                    wait_until(lambda: (state().get('settings_ui') or {}).get('search') == text, 'Search did not update')
                    return state()['settings_ui']['controls']
                def open_import():
                    row = next(c for c in seek('Import browser data') if c['id'] == 'import')
                    x, y, w, h = row['bounds']
                    click([x, y+66, min(300, w), 32])
                    wait_until(lambda: state().get('import_ui'), 'No import panel')
                def ready():
                    wait_until(lambda: (state().get('import_ui') or {}).get('ready'), 'No preview: '+str(state().get('import_ui')))
                def apply():
                    click(state()['import_ui']['apply'])
                    wait_until(lambda: (state().get('import_ui') or {}).get('result') is not None, 'Import did not finish')
                    return state()['import_ui']['result']
                def picker():
                    pid = state()['start_page']['picker_pid']
                    found = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(pid)],
                                           capture_output=True, text=True, timeout=5)
                    return found.stdout.strip().splitlines()[-1] if found.returncode == 0 and found.stdout.strip() else None
                if launch == 0:
                    open_import()
                    click(state()['import_ui']['profiles'])
                    time.sleep(.3)
                    p = state()['import_ui']['profiles']
                    click([p[0], p[1]+48, 240, 31])
                    ready()
                    ui = state()['import_ui']
                    assert ui['preview_bookmarks'] == 2 and ui['preview_history'] == 2, ui
                    assert state()['bookmark_count'] == 1 and state()['history_count'] == 0
                    shot('ru-preview.png')
                    click(ui['history'])
                    result = apply()
                    assert result['bookmarks'] == 1 and result['history'] == 0, result
                    assert tomllib.loads((Path(result['backup'])/'profile.toml').read_text())['bookmarks'][0]['title'] == 'My original title'
                    wait_until(lambda: state()['bookmark_count'] == 2 and state()['history_count'] == 0, 'Wrong selective import')
                    click(state()['import_ui']['preview'])
                    ready()
                    click(state()['import_ui']['bookmarks'])
                    click(state()['import_ui']['history'])
                    result = apply()
                    assert result['bookmarks'] == 0 and result['history'] == 2
                    wait_until(lambda: state()['history_count'] == 2, 'History was not imported')
                    # Native chooser accepts a UTF-8 path and previews without applying.
                    click(state()['import_ui']['browse'])
                    wait_until(lambda: state()['start_page']['picker_pid'] > 0 and picker(), 'No native import chooser')
                    chooser = picker()
                    subprocess.run(['xdotool', 'windowfocus', '--sync', chooser], check=True)
                    subprocess.run(['xdotool', 'key', '--clearmodifiers', 'ctrl+l'], check=True)
                    time.sleep(.2)
                    clipboard = subprocess.Popen(['xclip', '-selection', 'clipboard', '-in', '-loops', '1'],
                                                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    clipboard.communicate(str(html).encode('utf-8'), timeout=5)
                    subprocess.run(['xdotool', 'key', '--clearmodifiers', 'ctrl+a', 'ctrl+v'], check=True)
                    time.sleep(.3)
                    subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Return'], check=True)
                    time.sleep(1)
                    if state()['start_page']['picker_pid'] > 0:
                        subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Return'], check=True)
                    wait_until(lambda: state()['start_page']['picker_pid'] == -1, 'Chooser did not close')
                    ready()
                    assert state()['import_ui']['preview_bookmarks'] == 1
                    assert state()['bookmark_count'] == 2
                    # Previous type choices persist, so nothing is selected for HTML yet.
                    click(state()['import_ui']['apply'])
                    time.sleep(.3)
                    assert state()['bookmark_count'] == 2
                    click(state()['import_ui']['bookmarks'])
                    result = apply()
                    assert result['bookmarks'] == 1
                    click(state()['import_ui']['preview'])
                    ready()
                    result = apply()
                    assert result['bookmarks'] == 0 and result['history'] == 0 and not result['backup']
                    # Input edits invalidate a previous preview; malformed input cannot be applied.
                    click(state()['import_ui']['input'])
                    paste(window, str(invalid))
                    key_at(window, 'Return')
                    wait_until(lambda: not state()['import_ui']['busy'] and not state()['import_ui']['ready'], 'Malformed preview was accepted')
                    assert state()['bookmark_count'] == 3 and state()['history_count'] == 2
                    row = next(c for c in seek('language') if c['id'] == 'language')
                    click(next(c['bounds'] for c in row['options'] if c['value'] == 'en'))
                    key_at(window, 'Escape')
                    row = next(c for c in seek('ui_zoom') if c['id'] == 'ui_zoom')
                    click(next(c['bounds'] for c in row['options'] if c['value'] == '125'))
                    wait_until(lambda: state()['settings']['ui_zoom'] == 125, 'Interface scale did not change')
                    key_at(window, 'Escape')
                    open_import()
                    shot('en-125.png')
                    subprocess.run(['xdotool', 'windowsize', window, '900', '620'], check=True)
                    wait_until(lambda: state()['interface']['width'] <= 900, 'Window did not resize')
                    shot('en-small.png')
                else:
                    assert state()['bookmark_count'] == 3 and state()['history_count'] == 2, state()
                    saved = tomllib.loads((profile/'profile.toml').read_text())
                    assert next(b['title'] for b in saved['bookmarks'] if b['url'] == 'https://kept.test/') == 'My original title'
                key_at(window, 'q', 4)
                proc.wait(timeout=20)
                assert proc.returncode == 0
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=5)
    for path, digest in source_hashes.items():
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    assert len(list((profile/'import-backups').iterdir())) == 3
    (out/'PASS.txt').write_text('PASS: detected Chromium profile, preview without mutation, selective bookmarks/history, native UTF-8 chooser, duplicate rejection, backup, malformed input, RU/EN and scaling, persistence, unchanged sources.\n')
    print('PASS: native browser import and profile retention')


if __name__ == '__main__':
    main()
