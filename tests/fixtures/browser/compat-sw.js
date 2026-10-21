self.addEventListener('install',event=>event.waitUntil(self.skipWaiting()));
self.addEventListener('activate',event=>event.waitUntil(self.clients.claim()));
self.addEventListener('fetch',event=>{if(new URL(event.request.url).pathname==='/sw-probe')event.respondWith(new Response('offline-ready',{headers:{'Content-Type':'text/plain'}}));});
