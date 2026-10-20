chrome.runtime.sendMessage({type: 'aura-m0-probe'}, response => {
  const error = chrome.runtime.lastError;
  document.documentElement.setAttribute('data-aura-mv3', JSON.stringify({
    content_script: true, runtime_messaging: !error, ...response,
    messaging_error: error ? error.message : ''
  }));
});
