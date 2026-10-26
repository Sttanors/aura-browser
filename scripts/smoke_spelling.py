"""Actual CEF spelling markers, replacements, saved/private dictionary lifecycle."""
import json,os,re,shutil,signal,subprocess,threading,time
from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
from dependencies import ROOT
from smoke_browser import wait_until,type_text as ascii_text
from smoke_window import key_at,click_at
def type_text(window,value):
    if value.isascii():ascii_text(window,value)
    else:subprocess.run(['xdotool','type','--window',str(window),'--clearmodifiers','--delay','60','--',value],check=True)
probes={}
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_POST(self):
        data=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if 'rich' in data:data['rich']=data['rich'].replace('\u00a0',' ')
        probes[data['name']]=data
        self.send_response(204);self.end_headers()
    def do_GET(self):
        name=self.path.strip('/').split('?')[0]
        body=f'''<!doctype html><meta charset="utf-8"><title>Spelling {name}</title>
        <style>body{{font:20px monospace}}input,textarea,div[contenteditable]{{position:absolute;left:24px;width:500px;height:42px;font:20px monospace;box-sizing:border-box;padding:6px}}</style>
        <input id="en" lang="en" spellcheck="true" style="top:24px">
        <textarea id="ru" lang="ru" spellcheck="true" style="top:90px"></textarea>
        <div id="rich" contenteditable="true" spellcheck="true" style="top:156px;border:1px solid gray"></div>
        <input id="disabled" spellcheck="false" style="top:222px">
        <input id="secret" type="password" spellcheck="true" style="top:288px">
        <script>const token=Math.random().toString(36);setInterval(()=>{{const data={{name:{json.dumps(name)},token}};for(const id of ['en','ru','rich','disabled','secret']){{const e=document.getElementById(id);data[id]=e.value??e.textContent}}fetch('/probe',{{method:'POST',body:JSON.stringify(data)}}).catch(()=>{{}})}},150)</script>'''.encode()
        self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)

