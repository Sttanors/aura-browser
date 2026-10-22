/* Aura local reader: article markup is never inserted into the live document. */
function auraOpenReader(config) {
    if (globalThis.__auraReader) { globalThis.__auraReader.close(); return false; }
    if(document.getElementsByTagName('*').length>50000) return 'too_large';
    const clone = document.cloneNode(true);
    const article = new Readability(clone, {maxElemsToParse: 50000, serializer: element => element}).parse();
    if (!article || !article.content || article.textContent.trim().length < 300) return false;
    if(article.textContent.length>2000000) return 'too_large';
    const blocks = [];
    let oversized=false;
    function collect(node) {
        if (blocks.length >= 2000) {oversized=true;return;}
        if (node.nodeType !== 1) return;
        if (/^(P|H[1-6]|LI|PRE|BLOCKQUOTE)$/.test(node.tagName)) {
            const text = node.textContent.replace(/\s+/g, ' ').trim();
            if (text) blocks.push({tag: /^H/.test(node.tagName) ? 'h2' : node.tagName === 'PRE' ? 'pre' : node.tagName === 'BLOCKQUOTE' ? 'blockquote' : 'p', text});
        } else for (const child of node.children) collect(child);
    }
    collect(article.content);
    if(oversized) return 'too_large';
    if (!blocks.length) blocks.push({tag:'p',text:article.textContent});
    const previous = document.activeElement;
    const host = document.createElement('dialog');
    for (const [key,value] of Object.entries({position:'fixed',inset:'0',margin:'0',padding:'0',width:'100vw',height:'100vh','max-width':'none','max-height':'none',border:'0',background:config.dark?'#0D111B':'#FAFBFF',color:config.dark?'#EDF3FF':'#182033','box-sizing':'border-box'})) host.style.setProperty(key,value,'important');
    host.setAttribute('aria-label',config.labels['Reader mode']);
    const mount = document.createElement('div');
    mount.style.height='100%';host.append(mount);
    const shadow = mount.attachShadow({mode:'closed'});
    const sheet = new CSSStyleSheet();
    sheet.replaceSync(`:host{color-scheme:${config.dark?'dark':'light'}}*{box-sizing:border-box}.shell{height:100%;overflow:auto;background:${config.dark?'#0D111B':'#FAFBFF'};color:${config.dark?'#EDF3FF':'#182033'};font:14px/1.5 system-ui,sans-serif}.bar{position:sticky;top:0;background:inherit;border-bottom:1px solid ${config.dark?'#49556C':'#8390A8'};padding:12px 24px;display:flex;align-items:center;gap:12px;flex-wrap:wrap;z-index:1}.bar strong{margin-right:auto;font-size:16px}button,select{font:inherit;color:inherit;background:transparent;border:1px solid ${config.dark?'#71809D':'#74829A'};border-radius:6px;min-height:36px;padding:4px 10px}button:not(:disabled):hover,select:hover{background:${config.dark?'#27314C':'#E1E7FA'}}button:disabled{opacity:.45}button:focus-visible,select:focus-visible,.shell:focus-visible{outline:2px solid ${config.dark?'#A7B6FF':'#3F51B5'};outline-offset:2px}label{display:flex;align-items:center;gap:8px;flex-wrap:wrap;font-size:12px}.size{display:flex;align-items:center;gap:8px}article{max-width:var(--reader-width);margin:36px auto 80px;padding:0 24px;font:var(--reader-size)/1.7 var(--reader-font);overflow-wrap:anywhere}h1{font-size:1.8em;line-height:1.25;margin:0 0 16px}h2{font-size:1.3em;line-height:1.4;margin:1.5em 0 .5em}p{margin:0 0 1.1em}.meta{font:13px/1.5 system-ui,sans-serif;opacity:.8;margin-bottom:32px}pre{font: .85em/1.6 monospace;white-space:pre-wrap}blockquote{border-left:3px solid #7B8ECC;margin-left:0;padding-left:20px}@media(max-width:600px){.bar{padding:10px 12px;gap:8px}article{margin-top:24px;padding:0 16px}}`);
    shadow.adoptedStyleSheets = [sheet];
    function element(tag,text,parent) {
        const node=document.createElement(tag);
        if(text!==undefined) node.textContent=text;
        if(parent) parent.append(node);
        return node;
    }
    const shell=element('div',undefined,shadow);shell.className='shell';shell.tabIndex=0;
    const bar=element('header',undefined,shell);bar.className='bar';
    element('strong',config.labels['Reader mode'],bar);
    const exit=element('button',config.labels['Exit reader'],bar);exit.type='button';
    const controls=element('div',undefined,shell);controls.className='bar';controls.style.position='static';
    const sizeLabel=element('label',config.labels['Text size'],controls);
    const size=element('span',undefined,sizeLabel);size.className='size';
    const smaller=element('button','A−',size);smaller.setAttribute('aria-label',config.labels['Text size']+' −');
    const sizeValue=element('span','',size);
    const larger=element('button','A+',size);larger.setAttribute('aria-label',config.labels['Text size']+' +');
    function choice(title,key,values) {
        const label=element('label',config.labels[title],controls),select=element('select',undefined,label);
        select.setAttribute('aria-label',config.labels[title]);
        for(const [value,name] of values) {const option=element('option',config.labels[name],select);option.value=value;}
        select.value=String(config[key]);select.onchange=()=>{config[key]=key==='width'?Number(select.value):select.value;apply(true);};
        return select;
    }
    const family=choice('Reader font','font',[['serif','Serif'],['sans','Sans serif']]);
    const width=choice('Reader column width','width',[[560,'Narrow'],[720,'Medium'],[900,'Wide']]);
    const body=element('article',undefined,shell);body.setAttribute('aria-label',article.title||config.labels['Reader mode']);
    element('h1',(article.title||document.title).slice(0,500),body);
    const meta=element('div',[article.byline?.slice(0,1000),location.hostname].filter(Boolean).join(' — '),body);meta.className='meta';
    for(const block of blocks) element(block.tag,block.text,body);
    function notify(value) {globalThis.auraReaderState(JSON.stringify(value));}
    function apply(save) {
        shell.style.setProperty('--reader-size',config.size+'px');
        shell.style.setProperty('--reader-width',config.width+'px');
        shell.style.setProperty('--reader-font',config.font==='serif'?'Georgia, serif':'system-ui, sans-serif');
        sizeValue.textContent=config.size+' px';smaller.disabled=config.size<=18;larger.disabled=config.size>=32;
        if(save) {notify({action:'preferences',size:config.size,width:config.width,font:config.font});requestAnimationFrame(report);}
    }
    function report() {
        if(!host.isConnected) return;
        const rect=node=>{const r=node.getBoundingClientRect();return [r.x,r.y,r.width,r.height].map(Math.round);};
        notify({action:'view',blocks:blocks.length,characters:article.textContent.length,scroll:Math.round(shell.scrollTop),exit:rect(exit),smaller:rect(smaller),larger:rect(larger),font:rect(family),width:rect(width),body:rect(body),text:rect(body.querySelector('p')||body)});
    }
    let pending=null;
    function schedule(){if(pending===null)pending=setTimeout(()=>{pending=null;report();},100);}
    function close() {
        if(!host.isConnected) {delete globalThis.__auraReader;notify({action:'closed'});return;}
        host.close();host.remove();delete globalThis.__auraReader;
        window.removeEventListener('resize',schedule);if(pending!==null)clearTimeout(pending);
        if(previous?.isConnected) previous.focus({preventScroll:true});
        notify({action:'closed'});
    }
    smaller.onclick=()=>{config.size=Math.max(18,config.size-2);apply(true);};
    larger.onclick=()=>{config.size=Math.min(32,config.size+2);apply(true);};
    exit.onclick=close;host.addEventListener('cancel',event=>{event.preventDefault();close();});
    globalThis.__auraReader={close};apply(false);
    document.documentElement.append(host);host.showModal();exit.focus({preventScroll:true});
    window.addEventListener('resize',schedule);shell.addEventListener('scroll',schedule);requestAnimationFrame(report);
    return true;
}
