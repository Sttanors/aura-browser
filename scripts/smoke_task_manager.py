"""Actual CEF process metrics, shared renderers, private filtering and guarded termination."""
import json
import os
import re
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
    startup_requested = threading.Event()
    startup_requests = 0
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.startswith('/startup-hang'):
            Handler.startup_requests += 1
            self.startup_requested.set()
        name = 'PRIVATE_PROCESS_SENTINEL' if self.path.startswith('/private') else 'Task child' if self.path.startswith('/child') else 'Task second' if self.path.startswith('/second') else 'Task alpha'
        startup = '<script>while(true){}</script>' if self.path.startswith('/startup-hang') and self.startup_requests == 1 else ''
        data = f'''<!doctype html><title>{name}</title>{startup}
            <button onclick="window.open('/child','related')" style="position:absolute;left:20px;top:20px;width:180px;height:40px">Open child</button>
            <button style="position:absolute;left:20px;top:100px" onclick="while(true){{}}">Hang test page</button>
            <script>globalThis.memory=new Uint8Array(8*1024*1024);memory.fill(42);
            setInterval(()=>{{const until=performance.now()+25;while(performance.now()<until){{}}}},250);</script>'''.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY') == '1'
    out = ROOT / 'artifacts/task-manager-smoke'
    out.mkdir(parents=True, exist_ok=True)
    profile = out / 'profile'
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    (profile / 'profile.toml').write_text('language="en"\nrestore=false\ncheck_updates=false\nfilter_updates=false\n')
    report = out / 'report.json'
    report.unlink(missing_ok=True)
    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = f'http://127.0.0.1:{server.server_port}'
        with (out / 'process.log').open('w') as log:
            proc = subprocess.Popen([str(ROOT / 'build/linux-x64-release/aura_browser'), f'--data-dir={profile}', f'--report={report}', f'--url={base}/alpha'], stdout=log, stderr=log, start_new_session=True, env={**os.environ, 'AURA_BACKGROUND_TEST': '1'})
            try:
                def root():
                    if proc.poll() is not None:
                        raise RuntimeError('Task browser exited')
                    try:
                        return json.loads(report.read_text())
                    except (OSError, ValueError):
                        return {}
                def windows():
                    return root().get('windows', [])
                def state(w):
                    return next((s for s in windows() if s['window'] == w), {})
                def current(w):
                    s = state(w)
                    return s['tabs'][s['active_tab']]
                def rows(w):
                    return (state(w).get('tasks_ui') or {}).get('rows', [])
                def group(w):
                    return next((r for r in rows(w) if any(t['title'] == 'Task alpha' for t in r['tabs'])), {})
                def click(w, r):
                    factor=state(w).get('interface',{}).get('applied',100)/100
                    click_at(w, round((r[0]+r[2]/2)*factor), round((r[1]+r[3]/2)*factor))
                def manager(w):
                    key_at(w, 'Escape', 1)
                    wait_until(lambda: state(w).get('tasks_ui'), 'No task manager')
                def visible_group(w, predicate=None):
                    key_at(w, 'Home')
                    for _ in range(12):
                        r = next((r for r in rows(w) if predicate(r)), {}) if predicate else group(w)
                        if r.get('end'):
                            return r
                        key_at(w, 'Next')
                    raise RuntimeError('Renderer row is not reachable')
                def navigate(w, path):
                    key_at(w, 'Escape')
                    key_at(w, 'l', 4)
                    type_text(w, base+path)
                    key_at(w, 'Return')
                    wait_until(lambda: current(w)['loaded'] and current(w)['url'] == base+path, 'Navigation failed')
                def new_window(w, private=False):
                    old = {s['window'] for s in windows()}
                    key_at(w, 'n', 5 if private else 4)
                    wait_until(lambda: any(s['window'] not in old for s in windows()), 'No new window')
                    return next(s['window'] for s in windows() if s['window'] not in old)
                def shot(w, name):
                    subprocess.run(['xdotool', 'windowraise', str(w)], check=True)
                    time.sleep(.3)
                    subprocess.run(['import', '-window', str(w), str(out/name)], check=True, timeout=10)
                wait_until(lambda: windows() and windows()[0]['tabs'][0]['loaded'] and windows()[0]['tabs'][0]['title']=='Task alpha' and windows()[0]['tabs'][0]['frames']>0, 'No process source')
                first = windows()[0]['window']
                click_at(first, 300, 118)
                wait_until(lambda: len(state(first)['tabs']) == 2 and current(first)['title'] == 'Task child', 'No related child')
                second = new_window(first)
                navigate(second, '/second')
                manager(first)
                try:
                    wait_until(lambda: group(first).get('process', 0) > 0 and len(group(first).get('tabs', [])) >= 2 and group(first).get('killable') and group(first).get('cpu', 0) > .1 and group(first).get('memory', 0) > 0, 'No shared renderer with live metrics', timeout=35)
                finally:
                    (out / 'metrics.json').write_text(json.dumps(rows(first), indent=2))
                assert all(not r['killable'] for r in rows(first) if r['kind'] != 'Page process')
                shot(first, 'en-processes.png')
                click(first, visible_group(first)['end'])
                wait_until(lambda: state(first)['dialog_count'] == 1, 'No terminate confirmation')
                shot(first, 'en-confirm.png')
                key_at(first, 'Escape')
                assert all(not t['failed'] for t in state(first)['tabs'])
                # Changing another window invalidates the saved target snapshot.
                click(first, visible_group(first)['end'])
                wait_until(lambda: state(first)['dialog_count'] == 1, 'No stale target prompt')
                navigate(second, '/second?changed')
                key_at(first, 'Return')
                wait_until(lambda: state(first)['dialog_count'] == 0, 'Stale prompt did not finish')
                assert not state(first)['audit'].get('page_processes_ended')
                assert all(not t['failed'] for t in state(first)['tabs'])
                private = new_window(second, True)
                navigate(private, '/private')
                manager(private)
                wait_until(lambda: any('PRIVATE_PROCESS_SENTINEL' in t['title'] for r in rows(private) for t in r['tabs']), 'No private process')
                time.sleep(2)
                assert 'PRIVATE_PROCESS_SENTINEL' not in json.dumps(rows(first))
                assert 'Task alpha' not in json.dumps(rows(private))
                key_at(private, 'w', 5)
                wait_until(lambda: not state(private), 'Private window did not close')
                time.sleep(3)
                # End an actual unresponsive renderer from its native UI.
                key_at(first, 'Escape')
                key_at(first, '1', 4)
                click_at(first, 300, 198)
                manager(first)
                time.sleep(2)
                target = visible_group(first)
                assert target['killable']
                affected = [(t['window'], t['tab']) for t in target['tabs']]
                click(first, target['end'])
                wait_until(lambda: state(first)['dialog_count'] == 1, 'No final confirmation')
                key_at(first, 'Return')
                wait_until(lambda: state(first)['audit'].get('page_processes_ended') == 1, 'Process not ended')
                wait_until(lambda: all(t['failed'] and t['process_ended'] for t in state(first)['tabs']), 'Shared tabs did not terminate together')
                assert proc.poll() is None and state(second)
                (out / 'affected.json').write_text(json.dumps(affected))
                key_at(first, 'Escape')
                key_at(first, 'r', 4)
                wait_until(lambda: current(first)['loaded'] and not current(first)['failed'] and current(first)['title'] == 'Task alpha', 'Reload after termination failed')
                # A renderer hung before load completion must also be killable.
                key_at(first, 't', 4)
                key_at(first, 'l', 4)
                type_text(first, base+'/startup-hang')
                key_at(first, 'Return')
                wait_until(lambda: Handler.startup_requested.is_set() and current(first)['url']==base+'/startup-hang' and not current(first)['loaded'], 'Startup hang document did not begin loading')
                hung_id = current(first)['id']
                manager(first)
                wait_until(lambda: any(r['killable'] and any(t['tab'] == hung_id for t in r['tabs']) for r in rows(first)), 'Loading renderer is not killable')
                assert not current(first)['loaded']
                time.sleep(2)
                target = visible_group(first, lambda r: any(t['tab'] == hung_id for t in r['tabs']))
                click(first, target['end'])
                wait_until(lambda: state(first)['dialog_count'] == 1, 'No startup hang confirmation')
                key_at(first, 'Return')
                wait_until(lambda: current(first)['failed'] and current(first)['process_ended'] and state(first)['audit'].get('page_processes_ended') == 2, 'Startup hang not ended')
                key_at(first,'Escape')
                shot(first,'en-ended.png')
                key_at(first,'r',4)
                wait_until(lambda: current(first)['loaded'] and not current(first)['failed'] and not current(first)['process_ended'], 'Startup hang reload did not recover')
                # RU, a smaller window, full 200% UI scaling, and keyboard focus.
                def choose(id, value):
                    key_at(first, 'Escape')
                    key_at(first, 'k', 4)
                    wait_until(lambda: state(first).get('commands_ui'), 'No settings palette')
                    type_text(first, id)
                    wait_until(lambda: any(i['id']==id and i['kind']=='setting' for i in state(first)['commands_ui']['items']), 'Setting not found')
                    click(first, next(i['bounds'] for i in state(first)['commands_ui']['items'] if i['id']==id and i['kind']=='setting'))
                    wait_until(lambda: state(first).get('settings_ui'), 'No settings UI')
                    control=next(c for c in state(first)['settings_ui']['controls'] if c['id']==id)
                    click(first, next(o['bounds'] for o in control['options'] if o['value']==str(value)))
                    wait_until(lambda: str(state(first)['settings'][id])==str(value), 'Setting did not change')
                choose('language','ru')
                subprocess.run(['xdotool','windowsize',str(first),'1000','740'],check=True)
                key_at(first,'Escape')
                manager(first)
                time.sleep(1)
                shot(first,'ru-small.png')
                subprocess.run(['xdotool','windowsize',str(first),'2000','1400'],check=True)
                choose('ui_zoom',200)
                manager(first)
                wait_until(lambda: state(first)['interface']['applied']==200,'UI 200% not applied')
                time.sleep(1)
                shot(first,'ru-200.png')
                key_at(first,'Tab')
                key_at(first,'Tab')
                key_at(first,'Return')
                assert state(first).get('tasks_ui') is not None
                subprocess.run(['xdotool','windowsize',str(first),'1800','1240'],check=True)
                wait_until(lambda: state(first)['interface']['height']==620,'Minimum logical height not applied')
                key_at(first,'Escape')
                click(first,[16,12,208,62])
                time.sleep(.2)
                shot(first,'ru-menu-minimum.png')
                key_at(first,'End')
                key_at(first,'Return')
                wait_until(lambda: state(first).get('tasks_ui'),'Task manager menu action failed')
                key_at(first, 'q', 4)
                proc.wait(timeout=30)
                assert proc.returncode == 0
                final = json.loads(report.read_text())
                assert final['cef_created_total'] == final['cef_closed_total']
                assert 'PRIVATE_PROCESS_SENTINEL' not in (profile/'profile.toml').read_text()
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait(timeout=5)
        server.shutdown()
    print('PASS: real CEF CPU/memory, shared renderer tabs, protected tasks, cancel/stale target, private filtering, hung renderer termination and reload')


if __name__ == '__main__':
    main()
