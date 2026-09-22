/* AMBILIGHT BRIDGE — PREVIEW DEMO API
 * Visual-only simulator. No ESP32 calls, no credentials, no writes.
 */
(function(){
/* ============================================================
   AMBILIGHT BRIDGE — OFFLINE UI PREVIEW
   Visual-only mode: no ESP32, no auth, no API writes.
   ============================================================ */
(function(){
  const demoState={
    fw:"5.5.2-C3-OFF-FIX", firmware:"5.5.2-C3-OFF-FIX", contract:1,
    tvOnline:true, tv_online:true, tvIP:"192.168.1.148", wifi:true, rssi:-52,
    ip:"192.168.1.228", ap_active:false, ap_ip:"",
    brightness:160, smoothing:70, blackThreshold:4, goodFrames:142619,
    badFrames:1, heap:149336, uptime:28420, segmentCount:12,
    sideCloneEnabled:true, sideCloneBrightness:245, mapperSideBypass:false,
    moodLinkMode:0, tvTopoDetected:true, dyn_on:false, dyn_min:0, dyn_max:255,
    dyn_resp:35, mood_dyn:false, mood_dep:25, tv_sync:false, tv_bsync:false,
    left_mood:{mode:0,effect:3,sat:175,bri:200,speed:96,pal:9,scale:70,motion:65,glow:75,density:55,turb:45,cm:3,auto:false,rev:false,hue:320},
    right_mood:{mode:0,effect:4,sat:255,bri:200,speed:28,pal:6,scale:70,motion:65,glow:75,density:55,turb:45,cm:3,auto:false,rev:false,hue:60},
    zone_current:[[12,22,48],[12,22,48],[12,22,48],[12,22,48]],
    zone_target:[[20,80,220],[255,70,20],[20,80,220],[160,20,255]],
    zones:[{r:20,g:80,b:220},{r:255,g:70,b:20},{r:20,g:80,b:220},{r:160,g:20,b:255}],
    segments:Array.from({length:12},(_,i)=>({start:i*10,count:10,source:[1,1,1,2,2,2,3,3,3,4,4,4][i],brightness:255,reverse:false}))
  };
  const mapper={segments:demoState.segments, sources:["—","LEFT 0","LEFT 1","RIGHT 0","RIGHT 1"]};
  const topo={detected:true,left:2,top:0,right:2,bottom:0,layers:1};
  const caps={led_count:120,pin:4,led_type:"WS2815",mapper_fixed:true,segment_count:12,side_clone:true,topology:true};
  const realtime=()=>({seq:++window.__previewSeq,ts:Date.now(),tv:true,zones:demoState.zones});
  window.__previewSeq=142619;

  function json(data,status=200){
    return new Response(JSON.stringify(data),{status,headers:{"Content-Type":"application/json"}});
  }
  const realFetch=window.fetch.bind(window);
  window.fetch=async function(input,init){
    const u=typeof input==="string"?input:(input&&input.url)||"";
    const p=(new URL(u,location.href)).pathname;
    if(p.startsWith("/api/")){
      if(init && init.method && init.method!=="GET"){
        return json({ok:true,success:true,message:"Preview mode — nincs valódi ESP32-módosítás."});
      }
      if(p==="/api/state") return json(demoState);
      if(p==="/api/realtime") return json(realtime());
      if(p==="/api/mapper") return json(mapper);
      if(p==="/api/topology") return json(topo);
      if(p==="/api/capabilities") return json(caps);
      if(p==="/api/auth") return json({enabled:false,setup:false});
      if(p==="/api/scan") return json({networks:[{ssid:"Preview WiFi",rssi:-42,secure:true}]});
      return json({});
    }
    return realFetch(input,init);
  };

  class PreviewWebSocket {
    constructor(){this.readyState=0;this.url="ws://preview";this.bufferedAmount=0;
      setTimeout(()=>{this.readyState=1;this.onopen&&this.onopen();
        this._timer=setInterval(()=>this.onmessage&&this.onmessage({data:JSON.stringify(realtime())}),1000);
      },80);
    }
    send(){}
    close(){this.readyState=3;clearInterval(this._timer);this.onclose&&this.onclose();}
    addEventListener(t,fn){this["on"+t]=fn}
    removeEventListener(){}
  }
  PreviewWebSocket.CONNECTING=0;PreviewWebSocket.OPEN=1;PreviewWebSocket.CLOSING=2;PreviewWebSocket.CLOSED=3;
  window.WebSocket=PreviewWebSocket;

  window.__AMBILIGHT_PREVIEW__=true;

})();
})();