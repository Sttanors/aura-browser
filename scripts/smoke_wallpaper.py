"""Dynamic shortcuts and local PNG/JPEG wallpaper with real native file selection."""
import json,os,subprocess,signal,shutil,time,math
from pathlib import Path
from PIL import Image
from dependencies import ROOT
from smoke_browser import wait_until,type_text
from smoke_window import click_at,key_at


def main():
    if os.environ.get('AURA_TEST_ISOLATED_DISPLAY')!='1':raise RuntimeError('Use verify_browser.py --suite=wallpaper')
    out=ROOT/'artifacts/wallpaper-smoke';out.mkdir(parents=True,exist_ok=True)
    profile=out/'profile'
    if profile.exists():shutil.rmtree(profile)
    profile.mkdir();(profile/'profile.toml').write_text('language="ru"\nfilter_updates=false\nrestore=false\nntp_link_1="https://legacy.test/"\nntp_link_2="https://second.test/"\nntp_link_3=""\n')
    warm=out/'Тёплый фон.png';cool=out/'Cool background.jpg';bad=out/'oversized.png'
    image=Image.new('RGB',(640,360));image.putdata([(min(245,110+x//5),35+y//5,35+(x+y)//20) for y in range(360) for x in range(640)]);image.resize((6000,4000)).save(warm)
    image=Image.new('RGB',(640,360));image.putdata([(20+x//12,50+y//4,min(250,130+x//6)) for y in range(360) for x in range(640)]);image.resize((7680,4320)).save(cool)
    assert Image.open(warm).size==(6000,4000) and Image.open(cool).size==(7680,4320)
    payload=bytearray(warm.read_bytes());payload[16:20]=(100000).to_bytes(4,'big');bad.write_bytes(payload)
    reports=[];expected_colors=None;expected_links=None
    for launch in range(2):
        report=out/f'report-{launch}.json';report.unlink(missing_ok=True)
        with (out/f'process-{launch}.log').open('w') as log:
            proc=subprocess.Popen([str(ROOT/'build/linux-x64-release/aura_browser'),f'--data-dir={profile}',f'--report={report}'],cwd=ROOT,stdout=log,stderr=log,start_new_session=True,env={**os.environ,'XMODIFIERS':'@im=none','AURA_BACKGROUND_TEST':'1'})
            try:
                def state():
                    if proc.poll() is not None:raise RuntimeError(f'Browser exited {proc.returncode}')
                    return json.loads(report.read_text()) if report.exists() else {}
                wait_until(lambda:state().get('start_page') and state().get('presentation_frames',0)>0,'No start page')
                window=str(state()['window']);width=1360
                def click(r):x,y,w,h=r;click_at(window,x+w//2,y+h//2)
                def shot(name):time.sleep(.3);subprocess.run(['import','-window',window,str(out/name)],check=True,timeout=10)
                def paste(win,value):
                    key_at(win,'a',4)
                    if not value:key_at(win,'BackSpace');return
                    clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    clip.communicate(value.encode(),timeout=5);key_at(win,'v',4);time.sleep(.15)
                def dismiss():
                    for _ in range(3):
                        if state()['panel']==0:break
                        key_at(window,'Escape');time.sleep(.25)
                    wait_until(lambda:state()['panel']==0,'Panel stayed open')
                def manager():
                    dismiss();click(state()['start_page']['manage']);wait_until(lambda:state().get('shortcuts_ui') is not None,'No link manager')
                def seek(id):
                    if state()['panel']!=4:click_at(window,width-53,55)
                    wait_until(lambda:state().get('settings_ui') is not None,'No settings')
                    key_at(window,'f',4);type_text(window,id);key_at(window,'Return')
                    wait_until(lambda:(state().get('settings_ui') or {}).get('search')==id,'No setting')
                    return next(c for c in state()['settings_ui']['controls'] if c['id']==id)
                def toggle(id,value):
                    if state()['settings'][id]!=value:
                        c=seek(id);x,y,w,h=c['bounds'];click_at(window,x+w-22,y+26)
                        wait_until(lambda:state()['settings'][id]==value,'Toggle failed '+id)
                def choose(id,value):
                    c=seek(id);click(next(o['bounds'] for o in c['options'] if o['value']==value))
                    wait_until(lambda:state()['settings'][id]==value,'Choice failed '+id)
                def import_path(path):
                    c=seek('ntp_image_path');x,y,w,h=c['bounds'];click_at(window,x+60,y+82);paste(window,str(path));key_at(window,'Return')
                def colors():return state()['start_page']['theme_colors']
                def verify_contrast(values):
                    def lum(value):
                        channels=[((value>>s)&255)/255 for s in (16,8,0)]
                        channels=[c/12.92 if c<=.04045 else ((c+.055)/1.055)**2.4 for c in channels]
                        return sum(c*w for c,w in zip(channels,(.2126,.7152,.0722)))
                    def ratio(a,b):a,b=sorted((lum(a),lum(b)));return (b+.05)/(a+.05)
                    assert ratio(values[2],values[0])>=4.5,values
                    assert ratio(values[3],values[0])>=4.5,values
                    assert ratio(values[4],values[1])>=4.5,values
                def picker_window():
                    pid=state()['start_page']['picker_pid']
                    result=subprocess.run(['xdotool','search','--onlyvisible','--pid',str(pid)],capture_output=True,text=True,timeout=5)
                    return result.stdout.strip().splitlines()[-1] if result.returncode==0 and result.stdout.strip() else None
                if launch==0:
                    assert len(state()['start_page']['links'])==2,'Legacy shortcut migration failed'
                    baseline=colors();manager()
                    for i in range(7):
                        ui=state()['shortcuts_ui'];click(ui['title']);paste(window,'Ссылка '+str(i));click(ui['url']);paste(window,'https://example.test/'+str(i))
                        before=len(ui['items']);click(ui['save']);wait_until(lambda:len(state()['shortcuts_ui']['items'])==before+1,'Could not add more shortcuts')
                    ui=state()['shortcuts_ui'];click(ui['url']);paste(window,'javascript:alert(1)');click(ui['save'])
                    wait_until(lambda:bool(state()['shortcuts_ui']['error']),'Invalid URL was accepted')
                    assert len(state()['shortcuts_ui']['items'])==9 and state()['shortcuts_ui']['url_value']=='javascript:alert(1)'
                    click(ui['url']);paste(window,'https://tenth.test/');click(ui['save']);wait_until(lambda:len(state()['shortcuts_ui']['items'])==10,'Tenth shortcut missing')
                    key_at(window,'Home');time.sleep(.3);click(state()['shortcuts_ui']['items'][0]['edit'])
                    click(state()['shortcuts_ui']['title']);paste(window,'Моя ссылка');click(state()['shortcuts_ui']['save'])
                    wait_until(lambda:state()['shortcuts_ui']['items'][0]['title']=='Моя ссылка','Edit did not save')
                    click(state()['shortcuts_ui']['items'][0]['down']);wait_until(lambda:state()['shortcuts_ui']['items'][1]['title']=='Моя ссылка','Reorder failed')
                    click(state()['shortcuts_ui']['items'][1]['remove']);wait_until(lambda:len(state()['shortcuts_ui']['items'])==9,'Delete failed')
                    click(state()['shortcuts_ui']['undo']);wait_until(lambda:len(state()['shortcuts_ui']['items'])==10,'Undo failed')
                    expected_links=[(x['title'],x['url']) for x in state()['shortcuts_ui']['items']]
                    shot('shortcuts-10-ru.png');dismiss();shot('ten-shortcuts.png')
                    c=seek('ntp_image_path');click(c['browse'])
                    wait_until(lambda:state()['start_page']['picker_pid']>0,'File chooser did not start')
                    wait_until(lambda:picker_window() is not None,'No native file chooser')
                    chooser=picker_window()
                    subprocess.run(['xdotool','windowfocus','--sync',chooser],check=True)
                    subprocess.run(['xdotool','key','--clearmodifiers','ctrl+l'],check=True);time.sleep(.3)
                    clip=subprocess.Popen(['xclip','-selection','clipboard','-in','-loops','1'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                    clip.communicate(str(warm).encode(),timeout=5)
                    subprocess.run(['xdotool','key','--clearmodifiers','ctrl+a','ctrl+v'],check=True);time.sleep(.4)
                    subprocess.run(['import','-window',chooser,str(out/'native-chooser.png')],check=True,timeout=10)
                    subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True);time.sleep(1)
                    if state()['start_page']['picker_pid']>0:subprocess.run(['xdotool','key','--clearmodifiers','Return'],check=True)
                    wait_until(lambda:state()['start_page']['image_loaded'] and state()['settings']['ntp_background']=='image','Native file chooser did not import image',timeout=20)
                    dismiss();assert colors()!=baseline;verify_contrast(colors());shot('warm-auto-theme.png')
                    warm_colors=colors();toggle('ntp_image_theme',False);dismiss();assert colors()==baseline
                    toggle('ntp_image_theme',True);dismiss();assert colors()==warm_colors
                    choose('ntp_image_fit','contain');dismiss();shot('image-contain.png')
                    import_path(cool);wait_until(lambda:state()['settings']['ntp_image_path'].endswith('.jpg'),'JPEG did not import');dismiss()
                    wait_until(lambda:colors()!=warm_colors,'Palette did not follow a different image');verify_contrast(colors());shot('cool-auto-theme.png')
                    expected_colors=colors();managed=Path(state()['settings']['ntp_image_path']);assert managed.exists() and managed.parent==profile
                    original=state()['settings']['ntp_image_path'];import_path(bad)
                    wait_until(lambda:(state().get('settings_ui') or {}).get('validation_error')=='ntp_image_path','Oversized image was accepted')
                    assert state()['settings']['ntp_image_path']==original and colors()==expected_colors
                    shot('image-validation.png');dismiss()
                    c=seek('ntp_image_path');click(c['browse']);wait_until(lambda:state()['start_page']['picker_pid']>0,'No second chooser')
                    wait_until(lambda:picker_window() is not None,'No chooser to cancel');subprocess.run(['xdotool','windowfocus','--sync',picker_window()],check=True);subprocess.run(['xdotool','key','--clearmodifiers','Escape'],check=True)
                    wait_until(lambda:state()['start_page']['picker_pid']<0,'Chooser did not cancel');dismiss();assert colors()==expected_colors
                    warm.unlink();cool.unlink() # The imported copies must work without their source files.
                    subprocess.run(['xdotool','windowsize','--sync',window,'900','620'],check=True);width=900;time.sleep(.3)
                    key_at(window,'End');shot('photo-small-ru.png')
                else:
                    wait_until(lambda:state()['start_page']['image_loaded'],'Imported image did not load after restart')
                    assert colors()==expected_colors and len(state()['start_page']['links'])==10
                    manager();assert [(x['title'],x['url']) for x in state()['shortcuts_ui']['items']]==expected_links;dismiss()
                    key_at(window,'F7');time.sleep(.3);verify_contrast(colors());shot('photo-light.png');key_at(window,'F7')
                    seek('ntp_image_path');choose('language','en');dismiss();shot('photo-en.png')
                    click_at(window,width-53,55);wait_until(lambda:state().get('settings_ui') is not None,'No settings')
                    key_at(window,'f',4);type_text(window,'Remove background image');key_at(window,'Return')
                    wait_until(lambda:any(x['id']=='wallpaper_clear' for x in state()['settings_ui']['controls']),'No clear image action')
                    c=next(x for x in state()['settings_ui']['controls'] if x['id']=='wallpaper_clear');x,y,w,h=c['bounds'];click_at(window,x+80,y+82)
                    wait_until(lambda:state()['settings']['ntp_image_path']=='','Image did not clear');dismiss()
                    assert not state()['start_page']['image_loaded'] and not (profile/'wallpaper.jpg').exists()
                    manager();shot('shortcuts-en.png');dismiss()
                click_at(window,width-28,15);proc.wait(timeout=20);assert proc.returncode==0
                data=json.loads(report.read_text());assert data['cef_created']==data['cef_closed'];assert not data['audit'].get('renderer_crashes');reports.append(data)
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid,signal.SIGTERM)
                    try:proc.wait(timeout=4)
                    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    (out/'verification.json').write_text(json.dumps({'passed':True,'launches':reports},indent=2))
    print('Wallpaper PASS: 10 links, migration/edit/order/undo, native chooser/cancel, 24 MP PNG / 33 MP JPEG, auto theme/contrast, corrupt image rejection, managed copy and restart',flush=True)
if __name__=='__main__':main()
