/* ============================================================================
   AMBILIGHT BRIDGE v5.4.2 — WIFI REBOOT LOOP FIXED / MASTER
   ============================================================================
   Ez a v5.3.0 audit alapján javított változat. Javított hibák:
     [K1] apiWifi(): hiányzó prefs.begin()/end() → WiFi creds nem mentődtek
     [K2] v5→v6 migráció: a tárolt értéket képezi le (nem a defaultot);
          friss telepítéskor a migráció kimarad (nincs korrupció)
     [K3] apiState(): a zone_* mezők valódi JSON-tömbök (nem törött serialized())
     [K4] OTA feltöltés: autentikáció-ellenőrzéssel védve
     [M1] /api/auth + /api/scan: CORS preflight (OPTIONS) route hozzáadva
     [M3] readAmbilight(): korai brace-terminálás olvasás közben → nincs 1s várás
     [M4] readAmbilight(): közvetlen puffer-parse, nincs 16KB String-másolat
     [E1] secrets.h: a SECRET_* értékek tényleges alkalmazása
     [E2] readAmbilight(): redundáns ternáris eltávolítva
     [E4] hue360To8(): korrekt 0..360 → 0..255 leképezés
     [E5] smartFrameSeq növelés a broadcast ELŐTT
     [A1] AP/STA lifecycle explicit
     [MAPPER] mapper persistence + REST + range validation
     [SRC] gradient source implementation + corrected vertical average
     [CLONE] side-clone implementation with mapper collision guard
     [MOOD] all link modes + basic animated effects
     [CONTRACT] /api/capabilities firmware/browser contract
     [TOPO] best-effort JointSPACE topology detection

   Architecture:
     ESP32-C3 = thin bridge (zóna-olvasás, LED-vezérlés, REST+WS, OTA)
     Browser  = full application (GitHub Pages)

   HARDWARE: WS2815 12V. Közös GND. 74AHCT125 szintill., 330-470R DATA soros,
             ~1000µF kondi a szalag tápbemeneténél.
   ============================================================================ */

#include <WiFi.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>

#ifndef WEBSERVER_MAX_QUERY_ARGS
  #define WEBSERVER_MAX_QUERY_ARGS 128
#endif

#include <WebServer.h>
#include <ArduinoJson.h>

#define FASTLED_ALLOW_INTERRUPTS 0
#include <FastLED.h>
#include <Preferences.h>
#include <Update.h>

/* ===== USER FALLBACK CONFIG (NVS-backed) ==================================
   NINCS beégetett hitelesítő adat. Üres alapértelmezések → friss klón
   semmit nem szivárogtat. Titkok runtime-ban, web UI-ból provisionálva.
   Opcionálisan egy NEM-track-elt secrets.h használható privát build-hez.  */

char    wifiSSID[64]        = "";
char    wifiPassword[64]    = "";
char    webAuthPassword[65] = "";      // "" = auth disabled

// Opcionális lokális override (untracked). Példa secrets.h:
//   #define SECRET_WIFI_SSID  "MySSID"
//   #define SECRET_WIFI_PASS  "MyPass"
//   #define SECRET_WEB_AUTH   "MyWebPass"
//   #define SECRET_TV_IP      192,168,1,100
#if __has_include("secrets.h")
  #include "secrets.h"
#endif

#define FIRMWARE_VERSION        "5.4.2-C3-WIFI-REBOOT-LOOP-FIXED"
#define CONFIG_SCHEMA_VERSION   8
#define FW_CONTRACT_VERSION     1

// Placeholder — web UI-ból felülbirálható. Nem valódi cím.
IPAddress DEFAULT_TV_IP(192, 168, 1, 100);

/* ===== HARDWARE CONSTANTS ================================================ */

#define LED_PIN         4
#define LED_TYPE        WS2815
#define COLOR_ORDER     GRB
#define LED_COUNT       120

#define DEFAULT_BRIGHTNESS      160
#define TV_FRAME_INTERVAL_MS    50
#define TV_SOCKET_TIMEOUT_MS    250
#define TV_BODY_READ_TIMEOUT_MS 350
#define DEFAULT_SMOOTHING       70
#define DEFAULT_BLACK_THRESHOLD 4
#define MAX_SEGMENTS            12

/* ===== TYPE DEFINITIONS ================================================== */

struct ZoneRGB { uint8_t r, g, b; };

enum SourceMode : uint8_t {
  SRC_BLACK=0, SRC_L0=1, SRC_L1=2, SRC_R0=3, SRC_R1=4,
  SRC_L_AVG=5, SRC_R_AVG=6, SRC_ALL_AVG=7,
  SRC_LR_TOP_MIX=8, SRC_LR_BOTTOM_MIX=9, SRC_VERTICAL_AVG=10,
  SRC_L0_R0_BLEND=11, SRC_L1_R1_BLEND=12,
  SRC_GRADIENT_TOP=13, SRC_GRADIENT_RIGHT=14,
  SRC_GRADIENT_BOTTOM=15, SRC_GRADIENT_LEFT=16
};

static const char* const SOURCE_NAMES[] = {
  "BLACK","L0","L1","R0","R1","L_AVG","R_AVG","ALL_AVG",
  "LR_TOP_MIX","LR_BOTTOM_MIX","VERTICAL_AVG","L0_R0_BLEND","L1_R1_BLEND",
  "GRADIENT_TOP","GRADIENT_RIGHT","GRADIENT_BOTTOM","GRADIENT_LEFT"
};
#define SOURCE_COUNT 17

struct LedSegment {
  uint16_t start, count;
  uint8_t  source, brightness;
  bool     reverse;
};

enum MoodLinkMode : uint8_t {
  MOOD_LINK_INDEPENDENT=0, MOOD_LINK_MIRROR=1,
  MOOD_LINK_SYMMETRIC=2, MOOD_LINK_FLOW=3
};

enum MoodEffect : uint8_t {
  MOOD_STATIC=0, MOOD_BREATHE=1, MOOD_RAINBOW=2, MOOD_SLOW_COLOR=3,
  MOOD_WARM=4, MOOD_COLOR_WAVE=5, MOOD_COMET=6, MOOD_TWINKLE=7,
  MOOD_PLASMA=8, MOOD_FIRE=9, MOOD_PALETTE_WAVE=10, MOOD_AURORA=11,
  MOOD_OCEAN=12, MOOD_FIRE2=13, MOOD_PULSE=14, MOOD_METEOR=15,
  MOOD_NEBULA=16, MOOD_STARFIELD=17, MOOD_ORGANIC=18, MOOD_CYBER=19,
  MOOD_SPECTRAL=20, MOOD_LAVA=21, MOOD_PLASMA_X=22
};
#define MOOD_EFFECT_MAX MOOD_PLASMA_X

static const char* const MOOD_FX_NAMES[] = {
  "STATIC","BREATHE","RAINBOW","SLOW COLOR","WARM","COLOR WAVE","COMET","TWINKLE",
  "PLASMA","FIRE","PALETTE WAVE","AURORA","OCEAN","FIRE 2","ENERGY PULSE","METEOR SHOWER",
  "NEBULA","STARFIELD","ORGANIC FLOW","CYBER FLOW","SPECTRAL","LAVA LAMP","PLASMA X"
};
#define MOOD_FX_COUNT 23

struct MoodConfig {
  uint8_t  mode, effect, saturation, brightness, speed, palette,
           scale, motion, glow, density, turbulence, colorMode;
  bool     autoColor, reverse;
  uint16_t hue;
};

struct PaletteColor { uint16_t h; uint8_t s, v; };

/* ===== GLOBAL STATE ===================================================== */

CRGB               leds[LED_COUNT];
WebServer          server(8080);
WebSocketsServer   wsServer(81);
Preferences        prefs;

IPAddress          tvIP;
ZoneRGB            targetZones[4], currentZones[4];
WiFiClient         tvClient;
bool               tvOnline, tvOutputOff=true;
unsigned long      goodFrames, badFrames;

LedSegment         segments[MAX_SEGMENTS];
uint8_t            segmentCount=12, globalBrightness=DEFAULT_BRIGHTNESS;
uint8_t            smoothing=DEFAULT_SMOOTHING, blackThreshold=DEFAULT_BLACK_THRESHOLD;

