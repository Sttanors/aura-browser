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
