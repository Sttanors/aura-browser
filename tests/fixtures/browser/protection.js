let dynamicAdded=false,extensionRequested=false;
setInterval(()=>{
  if(!dynamicAdded){const e=document.createElement('div');e.className='ad-banner-container';e.id='dynamic-ad';e.textContent='Dynamic advertisement';document.body.append(e);dynamicAdded=true;}
  let extension=null;try{extension=JSON.parse(document.documentElement.dataset.auraExtension||'null');}catch{}
  if(extension&&!extensionRequested){extensionRequested=true;const s=document.createElement('script');s.src='/extension-blocked.js';document.head.append(s);}
  const f=document.querySelector('#font-frame');
  const fonts=f.contentDocument?.body?{size:f.contentWindow.getComputedStyle(f.contentDocument.body).fontSize,tiny:f.contentDocument.querySelector('span').getBoundingClientRect().height}:null;
  const result={url:location.href,fonts,dark:!!document.querySelector('style.darkreader'),background:getComputedStyle(document.body).backgroundColor,hidden:getComputedStyle(document.querySelector('.ad-banner-container')).display==='none',dynamicHidden:getComputedStyle(document.querySelector('#dynamic-ad')).display==='none',adScript:!!window.auraAdNetwork,extension,extensionRequestRan:!!window.auraExtensionNetwork};
  fetch('/features-probe?data='+encodeURIComponent(JSON.stringify(result)),{cache:'no-store'}).catch(()=>{});
},600);
