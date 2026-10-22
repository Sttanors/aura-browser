// CDP inspection of a disposable Hello translation test; reject optional consent only.
const port=Number(process.argv[2]), host=process.argv[3], mode=process.argv[4]??'text';
if(!Number.isInteger(port)||port<1||port>65535) throw Error('Invalid local debugging port');
const targets=await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page=targets.find(p=>p.type==='page'&&new URL(p.url).hostname===host)??(host==='translate.google.com'?targets.find(p=>p.type==='page'&&new URL(p.url).hostname==='consent.google.com'):null);
if(!page) throw Error('Provider page is not available');
const socket=new WebSocket(page.webSocketDebuggerUrl);
const pending=new Map();let id=0;
const timer=setTimeout(()=>{socket.close();process.exit(2);},8000);
socket.addEventListener('message',event=>{const message=JSON.parse(String(event.data));if(message.id&&pending.has(message.id)){pending.get(message.id)(message);pending.delete(message.id);}});
await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
function call(method,params={}){return new Promise(resolve=>{pending.set(++id,resolve);socket.send(JSON.stringify({id,method,params}));});}
try {
    const consent=new URL(page.url).hostname==='consent.google.com';
    if(consent) await call('Runtime.evaluate',{expression:`(()=>{const b=Array.from(document.querySelectorAll('button')).find(b=>b.textContent.trim()==='Reject all');if(b){b.click();return true;}return false;})()`});
    const response=await call('Accessibility.getFullAXTree',{depth:30});
    const text=(response.result?.nodes??[]).flatMap(node=>[node.name?.value,node.value?.value]).filter(value=>typeof value==='string').join('\n');
    console.log(JSON.stringify({url:page.url,title:page.title,translated:mode==='page'?text.includes('Пример домена'):text.includes('Привет'),requires_verification:text.includes('Checking if the connection is secure')||text.includes('Verify you are human'),text:text.slice(0,12000)}));
} finally {clearTimeout(timer);socket.close();}
