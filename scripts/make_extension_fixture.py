"""Generate small signed CRX3 fixtures using an ephemeral local test key."""
import io,zipfile,json,subprocess,hashlib,struct,tempfile,zlib
from pathlib import Path
from dependencies import ROOT

def varint(n):
    out=b''
    while n>127:out+=bytes([(n&127)|128]);n>>=7
    return out+bytes([n])
def field(n,data):return varint(n<<3|2)+varint(len(data))+data

def main():
    dest=ROOT/'tests/fixtures/extensions';dest.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aura-test-crx-') as temporary:
        d=Path(temporary);key=d/'key.pem';payload=d/'payload';sig=d/'sig'
        subprocess.run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(key)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
        pub=subprocess.check_output(['openssl','pkey','-in',str(key),'-pubout','-outform','DER'])
        crxid=hashlib.sha256(pub).digest()[:16]
        fixture_id=''.join(chr(97+(b>>4))+chr(97+(b&15)) for b in crxid)
        manifest={'manifest_version':3,'name':'Aura MV3 integration test','version':'1.0','permissions':['storage','declarativeNetRequest'],'host_permissions':['http://127.0.0.2/*'],'background':{'service_worker':'worker.js'},'content_scripts':[{'matches':['http://127.0.0.2/*'],'js':['content.js']}],'action':{'default_popup':'popup.html','default_icon':{'32':'icon.png'}},'options_page':'options.html'}
        content="""chrome.runtime.sendMessage({test:true}).then(result=>{document.documentElement.dataset.auraExtension=JSON.stringify({...result,content:true,id:chrome.runtime.id});});"""
        worker="""chrome.runtime.onMessage.addListener((msg,sender,reply)=>{(async()=>{await chrome.storage.local.set({aura:true});let data=await chrome.storage.local.get('aura');await chrome.declarativeNetRequest.updateDynamicRules({removeRuleIds:[1],addRules:[{id:1,priority:1,action:{type:'block'},condition:{urlFilter:'/extension-blocked.js',resourceTypes:['script']}}]});reply({worker:true,storage:data.aura});})();return true;});"""
        popup_html="""<!doctype html><html><head><meta charset="utf-8"><title>Aura extension popup</title><style>
html{width:320px;background:#fff;color:#222}body{position:relative;width:288px;height:176px;margin:0;padding:16px;font:14px sans-serif}h2{margin:0;font-size:18px}input{position:absolute;top:48px;left:16px;width:288px;height:32px;box-sizing:border-box}button{position:absolute;top:96px;width:86px;height:30px}#apply{left:16px}#grow{left:112px}#close{left:208px}#result{position:absolute;left:16px;top:144px;margin:0}
</style></head><body><h2>Extension popup</h2><input id="value" aria-label="Value"><button id="apply">Apply</button><button id="grow">Grow</button><button id="close">Close</button><p id="result">Ready</p><script src="popup.js"></script></body></html>"""
        popup_js="""const input=document.getElementById('value');chrome.storage.local.get('popupValue').then(data=>{input.value=data.popupValue||'';if(input.value)document.title='Popup restored: '+input.value;});document.getElementById('apply').onclick=async()=>{if(!confirm('Save this value?'))return;await chrome.storage.local.set({popupValue:input.value});const result=await chrome.runtime.sendMessage({test:true});document.getElementById('result').textContent='Saved';document.title='Popup saved: '+input.value+' / '+result.worker;};document.getElementById('grow').onclick=()=>{document.documentElement.style.width='460px';document.body.style.width='428px';document.body.style.height='420px';};document.getElementById('close').onclick=()=>window.close();"""
        def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
        pixels=b''.join(b'\0'+b''.join(bytes((120,100,235,255)) if not (10<=x<22 and 7<=y<25) else bytes((255,255,255,255)) for x in range(32)) for y in range(32))
        icon=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',32,32,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b'')
        files={'manifest.json':json.dumps(manifest),'content.js':content,'worker.js':worker,'icon.png':icon,'popup.html':popup_html,'popup.js':popup_js,'options.html':'<!doctype html><title>Aura extension options</title><h1>Extension options work</h1>'}
        def package(name,entries):
            stream=io.BytesIO()
            with zipfile.ZipFile(stream,'w',zipfile.ZIP_DEFLATED) as z:
                for path,data in entries.items():z.writestr(path,data)
            zipped=stream.getvalue();signed=field(1,crxid)
            payload.write_bytes(b'CRX3 SignedData\0'+struct.pack('<I',len(signed))+signed+zipped)
            subprocess.run(['openssl','dgst','-sha256','-sign',str(key),'-out',str(sig),str(payload)],check=True)
            header=field(2,field(1,pub)+field(2,sig.read_bytes()))+field(10000,signed)
            (dest/name).write_bytes(b'Cr24'+struct.pack('<II',3,len(header))+header+zipped)
        package('valid.crx',files)
        package('traversal.crx',{**files,'../escape.js':'bad'})
        package('mv2.crx',{'manifest.json':json.dumps({**manifest,'manifest_version':2})})
        (dest/'id.txt').write_text(fixture_id+'\n')
        print(fixture_id)
if __name__=='__main__':main()
