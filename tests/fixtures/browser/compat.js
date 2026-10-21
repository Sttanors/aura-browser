(async()=>{
const params=new URLSearchParams(location.search),remote=params.get('remote');
const results={checks:{},capabilities:{},errors:{}};
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const limit=(promise,ms=8000)=>Promise.race([promise,new Promise((_,reject)=>setTimeout(()=>reject(Error('Timed out')),ms))]);
async function check(name,run){try{results.checks[name]=Boolean(await limit(Promise.resolve().then(run)));}catch(error){results.checks[name]=false;results.errors[name]=String(error);}}
async function frame(url){const element=document.createElement('iframe');element.src=url;document.body.append(element);await limit(new Promise((resolve,reject)=>{element.onload=resolve;element.onerror=reject;}));return element;}
await check('secure_context',()=>isSecureContext&&!!crypto.subtle);
await check('unicode_file',async()=>await new File(['Привет / English'],'Пример.txt',{type:'text/plain'}).text()==='Привет / English');
await check('canvas',()=>{const c=document.createElement('canvas');c.width=c.height=2;const x=c.getContext('2d');x.fillStyle='#123456';x.fillRect(0,0,2,2);return String(x.getImageData(0,0,1,1).data)==='18,52,86,255';});
await check('webcrypto',async()=>{const k=await crypto.subtle.generateKey({name:'AES-GCM',length:128},true,['encrypt','decrypt']),iv=crypto.getRandomValues(new Uint8Array(12)),text=new TextEncoder().encode('Русский / English');const encrypted=await crypto.subtle.encrypt({name:'AES-GCM',iv},k,text);return new TextDecoder().decode(await crypto.subtle.decrypt({name:'AES-GCM',iv},k,encrypted))==='Русский / English';});
await check('worker',async()=>{const w=new Worker('/compat-worker.js');try{const value=await limit(new Promise((resolve,reject)=>{w.onmessage=e=>resolve(e.data);w.onerror=reject;w.postMessage([1,2,3]);}));return value.sum===6&&value.text==='Привет / English';}finally{w.terminate();}});
await check('indexeddb',async()=>{const open=indexedDB.open('aura-compat',1);open.onupgradeneeded=()=>open.result.createObjectStore('data');const db=await limit(new Promise((resolve,reject)=>{open.onsuccess=()=>resolve(open.result);open.onerror=()=>reject(open.error);}));try{const request=db.transaction('data').objectStore('data').get('marker');results.capabilities.initial_indexeddb=await new Promise((resolve,reject)=>{request.onsuccess=()=>resolve(request.result||'');request.onerror=()=>reject(request.error);});await new Promise((resolve,reject)=>{const t=db.transaction('data','readwrite');t.objectStore('data').put('fictional-persistent-marker','marker');t.oncomplete=resolve;t.onerror=()=>reject(t.error);});return true;}finally{db.close();}});
await check('cache_storage',async()=>{const cache=await caches.open('aura-compat');await cache.put('/cached-result',new Response('cache-content'));return await(await cache.match('/cached-result')).text()==='cache-content';});
await check('service_worker',async()=>{await navigator.serviceWorker.register('/compat-sw.js');await navigator.serviceWorker.ready;if(!navigator.serviceWorker.controller)await limit(new Promise(resolve=>navigator.serviceWorker.addEventListener('controllerchange',resolve,{once:true})));return await(await fetch('/sw-probe')).text()==='offline-ready';});
await check('cors_allow',async()=>await(await fetch(remote+'/cors-allowed')).text()==='allowed');
await check('cors_deny',async()=>{try{await fetch(remote+'/cors-denied');return false;}catch(error){return error instanceof TypeError;}});
await check('cross_origin_frame',async()=>{const f=await frame(remote+'/foreign');try{void f.contentWindow.document;return false;}catch(error){return error.name==='SecurityError';}finally{f.remove();}});
await check('csp',async()=>{const f=await frame('/csp.html');const passed=f.contentWindow.externalRan===true&&f.contentWindow.inlineRan!==true;f.remove();return passed;});
await check('webm_playback',async()=>{const v=document.createElement('video');v.src='/compat.webm';v.muted=true;v.autoplay=true;v.loop=true;document.body.append(v);try{await v.play();await wait(350);return v.currentTime>0&&v.videoWidth===160&&v.videoHeight===90;}finally{v.pause();v.remove();}});
await check('popups_blocked',()=>{const popup=window.open('/foreign','aura-compat-popup');if(popup){popup.close();return false;}return true;});
const video=document.createElement('video');results.capabilities.webm=video.canPlayType('video/webm; codecs="vp9,opus"');results.capabilities.h264=video.canPlayType('video/mp4; codecs="avc1.42E01E"');results.capabilities.hevc=video.canPlayType('video/mp4; codecs="hvc1"');
results.capabilities.webgl=!!document.createElement('canvas').getContext('webgl');results.capabilities.user_agent=navigator.userAgent;
try{await navigator.requestMediaKeySystemAccess('com.widevine.alpha',[{initDataTypes:['cenc'],videoCapabilities:[{contentType:'video/mp4; codecs="avc1.42E01E"'}]}]);results.capabilities.widevine=true;}catch(_){results.capabilities.widevine=false;}
results.passed=Object.values(results.checks).every(Boolean);document.title=results.passed?'Compatibility PASS':'Compatibility FAIL';document.querySelector('#result').textContent=JSON.stringify(results,null,2);
await fetch('/compat-report',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(results)});
})();
