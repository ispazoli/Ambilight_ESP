/* AMBILIGHT BRIDGE — PRO CONTROL CENTER DEMO API v5 */
(function(){
"use strict";
window.__AMBILIGHT_PREVIEW__=true;
const sources=["BLACK","L0","L1","R0","R1","L_AVG","R_AVG","ALL_AVG","LR_TOP","LR_BOT","VERT_AVG","L0R0_BL","L1R1_BL","GRAD_TOP","GRAD_RIGHT","GRAD_BOT","GRAD_LEFT"];
const state={firmware:"5.5.2-C3-OFF-FIX",schema:10,contract:1,tv_ip:"192.168.1.148",tv_online:true,esp_ip:"preview",http_port:8080,ws_port:81,rssi:-47,good:12480,bad:2,free_heap:121241,uptime:482391,brightness:160,smoothing:70,black_threshold:4,dyn_on:true,dyn_min:25,dyn_max:255,dyn_resp:35,mood_dyn:true,mood_dep:25,tv_sync:true,tv_bsync:false,clone_on:true,clone_bri:255,clone_l_start:30,clone_l_count:30,clone_r_start:90,clone_r_count:30,clone_l_rev:false,clone_r_rev:false,mood_link:0,wifi_ssid:"Ambilight-Demo",wifi_pass:"",smart_seq:18420,fx_contract_count:23,segments:Array.from({length:12},(_,i)=>({start:i*10,count:10,source:[1,1,1,2,2,2,3,3,3,4,4,4][i],brightness:255,reverse:false})),left_mood:{mode:0,effect:12,auto:true,hue:210,sat:220,bri:110,speed:28,pal:11,scale:70,motion:65,glow:75,density:55,turb:45,cm:2,rev:false},right_mood:{mode:0,effect:13,auto:true,hue:30,sat:220,bri:110,speed:28,pal:2,scale:70,motion:65,glow:75,density:55,turb:45,cm:2,rev:false}};
const started=Date.now();const clone=o=>JSON.parse(JSON.stringify(o));
const json=(d,s=200)=>new Response(JSON.stringify(d),{status:s,headers:{"Content-Type":"application/json"}});
const parseBody=o=>{if(!o||o.body==null)return{};if(o.body instanceof URLSearchParams)return Object.fromEntries(o.body);if(typeof o.body==="string"){try{return JSON.parse(o.body)}catch(e){return Object.fromEntries(new URLSearchParams(o.body))}}return{}};
function frame(){const t=Date.now()/1000,z=[];for(let i=0;i<4;i++){const h=(t*18+i*82)%360,s=.78,v=.92,x=h/60,c=v*s,xx=c*(1-Math.abs(x%2-1)),m=v-c,a=[[c,xx,0],[xx,c,0],[0,c,xx],[0,xx,c],[xx,0,c],[c,0,xx]][Math.floor(x)%6];z.push({r:Math.round((a[0]+m)*255),g:Math.round((a[1]+m)*255),b:Math.round((a[2]+m)*255)})}return{seq:++state.smart_seq,ts:Date.now(),tv:true,zones:z}}
function stateOut(){return {...clone(state),uptime:state.uptime+Math.floor((Date.now()-started)/1000)}}
const realFetch=window.fetch.bind(window);
window.fetch=async(input,options={})=>{const u=typeof input==="string"?input:(input&&input.url)||"";const url=new URL(u,location.href),p=url.pathname;if(window.__AMBILIGHT_PREVIEW__&&(url.hostname==="preview"||url.hostname==="ambilight.local"||url.hostname===location.hostname)){if(p==="/api/capabilities")return json({mapper:{maxSegments:12,sources:sources.map((name,id)=>({id,name}))},features:{smartEngine:true,mood:true,sideClone:true,ota:false}});if(p==="/api/state")return json(stateOut());if(p==="/api/realtime")return json(frame());if(p==="/api/auth")return json({enabled:true,setup:false});if(options.method==="POST"){const b=parseBody(options);if(p==="/api/reboot")return json({ok:true,reboot:true});if(p==="/api/ledtest")return json({ok:true,mode:b});if(p==="/api/auth")return json({ok:true,enabled:!!b.password});if(p==="/api/tv"){if(b.ip)state.tv_ip=b.ip;return json({ok:true})};if(p==="/api/wifi"){if(b.ssid)state.wifi_ssid=b.ssid;return json({ok:true,changed:true})};if(p==="/api/mapper"){if(Array.isArray(b.segments))state.segments=b.segments;return json({ok:true})};if(p==="/api/config"){for(const [k,v] of Object.entries(b)){const d=k==="blackThreshold"?"black_threshold":k;if(d in state){state[d]=typeof state[d]==="boolean"?!!Number(v):Number(v)}}return json({ok:true})};if(p==="/api/mood"){state.left_mood={...state.left_mood,mode:Number(b.leftMode||0),effect:Number(b.leftEffect||12),hue:Number(b.leftHue||0),sat:Number(b.leftSat||255),bri:Number(b.leftVal||200),speed:Number(b.leftSpeed||28),pal:Number(b.leftPalette||0)};state.right_mood={...state.right_mood,mode:Number(b.rightMode||0),effect:Number(b.rightEffect||12),hue:Number(b.rightHue||0),sat:Number(b.rightSat||255),bri:Number(b.rightVal||200),speed:Number(b.rightSpeed||28),pal:Number(b.rightPalette||0)};return json({ok:true})};if(p==="/api/sideclone"){Object.entries(b).forEach(([k,v])=>{const map={enabled:"clone_on",brightness:"clone_bri",leftStart:"clone_l_start",leftCount:"clone_l_count",rightStart:"clone_r_start",rightCount:"clone_r_count",leftReverse:"clone_l_rev",rightReverse:"clone_r_rev"};const d=map[k];if(d)state[d]=typeof state[d]==="boolean"?!!Number(v):Number(v)});return json({ok:true})};return json({ok:true})}return json({error:"demo endpoint not implemented"},404)}return realFetch(input,options)};
class DemoWebSocket{constructor(url){this.url=url;this.readyState=0;setTimeout(()=>{this.readyState=1;this.onopen&&this.onopen();this._timer=setInterval(()=>{if(this.readyState===1&&this.onmessage)this.onmessage({data:JSON.stringify(frame())})},250)},60)}send(){}close(){if(this.readyState===3)return;this.readyState=3;clearInterval(this._timer);this.onclose&&this.onclose()}}
DemoWebSocket.OPEN=1;DemoWebSocket.CLOSED=3;window.WebSocket=DemoWebSocket;
"use strict";
/* ═══════════════════════════════════════════════════════════════════════════
   SECTION 1 — UTILITIES & SMART ENGINE MODULES
   ═══════════════════════════════════════════════════════════════════════ */
const $=id=>document.getElementById(id),$$=(s,p)=>[...(p||document).querySelectorAll(s)];
const clamp=(v,lo=0,hi=255)=>Math.max(lo,Math.min(hi,Number(v)||0));
const clamp01=v=>Math.max(0,Math.min(1,Number(v)||0));
const ZN=["L0","L1","R0","R1"];

/* ── TV Frame Pipeline ────────────────────────────────────────── */
class TVFramePipeline{
  constructor(o={}){
    this.smoothing=clamp01(o.smoothing??.35);this.blackThreshold=clamp(Number(o.blackThreshold??4));
    this.staleTimeoutMs=Math.max(100,Number(o.staleTimeoutMs??1000));
    this.zoneCount=Math.max(1,Math.min(4,Number(o.zoneCount??4)));
    this.deadband=Math.max(0,Number(o.deadband??1));this.maxDelta=Math.max(0,Number(o.maxDelta??48));
    this.lastSeq=-1;this.lastTs=-1;this.lastAcceptedAt=0;this.previousZones=null;this.lastProcessed=null;
    this.stats={accepted:0,duplicate:0,outOfOrder:0,invalid:0,reboot:0};
  }
  accept(frame,now=Date.now()){
    if(!frame||!Array.isArray(frame.zones)||frame.zones.length<this.zoneCount){this.stats.invalid++;return this._reject("invalid")}
    const seq=Number(frame.seq??-1),ts=Number(frame.ts??now);
    if(seq<=this.lastSeq){
      if(seq===this.lastSeq){this.stats.duplicate++;return this._reject("duplicate")}
      if(seq<=8&&this.lastSeq>1000000){this.stats.reboot++;this.lastSeq=seq;this.lastTs=ts}
      else{this.stats.outOfOrder++;return this._reject("out-of-order")}
    }
    this.lastSeq=seq;this.lastTs=ts;this.lastAcceptedAt=now;
    const zones=frame.zones.slice(0,this.zoneCount).map(z=>({r:clamp(z.r||0),g:clamp(z.g||0),b:clamp(z.b||0)}));
    let processed;
    if(!this.previousZones){processed=zones.map(z=>({...z}))}
    else{      const f=this.smoothing,inv=1-f;
      processed=zones.map((z,i)=>{const p=this.previousZones[i];let r=p.r*inv+z.r*f,g=p.g*inv+z.g*f,b=p.b*inv+z.b*f;
        const dr=Math.abs(r-p.r),dg=Math.abs(g-p.g),db=Math.abs(b-p.b);
        if(dr<this.deadband)r=p.r;if(dg<this.deadband)g=p.g;if(db<this.deadband)b=p.b;
        if(dr>this.maxDelta)r=p.r+(this.maxDelta*Math.sign(z.r-p.r));
        if(dg>this.maxDelta)g=p.g+(this.maxDelta*Math.sign(z.g-p.g));
        if(db>this.maxDelta)b=p.b+(this.maxDelta*Math.sign(z.b-p.b));
        return{r:Math.round(r),g:Math.round(g),b:Math.round(b)};
      });
    }
    this.previousZones=this.previousZones?this.previousZones.map((z,i)=>({...z})):zones.map(z=>({...z}));
    const tvOnline=frame.tv!==undefined?!!frame.tv:(processed.some(z=>z.r+z.g+z.b>this.blackThreshold*3));
    this.lastProcessed={seq,ts,zones:processed,tvOnline,accepted:true};this.stats.accepted++;
    return this.lastProcessed;
  }
  _reject(reason){return {accepted:false,reason,zones:this.lastProcessed?this.lastProcessed.zones:Array(this.zoneCount).fill({r:0,g:0,b:0})}}
  status(now=Date.now()){const age=now-this.lastAcceptedAt;return{ageMs:age,stale:age>this.staleTimeoutMs,active:age<=this.staleTimeoutMs,stats:{...this.stats}}}
}

/* ── Scene Analyzer ───────────────────────────────────────────── */
class SceneAnalyzer{
  constructor(o={}){this.motionSmoothing=clamp01(o.motionSmoothing??.7);this.changeSmoothing=clamp01(o.changeSmoothing??.5);this.previousZones=null;this.previousBrightness=0;this.previousSat=0;this.motion=0;this.sceneChange=0;this.lastTimestamp=0}
  analyze(input){
    const zones=(input.zones||[]).slice(0,4).map(z=>({r:clamp(z.r||0),g:clamp(z.g||0),b:clamp(z.b||0)}));
    let brightness=0,sat=0,sumR=0,sumG=0,sumB=0;const lum=[];
    zones.forEach(z=>{const l=.299*z.r+.587*z.g+.114*z.b;lum.push(l);brightness+=l;sumR+=z.r;sumG+=z.g;sumB+=z.b});
    brightness/=4;sumR/=4;sumG/=4;sumB/=4;
    const maxLum=Math.max(...lum),minLum=Math.min(...lum);const contrast=(maxLum-minLum+1)/(brightness+1);
    zones.forEach(z=>{const l=.299*z.r+.587*z.g+.114*z.b;const mx=Math.max(z.r,z.g,z.b),mn=Math.min(z.r,z.g,z.b);sat+=mx>0?(mx-mn)/mx:0});
    sat/=4;
    if(this.previousZones){let md=0;for(let i=0;i<4;i++){const p=this.previousZones[i],z=zones[i];md+=Math.abs(z.r-p.r)+Math.abs(z.g-p.g)+Math.abs(z.b-p.b)}md/=12;this.motion=this.motion*this.motionSmoothing+md*(1-this.motionSmoothing)}
    const bDelta=Math.abs(brightness-this.previousBrightness)/255;this.sceneChange=this.sceneChange*this.changeSmoothing+bDelta*(1-this.changeSmoothing);
    this.previousZones=zones.map(z=>({...z}));this.previousBrightness=brightness;this.previousSat=sat;this.lastTimestamp=Date.now();
    const domH=this._dominantHue(sumR,sumG,sumB);const energy=(brightness/255*.4+sat*.4+this.motion/255*.2)*100;
    return {brightness:brightness/255*100,saturation:sat*100,contrast:Math.min(contrast*50,100),motion:this.motion/255*100,sceneChange:this.sceneChange*100,energy,dominantColor:{r:Math.round(sumR),g:Math.round(sumG),b:Math.round(sumB)},dominantHue:domH,warmCool:domH<60||domH>300?1:domH>120&&domH<240?-1:0,zones:zones.map((z,i)=>({name:ZN[i],...z,luminance:Math.round(lum[i])}))};
  }
  _dominantHue(r,g,b){r/=255;g/=255;b/=255;const mx=Math.max(r,g,b),mn=Math.min(r,g,b),d=mx-mn;if(d===0)return 0;let h=0;if(mx===r)h=((g-b)/d)%6;else if(mx===g)h=(b-r)/d+2;else h=(r-g)/d+4;h=Math.round(h*60);return h<0?h+360:h}
}

/* ── Adaptive Controller ──────────────────────────────────────── */
class AdaptiveController{
  constructor(o={}){
    this.brightnessMin=Number(o.brightnessMin??10);this.brightnessMax=Number(o.brightnessMax??100);
    this.speedMin=Number(o.speedMin??10);this.speedMax=Number(o.speedMax??90);
    this.reactionMin=Number(o.reactionMin??10);this.reactionMax=Number(o.reactionMax??90);
    this.response=Number(o.response??35);this.currentBrightness=50;this.currentSpeed=50;this.currentReaction=50;this.initialized=false;
  }
  update(scene){
    if(!scene||scene.brightness===undefined)return {valid:false};
    const b=Math.round(this.brightnessMin+(scene.brightness/100)*(this.brightnessMax-this.brightnessMin));
    const sp=Math.round(this.speedMax-(scene.motion/100)*(this.speedMax-this.speedMin));
    const re=Math.round(this.reactionMin+(scene.sceneChange/100)*(this.reactionMax-this.reactionMin));
    if(!this.initialized){this.currentBrightness=b;this.currentSpeed=sp;this.currentReaction=re;this.initialized=true}
    else{const r=this.response/100;this.currentBrightness=this.currentBrightness*(1-r)+b*r;this.currentSpeed=this.currentSpeed*(1-r)+sp*r;this.currentReaction=this.currentReaction*(1-r)+re*r}
    let st="NORMAL";if(scene.brightness<8)st="DARK";else if(scene.brightness>85)st="BRIGHT";else if(scene.motion>60)st="ACTION";else if(scene.motion<10&&scene.brightness<40)st="CALM";
    return {valid:true,brightness:Math.round(this.currentBrightness),speed:Math.round(this.currentSpeed),reaction:Math.round(this.currentReaction),response:this.response,sceneType:st};
  }
}

/* ── Smart Engine ──────────────────────────────────────────────── */
class SmartEngine{
  constructor(o={}){
    this.mode=o.mode||"SMART PRO";this.enabled=o.enabled!==false;this.updateInterval=Number(o.updateInterval??50);
    this.sceneAnalyzer=new SceneAnalyzer({motionSmoothing:o.motionSmoothing??.7});this.adaptiveController=new AdaptiveController(o);
    this.lastScene=null;this.lastAdaptive=null;this.lastControl=null;this.running=false;this._listeners={};
  }
  on(evt,fn){if(!this._listeners[evt])this._listeners[evt]=[];this._listeners[evt].push(fn)}
  _emit(evt,data){(this._listeners[evt]||[]).forEach(fn=>{try{fn(data)}catch(e){}})}
  start(){this.running=true}
  stop(){this.running=false}
  process(state){
    if(!this.enabled||!this.running)return null;
    const zones=state.zones||state;const scene=this.sceneAnalyzer.analyze({zones});this.lastScene=scene;
    const adaptive=this.adaptiveController.update(scene);this.lastAdaptive=adaptive;
    const control={brightness:adaptive.brightness||0,speed:adaptive.speed||0,reaction:adaptive.reaction||0,mode:this.mode,enabled:this.enabled,valid:adaptive.valid};
    this.lastControl=control;
    this._emit("update",{scene,adaptive,control});return {scene,adaptive,control};
  }
  setMode(m){this.mode=m}
}

/* ═══════════════════════════════════════════════════════════════════════════
   SECTION 2 — APPLICATION CORE
   ═══════════════════════════════════════════════════════════════════════════ */
let espIP=localStorage.getItem("ab_ip")||"",auth=localStorage.getItem("ab_auth")||"";
let espHost="",espPort=8080,wsPort=81,ws=null,wsTO=null,pollTO=null,config=null,fps_f=0,fps_t=performance.now(),fps_v=0,wsRetryMs=1000;
let mapperSources=[],mapperMaxSegments=12;
const SRC_FALLBACK=["BLACK","L0","L1","R0","R1","L_AVG","R_AVG","ALL_AVG","LR_TOP","LR_BOT","VERT_AVG","L0R0_BL","L1R1_BL","GRAD_TOP","GRAD_RIGHT","GRAD_BOT","GRAD_LEFT"];
let securePage=location.protocol==="https:";
let localESPPage=location.protocol==="http:" && (location.port==="8080" || location.hostname==="ambilight.local");
function normalizeESP(v){
  v=(v||"").trim();
  if(!v)return {host:"ambilight.local",port:8080};
  try{
    if(!/^https?:\/\//i.test(v)) v="http://"+v;
    const u=new URL(v);
    return {host:u.hostname,port:Number(u.port)||8080};
  }catch(e){throw Error("Érvénytelen ESP cím: "+v)}
}
function setESPAddress(v){
  const n=normalizeESP(v); espHost=n.host; espPort=n.port; espIP=espHost+(espPort!==8080?":"+espPort:"");
  localStorage.setItem("ab_ip",espIP);
}
function apiUrl(p){
  if(localESPPage)return p;
  return espHost?"http://"+espHost+":"+espPort+p:null;
}
function wsUrl(){
  if(localESPPage)return "ws://"+location.hostname+":"+wsPort;
  return espHost?"ws://"+espHost+":"+wsPort:null;
}let pipeline=new TVFramePipeline({smoothing:.25,deadband:1,maxDelta:48,blackThreshold:4,staleTimeoutMs:1000});
let engine=new SmartEngine({mode:"SMART PRO",brightnessMin:10,brightnessMax:100,speedMin:10,speedMax:90,reactionMin:10,reactionMax:90,response:35});
engine.start();
engine.on("update",r=>{
  if(r.scene)renderScene(r.scene);
  if(r.adaptive)renderAdaptive(r.adaptive);
  if(r.scene)renderZoneAnalysis(r.scene);
});

/* ── API ────────────────────────────────────────────────────────── */
function apiH(h){return {...h||{},...(auth?{Authorization:auth}:{})}}
async function api(p,o={}){
  const u=apiUrl(p);if(!u)throw Error("No IP");
  const r=await fetch(u,{...o,cache:"no-store",headers:apiH(o.headers||{})});
  if(r.status===401&&!auth){const pw=prompt("Web auth jelszó:","ambilight");if(pw){auth="Basic "+btoa("admin:"+pw);localStorage.setItem("ab_auth",auth);return api(p,o)}}
  return r;
}
async function responseError(r,path){
  let detail="";try{detail=(await r.text()).trim()}catch(e){}
  if(detail.length>240)detail=detail.slice(0,240)+"…";
  return Error(path+" — HTTP "+r.status+(detail?" — "+detail:""));
}
async function apiGet(p){const r=await api(p);if(!r.ok)throw await responseError(r,p);return r.json()}
async function apiPost(p,b){const r=await api(p,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(b)});if(!r.ok)throw await responseError(r,p);return r.json()}
async function apiFormPost(p,b){
  const body=new URLSearchParams();
  Object.entries(b||{}).forEach(([k,v])=>{if(v!==undefined&&v!==null)body.set(k,String(v))});
  const r=await api(p,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded;charset=UTF-8"},body});
  if(!r.ok)throw await responseError(r,p); return r.json();
}

/* ── Toast ──────────────────────────────────────────────────────── */
function toast(m,e){const t=$("toast");t.textContent=m;t.className="toast "+(e?"err":"ok")+" show";setTimeout(()=>t.classList.remove("show"),2500)}

/* ── WebSocket ──────────────────────────────────────────────────── */
function startWS(){
  if(!espIP||ws)return;
  try{ws=new WebSocket(wsUrl())}catch(e){scheduleWS();return}
  ws.onopen=()=>{
    wsRetryMs=1000;
    if(pollTO){clearTimeout(pollTO);pollTO=null;}
    const d=$("wsDot");if(d)d.style.display="inline-block";const l=$("wsLabel");if(l)l.style.display="inline";updateConn(true,config?.tv_online??false);
  };
  ws.onmessage=e=>{try{
    const f=JSON.parse(e.data);
    if(f.moodStatus){updateConn(true,!!f.tv);return;}
    if(pipeline.accept(f,Date.now()).accepted){
      fps_f++;const now=performance.now();
      if(now-fps_t>=1000){fps_v=Math.round(fps_f*1000/(now-fps_t));fps_f=0;fps_t=now;const fc=$("fpsChip");if(fc)fc.textContent=fps_v+" FPS"}
      engine.process({zones:f.zones});updateLEDBar(f.zones);updateZones(f.zones);updateConn(true,!!f.tv);
    }
  }catch(err){}}
  ws.onclose=()=>{
    ws=null;const d=$("wsDot");if(d)d.style.display="none";const l=$("wsLabel");if(l)l.style.display="none";
    scheduleWS();startPoll();
  };
  ws.onerror=()=>{if(ws)ws.close()};
}
function scheduleWS(){
  if(ws||!espIP)return;
  if(wsTO)clearTimeout(wsTO);
  const delay=wsRetryMs;
  wsTO=setTimeout(()=>{wsTO=null;startWS()},delay);
  wsRetryMs=Math.min(wsRetryMs*2,15000);
}
function startPoll(){
  if(ws||!espIP)return;
  if(pollTO)clearTimeout(pollTO);
  pollTO=setTimeout(pollRealtime,0);
}
let pollBusy=false;
async function pollRealtime(){
  pollTO=null;
  if(ws||pollBusy||!espIP)return;
  pollBusy=true;
  try{const f=await apiGet("/api/realtime");if(pipeline.accept(f,Date.now()).accepted){
    fps_f++;const n=performance.now();if(n-fps_t>=1000){fps_v=Math.round(fps_f*1000/(n-fps_t));fps_f=0;fps_t=n;const fc=$("fpsChip");if(fc)fc.textContent=fps_v+" FPS"}
    engine.process({zones:f.zones});updateLEDBar(f.zones);updateZones(f.zones);updateConn(true,!!f.tv);
  }}catch(e){}
  pollBusy=false;
  if(!ws&&espIP)pollTO=setTimeout(pollRealtime,500);
}

/* ── Connect ────────────────────────────────────────────────────── */
function updateConn(on,tvOnline){
  const td=$("tvDot"),ed=$("espDot");
  const tv=!!tvOnline;
  if(on){
    ed.classList.add("on");$("espLabel").textContent="ESP ONLINE";
    td.classList.toggle("on",tv);$("tvLabel").textContent=tv?"TV ONLINE":"TV —";
  }else{
    ed.classList.remove("on");td.classList.remove("on");
    $("tvLabel").textContent="TV —";$("espLabel").textContent="ESP —";
  }
}
function applyCapabilities(cap){
  const m=cap?.mapper||{};
  mapperMaxSegments=Math.max(1,Math.min(12,Number(m.maxSegments)||12));
  mapperSources=Array.isArray(m.sources)?m.sources.sort((a,b)=>(a.id??0)-(b.id??0)).map(x=>String(x.name??("SRC_"+x.id))):[];
}

async function doConnect(){
  const raw=$("modalIP").value.trim()||(localESPPage?location.hostname:"ambilight.local"),pw=$("modalAuth").value;
  if(securePage && !localESPPage){
    toast("GitHub Pages HTTPS: az ESP32 HTTP API közvetlenül blokkolható. Nyisd meg ezt a vezérlőt az ESP32-ről: http://ESP:8080",1);
    return;
  }
  if(localESPPage){
    espHost=location.hostname;
    espPort=Number(location.port)||8080;
    espIP=espHost+(espPort!==8080?":"+espPort:"");
  } else {
    try{setESPAddress(raw)}catch(e){toast(e.message,1);return}
  }
  try{setESPAddress(raw)}catch(e){toast(e.message,1);return}
  if(pw){auth="Basic "+btoa("admin:"+pw);localStorage.setItem("ab_auth",auth)}
  try{
    const cap=await apiGet("/api/capabilities");
    applyCapabilities(cap);
    const s=await apiGet("/api/state");
    config={...cap,...normalizeState(s)};
    $("connectModal").style.display="none";applyConfig(s);updateConn(true,config.tv_online);startWS();startPoll();
  }catch(e){updateConn(false);toast("ESP kapcsolat hiba: "+e.message,1)}
}
async function loadAll(){
  try{
    const cap=await apiGet("/api/capabilities");applyCapabilities(cap);
    const s=await apiGet("/api/state");config={...cap,...normalizeState(s)};
    applyConfig(s);$("connectModal").style.display="none";updateConn(true,config.tv_online);
    if(!ws)startWS();startPoll();
  }catch(e){$("connectModal").style.display="flex";updateConn(false)}
}

/* ── UI Updates ─────────────────────────────────────────────────── */
function updateZones(z){
  const g=$("zoneGrid");if(!g)return;
  g.innerHTML=z.map((z,i)=>`<div class="zoneCard"><div class="zoneSwatch" style="background:rgb(${z.r},${z.g},${z.b})"></div><div class="zoneData"><strong>${ZN[i]}</strong>R:${z.r} G:${z.g} B:${z.b}</div></div>`).join("");
}
function updateLEDBar(z){
  const b=$("ledBar");if(!b)return;
  b.innerHTML=Array.from({length:120},(_,i)=>{const zi=i<30?z[1]:i<60?z[0]:i<90?z[2]:z[3];return`<div class="px" style="background:rgb(${zi.r},${zi.g},${zi.b})"></div>`}).join("");
}
function updateMapperPreview(){
  const segs=collectSegs(),b=$("mapperBar");if(!b)return;
  b.innerHTML=Array.from({length:120},(_,i)=>{
    let hit=null;for(let s=segs.length-1;s>=0;s--){const e=Math.min(120,segs[s].start+segs[s].count);if(i>=segs[s].start&&i<e&&segs[s].count>0){hit=segs[s];break}}
    if(!hit)return`<div class="px" style="background:#111"></div>`;
    const sc=hit.source;
    const col=sc===1?"#f33":sc===2?"#0f0":sc===3?"#33f":sc===4?"#ff0":sc===0?"#222":sc>=13?"#f0f":"#888";
    return`<div class="px" style="background:${col}"></div>`;
  }).join("");
}

/* ── Dash stats ─────────────────────────────────────────────────── */
function renderStats(s){
  const st=$("dashStats");if(!st)return;
  st.innerHTML=[
    {l:"TV",v:s.tv_online?"ONLINE":"OFF",c:s.tv_online?"good":"bad"},
    {l:"Fényerő",v:Math.round((s.brightness??160)/255*100)+"%",c:"info"},
    {l:"RSSI",v:(s.rssi||0)+" dBm",c:s.rssi>-60?"good":s.rssi>-80?"warn":"bad"},
    {l:"Good Frames",v:(s.good||0).toLocaleString(),c:"good"},
    {l:"Errors",v:(s.bad||0).toLocaleString(),c:s.bad>10?"warn":"good"},
    {l:"Firmware",v:s.firmware||"—",c:"accent"},
    {l:"Uptime",v:Math.floor((s.uptime||0)/60000)+" min",c:"info"},
    {l:"Free Heap",v:((s.free_heap||0)/1024).toFixed(1)+" KB",c:"accent"},
    {l:"Mood FX contract",v:(s.fx_contract_count||FX_COUNT)+"/"+FX_COUNT,c:(s.fx_contract_count===FX_COUNT)?"good":"warn"}
  ].map(x=>`<div class="statBox"><div class="statVal ${x.c}">${x.v}</div><div class="statLabel">${x.l}</div></div>`).join("");
}
function renderDiag(s){
  $("diagGood").textContent=(s.good||0).toLocaleString();
  $("diagBad").textContent=(s.bad||0).toLocaleString();
  $("diagSeq").textContent=s.smart_seq||0;
  $("diagFPS").textContent=fps_v;
  $("diagError").textContent=s.last_error||"—";
  const si=$("sysInfo");if(si)si.innerHTML=[
    {l:"ESP IP",v:s.esp_ip||"—"},{l:"TV IP",v:s.tv_ip||"—"},{l:"Firmware",v:s.firmware||"—"},
    {l:"Schema",v:s.schema||"—"},{l:"RSSI",v:(s.rssi||0)+" dBm"},{l:"Uptime",v:Math.floor((s.uptime||0)/60000)+" min"},
    {l:"Free Heap",v:((s.free_heap||0)/1024).toFixed(1)+" KB"},{l:"WS Port",v:s.ws_port||81},{l:"HTTP Port",v:s.http_port||8080}
  ].map(x=>`<div class="statBox"><div class="statVal info" style="font-size:18px">${x.v}</div><div class="statLabel">${x.l}</div></div>`).join("");
}

/* ── Scene Panel ────────────────────────────────────────────────── */
function renderScene(sc){
  const p=$("scenePanel");if(!p)return;  p.innerHTML=`
    <div class="meterLabel"><span>Fényerő</span><span>${Math.round(sc.brightness)}%</span></div><div class="meterBar"><div class="meterFill brightness" style="width:${Math.round(sc.brightness)}%"></div></div>
    <div class="meterLabel"><span>Telítettség</span><span>${Math.round(sc.saturation)}%</span></div><div class="meterBar"><div class="meterFill saturation" style="width:${Math.round(sc.saturation)}%"></div></div>
    <div class="meterLabel"><span>Mozgás</span><span>${Math.round(sc.motion)}%</span></div><div class="meterBar"><div class="meterFill motion" style="width:${Math.round(sc.motion)}%"></div></div>
    <div class="meterLabel"><span>Energia</span><span>${Math.round(sc.energy)}%</span></div><div class="meterBar"><div class="meterFill energy" style="width:${Math.round(sc.energy)}%"></div></div>
    <div style="display:flex;gap:8px;align-items:center;margin-top:10px;flex-wrap:wrap">
      <span style="font-size:10px;color:var(--muted)">Domináns:</span>
      <span style="display:inline-block;width:16px;height:16px;border-radius:4px;background:rgb(${sc.dominantColor.r},${sc.dominantColor.g},${sc.dominantColor.b})"></span>
      <span style="font-size:11px;color:var(--text)">${sc.dominantHue}°</span>
      <span style="font-size:10px;color:var(--muted)">· Warm/Cool: ${sc.warmCool>0?'Meleg':sc.warmCool<0?'Hideg':'Semleges'}</span>
    </div>
    <div style="margin-top:8px"><span class="sceneBadge ${sc.brightness<8?'dark':sc.brightness>85?'bright':sc.motion>60?'action':sc.motion<10&&sc.brightness<40?'calm':'normal'}">${sc.brightness<8?'DARK':sc.brightness>85?'BRIGHT':sc.motion>60?'ACTION':sc.motion<10&&sc.brightness<40?'CALM':'NORMAL'}</span></div>
  `;
}
function renderAdaptive(ad){
  const p=$("adaptivePanel");if(!p)return;
  p.innerHTML=`
    <div class="meterLabel"><span>Fényerő</span><span>${ad.brightness}%</span></div><div class="meterBar"><div class="meterFill brightness" style="width:${ad.brightness}%"></div></div>
    <div class="meterLabel"><span>Sebesség</span><span>${ad.speed}%</span></div><div class="meterBar"><div class="meterFill speed" style="width:${ad.speed}%"></div></div>
    <div class="meterLabel"><span>Reakció</span><span>${ad.reaction}%</span></div><div class="meterBar"><div class="meterFill speed" style="width:${ad.reaction}%"></div></div>
    <div style="margin-top:10px;display:flex;gap:8px;align-items:center"><span style="font-size:10px;color:var(--muted)">Jelenet:</span><span class="sceneBadge ${(ad.sceneType||'NORMAL').toLowerCase()}">${ad.sceneType||'NORMAL'}</span></div>
  `;
}
function renderZoneAnalysis(sc){
  const p=$("zoneAnalysis");if(!p||!sc.zones)return;
  p.innerHTML=sc.zones.map(z=>`<div class="zoneCard"><div class="zoneSwatch" style="background:rgb(${z.r},${z.g},${z.b})"></div><div class="zoneData"><strong>${z.name}</strong>Lum: ${z.luminance} · R:${z.r} G:${z.g} B:${z.b}</div></div>`).join("");
}
function renderEngineStatus(){
  const p=$("engineStatus");if(!p)return;
  p.innerHTML=`
    <div class="statBox"><div class="statVal info">${fps_v}</div><div class="statLabel">FPS</div></div>
    <div style="margin-top:8px;font-size:11px;color:var(--soft)">
      <div>Mód: <b style="color:var(--cyan)">SMART PRO</b></div>
      <div>Pipeline: <b style="color:var(--green)">${pipeline?pipeline.stats.accepted:0} accepted</b></div>
      <div>WebSocket: <b style="color:${ws?'var(--green)':'var(--red)'}">${ws?'AKTÍV':'INAKTÍV'}</b></div>
    </div>
  `;
}
setInterval(renderEngineStatus,2000);

/* ── Firmware → Browser state normalization ─────────────────────
   v5.4.x firmware is the canonical source. Older UI field names are
   normalized here so existing pages/features do not lose state. */
function normalizeState(s){
  s=s||{};
  return {
    ...s,
    firmware:s.firmware||s.fw||"—",
    schema:s.schema??s.contract??"—",
    tv_ip:s.tv_ip||s.tvIP||"",
    tv_online:s.tv_online??s.tvOnline??false,
    esp_ip:s.esp_ip||s.ip||"",
    good:s.good??s.goodFrames??0,
    bad:s.bad??s.badFrames??0,
    free_heap:s.free_heap??s.heap??0,
    smart_seq:s.smart_seq??s.seq??0,
    http_port:s.http_port??8080,
    ws_port:s.ws_port??81,
    black_threshold:s.black_threshold??s.blackThreshold??4,
    clone_on:s.clone_on??s.sideCloneEnabled??false,
    clone_bri:s.clone_bri??s.sideCloneBrightness??255,
    clone_l_start:s.clone_l_start??30,
    clone_l_count:s.clone_l_count??30,
    clone_r_start:s.clone_r_start??90,
    clone_r_count:s.clone_r_count??30,
    clone_l_rev:s.clone_l_rev??false,
    clone_r_rev:s.clone_r_rev??false,
    mood_link:s.mood_link??s.moodLinkMode??0,
    segments:Array.isArray(s.segments)?s.segments.map(x=>({
      start:x.start??0,count:x.count??0,source:x.source??0,
      bri:x.bri??x.brightness??255,brightness:x.brightness??x.bri??255,
      rev:x.rev??x.reverse??false,reverse:x.reverse??x.rev??false
    })):[],
    zone_current:s.zone_current||[],
    zone_target:s.zone_target||[],
    zones:s.zones||[]
  };
}
function applyConfig(raw){
  const s=normalizeState(raw);
  config={...(config||{}),...s};
  setVal("cfgTvIP",s.tv_ip||"");$("cfgTvSync").checked=!!s.tv_sync;$("cfgTvBSync").checked=!!s.tv_bsync;
  $("cfgCloneOn").checked=!!s.clone_on;setVal("cfgCloneBri",s.clone_bri??255);$("cfgCloneBriV").textContent=s.clone_bri??255;
  setVal("cfgCloneLS",s.clone_l_start??30);setVal("cfgCloneLC",s.clone_l_count??30);
  setVal("cfgCloneRS",s.clone_r_start??90);setVal("cfgCloneRC",s.clone_r_count??30);
  $("cfgCloneLR").checked=!!s.clone_l_rev;$("cfgCloneRR").checked=!!s.clone_r_rev;
  setVal("cfgBri",s.brightness??160);$("cfgBriV").textContent=s.brightness??160;
  setVal("cfgSmooth",s.smoothing??70);$("cfgSmoothV").textContent=s.smoothing??70;
  setVal("cfgBlack",s.black_threshold??4);$("cfgBlackV").textContent=s.black_threshold??4;
  $("cfgDynOn").checked=!!s.dyn_on;setVal("cfgDynMin",s.dyn_min??0);setVal("cfgDynMax",s.dyn_max??255);setVal("cfgDynResp",s.dyn_resp??35);
  $("cfgMoodDyn").checked=!!s.mood_dyn;setVal("cfgMoodDep",s.mood_dep??25);$("cfgMoodDepV").textContent=s.mood_dep??25;
  $("moodLinkSel").value=s.mood_link??0;
  setVal("cfgWifiSSID",s.wifi_ssid||"");setVal("cfgWifiPass",s.wifi_pass||"");
  applyMood("left",s.left_mood);applyMood("right",s.right_mood);
  renderSegs(s.segments||[]);renderStats(s);renderDiag(s);updateMapperPreview();
}
function setVal(id,v){const e=$(id);if(e)e.value=v}

/* ── Mood Forms ─────────────────────────────────────────────────── */
/* Kanonikus Mood ID contract 0–22 — a firmware MOOD_FX_NAMES[]-szal pontosan egyező sorrend */
const EFFECTS=["Static","Breathe","Rainbow","Slow Color","Warm","Color Wave","Comet","Twinkle","Plasma","Fire","Palette Wave","Aurora","Ocean","Fire 2","Energy Pulse","Meteor Shower","Nebula","Starfield","Organic Flow","Cyber Flow","Spectral","Lava Lamp","Plasma X"];
const FX_COUNT=EFFECTS.length; /* 23 */
const PALETTES=["RED","Scarlet","Orange","Amber","Gold","Yellow","Lime","Green","Spring","Emerald","Turquoise","Cyan","Sky","Blue","Royal Blue","Indigo","Violet","Purple","Magenta","Pink","Rose","Crimson","Deep Red","Ice White"];
function moodForm(side){
  return `<div class="checkRow"><input type="checkbox" id="m_${side}_on" checked><label>Engedélyezve</label></div>
<div><label>Effekt</label><select id="m_${side}_eff">${EFFECTS.map((e,i)=>`<option value="${i}">${e}</option>`).join("")}</select></div>
<div class="checkRow"><input type="checkbox" id="m_${side}_auto"><label>Auto szín (TV)</label></div>
<div class="rangeRow"><label style="min-width:60px">Hue</label><input type="range" id="m_${side}_hue" min="0" max="360" value="210"><span id="m_${side}_hueV">210°</span></div>
<div class="frow3"><div class="rangeRow"><label>Sat</label><input type="range" id="m_${side}_sat" min="0" max="255" value="220"><span id="m_${side}_satV" style="font-size:9px">220</span></div><div class="rangeRow"><label>Bri</label><input type="range" id="m_${side}_bri" min="0" max="255" value="110"><span id="m_${side}_briV" style="font-size:9px">110</span></div><div class="rangeRow"><label>Speed</label><input type="range" id="m_${side}_sp" min="1" max="100" value="28"><span id="m_${side}_spV" style="font-size:9px">28</span></div></div>
<div><label>Paletta</label><select id="m_${side}_pal">${PALETTES.map((e,i)=>`<option value="${i}">${e}</option>`).join("")}</select></div>
<div class="frow3"><div class="rangeRow"><label>Scale</label><input type="range" id="m_${side}_sc" min="1" max="100" value="70"><span id="m_${side}_scV" style="font-size:9px">70</span></div><div class="rangeRow"><label>Motion</label><input type="range" id="m_${side}_mot" min="0" max="100" value="65"><span id="m_${side}_motV" style="font-size:9px">65</span></div><div class="rangeRow"><label>Glow</label><input type="range" id="m_${side}_gl" min="0" max="100" value="75"><span id="m_${side}_glV" style="font-size:9px">75</span></div></div>
<div class="frow3"><div class="rangeRow"><label>Density</label><input type="range" id="m_${side}_den" min="0" max="100" value="55"><span id="m_${side}_denV" style="font-size:9px">55</span></div><div class="rangeRow"><label>Turb</label><input type="range" id="m_${side}_tur" min="0" max="100" value="45"><span id="m_${side}_turV" style="font-size:9px">45</span></div><div><label>Színmód</label><select id="m_${side}_cm"><option value="0">Paletta</option><option value="1">Fix hue</option><option value="2">Auto TV</option><option value="3">Hue gradiens</option></select></div></div>
<div class="checkRow"><input type="checkbox" id="m_${side}_rev"><label>Fordított</label></div>`;
}
function applyMood(side,m){
  if(!m)return;$("m_"+side+"_on").checked=m.mode>0;$("m_"+side+"_eff").value=m.effect??12;$("m_"+side+"_auto").checked=!!m.auto;
  setVal("m_"+side+"_hue",m.hue??210);$("m_"+side+"_hueV").textContent=(m.hue??210)+"°";
  setVal("m_"+side+"_sat",m.sat??220);$("m_"+side+"_satV").textContent=m.sat??220;
  setVal("m_"+side+"_bri",m.bri??110);$("m_"+side+"_briV").textContent=m.bri??110;
  setVal("m_"+side+"_sp",m.speed??28);$("m_"+side+"_spV").textContent=m.speed??28;
  $("m_"+side+"_pal").value=m.pal??(side==="left"?0:1);
  setVal("m_"+side+"_sc",m.scale??70);$("m_"+side+"_scV").textContent=m.scale??70;
  setVal("m_"+side+"_mot",m.motion??65);$("m_"+side+"_motV").textContent=m.motion??65;
  setVal("m_"+side+"_gl",m.glow??75);$("m_"+side+"_glV").textContent=m.glow??75;
  setVal("m_"+side+"_den",m.density??55);$("m_"+side+"_denV").textContent=m.density??55;
  setVal("m_"+side+"_tur",m.turb??45);$("m_"+side+"_turV").textContent=m.turb??45;
  $("m_"+side+"_cm").value=m.cm??2;$("m_"+side+"_rev").checked=!!m.rev;
}
function colMood(side){return{mode:$("m_"+side+"_on").checked?1:0,effect:+$("m_"+side+"_eff").value,auto:$("m_"+side+"_auto").checked,hue:+$("m_"+side+"_hue").value,sat:+$("m_"+side+"_sat").value,bri:+$("m_"+side+"_bri").value,speed:+$("m_"+side+"_sp").value,pal:+$("m_"+side+"_pal").value,scale:+$("m_"+side+"_sc").value,motion:+$("m_"+side+"_mot").value,glow:+$("m_"+side+"_gl").value,density:+$("m_"+side+"_den").value,turb:+$("m_"+side+"_tur").value,cm:+$("m_"+side+"_cm").value,rev:$("m_"+side+"_rev").checked}}

/* ── Segments ───────────────────────────────────────────────────── */
const SIDES=["BOTTOM (0–29)","LEFT (30–59)","TOP (60–89)","RIGHT (90–119)"];
const FIXED_MAPPER_SEGMENTS=12;
const FIXED_MAPPER_LEDS=10;
function canonicalMapperDefaults(){
  return Array.from({length:FIXED_MAPPER_SEGMENTS},(_,i)=>({
    start:i*FIXED_MAPPER_LEDS,
    count:FIXED_MAPPER_LEDS,
    source:[1,1,1,2,2,2,3,3,3,4,4,4][i],
    bri:255,brightness:255,rev:false,reverse:false
  }));
}
function renderSegs(segs){
  const el=$("segList");if(!el)return;
  if(!Array.isArray(segs)||segs.length!==FIXED_MAPPER_SEGMENTS)segs=canonicalMapperDefaults();
  const names=mapperSources.length?mapperSources:SRC_FALLBACK;
  let h="";
  for(let side=0;side<4;side++){
    h+=`<div class="segSide">${SIDES[side]}</div>`;
    for(let s=0;s<3;s++){
      const i=side*3+s,seg=segs[i]||canonicalMapperDefaults()[i];
      const start=i*FIXED_MAPPER_LEDS,end=start+FIXED_MAPPER_LEDS-1;
      const bri=seg.brightness??seg.bri??255,rev=seg.reverse??seg.rev??false;
      h+=`<div class="segRow"><div><label>LED</label><div class="segAddr">${start}–${end}</div></div><div><label>SRC</label><select id="s${i}src" onchange="updateMapperPreview()">${names.map((n,j)=>`<option value="${j}" ${(seg.source??0)===j?"selected":""}>${n}</option>`).join("")}</select></div><div><label>Fény</label><input type="number" id="s${i}bri" value="${bri}" min="0" max="255"></div><div><label>↔</label><input type="checkbox" id="s${i}rev" ${rev?"checked":""}></div></div>`;
    }
  }
  el.innerHTML=h;
}
function collectSegs(){
  const o=[];
  for(let i=0;i<FIXED_MAPPER_SEGMENTS;i++){
    o.push({
      start:i*FIXED_MAPPER_LEDS,
      count:FIXED_MAPPER_LEDS,
      source:+($("s"+i+"src")?.value||0),
      brightness:+($("s"+i+"bri")?.value||255),
      reverse:$("s"+i+"rev")?.checked||false
    });
  }
  return o;
}
function collectCfg(){
  return {
    brightness:+$("cfgBri").value,smoothing:+$("cfgSmooth").value,black_threshold:+$("cfgBlack").value,
    dyn_on:$("cfgDynOn").checked,dyn_min:+$("cfgDynMin").value,dyn_max:+$("cfgDynMax").value,dyn_resp:+$("cfgDynResp").value,
    mood_dyn:$("cfgMoodDyn").checked,mood_dep:+$("cfgMoodDep").value,
    tv_sync:$("cfgTvSync").checked,tv_bsync:$("cfgTvBSync").checked,
    clone_on:$("cfgCloneOn").checked,clone_bri:+$("cfgCloneBri").value,
    clone_l_start:+$("cfgCloneLS").value,clone_l_count:+$("cfgCloneLC").value,
    clone_r_start:+$("cfgCloneRS").value,clone_r_count:+$("cfgCloneRC").value,
    clone_l_rev:$("cfgCloneLR").checked,clone_r_rev:$("cfgCloneRR").checked,
    ml_start:config?.ml_start??30,ml_count:config?.ml_count??30,
    mr_start:config?.mr_start??90,mr_count:config?.mr_count??30,
    mood_link:+$("moodLinkSel").value,
    left:colMood("left"),right:colMood("right"),
    segments:collectSegs()
  };
}

/* ── Actions ────────────────────────────────────────────────────── */
async function saveConfig(){
  let step="TV";
  try{
    const cfg=collectCfg();
    // TV IP is persisted first, independently from the larger configuration transaction.
    if($("cfgTvIP").value) await apiFormPost("/api/tv",{ip:$("cfgTvIP").value});
    step="CONFIG";
    await apiFormPost("/api/config",{brightness:cfg.brightness,smoothing:cfg.smoothing,blackThreshold:cfg.black_threshold,
      dyn_on:cfg.dyn_on?1:0,dyn_min:cfg.dyn_min,dyn_max:cfg.dyn_max,dyn_resp:cfg.dyn_resp,
      mood_dyn:cfg.mood_dyn?1:0,mood_dep:cfg.mood_dep,tv_sync:cfg.tv_sync?1:0,tv_bsync:cfg.tv_bsync?1:0});
    step="MAPPER";
    await apiPost("/api/mapper",{segments:cfg.segments});
    step="MOOD";
    const moodArgs=(side)=>({
      [side+"Mode"]:cfg[side].mode?1:0,[side+"Effect"]:cfg[side].effect,[side+"Hue"]:cfg[side].hue,
      [side+"Sat"]:cfg[side].sat,[side+"Val"]:cfg[side].bri,[side+"Speed"]:cfg[side].speed,
      [side+"Palette"]:cfg[side].pal,[side+"Scale"]:cfg[side].scale,[side+"Motion"]:cfg[side].motion,
      [side+"Glow"]:cfg[side].glow,[side+"Density"]:cfg[side].den,[side+"Turbulence"]:cfg[side].turb,
      [side+"ColorMode"]:cfg[side].cm,[side+"Auto"]:cfg[side].auto?1:0,[side+"Reverse"]:cfg[side].rev?1:0
    });
    await apiFormPost("/api/mood",{...moodArgs("left"),...moodArgs("right"),linkMode:cfg.mood_link});
    step="SIDECLONE";
    await apiFormPost("/api/sideclone",{enabled:cfg.clone_on?1:0,brightness:cfg.clone_bri,leftStart:cfg.clone_l_start,leftCount:cfg.clone_l_count,rightStart:cfg.clone_r_start,rightCount:cfg.clone_r_count,leftReverse:cfg.clone_l_rev?1:0,rightReverse:cfg.clone_r_rev?1:0});
    toast("Beállítások elmentve ✓");setTimeout(loadAll,300);
  }catch(e){toast("Mentési hiba ["+step+"]: "+e.message,1)}
}
async function defaults(){
  try{
    const segs=canonicalMapperDefaults().filter(x=>x.count>0);
    await apiFormPost("/api/config",{brightness:160,smoothing:70,blackThreshold:4});
    await apiPost("/api/mapper",{segments:segs});
    await apiFormPost("/api/mood",{leftMode:0,rightMode:0,leftHue:0,rightHue:120,leftSat:255,rightSat:255,leftVal:200,rightVal:200,linkMode:0});
    await apiFormPost("/api/sideclone",{enabled:1,brightness:255,leftStart:30,leftCount:30,rightStart:90,rightCount:30,leftReverse:0,rightReverse:0});
    toast("Gyári alapbeállítások visszaállítva ✓");setTimeout(loadAll,500);
  }catch(e){toast("Gyári visszaállítás hiba: "+e.message,1)}
}
async function restartESP(){try{await apiFormPost("/api/reboot?confirm=1",{});toast("ESP32 újraindul...");updateConn(false)}catch(e){toast("Hiba: "+e.message,1)}}
async function ledTest(m){const rgb={red:[255,0,0],green:[0,255,0],blue:[0,0,255],white:[255,255,255]}[m]||[255,255,255];try{await apiFormPost("/api/ledtest",{r:rgb[0],g:rgb[1],b:rgb[2]});toast("LED teszt: "+m)}catch(e){toast("LED teszt hiba: "+e.message,1)}}
async function saveWiFi(){try{const r=await apiFormPost("/api/wifi",{ssid:$("cfgWifiSSID").value,password:$("cfgWifiPass").value});toast(r.changed===false?"WiFi már beállítva ✓":"WiFi mentve — újraindítás ✓")}catch(e){toast("WiFi mentési hiba: "+e.message,1)}}
async function saveAuth(){
  const p1=$("cfgAuthPass").value,p2=$("cfgAuthPass2").value;
  if(p1.length>63)return toast("Jelszó max 63 karakter",1);
  if(p1!==p2)return toast("A két jelszó nem egyezik",1);
  try{
    const r=await apiFormPost("/api/auth",{password:$("cfgAuthOn").checked?p1:""});
    if(!r.enabled){auth="";localStorage.removeItem("ab_auth")}
    toast(r.enabled?"Auth bekapcsolva ✓":"Auth kikapcsolva ✓");
    $("cfgAuthPass").value="";$("cfgAuthPass2").value="";
    refreshAuthState();
  }catch(e){toast("Auth mentési hiba: "+e.message,1)}
}
async function disableAuth(){
  try{await apiFormPost("/api/auth",{password:""});auth="";localStorage.removeItem("ab_auth");toast("Auth kikapcsolva ✓");refreshAuthState()}
  catch(e){toast("Hiba: "+e.message,1)}
}
async function refreshAuthState(){
  try{
    const a=await apiGet("/api/auth");
    const b=$("authStateBadge");
    if(b){
      if(a.enabled){b.textContent="BEKAPCSOLVA";b.className="sceneBadge action";}
      else if(a.setup){b.textContent="BEÁLLÍTÁS SZÜKSÉGES";b.className="sceneBadge action";}
      else {b.textContent="KIKAPCSOLVA";b.className="sceneBadge calm";}
    }
    // Első indításkor (setup) figyelmeztetés: a config nyitva, de az OTA jelszó nélkül tiltott.
    if(a.setup){ toast("Ajánlott jelszót beállítani — OTA-frissítés csak jelszóval elérhető",1); }
    const c=$("cfgAuthOn");if(c)c.checked=!!a.enabled;
  }catch(e){}
}
async function uploadOTA(){
  const f=$("otaFile").files[0];if(!f)return toast("Válassz .bin fájlt",1);
  if(!/\.bin$/i.test(f.name))return toast("Csak .bin fájl",1);
  const x=new XMLHttpRequest();x.open("POST",apiUrl("/api/ota"));
  if(auth)x.setRequestHeader("Authorization",auth);
  $("otaBtn").disabled=true;$("otaProgress").style.display="block";
  x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);$("otaBar").style.width=p+"%";$("otaText").textContent="Feltöltés "+p+"%"}};
  x.onload=()=>{if(x.status>=200&&x.status<300){$("otaText").textContent="Sikeres! Újraindul...";toast("OTA sikeres ✓");setTimeout(()=>location.reload(),8000)}else{$("otaText").textContent="HIBA: "+x.status;toast("OTA hiba",1)}};
  x.onerror=()=>{$("otaText").textContent="Hálózati hiba";toast("OTA hiba",1)};
  const fd=new FormData();fd.append("update",f);x.send(fd);
}

/* ── Tabs ───────────────────────────────────────────────────────── */
$$(".navBtn").forEach(b=>b.addEventListener("click",()=>{
  $$(".navBtn").forEach(x=>x.classList.remove("active"));b.classList.add("active");
  $$(".page").forEach(x=>x.classList.remove("active"));$("page-"+b.dataset.page).classList.add("active");
  if(b.dataset.page==="mapper")updateMapperPreview();
}));

/* ── Live range updates ─────────────────────────────────────────── */
["cfgBri","cfgSmooth","cfgBlack","cfgMoodDep","cfgCloneBri"].forEach(id=>$(id)?.addEventListener("input",()=>{const v=$(id).value;const e=$(id+"V");if(e)e.textContent=v}));
["left","right"].forEach(s=>["hue","sat","bri","sp","sc","mot","gl","den","tur"].forEach(p=>$("m_"+s+"_"+p)?.addEventListener("input",()=>{const v=$("m_"+s+"_"+p).value;const e=$("m_"+s+"_"+p+"V");if(e)e.textContent=p==="hue"?v+"°":v})));
})();