bool     dynBrightEnabled;      uint8_t dynBrightMin, dynBrightMax, dynBrightResp;
uint8_t  tvDynBright, moodDynBright;

MoodConfig   leftMood, rightMood;
MoodLinkMode moodLinkMode;
uint16_t     moodLeftStart=30, moodLeftCount=30, moodRightStart=90, moodRightCount=30;
bool         moodDynEnabled;    uint8_t moodDynDepth=25;

bool     tvMasterSyncEnabled=true, tvMasterBrightnessEnabled=true;
uint8_t  tvMasterBrightness=255;  bool tvMasterBrightnessAvailable;

bool     sideCloneEnabled=true;  uint8_t sideCloneBrightness=255;
uint16_t cloneLeftStart=30, cloneLeftCount=30, cloneRightStart=90, cloneRightCount=30;
bool     cloneLeftRev, cloneRightRev;

uint8_t  tvTopoLeft=2, tvTopoTop=0, tvTopoRight=2, tvTopoBottom=0, tvTopoLayers=1;
bool     tvTopoDetected;

volatile bool wifiDisconnected;
bool     wifiConnectInProgress, webStarted;
unsigned long lastWiFiAttempt, wifiAttemptStarted;

bool     provisioningMode = false;
unsigned long staConnectDeadline = 0;
#define AP_SSID        "Ambilight-Bridge-Setup"
#define AP_FALLBACK_MS 20000UL

bool otaInProgress, otaRestartPending, restartPending;
const char* restartReason = "";
unsigned long restartRequestedAt = 0;
bool otaAuthorized = false;                 // [K4] OTA jogosultság-jelző
bool ledTestActive;  CRGB ledTestColor;  unsigned long ledTestUntil, restartAt;

volatile uint32_t smartFrameSeq;
unsigned long smartLastFrameMs;

unsigned long lastFrameStart, lastSuccessfulPoll, lastStatus, lastMoodFrame;
unsigned long lastTVMasterPoll, lastTVWatchdog;
String  lastError="";
uint8_t tvConsecutiveFailures;
bool    tvSocketAlive, tvWatchdogArmed=true;
unsigned long tvNextConnectAttempt;

static char tvRawBuf[16384];                // egyetlen statikus puffer a TV válaszhoz
static char tvJsonBuf[8192];                // a rövid GET kérések válaszához

#define TV_RECONNECT_INTERVAL_MS    100
#define TV_WATCHDOG_INTERVAL_MS     100
#define TV_STALE_TIMEOUT_MS         1000
#define TV_FAILURES_BEFORE_OFFLINE  2
#define TV_MASTER_POLL_MS           5000
#define MOOD_FRAME_INTERVAL_MS      40

/* ===== UTILITIES ======================================================== */

static inline uint8_t clampU8(int v) { return (uint8_t)constrain(v, 0, 255); }
static ZoneRGB mix2(const ZoneRGB& a, const ZoneRGB& b) {
  return {(uint8_t)(((uint16_t)a.r+(uint16_t)b.r)/2),
          (uint8_t)(((uint16_t)a.g+(uint16_t)b.g)/2),
          (uint8_t)(((uint16_t)a.b+(uint16_t)b.b)/2)};
}
uint8_t sceneLuminance() {
  uint32_t s=0; for(int i=0;i<4;i++) s+=max(max(targetZones[i].r,targetZones[i].g),targetZones[i].b);
  return (uint8_t)(s/4);
}

/* ===== JSON EXTRACTOR (brace-counter) — rövid válaszokhoz ================= */

int extractJSON(const char* raw, unsigned rawLen, String& out) {
  int start=-1;
  for(unsigned i=0;i<rawLen;i++){ if(raw[i]=='{'){start=i;break;} }
  if(start<0) return -1;
  int depth=0; bool inString=false, escape=false;
  for(unsigned i=start;i<rawLen;i++){
    char c=raw[i];
    if(escape){escape=false;continue;}
    if(c=='\\'){escape=true;continue;}
    if(c=='"'){inString=!inString;continue;}
    if(inString)continue;
    if(c=='{')depth++; else if(c=='}'){depth--;if(depth==0){out=String(raw+start).substring(0,i-start+1);return i;}}
  }
  return -1;
}

/* ===== CORS + AUTH ====================================================== */

void addCorsHeaders(){
  // Megjegyzés: a böngésző-app (GitHub Pages) miatt wildcard origin szükséges.
  // Biztonságért: provisioning után ÁLLÍTS webAuth jelszót (POST /api/auth).
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers","Authorization, Content-Type");
  server.sendHeader("Access-Control-Max-Age","600");
}
static unsigned long authFailCount,authLastFail;
bool webAuthCheck(){
  addCorsHeaders();
  unsigned long now=millis();
  if(authFailCount>=5&&(now-authLastFail)<60000){server.send(429,"text/plain","Too many auth attempts");return false;}
  if(authFailCount>=5&&(now-authLastFail)>=60000)authFailCount=0;
  if(strlen(webAuthPassword)==0)return true;
  if(!server.authenticate("admin",webAuthPassword)){
    authFailCount++;authLastFail=now;
    server.requestAuthentication(BASIC_AUTH,"Ambilight Bridge");
    return false;
  }
  authFailCount=0; return true;
}
// Csendes auth-ellenőrzés (nem küld választ) — az OTA upload handlerhez.
bool webAuthSilent(){
  if(strlen(webAuthPassword)==0)return true;
  return server.authenticate("admin",webAuthPassword);
}
void handleCorsPreflight(){addCorsHeaders();server.send(204,"text/plain","");}

/* ===== ROOT — beépített konfigurációs portál ============================ */
void handleConfigPortal(){
  String apName = provisioningMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String html;
  html.reserve(2600);
  html += F("<!DOCTYPE html><html lang='hu'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Ambilight Bridge — Setup</title><style>"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:#060d18;color:#e3edfc;"
    "font-family:Inter,system-ui,Arial,sans-serif;display:flex;align-items:center;justify-content:center;padding:20px}"
    ".box{width:100%;max-width:430px;background:rgba(12,22,38,.9);border:1px solid rgba(120,155,210,.15);"
    "border-radius:16px;padding:26px 26px 22px;box-shadow:0 20px 60px rgba(0,0,0,.5)}"
    "h1{font-size:19px;margin:0 0 4px}p.sub{font-size:12px;color:#8ca0bc;margin:0 0 18px}"
    "label{display:block;font-size:11px;color:#8ca0bc;margin:12px 0 4px;font-weight:600;text-transform:uppercase}"
    "input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid rgba(120,155,210,.18);"
    "background:rgba(10,18,30,.7);color:#e3edfc;font-size:14px;outline:none}"
    "input:focus{border-color:#2ad4ff}"
    "button{width:100%;margin-top:18px;padding:12px;border-radius:999px;border:1px solid rgba(56,232,160,.3);"
    "background:rgba(56,232,160,.12);color:#38e8a0;font-size:14px;font-weight:700;cursor:pointer}"
    "button:active{transform:scale(.99)}.note{font-size:11px;color:#54657d;margin-top:14px;line-height:1.5}"
    ".wifi{display:flex;gap:8px}.wifi select{flex:1}.wifi button{width:auto;margin:0;padding:0 16px;font-size:12px}"
    ".ok{color:#38e8a0;font-size:13px;margin-top:12px;display:none}.err{color:#fb7185;font-size:13px;margin-top:12px;display:none}"
    "</style></head><body><div class='box'>"
    "<h1>✦ Ambilight Bridge</h1><p class='sub'>");
  html += provisioningMode ? F("Első beállítás — add meg a WiFi hálózatot") : F("WiFi beállítások");
  html += F("</p>"
    "<label>Elérhető hálózatok</label>"
    "<div class='wifi'><select id='net'><option value=''>— keresés... —</option></select>"
    "<button type='button' onclick='scan()'>↻</button></div>"
    "<label>WiFi név (SSID)</label><input id='ssid' autocomplete='off'>"
    "<label>WiFi jelszó</label><input id='pass' type='password' autocomplete='off'>"
    "<button onclick='save()'>Mentés és újraindítás</button>"
    "<div class='ok' id='ok'>✓ Mentve — az eszköz újraindul és csatlakozik.</div>"
    "<div class='err' id='err'>✗ Hiba a mentésnél.</div>"
    "<p class='note'>A mentés után az ESP32 újraindul, felcsatlakozik a routerre, és "
    "elérhető lesz a <b>http://ambilight.local:8080</b> címen.</p>"
    "</div><script>"
    "const $=id=>document.getElementById(id);"
    "async function scan(){try{const r=await fetch('/api/scan');const a=await r.json();"
    "const s=$('net');s.innerHTML='<option value=\"\">— válassz —</option>';"
    "a.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+(n.rssi?' ('+n.rssi+' dBm)':'');s.appendChild(o)});"
    "}catch(e){}}"
    "$('net').addEventListener('change',e=>{if(e.target.value)$('ssid').value=e.target.value});"
    "async function save(){const ssid=$('ssid').value.trim(),pass=$('pass').value;"
    "if(!ssid){$('err').textContent='Add meg a WiFi nevet';$('err').style.display='block';return}"
    "try{const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)});"
    "if(r.ok){$('ok').style.display='block';$('err').style.display='none';}else throw 0;"
    "}catch(e){$('err').style.display='block';}}"
    "scan();"
    "</script></body></html>");
  server.send(200,"text/html; charset=utf-8",html);
}

