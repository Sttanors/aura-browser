(() => {
  const dictionary = document.spellCheckCustomDictionary;
  let owned = [];
  globalThis.__auraSpellingApply = words => {
    if (!dictionary) return false;
    const next = new Set(words);
    dictionary.removeWords(owned.filter(word => !next.has(word)));
    dictionary.addWords(words);
    owned = words.slice();
    return true;
  };
})();
