(()=>{
    // Return only a busy flag, never field values. Work is deliberately bounded.
    const started = performance.now();
    let seen = 0;
    try {
        if (document.pictureInPictureElement || window.onbeforeunload || window.frames.length)
            return true;
        const walker = document.createTreeWalker(document, NodeFilter.SHOW_ELEMENT);
        for (let node; (node = walker.nextNode());) {
            if (++seen > 10000 || performance.now() - started > 15) return true;
            if (node.shadowRoot || node.localName.includes('-') ||
                node.isContentEditable || node.tagName === 'CANVAS') return true;
            if (node.tagName === 'AUDIO' || node.tagName === 'VIDEO') {
                if (!node.paused && !node.ended) return true;
            } else if (node.tagName === 'INPUT') {
                if (node.type === 'file' && node.files.length) return true;
                if (node.value !== node.defaultValue || node.checked !== node.defaultChecked) return true;
            } else if (node.tagName === 'TEXTAREA') {
                if (node.value !== node.defaultValue) return true;
            } else if (node.tagName === 'SELECT') {
                for (const option of node.options) {
                    if (++seen > 10000 || performance.now() - started > 15) return true;
                    if (option.selected !== option.defaultSelected) return true;
                }
            }
        }
        return performance.now() - started > 15;
    } catch (_) { return true; }
})()