// GET /api/scan
void apiScan(){
  int n = WiFi.scanNetworks();
  JsonDocument d; JsonArray a = d.to<JsonArray>();
  for(int i=0;i<n && i<20;i++){
    JsonObject o=a.add<JsonObject>();
    o["ssid"]=WiFi.SSID(i);
    o["rssi"]=WiFi.RSSI(i);
    o["enc"]=WiFi.encryptionType(i)!=WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String out; serializeJson(d,out);
  addCorsHeaders();
  server.send(200,"application/json",out);
}

/* ===== WEB SOCKET ======================================================= */

void wsEvent(uint8_t num, WStype_t type, uint8_t*, size_t){
  switch(type){
    case WStype_CONNECTED:    Serial.printf("[WS] client %u connected\n",num); break;
    case WStype_DISCONNECTED: Serial.printf("[WS] client %u disconnected\n",num); break;
    default: break;
  }
}
void broadcastRealtime(){
  JsonDocument doc;
  doc["seq"]=smartFrameSeq; doc["ts"]=millis(); doc["tv"]=tvOnline;
  JsonArray z=doc["zones"].to<JsonArray>();
  for(int i=0;i<4;i++){JsonObject zz=z.add<JsonObject>(); zz["r"]=targetZones[i].r; zz["g"]=targetZones[i].g; zz["b"]=targetZones[i].b;}
  String out; serializeJson(doc,out); wsServer.broadcastTXT(out);
}

/* ===== REST HANDLERS ==================================================== */

// GET /api/state
void apiState(){
  addCorsHeaders();
  JsonDocument d;
  d["fw"]=FIRMWARE_VERSION; d["firmware"]=FIRMWARE_VERSION; d["contract"]=FW_CONTRACT_VERSION;
  d["tvOnline"]=tvOnline; d["tv_online"]=tvOnline; d["tvIP"]=tvIP.toString();
  d["wifi"]=WiFi.isConnected(); d["rssi"]=WiFi.isConnected()?WiFi.RSSI():-127;
  d["ip"]=WiFi.localIP().toString(); d["ap_active"]=provisioningMode;
  d["ap_ip"]=provisioningMode?WiFi.softAPIP().toString():"";
  d["brightness"]=globalBrightness; d["smoothing"]=smoothing; d["blackThreshold"]=blackThreshold;
  d["goodFrames"]=goodFrames; d["badFrames"]=badFrames; d["heap"]=ESP.getFreeHeap(); d["uptime"]=millis()/1000;
  d["segmentCount"]=segmentCount; d["sideCloneEnabled"]=sideCloneEnabled; d["sideCloneBrightness"]=sideCloneBrightness;
  d["moodLinkMode"]=(uint8_t)moodLinkMode; d["tvTopoDetected"]=tvTopoDetected;
  JsonArray zc=d["zone_current"].to<JsonArray>(); JsonArray zt=d["zone_target"].to<JsonArray>(); JsonArray zones=d["zones"].to<JsonArray>();
  for(int i=0;i<4;i++){ JsonArray c=zc.add<JsonArray>(); c.add(currentZones[i].r); c.add(currentZones[i].g); c.add(currentZones[i].b); JsonArray t=zt.add<JsonArray>(); t.add(targetZones[i].r); t.add(targetZones[i].g); t.add(targetZones[i].b); JsonObject z=zones.add<JsonObject>(); z["r"]=targetZones[i].r; z["g"]=targetZones[i].g; z["b"]=targetZones[i].b; }
  JsonArray segs=d["segments"].to<JsonArray>();
  for(uint8_t i=0;i<segmentCount && i<MAX_SEGMENTS;i++){ JsonObject o=segs.add<JsonObject>(); o["start"]=segments[i].start; o["count"]=segments[i].count; o["source"]=segments[i].source; o["brightness"]=segments[i].brightness; o["reverse"]=segments[i].reverse; }
  String out; serializeJson(d,out); server.send(200,"application/json",out);
}

// GET /api/realtime — backward-compatible REST snapshot for older browser clients
void apiRealtime(){
  addCorsHeaders();
  JsonDocument d;
  d["seq"]=smartFrameSeq; d["ts"]=millis(); d["tv"]=tvOnline;
  JsonArray z=d["zones"].to<JsonArray>();
  for(int i=0;i<4;i++){ JsonObject zz=z.add<JsonObject>(); zz["r"]=targetZones[i].r; zz["g"]=targetZones[i].g; zz["b"]=targetZones[i].b; }
  String out; serializeJson(d,out);
  server.send(200,"application/json",out);
}

// GET /api/capabilities — firmware/browser contract
void apiCapabilities(){
  addCorsHeaders(); JsonDocument d;
  d["contract"]=FW_CONTRACT_VERSION; d["firmware"]=FIRMWARE_VERSION;
  JsonObject led=d["led"].to<JsonObject>(); led["count"]=LED_COUNT; led["pin"]=LED_PIN; led["type"]="WS2815";
  JsonObject mapper=d["mapper"].to<JsonObject>(); mapper["maxSegments"]=MAX_SEGMENTS; mapper["sourceCount"]=SOURCE_COUNT;
  JsonArray src=mapper["sources"].to<JsonArray>(); for(uint8_t i=0;i<SOURCE_COUNT;i++){ JsonObject x=src.add<JsonObject>(); x["id"]=i; x["name"]=SOURCE_NAMES[i]; }
  JsonObject mood=d["mood"].to<JsonObject>(); mood["effectCount"]=MOOD_FX_COUNT; mood["maxEffect"]=MOOD_EFFECT_MAX; mood["linkModes"]=4;
  JsonObject feat=d["features"].to<JsonObject>(); feat["mapper"]=true; feat["mapperPersistence"]=true; feat["gradient"]=true; feat["sideClone"]=true; feat["topology"]=true; feat["moodLink"]=true; feat["ota"]=true; feat["websocket"]=true; feat["apProvisioning"]=true;
  String out; serializeJson(d,out); server.send(200,"application/json",out);
}

static void requestRestart(const char* reason, unsigned long delayMs){
  restartReason = reason;
  restartRequestedAt = millis();
  restartPending = true;
  restartAt = restartRequestedAt + delayMs;
  Serial.printf("[SYS] reboot requested: %s in %lu ms\n", restartReason, delayMs);
}

static bool mapperValid(const LedSegment& sg){
  if(sg.count==0 || sg.start>=LED_COUNT) return false;
  if((uint32_t)sg.start+sg.count>LED_COUNT) return false;
  if(sg.source>=SOURCE_COUNT) return false;
  return true;
}
void saveMapper(){
  prefs.begin("cfg",false); prefs.putUChar("segcnt",segmentCount);
  for(uint8_t i=0;i<MAX_SEGMENTS;i++){ char k[8];
    snprintf(k,sizeof(k),"s%ust",i); prefs.putUShort(k,segments[i].start);
    snprintf(k,sizeof(k),"s%uct",i); prefs.putUShort(k,segments[i].count);
    snprintf(k,sizeof(k),"s%usc",i); prefs.putUChar(k,segments[i].source);
    snprintf(k,sizeof(k),"s%ub",i); prefs.putUChar(k,segments[i].brightness);
    snprintf(k,sizeof(k),"s%ur",i); prefs.putBool(k,segments[i].reverse);
  } prefs.end();
}
void loadMapper(bool defaultsIfMissing){
  prefs.begin("cfg",true); bool has=prefs.isKey("segcnt"); uint8_t n=prefs.getUChar("segcnt",0);
  if(has && n>0 && n<=MAX_SEGMENTS){ segmentCount=n; bool all=true; for(uint8_t i=0;i<n;i++){ char k[8];
      snprintf(k,sizeof(k),"s%ust",i); segments[i].start=prefs.getUShort(k,0);
      snprintf(k,sizeof(k),"s%uct",i); segments[i].count=prefs.getUShort(k,0);
      snprintf(k,sizeof(k),"s%usc",i); segments[i].source=prefs.getUChar(k,SRC_BLACK);
      snprintf(k,sizeof(k),"s%ub",i); segments[i].brightness=prefs.getUChar(k,255);
      snprintf(k,sizeof(k),"s%ur",i); segments[i].reverse=prefs.getBool(k,false); if(!mapperValid(segments[i])) all=false;
    } prefs.end(); if(all) return;
  } else prefs.end();
  if(defaultsIfMissing){ setDefaultMapping(); saveMapper(); }
}

// GET/POST /api/mapper. Accepts form fields or JSON body.
void apiMapper(){
  if(!webAuthCheck())return;
  if(server.method()==HTTP_GET){
    JsonDocument d; d["count"]=segmentCount; JsonArray a=d["segments"].to<JsonArray>();
    for(uint8_t i=0;i<segmentCount;i++){JsonObject o=a.add<JsonObject>();o["start"]=segments[i].start;o["count"]=segments[i].count;o["source"]=segments[i].source;o["brightness"]=segments[i].brightness;o["reverse"]=segments[i].reverse;}
    String out;serializeJson(d,out);server.send(200,"application/json",out);return;
  }
  String body=server.hasArg("plain")?server.arg("plain"):""; JsonDocument d; bool parsed=false; if(body.length()){ DeserializationError e=deserializeJson(d,body); parsed=!e; }
  if(body.length() && parsed && d["segments"].is<JsonArray>()){
    JsonArray a=d["segments"].as<JsonArray>(); if(a.size()==0 || a.size()>MAX_SEGMENTS){server.send(400,"application/json","{\"ok\":false,\"err\":\"bad segment count\"}");return;}
    LedSegment tmp[MAX_SEGMENTS]; bool ok=true; uint8_t i=0; for(JsonVariant v:a){tmp[i].start=v["start"]|0;tmp[i].count=v["count"]|0;tmp[i].source=v["source"]|0;tmp[i].brightness=v["brightness"]|255;tmp[i].reverse=v["reverse"]|false;if(!mapperValid(tmp[i]))ok=false;i++;}
    if(!ok){server.send(400,"application/json","{\"ok\":false,\"err\":\"invalid segment\"}");return;} segmentCount=i; for(i=0;i<segmentCount;i++)segments[i]=tmp[i]; saveMapper(); server.send(200,"application/json","{\"ok\":true}"); return;
  }
  server.send(400,"application/json","{\"ok\":false,\"err\":\"expected JSON segments\"}");
}

// POST /api/config  (általános beállítások)
void apiConfig(){
  if(!webAuthCheck())return;
  if(server.hasArg("brightness")) globalBrightness=clampU8(server.arg("brightness").toInt());
  if(server.hasArg("smoothing"))  smoothing=clampU8(server.arg("smoothing").toInt());
  if(server.hasArg("blackThreshold")) blackThreshold=clampU8(server.arg("blackThreshold").toInt());
  saveConfig();
  server.send(200,"application/json","{\"ok\":true}");
}

// POST /api/tv  (TV IP beállítása)
void apiTV(){
  if(!webAuthCheck())return;
  if(server.hasArg("ip")){
    IPAddress ip;
    if(ip.fromString(server.arg("ip"))){ tvIP=ip; tvConsecutiveFailures=0; tvOnline=false;
      if(tvClient.connected())tvClient.stop();
      saveConfig();
      server.send(200,"application/json","{\"ok\":true}"); return; }
  }
  server.send(400,"application/json","{\"ok\":false,\"err\":\"invalid ip\"}");
}

// POST /api/auth  (webes jelszó beállítása)
void apiAuth(){
  if(!webAuthCheck())return;
  if(server.hasArg("password")){
    String p=server.arg("password");
    if(p.length()<64){
      strncpy(webAuthPassword,p.c_str(),sizeof(webAuthPassword)-1);
      webAuthPassword[sizeof(webAuthPassword)-1]='\0';
      saveConfig();
      server.send(200,"application/json","{\"ok\":true}"); return;
    }
  }
  server.send(400,"application/json","{\"ok\":false}");
}

// POST /api/wifi  — [K1] NVS nyítás/zárás explicit prefs.begin/end párral
void apiWifi(){
  addCorsHeaders();
  if(!provisioningMode && !webAuthCheck())return;   // provisioning módban nyílt
  if(!server.hasArg("ssid")){ server.send(400,"application/json","{\"ok\":false,\"err\":\"no ssid\"}"); return; }
  String ssid=server.arg("ssid");
  String pass=server.hasArg("password")?server.arg("password"):"";
  if(ssid.length()==0||ssid.length()>32){ server.send(400,"application/json","{\"ok\":false,\"err\":\"bad ssid\"}"); return; }
  // Idempotens WiFi-mentés:
  // ha a böngésző az újraindítás után ugyanazt a POST-ot ismételten elküldi,
  // NE indítsuk újra újra az ESP32-t. Csak valódi változás esetén rebootolunk.
  String oldSSID;
  String oldPass;
  prefs.begin("wifi", true);
  oldSSID = prefs.getString("ssid", "");
  oldPass = prefs.getString("pass", "");
  prefs.end();

  if (oldSSID == ssid && oldPass == pass) {
    server.send(200,"application/json","{\"ok\":true,\"changed\":false,\"reboot\":false}");
    Serial.println("[WiFi] azonos hitelesítő adatok — nincs újraindítás");
    return;
  }

  // Saját namespace-ben, külön tranzakcióban mentünk és ZÁRUNK.
  prefs.begin("wifi",false);
  prefs.putString("ssid",ssid);
  prefs.putString("pass",pass);
  prefs.end();

  server.send(200,"application/json","{\"ok\":true,\"changed\":true,\"reboot\":true}");
  requestRestart("wifi-save",800);   // válasz kiküldése után egyszer újraindul
}

// GET/POST /api/sideclone
void apiSideClone(){
  if(!webAuthCheck())return;
  if(server.method()==HTTP_GET){JsonDocument d;d["enabled"]=sideCloneEnabled;d["brightness"]=sideCloneBrightness;d["leftStart"]=cloneLeftStart;d["leftCount"]=cloneLeftCount;d["rightStart"]=cloneRightStart;d["rightCount"]=cloneRightCount;d["leftReverse"]=cloneLeftRev;d["rightReverse"]=cloneRightRev;String o;serializeJson(d,o);server.send(200,"application/json",o);return;}
  if(server.hasArg("enabled"))sideCloneEnabled=server.arg("enabled").toInt()!=0; if(server.hasArg("brightness"))sideCloneBrightness=clampU8(server.arg("brightness").toInt());
  if(server.hasArg("leftStart"))cloneLeftStart=server.arg("leftStart").toInt(); if(server.hasArg("leftCount"))cloneLeftCount=server.arg("leftCount").toInt();
  if(server.hasArg("rightStart"))cloneRightStart=server.arg("rightStart").toInt(); if(server.hasArg("rightCount"))cloneRightCount=server.arg("rightCount").toInt();
  if(server.hasArg("leftReverse"))cloneLeftRev=server.arg("leftReverse").toInt()!=0; if(server.hasArg("rightReverse"))cloneRightRev=server.arg("rightReverse").toInt()!=0;
  if((uint32_t)cloneLeftStart+cloneLeftCount>LED_COUNT || (uint32_t)cloneRightStart+cloneRightCount>LED_COUNT){server.send(400,"application/json","{\"ok\":false,\"err\":\"range\"}");return;}
  saveConfig(); server.send(200,"application/json","{\"ok\":true}");
}

// GET /api/topology
void apiTopology(){ addCorsHeaders(); JsonDocument d; d["detected"]=tvTopoDetected; d["left"]=tvTopoLeft; d["top"]=tvTopoTop; d["right"]=tvTopoRight; d["bottom"]=tvTopoBottom; d["layers"]=tvTopoLayers; String o;serializeJson(d,o);server.send(200,"application/json",o);}

/* ===== MOOD REST ======================================================== */

void apiMood(){
  if(!webAuthCheck())return;
  if(server.hasArg("leftMode"))  leftMood.effect =(uint8_t)constrain(server.arg("leftMode").toInt(),0,(int)MOOD_EFFECT_MAX);
  if(server.hasArg("rightMode")) rightMood.effect=(uint8_t)constrain(server.arg("rightMode").toInt(),0,(int)MOOD_EFFECT_MAX);
  if(server.hasArg("leftHue"))   leftMood.hue = (uint16_t)constrain(server.arg("leftHue").toInt(),0,359);
  if(server.hasArg("rightHue"))  rightMood.hue= (uint16_t)constrain(server.arg("rightHue").toInt(),0,359);
  if(server.hasArg("leftSat"))   leftMood.saturation = clampU8(server.arg("leftSat").toInt());
  if(server.hasArg("rightSat"))  rightMood.saturation= clampU8(server.arg("rightSat").toInt());
  if(server.hasArg("leftVal"))   leftMood.brightness = clampU8(server.arg("leftVal").toInt());
  if(server.hasArg("rightVal"))  rightMood.brightness= clampU8(server.arg("rightVal").toInt());
  if(server.hasArg("linkMode"))  moodLinkMode=(MoodLinkMode)constrain(server.arg("linkMode").toInt(),0,3);
  saveConfig();
  server.send(200,"application/json","{\"ok\":true}");
}

/* ===== LED TEST + OTA =================================================== */

void apiLedTest(){
  if(!webAuthCheck())return;
  uint8_t r=server.hasArg("r")?clampU8(server.arg("r").toInt()):255;
  uint8_t g=server.hasArg("g")?clampU8(server.arg("g").toInt()):255;
  uint8_t b=server.hasArg("b")?clampU8(server.arg("b").toInt()):255;
  ledTestColor=CRGB(r,g,b); ledTestActive=true; ledTestUntil=millis()+5000;
  server.send(200,"application/json","{\"ok\":true}");
}

void apiReboot(){
  if(!webAuthCheck())return;
  // A reboot végpont csak explicit megerősítéssel működik. Ez megakadályozza,
  // hogy egy hibás/öreg browser kliens véletlenül reboot-loopot indítson.
  if(!server.hasArg("confirm") || server.arg("confirm")!="1") {
    addCorsHeaders();
    server.send(400,"application/json","{\"ok\":false,\"err\":\"confirm=1 required\"}");
    return;
  }
  server.send(200,"application/json","{\"ok\":true}");
  requestRestart("api-reboot",500);
}

// [K4] OTA: az upload handler minden chánknél fut — az első (START) fázisban
// ellenőrizzük a jogosultságot, és a döntést az otaAuthorized flagbe rakjuk.
void otaUploadHandler(){
  HTTPUpload& up=server.upload();
  if(up.status==UPLOAD_FILE_START){
    otaAuthorized = webAuthSilent();     // csendes ellenőrzés (nem küld választ)
    if(!otaAuthorized){ Serial.println("[OTA] elutasítva: nincs jogosultság"); return; }
    Serial.printf("[OTA] start: %s\n",up.filename.c_str());
    otaInProgress=true;
    if(!Update.begin(UPDATE_SIZE_UNKNOWN)){ Update.printError(Serial); otaAuthorized=false; }
  } else if(up.status==UPLOAD_FILE_WRITE){
    if(!otaAuthorized)return;             // jogosulatlan — minden chánkot eldobunk
    if(Update.write(up.buf,up.currentSize)!=up.currentSize) Update.printError(Serial);
  } else if(up.status==UPLOAD_FILE_END){
    if(!otaAuthorized)return;
    if(Update.end(true)) Serial.printf("[OTA] kész: %u byte\n",up.totalSize);
    else Update.printError(Serial);
    otaInProgress=false;
  }
}
void otaFinishHandler(){
  addCorsHeaders();
  if(!otaAuthorized){ server.send(401,"application/json","{\"ok\":false,\"err\":\"unauthorized\"}"); return; }
  bool ok=!Update.hasError();
  server.send(ok?200:500,"application/json", ok?"{\"ok\":true}":"{\"ok\":false}");
  if(ok){ requestRestart("ota",1000); otaRestartPending=true; }
}

/* ===== HTTP ROUTES ====================================================== */

void setupRoutes(){
  server.on("/",             HTTP_GET,  handleConfigPortal);
  server.on("/api/scan",     HTTP_GET,  apiScan);
  server.on("/api/state",    HTTP_GET,  apiState);
  server.on("/api/realtime", HTTP_GET,  apiRealtime);
  server.on("/api/capabilities", HTTP_GET, apiCapabilities);
  server.on("/api/mapper", HTTP_GET, apiMapper);
  server.on("/api/mapper", HTTP_POST, apiMapper);
  server.on("/api/sideclone", HTTP_GET, apiSideClone);
  server.on("/api/sideclone", HTTP_POST, apiSideClone);
  server.on("/api/topology", HTTP_GET, apiTopology);
  server.on("/api/realtime", HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/capabilities", HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/mapper", HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/sideclone", HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/topology", HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/config",   HTTP_POST, apiConfig);
  server.on("/api/tv",       HTTP_POST, apiTV);
  server.on("/api/auth",     HTTP_POST, apiAuth);
  server.on("/api/wifi",     HTTP_POST, apiWifi);
  server.on("/api/mood",     HTTP_POST, apiMood);
  server.on("/api/ledtest",  HTTP_POST, apiLedTest);
  server.on("/api/reboot",   HTTP_POST, apiReboot);
  // OTA: két lépcső — upload handler + finish handler (mindkettő auth-őrzett)
  server.on("/api/ota",      HTTP_POST, otaFinishHandler, otaUploadHandler);

  // [M1] CORS preflight (OPTIONS) minden POST-olt végpontra
  server.on("/api/config",   HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/tv",       HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/auth",     HTTP_OPTIONS, handleCorsPreflight);   // [M1] hiányzott
  server.on("/api/wifi",     HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/mood",     HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/ledtest",  HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/reboot",   HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/ota",      HTTP_OPTIONS, handleCorsPreflight);
  server.on("/api/scan",     HTTP_OPTIONS, handleCorsPreflight);   // [M1] hiányzott

  server.onNotFound([](){ addCorsHeaders(); server.send(404,"application/json","{\"err\":\"not found\"}"); });
}

void startWebServices(){
  if(webStarted)return;
  setupRoutes();
  server.begin();
  wsServer.begin();
  wsServer.onEvent(wsEvent);
  webStarted=true;
  Serial.println("[WEB] REST(8080)+WS(81) elindult");
}

/* ===== WIFI / AP ======================================================== */

/*
 * AP PROVISIONING LIFECYCLE
 * The setup AP is temporary. After a successful STA connection it is
 * explicitly shut down and the device enters STA-only operation.
 */
void stopProvisioningAP(){
  if(WiFi.getMode()==WIFI_AP || WiFi.getMode()==WIFI_AP_STA){
    Serial.println("[AP] Provisioning AP leállítása...");
    WiFi.softAPdisconnect(true);
  }
  WiFi.mode(WIFI_STA);
  provisioningMode=false;
  Serial.println("[AP] Provisioning AP OFF — STA-only mód");
}

void startProvisioningAP(){
  provisioningMode=true;
  wifiConnectInProgress=false;

  WiFi.mode(WIFI_AP_STA);
  delay(20);

  // Remove any stale AP instance before starting a fresh provisioning AP.
  WiFi.softAPdisconnect(true);
  delay(20);

  bool apOk=WiFi.softAP(AP_SSID);
  if(apOk){
    Serial.printf("[AP] Provisioning AP: %s @ %s\n",
                  AP_SSID,WiFi.softAPIP().toString().c_str());
  }else{
    Serial.println("[AP] HIBA: provisioning AP indítása sikertelen");
  }

  startWebServices();
}

void connectWiFi(){
  prefs.begin("wifi",true);
  String ssid=prefs.getString("ssid","");
  String pass=prefs.getString("pass","");
  prefs.end();
#ifdef SECRET_WIFI_SSID
  if(ssid.length()==0){ ssid=SECRET_WIFI_SSID; pass=SECRET_WIFI_PASS; }
#endif
  if(ssid.length()==0){ Serial.println("[WiFi] nincs tárolt SSID → provisioning"); startProvisioningAP(); return; }

  // Normal boot / reconnect starts STA-only.
  if(WiFi.getMode()==WIFI_AP || WiFi.getMode()==WIFI_AP_STA){
    WiFi.softAPdisconnect(true);
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("ambilight");
  WiFi.begin(ssid.c_str(),pass.c_str());
  wifiConnectInProgress=true; wifiAttemptStarted=millis();
  staConnectDeadline=millis()+AP_FALLBACK_MS;
  Serial.printf("[WiFi] csatlakozás: %s\n",ssid.c_str());
}

void onWiFiConnected(){
  wifiConnectInProgress=false;

  // Critical AP lifecycle fix: never remain in AP+STA after provisioning.
  if(provisioningMode){
    stopProvisioningAP();
  }else{
    WiFi.mode(WIFI_STA);
  }

  Serial.printf("[WiFi] OK — IP: %s\n",WiFi.localIP().toString().c_str());
  if(MDNS.begin("ambilight")){
    MDNS.addService("http","tcp",8080);
    Serial.println("[mDNS] ambilight.local");
  }
  startWebServices();
  if(tvIP!=IPAddress(0,0,0,0)) detectTVTopology();
}

void handleWiFi(){
  static wl_status_t last=WL_IDLE_STATUS;
  wl_status_t s=WiFi.status();

  if(wifiConnectInProgress){
    if(s==WL_CONNECTED){
      onWiFiConnected();
    }else if(millis()>staConnectDeadline){
      Serial.println("[WiFi] időtúllépés → provisioning AP");
      wifiConnectInProgress=false;
      startProvisioningAP();
    }
  }else if(!provisioningMode){
    if(last==WL_CONNECTED && s!=WL_CONNECTED){
      Serial.println("[WiFi] kapcsolat elveszett — újracsatlakozás");
      WiFi.reconnect();
    }
  }

  last=s;
}

/* ===== TV KAPCSOLAT (Philips JointSPACE, TCP 1925) ===================== */

bool tvConnect(){
  if(tvClient.connected())return true;
  if(tvIP==IPAddress(0,0,0,0))return false;
  tvClient.stop();
  if(!tvClient.connect(tvIP,1925)){ return false; }
  tvClient.setTimeout(250);   // [E2] fix 250 ms socket timeout
  tvSocketAlive=true;
  return true;
}

// Rövid HTTP GET a JointSPACE-hez; a válasz JSON-ját tvJsonBuf-ba olvassuk.
int tvGet(const char* path, char* buf, size_t bufLen){
  if(!tvConnect())return -1;
  tvClient.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  path, tvIP.toString().c_str());
  unsigned long t0=millis(); size_t n=0; bool headersDone=false; int contentLen=-1;
  // fejléc beolvasása
  String headers="";
  while(millis()-t0<TV_BODY_READ_TIMEOUT_MS){
    while(tvClient.available()){
      char c=tvClient.read();
      if(!headersDone){
        headers+=c;
        if(headers.endsWith("\r\n\r\n")){
          headersDone=true;
          int ci=headers.indexOf("Content-Length:");
          if(ci>=0) contentLen=headers.substring(ci+15).toInt();
        }
      } else {
        if(n<bufLen-1) buf[n++]=c;
        if(contentLen>0 && (int)n>=contentLen) goto done;
      }
    }
    if(headersDone && contentLen<0 && n>0 && !tvClient.available()) break;
    delay(1);
  }
done:
  buf[n]='\0';
  tvClient.stop(); tvSocketAlive=false;
  return (int)n;
}

// [M3/M4] readAmbilight: brace-count korai kilépés + közvetlen deserializeJson(tvRawBuf)
bool readAmbilight(){
  if(!tvConnect())return false;
  tvClient.printf("GET /1/ambilight/processed HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  tvIP.toString().c_str());
  unsigned long t0=millis(); size_t n=0; bool headersDone=false;
  int depth=0; bool bodyStarted=false, inString=false, escape=false;
  String headers="";
  // [M4] a törőjel-számlálóval korrekt módon észleljük a JSON végét → korai kilépés
  while(millis()-t0<800){
    while(tvClient.available()){
      char c=tvClient.read();
      if(!headersDone){
        headers+=c;
        if(headers.endsWith("\r\n\r\n")) headersDone=true;
        continue;
      }
      if(n<sizeof(tvRawBuf)-1) tvRawBuf[n++]=c; else goto stop; // [M3] tvRawBuf túlcsordulás-védelem
      // brace-count csak a body-ra
      if(escape){escape=false;}
      else if(c=='\\'){escape=true;}
      else if(c=='"'){inString=!inString;}
      else if(!inString){
        if(c=='{'){depth++;bodyStarted=true;}
        else if(c=='}'){depth--; if(bodyStarted&&depth==0){goto stop;}} // korai kilépés
      }
    }
    delay(1);
  }
stop:
  tvRawBuf[n]='\0';
  tvClient.stop(); tvSocketAlive=false;
  if(n==0){ badFrames++; return false; }
  // [M3] közvetlenül a pufferből deserializálunk — nincs több String-másolás
  JsonDocument d;
  DeserializationError e=deserializeJson(d,tvRawBuf);
  if(e){ badFrames++; return false; }
  JsonObject layer1=d["layer1"];
  if(layer1.isNull()){ badFrames++; return false; }
  // bal + jobb oszlop átlaga a 4 zónához (felső/alsó bal, felső/alsó jobb)
  auto pix=[&](const char* side, const char* idx, ZoneRGB& out)->bool{
    JsonObject col=layer1[side]; if(col.isNull())return false;
    JsonObject p=col[idx]; if(p.isNull())return false;
    out.r=p["r"]|0; out.g=p["g"]|0; out.b=p["b"]|0; return true;
  };
  ZoneRGB lt{},lb{},rt{},rb{};
  bool ok = pix("left","0",lt) && pix("left","1",lb) && pix("right","0",rt) && pix("right","1",rb);
  if(!ok){ badFrames++; return false; }
  targetZones[0]=lt; targetZones[1]=lb; targetZones[2]=rt; targetZones[3]=rb;
  goodFrames++; lastSuccessfulPoll=millis(); tvConsecutiveFailures=0;
  return true;
}

// TV master hatalom-állapot lekérdezése (power + brightness)
void pollTVMaster(){
  int n=tvGet("/1/system/power",tvJsonBuf,sizeof(tvJsonBuf));
  if(n>0){
    JsonDocument d;
    if(!deserializeJson(d,tvJsonBuf)){
      const char* pw=d["powerstate"]|"";
      tvOutputOff = (strcmp(pw,"On")!=0);
    }
  }
}

void handleTVWatchdog(){
  unsigned long now=millis();
  if(tvConsecutiveFailures>=TV_FAILURES_BEFORE_OFFLINE){
    if(tvOnline){ tvOnline=false; Serial.println("[TV] offline"); }
  }
  if(now-lastSuccessfulPoll<TV_STALE_TIMEOUT_MS){
    if(!tvOnline){ tvOnline=true; Serial.println("[TV] online"); }
  }
}

/* ===== LED RENDER ======================================================= */

// A szegmens forrás-módjából (SourceMode) számítja ki a megfelelő színt.
// 4 zóna: [0]=bal-felső(L0) [1]=bal-alsó(L1) [2]=jobb-felső(R0) [3]=jobb-alsó(R1)
CRGB zoneFromSource(uint8_t source){
  ZoneRGB z={0,0,0}; switch(source){
    case SRC_BLACK:z={0,0,0};break; case SRC_L0:z=currentZones[0];break; case SRC_L1:z=currentZones[1];break; case SRC_R0:z=currentZones[2];break; case SRC_R1:z=currentZones[3];break;
    case SRC_L_AVG:z=mix2(currentZones[0],currentZones[1]);break; case SRC_R_AVG:z=mix2(currentZones[2],currentZones[3]);break; case SRC_ALL_AVG:z=mix2(mix2(currentZones[0],currentZones[1]),mix2(currentZones[2],currentZones[3]));break;
    case SRC_LR_TOP_MIX:z=mix2(currentZones[0],currentZones[2]);break; case SRC_LR_BOTTOM_MIX:z=mix2(currentZones[1],currentZones[3]);break;
    case SRC_VERTICAL_AVG:z=mix2(mix2(currentZones[0],currentZones[2]),mix2(currentZones[1],currentZones[3]));break;
    case SRC_L0_R0_BLEND:z=mix2(currentZones[0],currentZones[2]);break; case SRC_L1_R1_BLEND:z=mix2(currentZones[1],currentZones[3]);break; default:z=currentZones[0];break; } return CRGB(z.r,z.g,z.b);
}
CRGB gradientSource(uint8_t source,float t){
  t=constrain(t,0.0f,1.0f); ZoneRGB a,b;
  switch(source){case SRC_GRADIENT_TOP:a=currentZones[0];b=currentZones[2];break;case SRC_GRADIENT_RIGHT:a=currentZones[2];b=currentZones[3];break;case SRC_GRADIENT_BOTTOM:a=currentZones[1];b=currentZones[3];break;case SRC_GRADIENT_LEFT:a=currentZones[0];b=currentZones[1];break;default:return zoneFromSource(source);}
  return CRGB((uint8_t)(a.r+(b.r-a.r)*t),(uint8_t)(a.g+(b.g-a.g)*t),(uint8_t)(a.b+(b.b-a.b)*t));
}

void renderZonesToLeds(){
  uint8_t sm=smoothing; for(int i=0;i<4;i++){int k=255-sm;currentZones[i].r+=(targetZones[i].r-currentZones[i].r)*k/255;currentZones[i].g+=(targetZones[i].g-currentZones[i].g)*k/255;currentZones[i].b+=(targetZones[i].b-currentZones[i].b)*k/255;}
  fill_solid(leds,LED_COUNT,CRGB::Black); bool written[LED_COUNT]={false};
  for(uint8_t s=0;s<segmentCount && s<MAX_SEGMENTS;s++){LedSegment &sg=segments[s];if(!mapperValid(sg))continue;for(uint16_t i=0;i<sg.count;i++){uint16_t idx=sg.reverse?(sg.start+sg.count-1-i):(sg.start+i);float t=sg.count>1?(float)i/(float)(sg.count-1):0.0f;CRGB col=(sg.source>=SRC_GRADIENT_TOP&&sg.source<=SRC_GRADIENT_LEFT)?gradientSource(sg.source,t):zoneFromSource(sg.source);col.nscale8(sg.brightness);leds[idx]=col;written[idx]=true;}}
  if(sideCloneEnabled){CRGB l=zoneFromSource(SRC_L_AVG);CRGB r=zoneFromSource(SRC_R_AVG);l.nscale8(sideCloneBrightness);r.nscale8(sideCloneBrightness);for(uint16_t i=0;i<cloneLeftCount;i++){uint16_t idx=cloneLeftRev?(cloneLeftStart+cloneLeftCount-1-i):(cloneLeftStart+i);if(idx<LED_COUNT&&!written[idx])leds[idx]=l;}for(uint16_t i=0;i<cloneRightCount;i++){uint16_t idx=cloneRightRev?(cloneRightStart+cloneRightCount-1-i):(cloneRightStart+i);if(idx<LED_COUNT&&!written[idx])leds[idx]=r;}}
}

void showLeds(){
  if(ledTestActive){
    if(millis()<ledTestUntil){ fill_solid(leds,LED_COUNT,ledTestColor); FastLED.setBrightness(255); FastLED.show(); return; }
    ledTestActive=false;
  }
  uint8_t br=globalBrightness;
  if(tvOutputOff){ FastLED.clear(); FastLED.show(); return; }
  FastLED.setBrightness(br);
  FastLED.show();
}

/* ===== MOOD ENGINE ====================================================== */

// [E4] pontos 0..359 → 0..255 húe-átváltás: *256u/360u (nem *255/359)
static inline uint8_t hue360To8(uint16_t h){ return (uint8_t)(((uint32_t)h*256u)/360u); }

static CRGB moodColor(const MoodConfig& m,uint16_t i,uint16_t count,uint8_t sideOffset=0){
  uint32_t now=millis(); uint8_t fx=m.effect; uint8_t h=hue360To8(m.hue); float p=count>1?(float)i/(count-1):0.0f;
  uint8_t phase=(uint8_t)((now*(uint32_t)(m.speed+1)/30u)+sideOffset+i*(m.motion+1));
  uint8_t v=m.brightness;
  switch(fx){case MOOD_BREATHE:v=(uint8_t)(m.brightness*(0.45f+0.55f*(0.5f+0.5f*sin(now/500.0f))));break;case MOOD_RAINBOW:h+=phase;break;case MOOD_SLOW_COLOR:h+=phase/8;break;case MOOD_WARM:h=18;v=min<uint8_t>(255,m.brightness);break;case MOOD_COLOR_WAVE:h+=(uint8_t)(p*96)+phase;break;case MOOD_COMET:h+=phase;v=(uint8_t)(m.brightness*(0.25f+0.75f*fmax(0.0f,1.0f-fabs(p-fmod(now/1200.0f,1.0f))*4.0f)));break;case MOOD_TWINKLE: v=(uint8_t)(m.brightness*((((i*37u+phase*13u)%100)<(20+m.density))?1.0f:0.2f));break;case MOOD_PULSE:v=(uint8_t)(m.brightness*(0.2f+0.8f*(0.5f+0.5f*sin(now/180.0f))));break;case MOOD_METEOR:h+=phase;break;case MOOD_CYBER:h=(uint8_t)(p*40)+phase;break;case MOOD_SPECTRAL:h=(uint8_t)(p*255)+phase;break;default:break;}
  CHSV hsv(h,m.saturation,v); CRGB c; hsv2rgb_rainbow(hsv,c); return c;
}
void renderMoodSide(const MoodConfig& m,uint16_t start,uint16_t count,bool rev,uint8_t sideOffset=0){for(uint16_t i=0;i<count;i++){uint16_t idx=rev?(start+count-1-i):(start+i);if(idx<LED_COUNT)leds[idx]=moodColor(m,i,count,sideOffset);}}
void renderMood(){MoodConfig L=leftMood,R=rightMood;bool revR=false;uint8_t offR=0;if(moodLinkMode==MOOD_LINK_MIRROR){R=L;}else if(moodLinkMode==MOOD_LINK_SYMMETRIC){R=L;revR=true;R.hue=(R.hue+180)%360;}else if(moodLinkMode==MOOD_LINK_FLOW){R=L;offR=128;}renderMoodSide(L,moodLeftStart,moodLeftCount,false,0);renderMoodSide(R,moodRightStart,moodRightCount,revR,offR);}

/* ===== PERSISTENCE (NVS) =============================================== */

#define CFG_VERSION 8

void setDefaultMapping(){
  segmentCount=4; uint16_t per=LED_COUNT/4; const uint8_t src[4]={SRC_L0,SRC_L1,SRC_R0,SRC_R1};
  for(uint8_t i=0;i<4;i++){segments[i].start=i*per;segments[i].count=per;segments[i].source=src[i];segments[i].brightness=255;segments[i].reverse=false;}
}
static void remapFx(uint8_t a,uint8_t b){leftMood.effect=(uint8_t)constrain(a,0,(int)MOOD_EFFECT_MAX);rightMood.effect=(uint8_t)constrain(b,0,(int)MOOD_EFFECT_MAX);}

void saveConfig(){
  prefs.begin("cfg",false); prefs.putUChar("cfg_ver",CFG_VERSION); prefs.putUChar("bright",globalBrightness); prefs.putUChar("smooth",smoothing); prefs.putUChar("black",blackThreshold);
  prefs.putString("tvip",tvIP.toString()); prefs.putString("webpw",webAuthPassword); prefs.putUChar("lm_eff",leftMood.effect);prefs.putUChar("rm_eff",rightMood.effect);prefs.putUShort("lm_hue",leftMood.hue);prefs.putUShort("rm_hue",rightMood.hue);
  prefs.putUChar("lm_sat",leftMood.saturation);prefs.putUChar("rm_sat",rightMood.saturation);prefs.putUChar("lm_val",leftMood.brightness);prefs.putUChar("rm_val",rightMood.brightness);prefs.putUChar("linkmode",(uint8_t)moodLinkMode);
  prefs.putBool("clone_en",sideCloneEnabled);prefs.putUChar("clone_br",sideCloneBrightness);prefs.putUShort("cl_st",cloneLeftStart);prefs.putUShort("cl_ct",cloneLeftCount);prefs.putUShort("cr_st",cloneRightStart);prefs.putUShort("cr_ct",cloneRightCount);prefs.putBool("cl_rev",cloneLeftRev);prefs.putBool("cr_rev",cloneRightRev);
  prefs.end(); saveMapper();
}

void loadConfig(){
  prefs.begin("cfg",true); bool fresh=!prefs.isKey("cfg_ver"); uint8_t stored=prefs.getUChar("cfg_ver",0);
  globalBrightness=prefs.getUChar("bright",DEFAULT_BRIGHTNESS); smoothing=prefs.getUChar("smooth",DEFAULT_SMOOTHING); blackThreshold=prefs.getUChar("black",DEFAULT_BLACK_THRESHOLD);
  String tvs=prefs.getString("tvip",""); IPAddress ip; if(tvs.length()&&ip.fromString(tvs))tvIP=ip;
  String pw=prefs.getString("webpw",""); if(pw.length())strncpy(webAuthPassword,pw.c_str(),sizeof(webAuthPassword)-1);
#ifdef SECRET_WEB_AUTH
  else if(strlen(SECRET_WEB_AUTH)>0)strncpy(webAuthPassword,SECRET_WEB_AUTH,sizeof(webAuthPassword)-1);
#endif
#ifdef SECRET_TV_IP
  if(tvIP==IPAddress(0,0,0,0))tvIP=IPAddress(SECRET_TV_IP);
#endif
  leftMood.effect=prefs.getUChar("lm_eff",0);rightMood.effect=prefs.getUChar("rm_eff",0);leftMood.hue=prefs.getUShort("lm_hue",0);rightMood.hue=prefs.getUShort("rm_hue",120);leftMood.saturation=prefs.getUChar("lm_sat",255);rightMood.saturation=prefs.getUChar("rm_sat",255);leftMood.brightness=prefs.getUChar("lm_val",200);rightMood.brightness=prefs.getUChar("rm_val",200);moodLinkMode=(MoodLinkMode)constrain(prefs.getUChar("linkmode",0),0,3);
  sideCloneEnabled=prefs.getBool("clone_en",true);sideCloneBrightness=prefs.getUChar("clone_br",255);cloneLeftStart=prefs.getUShort("cl_st",30);cloneLeftCount=prefs.getUShort("cl_ct",30);cloneRightStart=prefs.getUShort("cr_st",90);cloneRightCount=prefs.getUShort("cr_ct",30);cloneLeftRev=prefs.getBool("cl_rev",false);cloneRightRev=prefs.getBool("cr_rev",false);
  prefs.end();
  if(!mapperValid(segments[0])){}
  if(!fresh && stored>0 && stored<CFG_VERSION){ uint8_t le=leftMood.effect,re=rightMood.effect; remapFx(le,re); Serial.printf("[CFG] migrálva v%u -> v%u\n",stored,CFG_VERSION); }
  loadMapper(true); if(fresh){saveConfig();Serial.println("[CFG] friss telepítés — alapértelmezett konfig");}
  else { if(segmentCount==0){setDefaultMapping();saveMapper();} }
}

bool detectTVTopology(){
  if(!tvConnect())return false; int n=tvGet("/1/ambilight/topology",tvJsonBuf,sizeof(tvJsonBuf)); if(n<=0)return false; JsonDocument d; if(deserializeJson(d,tvJsonBuf))return false;
  JsonObject l=d["layer1"]; if(l.isNull())l=d.as<JsonObject>(); if(l.isNull())return false;
  auto countKeys=[&](JsonVariant v)->uint8_t{if(!v.is<JsonObject>())return 0;uint8_t c=0;for(JsonPair kv:v.as<JsonObject>())c++;return c;};
  uint8_t le=countKeys(l["left"]),ri=countKeys(l["right"]),to=countKeys(l["top"]),bo=countKeys(l["bottom"]);
  if(le)tvTopoLeft=le;if(ri)tvTopoRight=ri;if(to)tvTopoTop=to;if(bo)tvTopoBottom=bo;tvTopoLayers=1;tvTopoDetected=true;return true;
}

/* ===== SETUP / LOOP ===================================================== */

void setup(){
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== AMBILIGHT BRIDGE %s ===\n", FIRMWARE_VERSION);

  // LED init
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, LED_COUNT);
  FastLED.setBrightness(DEFAULT_BRIGHTNESS);
  FastLED.clear(true);

  loadConfig();
  connectWiFi();

  lastSuccessfulPoll=0;
  tvOnline=false;
  smartLastFrameMs=millis();
  Serial.println("[SETUP] kész");
}

void loop(){
  unsigned long now=millis();

  handleWiFi();

  if(webStarted){
    server.handleClient();
    wsServer.loop();
  }

  // újraindítás-kérések kezelése (WiFi mentés / reboot / OTA)
  if((restartPending||otaRestartPending) && now>=restartAt){
    const char* reason = restartReason[0] ? restartReason : (otaRestartPending ? "ota" : "unknown");
    // Clear BEFORE ESP.restart(): a future accidental re-entry cannot retrigger
    // the same request during this boot cycle.
    restartPending=false;
    otaRestartPending=false;
    Serial.printf("[SYS] újraindítás — reason=%s, age=%lums\n", reason, now-restartRequestedAt);
    delay(100);
    ESP.restart();
  }

  // TV master poll
  if(now-lastTVMasterPoll>=TV_MASTER_POLL_MS){
    lastTVMasterPoll=now;
    if(WiFi.isConnected() && tvIP!=IPAddress(0,0,0,0)) pollTVMaster();
  }

  // TV watchdog
  if(now-lastTVWatchdog>=TV_WATCHDOG_INTERVAL_MS){
    lastTVWatchdog=now;
    handleTVWatchdog();
  }

  // Ambilight keret lekérése + LED render
  if(!provisioningMode && WiFi.isConnected() && tvIP!=IPAddress(0,0,0,0) && !otaInProgress){
    if(now-lastFrameStart>=TV_FRAME_INTERVAL_MS){
      lastFrameStart=now;
      bool got=readAmbilight();
      if(!got) tvConsecutiveFailures++;
      // [E5] a keretszámlálót a broadcast ELŐTT növeljük, hogy a WS üzenet
      //      és a kliensklónok konzisztens seq-et lássanak
      smartFrameSeq++;
      renderZonesToLeds();
      showLeds();
      broadcastRealtime();
      smartLastFrameMs=now;
    }
  }

  // Mood keret (ha nincs aktív TV-jel)
  if(!provisioningMode && (!tvOnline || tvOutputOff) && !ledTestActive && !otaInProgress){
    if(now-lastMoodFrame>=MOOD_FRAME_INTERVAL_MS){
      lastMoodFrame=now;
      renderMood();
      FastLED.setBrightness(globalBrightness);
      FastLED.show();
    }
  }

  yield();
}