def main():
    assert os.environ.get('AURA_TEST_ISOLATED_DISPLAY')=='1'
    out=ROOT/'artifacts/spelling-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile';shutil.rmtree(profile,ignore_errors=True);profile.mkdir()
    with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start();base=f'http://127.0.0.1:{server.server_port}'
        seed='language="en"\nrestore=true\ncheck_updates=false\nfilter_updates=false\nconfirm_quit=false\n'
        seed+='[[containers]]\nid=0\nname="Default"\n[[containers]]\nid=1\nname="Work"\n'
        for name,container in [('normal',0),('container',1)]:seed+='[[windows]]\nactive=0\nworkspace=0\n[[windows.tabs]]\nurl='+json.dumps(base+'/'+name)+f'\ncontainer={container}\n'
        (profile/'profile.toml').write_text(seed)
        for launch in range(2):
            probes.clear()
            if launch:
                path=profile/'profile.toml';s=path.read_text();s=re.sub(r'(?m)^language\s*=.*$','language="ru"',s);s=re.sub(r'(?m)^ui_zoom\s*=.*$','ui_zoom=200',s);s=re.sub(r'(?m)^theme\s*=.*$','theme=1',s);path.write_text(s)
            report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
            with (out/f'process-{launch}.log').open('w') as log:
                p=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}'],stdout=log,stderr=log,start_new_session=True,env={**os.environ,'AURA_BACKGROUND_TEST':'1','XMODIFIERS':'@im=none'})
                try:
                    def root():
                        if p.poll() is not None:raise RuntimeError('Spelling browser exited')
                        try:return json.loads(report.read_text())
                        except (OSError,ValueError):return {}
                    def windows():return root().get('windows',[])
                    def state(w):return next((s for s in windows() if s['window']==w),{})
                    def current(w):s=state(w);return s['tabs'][s['active_tab']]
                    def find(name):return next((s['window'] for s in windows() if any(t['url']==base+'/'+name for t in s['tabs'])),None)
                    def factor(w):s=state(w)['interface'];return s['applied']/100*s['device_scale']
                    def click(w,r,button=1):f=factor(w);click_at(w,round((r[0]+r[2]/2)*f),round((r[1]+r[3]/2)*f),button)
                    def field(w,id,button=1):
                        index=['en','ru','rich','disabled','secret'].index(id);info=state(w)['interface'];f=factor(w);ds=info['device_scale']
                        click_at(w,round(252*f+52*ds),round(88*f+(42+index*66)*ds),button)
                    def text(w,name,id,value):
                        field(w,id);key_at(w,'a',4);type_text(w,value)
                        wait_until(lambda:probes.get(name,{}).get(id)==value,'No native form input '+id)
                        time.sleep(.5)
                    def context(w,id):
                        key_at(w,'Escape');field(w,id,3)
                        wait_until(lambda:bool(state(w).get('menu_ui')),'No page context menu')
                        return state(w)['menu_ui']
                    def misspelled(w,id,expected=True):
                        deadline=time.monotonic()+18
                        while True:
                            items=context(w,id)
                            present=any(i['id']==206 for i in items)
                            if present==expected:return items
                            key_at(w,'Escape')
                            if time.monotonic()>deadline:raise RuntimeError('Unexpected spelling state '+id+': '+str(items))
                            time.sleep(.4)
                    def menu(w,id):click(w,next(i['bounds'] for i in state(w)['menu_ui'] if i['id']==id))
                    def palette(w,q):
                        key_at(w,'k',4);type_text(w,q)
                        wait_until(lambda:(state(w).get('commands_ui') or {}).get('query')==q and state(w)['commands_ui'].get('items'),'No palette query '+q)
                    def dictionary(w):
                        palette(w,'spelling_dictionary');assert state(w)['commands_ui']['items'][0]['id']=='spelling_dictionary';key_at(w,'Return')
                        wait_until(lambda:bool(state(w).get('spelling_ui')),'No dictionary editor')
                    def add(w,word):
                        dictionary(w);type_text(w,word);key_at(w,'Return')
                        wait_until(lambda:any(i['word']==word for i in state(w)['spelling_ui']['items']),'Word not saved in editor')
                        key_at(w,'Escape')
                    def setting(w,id,value):
                        palette(w,id);key_at(w,'Return');wait_until(lambda:bool(state(w).get('settings_ui')),'No spelling setting')
                        c=next(c for c in state(w)['settings_ui']['controls'] if c['id']==id)
                        click(w,next(o['bounds'] for o in c['options'] if o['value']==value) if 'options' in c else c['bounds'])
                        wait_until(lambda:state(w)['settings'][id]==value,'Spelling preference not applied');key_at(w,'Escape')
                    def new(w,private=False,name='private'):
                        before={s['window'] for s in windows()};key_at(w,'n',5 if private else 4)
                        wait_until(lambda:any(s['window'] not in before for s in windows()),'No private native window')
                        nw=next(s['window'] for s in windows() if s['window'] not in before)
                        key_at(nw,'l',4);type_text(nw,base+'/'+name);key_at(nw,'Return')
                        wait_until(lambda:current(nw)['loaded'] and current(nw)['spelling_supported'] and probes.get(name),'No private spelling page',timeout=30)
                        return nw
                    def close(w):key_at(w,'w',5);wait_until(lambda:not state(w),'Private window not retired')
                    def shot(w,name):
                        subprocess.run(['xdotool','windowraise',str(w)],check=True);time.sleep(.35)
                        subprocess.run(['import','-window',str(w),str(out/name)],check=True,timeout=10)
                    # Loading/isolated-world readiness can precede the fixture's first timer.
                    wait_until(lambda:find('normal') and find('container') and probes.get('normal') and probes.get('container') and all(t['loaded'] and t['spelling_supported'] for s in windows() for t in s['tabs']),'Spelling bootstrap failed',timeout=40)
                    w=find('normal');other=find('container')
                    assert state(w)['spelling_resources_ready'] and not state(w)['spelling']['remote']
                    assert not state(w)['audit'].get('spelling_preference_errors')
                    if not launch:
                        token=probes['normal']['token'];browser=current(w)['cef_browser_id']
                        text(w,'normal','en','helllo ');items=misspelled(w,'en')
                        fix=next(i for i in items if 200<=i['id']<=204 and i['label'].lower()=='hello')
                        shot(w,'en-correction.png');menu(w,fix['id'])
                        wait_until(lambda:probes['normal']['en']=='hello ','Native English replacement failed')
                        text(w,'normal','ru','превет ');items=misspelled(w,'ru')
                        fix=next(i for i in items if 200<=i['id']<=204 and i['label'].lower()=='привет')
                        shot(w,'ru-correction.png');menu(w,fix['id'])
                        wait_until(lambda:probes['normal']['ru']=='привет ','Native Russian replacement failed')
                        text(w,'normal','rich','helllo ');items=misspelled(w,'rich');fix=next(i for i in items if i['label'].lower()=='hello');menu(w,fix['id'])
                        wait_until(lambda:probes['normal']['rich']=='hello ','Contenteditable replacement failed')
                        for id in ('disabled','secret'):
                            text(w,'normal',id,'helllo ');items=context(w,id);assert not any(200<=i['id']<=206 for i in items);key_at(w,'Escape')
                        word='AuraBrowzzzer'
                        text(other,'container','en',word+' ')
                        text(w,'normal','en',word+' ');misspelled(w,'en');menu(w,206)
                        wait_until(lambda:state(w)['spelling']['saved_words']==1,'Context dictionary addition failed')
                        misspelled(w,'en',False);key_at(w,'Escape');misspelled(other,'en',False);key_at(other,'Escape')
                        dictionary(w);click(w,next(i['delete'] for i in state(w)['spelling_ui']['items'] if i['word']==word))
                        wait_until(lambda:state(w)['spelling']['saved_words']==0,'Dictionary deletion failed');key_at(w,'Escape')
                        misspelled(w,'en');key_at(w,'Escape');add(w,'AuraBrowzzzer')
                        dictionary(w);type_text(w,'two words');key_at(w,'Return');wait_until(lambda:state(w)['spelling_ui']['error'],'Invalid dictionary word accepted')
                        key_at(w,'a',4);type_text(w,'AuraBrowzzzer');key_at(w,'Return');wait_until(lambda:state(w)['spelling_ui']['error'],'Duplicate word accepted')
                        shot(w,'dictionary-validation.png');key_at(w,'Escape')
                        private1=new(w,True,'private1');text(private1,'private1','en','PRIVATEWORDZZ ');misspelled(private1,'en');menu(private1,206)
                        wait_until(lambda:state(w)['spelling']['private_words']==1,'Private word not remembered')
                        private2=new(w,True,'private2');text(private2,'private2','en','PRIVATEWORDZZ ');misspelled(private2,'en',False);key_at(private2,'Escape')
                        close(private1);misspelled(private2,'en',False);key_at(private2,'Escape');close(private2)
                        wait_until(lambda:state(w)['spelling']['private_words']==0,'Private dictionary survived last private tab')
                        private3=new(w,True,'private3');text(private3,'private3','en','PRIVATEWORDZZ ');misspelled(private3,'en');key_at(private3,'Escape');close(private3)
                        setting(w,'spellcheck',False);items=context(w,'en');assert not any(200<=i['id']<=206 for i in items);key_at(w,'Escape')
                        assert not state(w)['spelling']['enabled'] and probes['normal']['token']==token and current(w)['cef_browser_id']==browser
                        setting(w,'spellcheck_languages','ru');assert state(w)['spelling']['languages']==['ru-RU']
                    else:
                        assert state(w)['settings']['spellcheck'] is False and state(w)['settings']['spellcheck_languages']=='ru'
                        assert state(w)['spelling']['saved_words']==1 and state(w)['spelling']['private_words']==0
                        for win in (w,other):
                            subprocess.run(['xdotool','windowsize','--sync',str(win),'1800','1240'],check=True)
                            wait_until(lambda:state(win)['interface']['applied']==200,'200% UI failed')
                        setting(w,'spellcheck_languages','both');setting(w,'spellcheck',True)
                        text(w,'normal','en','AuraBrowzzzer ');misspelled(w,'en',False);key_at(w,'Escape')
                        dictionary(w);shot(w,'ru-200-dictionary.png')
                        key_at(w,'F10');key_at(w,'Tab');key_at(w,'Escape')
                        text(w,'normal','ru','превет ');misspelled(w,'ru');shot(w,'ru-200-context.png');key_at(w,'Escape')
                    key_at(w,'q',4);p.wait(timeout=35)
                    final=json.loads(report.read_text());assert final['cef_created_total']==final['cef_closed_total'],final
                    for name in ('profile.toml','profile.toml.bak'):
                        path=profile/name
                        assert not path.exists() or 'PRIVATEWORDZZ' not in path.read_text()
                finally:
                    try:os.killpg(p.pid,signal.SIGTERM)
                    except ProcessLookupError:pass
                    try:p.wait(timeout=4)
                    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
    print('PASS: native RU/EN corrections in input/textarea/contenteditable, password/opt-out exclusion, live custom dictionary add/delete across containers/windows, validation, private-session lifetime, settings, restart, RU/EN UI 200%, balanced CEF lifecycle')
if __name__=='__main__':main()
