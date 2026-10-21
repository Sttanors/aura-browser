// Runs in Aura's isolated CDP world, never the website's JavaScript world.
let auraConfig = {};
let auraHidden = new Set();
let auraDirty = true;
let auraStyle = null;
globalThis.__auraSelectors = (selectors, reset = false) => {
    if (reset) auraHidden.clear();
    selectors.forEach(s => auraHidden.add(s));
    if (!auraStyle || !auraStyle.isConnected) {
        auraStyle = document.createElement('style');
        auraStyle.className = 'aura-cosmetic-filters';
        (document.head || document.documentElement)?.append(auraStyle);
    }
    // Separate rules: an unsupported selector must not invalidate other selectors.
    auraStyle.textContent = [...auraHidden].map(s => `${s}{display:none!important}`).join('\n');
};
globalThis.__auraApply = config => {
    auraConfig = config;
    if(!document.documentElement){
        const rootObserver=new MutationObserver(()=>{if(document.documentElement){rootObserver.disconnect();globalThis.__auraApply(auraConfig);}});
        rootObserver.observe(document,{childList:true});return;
    }
    const except = list => list.split(/[\s,;]+/).some(d => d && (location.hostname === d.toLowerCase() || location.hostname.endsWith('.' + d.toLowerCase())));
    const dark = config.dark && !except(config.darkExceptions);
    if (dark) DarkReader.enable({brightness:config.brightness, contrast:config.contrast, sepia:config.sepia});
    else DarkReader.disable();
    auraHidden.clear();
    if(auraStyle) auraStyle.textContent='';
    auraDirty = true;
};
const auraScan = () => {
    if (!document.documentElement || !auraConfig.ads || !auraConfig.cosmetics || !auraDirty) return;
    auraDirty = false;
    const classes = new Set(), ids = new Set();
    const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_ELEMENT);
    let n=walker.currentNode, count=0;
    do {
        if (n.id && ids.size < 3000) ids.add(n.id.slice(0,256));
        if (classes.size < 3000) n.classList?.forEach(c => classes.add(c.slice(0,256)));
    } while ((n=walker.nextNode()) && ++count<15000);
    globalThis.auraProtection(JSON.stringify({url:location.href, classes:[...classes], ids:[...ids]}));
};
const auraStart = () => {
    new MutationObserver(records => {
        if (records.some(r => r.type === 'attributes' || !(r.target instanceof Element) || (!r.target.closest('.darkreader,.aura-cosmetic-filters') && ![...r.addedNodes].every(n => n instanceof Element && (n.classList.contains('darkreader') || n.classList.contains('aura-cosmetic-filters')))))) auraDirty=true;
    }).observe(document.documentElement,{childList:true,subtree:true,attributes:true,attributeFilter:['class','id']});
    auraScan();
    // Bound scanning rate even on mutation-heavy pages; no perpetual animation loop.
    setInterval(auraScan,1200);
};
if(document.documentElement)auraStart();else document.addEventListener('DOMContentLoaded',auraStart,{once:true});

