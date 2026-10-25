/* Shared text-only extraction for reader and offline library. */
function auraExtractArticle() {
    if(document.getElementsByTagName('*').length>50000) return 'too_large';
    const clone = document.cloneNode(true);
    clone.querySelectorAll("[data-aura-reader]").forEach(node=>node.remove());
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
    return {title:(article.title||document.title).slice(0,500),byline:(article.byline||"").slice(0,1000),blocks,textContent:article.textContent};
}
