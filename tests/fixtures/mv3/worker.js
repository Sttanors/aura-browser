chrome.runtime.onMessage.addListener((message, sender, respond) => {
  if (message.type !== 'aura-m0-probe') return false;
  chrome.tabs.query({}, tabs => {
    const error = chrome.runtime.lastError;
    respond({service_worker: true, tabs_query: !error, tab_count: tabs ? tabs.length : 0,
      error: error ? error.message : ''});
  });
  return true;
});