// Procedural rules only perform literal DOM matching and CSS styling. They do
// not evaluate filter text as JavaScript and do not execute regular expressions.
const auraProcAttr = 'data-aura-proc-' + Math.random().toString(36).slice(2);
let auraProcRules = [], auraProcResults = [], auraProcSeen = [], auraProcCursor = 0;
let auraProcEpoch = 1, auraProcKey = '', auraProcStyle = null;
let auraProcPath = location.pathname + location.search;
const auraProcMarks = new Map();
const auraProcClear = () => {
    for (const node of auraProcMarks.keys()) node.removeAttribute(auraProcAttr);
    auraProcMarks.clear();
    auraProcRules = []; auraProcResults = []; auraProcSeen = []; auraProcKey = '';
    if (auraProcStyle) auraProcStyle.textContent = '';
};
const auraLiteral = value => {
    value = String(value).trim();
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) return value.slice(1,-1);
    return value;
};
const auraProcSelect = (rule, deadline) => {
    let nodes = [document];
    for (const op of rule.selector || []) {
        if (performance.now() > deadline) return null;
        const arg = String(op.arg || ''), type = op.type;
        if (type === 'css-selector') {
            const next = new Set();
            for (const node of nodes) {
                if (performance.now() > deadline) return null;
                let selector = arg.trim();
                if (!selector) continue;
                if (/^[>+~]/.test(selector)) selector = ':scope ' + selector;
                try { for (const found of node.querySelectorAll(selector)) {next.add(found);if(next.size>=1000)break;} } catch (_) {}
                if(next.size>=1000)break;
            }
            nodes = [...next];
        } else if (type === 'upward') {
            const next = new Set();
            for (let node of nodes) {
                if (/^\d+$/.test(arg)) {let depth=Math.min(32,Number(arg));while(depth-- && node)node=node.parentElement;}
                else {try {node=node.parentElement?.closest(arg);} catch (_) {node=null;}}
                if (node instanceof Element && node !== document.documentElement && node !== document.body)next.add(node);
            }
            nodes=[...next];
        } else {
            const next=[];
            for(const node of nodes) {
                if(performance.now()>deadline)return null;
                if(!(node instanceof Element))continue;
                let match=false;
                if(type==='has-text') match=(node.textContent||'').slice(0,65536).includes(auraLiteral(arg));
                else if(type==='min-text-length') match=(node.textContent||'').length>=Number(arg);
                else if(type==='matches-path') match=(location.pathname+location.search).includes(auraLiteral(arg));
                else if(type==='matches-attr') {
                    const at=arg.indexOf('=');
                    if(at<0)match=node.hasAttribute(arg.trim());
                    else match=node.getAttribute(arg.slice(0,at).trim())===auraLiteral(arg.slice(at+1));
                } else if(['matches-css','matches-css-before','matches-css-after'].includes(type)) {
                    const at=arg.indexOf(':');
                    if(at>0) {
                        const pseudo=type==='matches-css-before'?'::before':type==='matches-css-after'?'::after':null;
                        match=getComputedStyle(node,pseudo).getPropertyValue(arg.slice(0,at).trim()).trim()===auraLiteral(arg.slice(at+1));
                    }
                }
                if(match)next.push(node);
            }
            nodes=next;
        }
        if(!nodes.length)break;
    }
    return new Set(nodes.filter(node=>node instanceof Element&&node!==document.documentElement&&node!==document.body));
};
const auraProcRun = () => {
    const path=location.pathname+location.search;
    if(path!==auraProcPath){auraProcPath=path;auraProcEpoch++;}
    if(!auraConfig.ads||!auraConfig.cosmetics||!auraProcRules.length)return;
    const deadline=performance.now()+8,epoch=auraProcEpoch;
    let visited=0;
    while(visited++<auraProcRules.length&&performance.now()<deadline) {
        const index=auraProcCursor++%auraProcRules.length;
        if(auraProcSeen[index]===epoch)continue;
        const found=auraProcSelect(auraProcRules[index],deadline);
        if(!found)continue;
        const key='r'+index;
        for(const node of auraProcResults[index]||[])if(!found.has(node)) {
            const keys=auraProcMarks.get(node);keys?.delete(key);
            if(!keys?.size){node.removeAttribute(auraProcAttr);auraProcMarks.delete(node);}
            else node.setAttribute(auraProcAttr,[...keys].join(' '));
        }
        for(const node of found) {
            if(!auraProcMarks.has(node)&&auraProcMarks.size>=2000)continue;
            let keys=auraProcMarks.get(node);if(!keys){keys=new Set();auraProcMarks.set(node,keys);}
            keys.add(key);node.setAttribute(auraProcAttr,[...keys].join(' '));
        }
        auraProcResults[index]=found;auraProcSeen[index]=epoch;
    }
};
const auraProcSet = rules => {
    const key=JSON.stringify(rules);
    if(key===auraProcKey)return;
    auraProcClear();auraProcKey=key;auraProcRules=rules.slice(0,256);auraProcEpoch++;
    if(!auraProcStyle||!auraProcStyle.isConnected) {
        auraProcStyle=document.createElement('style');auraProcStyle.className='aura-cosmetic-filters';
        (document.head||document.documentElement)?.append(auraProcStyle);
    }
    auraProcStyle.textContent=auraProcRules.map((rule,i)=>`[${auraProcAttr}~="r${i}"]{${rule.action?.type==='style'?rule.action.arg:'display:none!important'}}`).join('\n');
    auraProcRun();
};
const auraOriginalSelectors=globalThis.__auraSelectors;
globalThis.__auraSelectors = (payload,reset=false) => {
    const selectors=Array.isArray(payload)?payload:(payload.selectors||[]);
    auraOriginalSelectors(selectors,reset);
    auraProcSet(Array.isArray(payload)?[]:(payload.procedural||[]));
};
const auraOriginalApply=globalThis.__auraApply;
globalThis.__auraApply=config=>{auraProcClear();auraOriginalApply(config);};
const auraProcWatch = () => new MutationObserver(records=>{
    if(records.some(r=>r.attributeName!==auraProcAttr&&(!(r.target instanceof Element)||!r.target.closest('.darkreader,.aura-cosmetic-filters'))))auraProcEpoch++;
}).observe(document.documentElement,{subtree:true,childList:true,characterData:true,attributes:true});
if(document.documentElement)auraProcWatch();else document.addEventListener('DOMContentLoaded',auraProcWatch,{once:true});
setInterval(auraProcRun,200);
