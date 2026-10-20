"""Exercise the actual browser UI, real CEF tabs and a persistent profile."""
from __future__ import annotations
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
from smoke_window import click_at, key_at


class Handler(QuietHandler):
    def end_headers(self):
        if self.path.startswith('/browser/a.html'):
            self.send_header('Set-Cookie', 'aura_probe=visible; SameSite=Lax; Path=/')
        super().end_headers()


def type_text(window, value):
    punctuation = {':':('semicolon',1), '/':('slash',0), '.':('period',0),
        '-':('minus',0), '_':('minus',1), '?':('slash',1), '=':('equal',0),
        '&':('7',1), '%':('5',1), ' ':('space',0), '#':('3',1)}
    for c in value:
        name, state = punctuation.get(c, (c.lower(), 1 if c.isupper() else 0))
        key_at(window, name, state)


def wait_until(predicate, message, timeout=12):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if predicate(): return
        time.sleep(.1)
    raise RuntimeError(message)


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY') != '1':
        raise RuntimeError('Use python3 scripts/verify_browser.py: GUI tests require an isolated software display')
    output=ROOT/'artifacts/browser-smoke'
    output.mkdir(parents=True,exist_ok=True)
    profile=output/'profile'
    if profile.exists(): shutil.rmtree(profile)
    server=ThreadingHTTPServer(('127.0.0.1',0),partial(Handler,directory=str(ROOT/'tests/fixtures')))
    worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    base=f'http://127.0.0.1:{server.server_port}/browser/'
    binary=ROOT/'build/linux-x64-release/aura_browser'
    reports=[]
    try:
        for launch in range(2):
            report_path=output/f'report-{launch}.json'
            report_path.unlink(missing_ok=True)
            with (output/f'process-{launch}.log').open('w') as log:
                process=subprocess.Popen([str(binary),f'--data-dir={profile}',f'--report={report_path}'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,start_new_session=True,env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'})
                try:
                    window=None
                    def mapped():
                        nonlocal window
                        if process.poll() is not None: raise RuntimeError(f'Browser exited {process.returncode}; see {output}')
                        if not report_path.exists():return False
                        report=json.loads(report_path.read_text())
                        window=str(report.get('window',0))
                        return int(window)>0 and report.get('presentation_frames',0)>0
                    wait_until(mapped,'No browser window')
                    def xdo(*args):return subprocess.check_output(['xdotool',*map(str,args)],text=True,timeout=5)
                    def title():return xdo('getwindowname',window)
                    def expect(value):wait_until(lambda:value in title(),f'Expected title {value!r}, got {title()!r}')
                    def navigate(url):key_at(window,'l',4);type_text(window,url);key_at(window,'Return');
                    wait_until(lambda:report_path.exists() and len(json.loads(report_path.read_text()).get('tabs',[]))>0,'Browser startup did not finish')
                    time.sleep(.3)
                    if launch==0:
                        subprocess.run(['import','-window',window,str(output/'new-tab.png')],check=True,timeout=10)
                        navigate(base+'a.html');expect('Alpha');time.sleep(.5)
                        click_at(window,352,252);type_text(window,'Real_browser');expect('Alpha input Real_browser')
                        click_at(window,360,194);expect('Beta')
                        key_at(window,'Left',8);expect('Alpha')
                        key_at(window,'Right',8);expect('Beta')
                        key_at(window,'r',4);time.sleep(.6)
                        navigate(base+'a.html');expect('Alpha')
                        key_at(window,'d',4)
                        key_at(window,'f',4);type_text(window,'Alpha');time.sleep(.7);key_at(window,'Escape')
                        click_at(window,350,368);time.sleep(.4);key_at(window,'Return');expect('Alpha dialog accepted')
                        subprocess.run(['import','-window',window,str(output/'before-download.png')],check=True,timeout=10)
                        click_at(window,360,315)
                        expect('Alpha download clicked')
                        time.sleep(.5)
                        subprocess.run(['import','-window',window,str(output/'after-download.png')],check=True,timeout=10)
                        download=profile/'downloads/aura-sample.bin'
                        payload=(ROOT/'tests/fixtures/browser/sample.bin').read_bytes()
                        wait_until(lambda:report_path.exists() and any(d['complete'] for d in json.loads(report_path.read_text()).get('downloads',[])), 'CEF did not finish the download', timeout=35)
                        assert download.read_bytes()==payload, 'Download bytes differ'
                        key_at(window,'Escape')
                        click_at(window,370,424);expect('Beta')
                        key_at(window,'w',4);expect('Alpha')
                        key_at(window,'t',5);expect('Beta')
                        key_at(window,'Tab',4);expect('Alpha')
                        key_at(window,'n',5);expect('Private tab')
                        navigate(base+'private.html');expect('Private isolated')
                        key_at(window,'w',4);time.sleep(.5)
                        key_at(window,'1',4);expect('Alpha')
                        key_at(window,'F6');key_at(window,'F7')
                        time.sleep(.4)
                        subprocess.run(['import','-window',window,str(output/'browser.png')],check=True,timeout=10)
                        key_at(window,'t',4);expect('New tab')
                        subprocess.run(['import','-window',window,str(output/'new-tab-light.png')],check=True,timeout=10)
                        xdo('windowsize','--sync',window,900,620)
                        for _ in range(5):key_at(window,'t',4)
                        time.sleep(.4)
                        assert len(json.loads(report_path.read_text())['tabs'])==8
                        subprocess.run(['import','-window',window,str(output/'new-tab-small-light.png')],check=True,timeout=10)
                        key_at(window,'F7');time.sleep(.3)
                        subprocess.run(['import','-window',window,str(output/'new-tab-small-dark.png')],check=True,timeout=10)
                        key_at(window,'1',4);expect('Alpha');time.sleep(.3)
                        subprocess.run(['import','-window',window,str(output/'tabs-small-first.png')],check=True,timeout=10)
                        key_at(window,'9',4);expect('New tab');key_at(window,'F7')
                        for _ in range(5):key_at(window,'w',4)
                        xdo('windowsize','--sync',window,1360,900)
                        wait_until(lambda:len(json.loads(report_path.read_text())['tabs'])==3,'Temporary tabs did not close')
                    else:
                        expect('New tab')
                        key_at(window,'1',4);expect('Alpha')
                        key_at(window,'2',4);expect('Beta')
                        key_at(window,'3',4);expect('New tab')
                    click_at(window,1332,15)
                    process.wait(timeout=15)
                    if process.returncode:raise RuntimeError(f'Browser exited {process.returncode}')
                finally:
                    if process.poll() is None:
                        if window:
                            click_at(window,1332,15)
                            try:process.wait(timeout=3)
                            except subprocess.TimeoutExpired:pass
                        if process.poll() is None:
                            os.killpg(process.pid,signal.SIGTERM)
                            try:process.wait(timeout=3)
                            except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
            report=json.loads(report_path.read_text());reports.append(report)
            assert report['cef_created']==report['cef_closed'],report
            assert report['audit'].get('renderer_crashes',0)==0,report
            assert report['presentation_frames']>5,report
            assert report['compositor']=='Skia raster / X11',report
            assert len(report['tabs'])==3,report
            assert not any(t['private'] for t in report['tabs']),report
        report=reports[0];audit=report['audit']
        for action in ('back','forward','reload','bookmark','private_tabs','page_opened_tabs','tabs_reopened','find_matches'):
            assert audit.get(action,0)>0,(action,audit)
        assert report['bookmark_count']==1,report
        assert report['downloads'][0]['complete'],report
        saved=tomllib.loads((profile/'profile.toml').read_text())
        assert len(saved['session'])==3,saved
        assert not any('private.html' in x['url'] for x in saved['history']+saved['session']),saved
        assert saved['theme']==3,saved
        (output/'verification.json').write_text(json.dumps({'passed':True,'launches':reports},indent=2))
        print(json.dumps({'passed':True,'launches':reports},indent=2))
    finally:
        server.shutdown();worker.join();server.server_close()


if __name__=='__main__':main()
