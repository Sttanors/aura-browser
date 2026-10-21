onmessage=event=>postMessage({sum:event.data.reduce((a,b)=>a+b,0),text:'Привет / English'});
