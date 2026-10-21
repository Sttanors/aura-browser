"""Upgrade the actual signed previous Linux release in an isolated user home."""
import hashlib,json,os,re,shutil,signal,subprocess,threading,time,tomllib,zipfile
from functools import partial
from http.server import ThreadingHTTPServer
from urllib.parse import urlparse,parse_qs
from dependencies import ROOT
from release_identity import read_identity
from smoke import QuietHandler
from smoke_browser import wait_until,type_text
from smoke_window import key_at
class Handler(QuietHandler):
    phase='old';probes={}
    def do_GET(self):
        p=urlparse(self.path)
        if p.path=='/upgrade.html':
            self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Cache-Control','no-store')
            if self.phase=='old':self.send_header('Set-Cookie','aura_upgrade=preserved; Max-Age=86400; SameSite=Lax; Path=/')
            self.end_headers();self.wfile.write('''<!doctype html><meta charset="utf-8"><title>Upgrade fixture</title><h1>Перенос профиля / profile upgrade</h1><script>const previous=localStorage.getItem('upgrade-marker')||'';localStorage.setItem('upgrade-marker','kept');fetch('/upgrade-probe?cookie='+encodeURIComponent(document.cookie)+'&stored='+encodeURIComponent(previous));</script>'''.encode())
        elif p.path=='/upgrade-probe':
            self.probes[self.phase]=parse_qs(p.query,keep_blank_values=True);self.send_response(204);self.end_headers()
        else:self.send_error(404)
def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=upgrade')
    identity=read_identity();out=ROOT/'artifacts/manual-upgrade-smoke';out.mkdir(parents=True,exist_ok=True)
    old=ROOT/'artifacts/aura2-linux64.run';metadata=ROOT/'artifacts/channel-current/aura-update.toml';signature=metadata.with_suffix('.toml.sig')
    if not all(p.is_file() for p in (old,metadata,signature)):raise RuntimeError('Prepare the previous published .run and signed channel snapshot first')
    public=re.search(r'-----BEGIN PUBLIC KEY-----\n.*?-----END PUBLIC KEY-----\n',(ROOT/'common/update_key.hpp').read_text(),re.S)[0]
    public_file=out/'channel-public.pem';public_file.write_text(public)
    subprocess.run(['openssl','pkeyutl','-verify','-rawin','-pubin','-inkey',str(public_file),'-in',str(metadata),'-sigfile',str(signature)],check=True)
    signed=tomllib.loads(metadata.read_text());asset=next(a for a in signed['artifacts'] if a['kind']=='run')
    assert signed['sequence']<identity.sequence and digest(old)==asset['sha256'] and old.stat().st_size==asset['size']
    new=ROOT/'artifacts'/('release-'+identity.version)/'aura2-linux64.run';archive=new.with_suffix('.zip')
    with zipfile.ZipFile(archive) as z:expected=hashlib.sha256(z.read('Aura2/aura_browser')).hexdigest()
    home=out/'Дом с пробелами';shutil.rmtree(home,ignore_errors=True);home.mkdir()
    data=home/'Данные $cash %25';env={**os.environ,'HOME':str(home),'XDG_DATA_HOME':str(data),'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'}
    profile=data/'aura-browser-profile';profile.mkdir(parents=True)
    (profile/'profile.toml').write_text('language="ru"\nrestore=false\ncheck_updates=false\nfilter_updates=false\nntp_title="Перенос профиля"\n')
    (profile/'preserve-me.txt').write_text('fictional profile marker')
    launcher=home/'.local/bin/aura-browser';reports=[]
    with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();url=f'http://127.0.0.1:{server.server_port}/upgrade.html'
        for phase,installer in (('old',old),('new',new)):
            Handler.phase=phase
            with (out/f'installer-{phase}.log').open('w') as log:subprocess.run(['bash',str(installer)],env=env,check=True,stdout=log,stderr=log,timeout=180)
            if phase=='new':
                assert subprocess.check_output([str(launcher),'--version'],env=env,text=True).strip()==identity.version
                assert digest(data/'aura-browser/aura_browser')==expected
            report=out/f'report-{phase}.json';report.unlink(missing_ok=True)
            with (out/f'process-{phase}.log').open('w') as log:
                process=subprocess.Popen([str(launcher),'--software-rendering',f'--report={report}',f'--url={url}'],env=env,stdout=log,stderr=log,start_new_session=True)
                try:
                    def state():
                        if process.poll() is not None:raise RuntimeError('Upgrade browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    wait_until(lambda:state().get('presentation_frames',0)>0,'No upgrade browser window',timeout=30);window=str(state()['window'])
                    # Older releases may finish their bootstrap navigation late.
                    if phase not in Handler.probes:
                        time.sleep(.7)
                        if phase not in Handler.probes:key_at(window,'l',4);type_text(window,url);key_at(window,'Return')
                    wait_until(lambda:phase in Handler.probes and any(t.get('title')=='Upgrade fixture' and t.get('loaded') for t in state().get('tabs',[])),'Upgrade page did not finish',timeout=25)
                    if phase=='old':
                        key_at(window,'d',4);wait_until(lambda:state().get('bookmark_count')==1,'Old release did not save a bookmark')
                    else:
                        assert state()['bookmark_count']==1 and state()['history_count']>=1
                        assert state()['settings']['language']=='ru' and state()['settings']['ntp_title']=='Перенос профиля'
                        assert Handler.probes[phase]['stored']==['kept'] and 'aura_upgrade=preserved' in Handler.probes[phase]['cookie'][0]
                        assert (profile/'preserve-me.txt').read_text()=='fictional profile marker'
                        subprocess.run(['import','-window',window,str(out/'upgraded.png')],check=True,timeout=10)
                    key_at(window,'q',4);process.wait(timeout=30);final=json.loads(report.read_text());reports.append(final)
                    assert final.get('cef_created_total',final['cef_created'])==final.get('cef_closed_total',final['cef_closed'])
                finally:
                    if process.poll() is None:os.killpg(process.pid,signal.SIGTERM);process.wait(timeout=5)
        server.shutdown()
    with (out/'uninstall.log').open('w') as log:subprocess.run([str(home/'.local/bin/aura-browser-uninstall')],env=env,check=True,stdout=log,stderr=log,timeout=30)
    assert not launcher.exists() and not (data/'aura-browser').exists()
    assert (profile/'preserve-me.txt').is_file() and (profile/'profile.toml').is_file()
    result={'from':signed['version'],'to':identity.version,'old_signed_hash':asset['sha256'],'new_binary_hash':expected,'cookie_and_localstorage':True,'bookmark_and_settings':True,'profile_after_uninstall':True}
    (out/'verification.json').write_text(json.dumps(result,indent=2));(out/'PASS.txt').write_text('PASS: actual signed old release -> new .run, real launcher/GUI, exact binary, bookmarks/history/settings/cookies/localStorage retained, uninstall keeps profile\n');print('Manual upgrade PASS:',signed['version'],'->',identity.version)
if __name__=='__main__':main()
