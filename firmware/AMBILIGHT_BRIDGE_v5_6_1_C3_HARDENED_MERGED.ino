/* ============================================================================
   AMBILIGHT BRIDGE v5.6.1-C3-HARDENED-PROPOSED
   ============================================================================
   v5.5.0 változások (biztonság + C3-optimalizálás + stabilitás):
     [SEC] Webes jelszó: nyílt szöveg helyett sózott SHA-256 hash (webslt/webhsh);
           konstans-idejű összehasonlítás; Basic-auth manuális ellenőrzése.
     [SEC] DNS-rebind védelem (hostAllowed) + CORS az Origin-t tükrözi (nem vak *).
     [SEC] Háromtállapotú auth: SETUP (friss, config nyitva/OTA tiltva),
           ENABLED (jelszó → minden védett), DISABLED (tudatos opt-out).
           Így az első konfiguráció nem akad el 403-on, de az OTA jelszó nélkül tiltott.
     [PERF] Mood-motor teljesen fixpontos/egész számos (sin8/beat8/beatsin8/scale8)
            — nincs float sin/fabs/fmod (ESP32-C3 RISC-V, nincs hardveres FPU).
     [FIX] tvOnline KIZÁRÓLAG eseményből (readAmbilight() sikeres keret) → nincs
           többé hamis "[TV] online" boot után; a watchdog csak OFFLINE-ra vált.
     [FIX] loadConfig(): a mapper a séma-migráció ELŐTT töltődik, hogy a
           migrációs saveConfig()->saveMapper() ne írja felül a tárolt mappinget.
     [FIX] loop(): TV- és mood-render kölcsönösen kizárólagos, egy show()/keret.
     [WDT] task watchdog a loop()-ra (core-verzió szerint guardolt).
     [NET] WiFi setAutoReconnect + periodikus, túlcsordulás-biztos backoff.
     [CFG] CONFIG_SCHEMA_VERSION 11 retained; v5.6.1 adds runtime/UI fixes only.

   ⚠ REGRESSION GUARD — eszközön kötelezően tesztelendő (itt nem fordítható/futtatható):
     R1) Auth Basic ellenőrzés: helyes/rossz jelszó, rate-limit (5x/60s), OTA tiltva jelszó nélkül.
     R2) Bootstrap: friss NVS → SETUP → saveConfig() lánc (config/tv/mapper/mood/sideclone)
         NEM kap 403-at; jelszó beállítás után mind auth-ot kér.
     R3) *** CONFIG + MAPPER MIGRÁCIÓ EGYÜTT ***: v8 NVS-ről indítás, egyedi LED-mapping
         beállítva → reboot v9 firmware-rel → a mapping MEGMARAD (nem íródik felül),
         cfg_ver=9 lesz. Friss telepítés → alap mapping.
     R4) tvOnline: boot után NINCS "[TV] online", amíg nincs sikeres readAmbilight().
     R5) Mood effektek (BREATHE/PULSE/COMET) vizuálisan helyesek fixpontos matekkal.
   ----------------------------------------------------------------------------
   Korábbi (v5.4.2) javitások:
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
     ESP32-C3 = bridge + local Control Center (zóna-olvasás, LED-vezérlés, REST+WS, OTA)
     Browser  = same-origin local application; GitHub Pages remains development/demo copy

   HARDWARE: WS2815 12V. Közös GND. 74AHCT125 szintill., 330-470R DATA soros,
             ~1000µF kondi a szalag tápbemeneténél.
   ============================================================================ */

#include <WiFi.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <esp_random.h>
#include <esp_system.h>
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
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
// A webes jelszót NEM tároljuk nyílt szövegben. Csak sózott SHA-256 hash + só
// kerül az NVS-be, illetve a memóriába.
//
// Három auth-állapot (a bootstrap/első-konfiguráció problemája miatt):
//   SETUP    : friss eszköz, még nincs jelszó és nincs tudatos lemondás róla
//              → a config-végpontok NYITVA (csak LAN, hostAllowed), hogy az első
//                beállítás egyáltalán elvégezhető legyen; OTA TILTVA.
//   ENABLED  : van jelszó (webAuthConfigured) → minden mutáló végpont + OTA auth-hoz kötött.
//   DISABLED : a felhasználó tudatosan lemondott a jelszóról (webAuthOptOut)
//              → config-végpontok NYITVA (LAN); OTA továbbra is TILTVA (nincs mihez auth-olni).
uint8_t webAuthSalt[16]     = {0};
uint8_t webAuthHash[32]     = {0};
bool    webAuthConfigured   = false;
bool    webAuthOptOut       = false;   // true = felhasználó tudatosan "nincs jelszó"

// Opcionális lokális override (untracked). Példa secrets.h:
//   #define SECRET_WIFI_SSID  "MySSID"
//   #define SECRET_WIFI_PASS  "MyPass"
//   #define SECRET_WEB_AUTH   "MyWebPass"
//   #define SECRET_TV_IP      192,168,1,100
#if __has_include("secrets.h")
  #include "secrets.h"
#endif

#define FIRMWARE_VERSION        "5.6.1-C3-HARDENED-MERGED"
#define CONFIG_SCHEMA_VERSION   11
#define FW_CONTRACT_VERSION     1

// Placeholder — web UI-ból felülbirálható. Nem valódi cím.
IPAddress DEFAULT_TV_IP(192, 168, 1, 100);

/* ===== HARDWARE CONSTANTS ================================================ */

#define LED_PIN         4
#define LED_TYPE        WS2815
#define COLOR_ORDER     GRB
#define LED_COUNT       120

#define DEFAULT_BRIGHTNESS      160
#define TV_FRAME_INTERVAL_MS    100
#define TV_SOCKET_TIMEOUT_MS    250
#define TV_BODY_READ_TIMEOUT_MS 350
#define DEFAULT_SMOOTHING       70
#define DEFAULT_BLACK_THRESHOLD 4

// Fixed physical LED topology: 4 sides × 3 segments × 10 LEDs = 120 LEDs.
// LED addressing is firmware-owned; the browser may change only source,
// brightness and direction.
#define MAPPER_SIDE_COUNT        4
#define MAPPER_SEGMENTS_PER_SIDE 3
#define MAPPER_LEDS_PER_SEGMENT  10
#define MAX_SEGMENTS              (MAPPER_SIDE_COUNT * MAPPER_SEGMENTS_PER_SIDE)

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

IPAddress          tvIP = DEFAULT_TV_IP;
ZoneRGB            targetZones[4], currentZones[4];
WiFiClient         tvClient;
bool               tvOnline, tvOutputOff=false;
unsigned long      goodFrames, badFrames;

LedSegment         segments[MAX_SEGMENTS];
uint8_t            segmentCount=12, globalBrightness=DEFAULT_BRIGHTNESS;
uint8_t            smoothing=DEFAULT_SMOOTHING, blackThreshold=DEFAULT_BLACK_THRESHOLD;

bool     dynBrightEnabled;      uint8_t dynBrightMin, dynBrightMax, dynBrightResp;
uint8_t  tvDynBright, moodDynBright;

MoodConfig   leftMood  = {1, MOOD_STATIC, 255, 200, 28, 0, 70, 65, 75, 55, 45, 2, true, false, 0};
MoodConfig   rightMood = {1, MOOD_STATIC, 255, 200, 28, 1, 70, 65, 75, 55, 45, 2, true, false, 120};
MoodLinkMode moodLinkMode;
uint16_t     moodLeftStart=30, moodLeftCount=30, moodRightStart=90, moodRightCount=30;
bool         moodDynEnabled;    uint8_t moodDynDepth=25;

bool     tvMasterSyncEnabled=true, tvMasterBrightnessEnabled=true;
uint8_t  tvMasterBrightness=255;  bool tvMasterBrightnessAvailable;

// [5.6.0] Ambilight forrás választás: 0 = measured (nyers, természetes zónaszínek),
//         1 = processed (a TV saját Ambilight-simításával). Alapértelmezés: measured,
//         hogy elkerüljük a TV + híd kétszeres simítását/túltelítését.
#define AMB_SRC_MEASURED   0
#define AMB_SRC_PROCESSED  1
uint8_t  ambilightSource = AMB_SRC_MEASURED;

// [5.6.1] TV státusz (Philips JointSPACE v1) — egy végpont/ciklus, ~1.25 s rotációval.
// A régi /1/system/power lekérdezés törölve (a NetTV 4.4.1 TV-n 404-et adott).
String   tvStatSource="", tvStatChannel="", tvStatMode="";
int      tvStatVolume=-1, tvStatVolMax=0;
bool     tvStatMuted=false;
uint8_t  tvStatusSlot=0;

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
#define TV_MASTER_POLL_MS           1250
#define TV_AMBILIGHT_READ_TIMEOUT_MS 800   // teljes ambilight-keret olvasási plafon
#define MOOD_FRAME_INTERVAL_MS      40
#define MOOD_WS_STATUS_INTERVAL_MS  1000
#define WDT_TIMEOUT_MS              8000   // task watchdog időtúlépés (loop beragadás ellen)

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

/* ===== CORS + AUTH ====================================================== */

/* ---- Sózott SHA-256 jelszó-kezelés (nincs nyílt szöveg sehol) ------------- */
static void hashPassword(const char* pw, const uint8_t* salt, uint8_t out[32]){
  mbedtls_sha256_context c; mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c,0);                 // 0 = SHA-256
  mbedtls_sha256_update(&c,salt,16);
  mbedtls_sha256_update(&c,(const uint8_t*)pw,strlen(pw));
  mbedtls_sha256_finish(&c,out);
  mbedtls_sha256_free(&c);
}
// Konstans-idejű összehasonlítás (timing-oldalcsatorna ellen).
static bool ctEqual(const uint8_t* a,const uint8_t* b,size_t n){ uint8_t d=0; for(size_t i=0;i<n;i++) d|=a[i]^b[i]; return d==0; }

// Új webes jelszó beállítása (üres → auth kikapcsolva). Új sót generál.
void setWebAuthPassword(const char* pw){
  if(pw==nullptr || pw[0]=='\0'){ webAuthConfigured=false; memset(webAuthHash,0,32); memset(webAuthSalt,0,16); return; }
  for(int i=0;i<16;i++) webAuthSalt[i]=(uint8_t)esp_random();
  hashPassword(pw,webAuthSalt,webAuthHash);
  webAuthConfigured=true;
}

// DNS-rebinding védelem: csak ismert Host fejléceket fogadunk el.
bool hostAllowed(){
  String h=server.hostHeader();
  int c=h.indexOf(':'); if(c>=0) h=h.substring(0,c);
  h.toLowerCase();
  if(h.length()==0) return true;                       // némely kliens nem küld Host-ot
  if(h=="ambilight.local"||h=="localhost"||h=="127.0.0.1") return true;
  if(WiFi.isConnected() && h==WiFi.localIP().toString()) return true;
  if(provisioningMode && h==WiFi.softAPIP().toString()) return true;
  return false;
}

void addCorsHeaders(){
  // Anti-rebinding a Host-fejléc ellenőrzése (hostAllowed). A CORS az Origin-t
  // tükrözi vissza (nem vak "*"), így hitelesített kérések is működnek, míg a
   // rebinding-et a Host-szűrés blokkolja.
  String origin = server.hasHeader("Origin") ? server.header("Origin") : "";
  if(origin.length()){
    server.sendHeader("Access-Control-Allow-Origin",origin);
    server.sendHeader("Access-Control-Allow-Credentials","true");
  } else {
    server.sendHeader("Access-Control-Allow-Origin","*");
  }
  server.sendHeader("Vary","Origin");
  server.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers","Authorization, Content-Type");
  server.sendHeader("Access-Control-Max-Age","600");
}

// Basic auth ellenőrzése a tárolt sózott hash ellen (nincs nyílt jelszó összehas.).
bool verifyBasicAuth(){
  if(!webAuthConfigured) return false;
  if(!server.hasHeader("Authorization")) return false;
  String hv=server.header("Authorization");
  if(!hv.startsWith("Basic ")) return false;
  String b64=hv.substring(6); b64.trim();
  uint8_t dec[160]; size_t dlen=0;
  if(mbedtls_base64_decode(dec,sizeof(dec)-1,&dlen,(const uint8_t*)b64.c_str(),b64.length())!=0) return false;
  dec[dlen]='\0';
  char* creds=(char*)dec;
  char* colon=strchr(creds,':');
  if(!colon){ memset(dec,0,sizeof(dec)); return false; }
  *colon='\0';
  const char* user=creds; const char* pass=colon+1;
  bool ok=false;
  if(strcmp(user,"admin")==0){ uint8_t cand[32]; hashPassword(pass,webAuthSalt,cand); ok=ctEqual(cand,webAuthHash,32); memset(cand,0,32); }
  memset(dec,0,sizeof(dec));
  return ok;
}

static unsigned long authFailCount,authLastFail;
// Mutáló végpontok őre: CORS + Host-szűrés + rate-limit + kötelező auth.
bool webAuthCheck(){
  addCorsHeaders();
  if(!hostAllowed()){ server.send(403,"application/json","{\"ok\":false,\"err\":\"bad host\"}"); return false; }
  unsigned long now=millis();
  if(authFailCount>=5&&(now-authLastFail)<60000){server.send(429,"text/plain","Too many auth attempts");return false;}
  if(authFailCount>=5&&(now-authLastFail)>=60000)authFailCount=0;
  if(!webAuthConfigured){
    // Bootstrap-kompromisszum: jelszó nélkül a config-végpontok NYITVA maradnak,
    // hogy az első beállítás (TV IP, mapper, mood stb.) egyáltalán elvégezhető
    // legyen. A támadási felületet a DNS-rebind szűrés (hostAllowed) korlátozza
    // a helyi hálózatra, a kritikus OTA pedig jelszó nélkül TILTOTT (lásd webAuthSilent).
    //   - SETUP (friss): itt engedve, a UI figyelmeztet a jelszó beállítására.
    //   - DISABLED (webAuthOptOut): a felhasználó tudatos választása.
    return true;
  }
  if(!verifyBasicAuth()){
    authFailCount++;authLastFail=now;
    server.requestAuthentication(BASIC_AUTH,"Ambilight Bridge");
    return false;
  }
  authFailCount=0; return true;
}
// Csendes auth-ellenőrzés (nem küld választ) — az OTA upload handlerhez.
// Jelszó nélkül az OTA TILTOTT (korábban nyílt volt — kritikus lyuk).
bool webAuthSilent(){
  if(!hostAllowed()) return false;
  if(!webAuthConfigured) return false;
  return verifyBasicAuth();
}
void handleCorsPreflight(){addCorsHeaders();server.send(204,"text/plain","");}


/* ===== EMBEDDED CONTROL CENTER PRO ==================================== */
#include "embedded_ui.h"


/* ===== ROOT — local Control Center + provisioning portal ================= */
void handleConfigPortal(){
  // Normal operation: serve the complete Control Center from the ESP32 itself.
  // Same-origin REST/WS removes GitHub Pages HTTPS -> ESP32 HTTP mixed-content blocking.
  if(!provisioningMode){
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html; charset=utf-8",AMBILIGHT_CC_HTML);
    return;
  }
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

// Mood-mode application heartbeat: low-rate WS status frame while no TV frame exists.
void broadcastMoodStatus(){
  JsonDocument doc;
  doc["seq"]=smartFrameSeq; doc["ts"]=millis(); doc["tv"]=tvOnline;
  doc["mood"]=true; doc["moodStatus"]=true;
  JsonArray z=doc["zones"].to<JsonArray>();
  for(int i=0;i<4;i++){JsonObject zz=z.add<JsonObject>(); zz["r"]=targetZones[i].r; zz["g"]=targetZones[i].g; zz["b"]=targetZones[i].b;}
  String out; serializeJson(doc,out); wsServer.broadcastTXT(out);
}

/* ===== REST HANDLERS ==================================================== */

// GET /api/state
void apiState(){
  if(!webAuthCheck())return;
  JsonDocument d;
  d["fw"]=FIRMWARE_VERSION; d["firmware"]=FIRMWARE_VERSION; d["contract"]=FW_CONTRACT_VERSION;
  d["tvOnline"]=tvOnline; d["tv_online"]=tvOnline; d["tvIP"]=tvIP.toString();
  d["wifi"]=WiFi.isConnected(); d["rssi"]=WiFi.isConnected()?WiFi.RSSI():-127;
  d["ip"]=WiFi.localIP().toString(); d["ap_active"]=provisioningMode;
  d["ap_ip"]=provisioningMode?WiFi.softAPIP().toString():"";
  d["brightness"]=globalBrightness; d["smoothing"]=smoothing; d["blackThreshold"]=blackThreshold;
  d["goodFrames"]=goodFrames; d["badFrames"]=badFrames; d["heap"]=ESP.getFreeHeap();
  d["uptime"]=millis()/1000; d["uptime_s"]=millis()/1000;
  d["smart_seq"]=smartFrameSeq; d["last_error"]=lastError;
  d["tv_signal"]=tvOnline?"active":"stale";
  d["tv_power"]="unknown";
  d["tv_power_reason"]="JointSPACE v1 /1/system/power is unavailable on this TV";
  d["tv_output_off"]=tvOutputOff;
  d["segmentCount"]=segmentCount; d["sideCloneEnabled"]=sideCloneEnabled; d["sideCloneBrightness"]=sideCloneBrightness;
  d["mapperSideBypass"]=mapperSideBypassActive();
  d["moodLinkMode"]=(uint8_t)moodLinkMode; d["tvTopoDetected"]=tvTopoDetected;
  d["dyn_on"]=dynBrightEnabled; d["dyn_min"]=dynBrightMin; d["dyn_max"]=dynBrightMax; d["dyn_resp"]=dynBrightResp;
  d["mood_dyn"]=moodDynEnabled; d["mood_dep"]=moodDynDepth;
  d["tv_sync"]=tvMasterSyncEnabled; d["tv_bsync"]=tvMasterBrightnessEnabled;
// [5.6.0] Ambilight forrás + élő TV státusz (source/channel/volume/mode)
d["amb_src"]=ambilightSource;
d["tv_source"]=tvStatSource; d["tv_channel"]=tvStatChannel; d["tv_mode"]=tvStatMode;
d["tv_volume"]=tvStatVolume; d["tv_vol_max"]=tvStatVolMax; d["tv_muted"]=tvStatMuted;
  auto moodJson=[&](JsonObject o,const MoodConfig& m){
    o["mode"]=m.mode;o["effect"]=m.effect;o["sat"]=m.saturation;o["bri"]=m.brightness;o["speed"]=m.speed;
    o["pal"]=m.palette;o["scale"]=m.scale;o["motion"]=m.motion;o["glow"]=m.glow;o["density"]=m.density;
    o["turb"]=m.turbulence;o["cm"]=m.colorMode;o["auto"]=m.autoColor;o["rev"]=m.reverse;o["hue"]=m.hue;
  };
  moodJson(d["left_mood"].to<JsonObject>(),leftMood); moodJson(d["right_mood"].to<JsonObject>(),rightMood);
  JsonArray zc=d["zone_current"].to<JsonArray>(); JsonArray zt=d["zone_target"].to<JsonArray>(); JsonArray zones=d["zones"].to<JsonArray>();
  for(int i=0;i<4;i++){ JsonArray c=zc.add<JsonArray>(); c.add(currentZones[i].r); c.add(currentZones[i].g); c.add(currentZones[i].b); JsonArray t=zt.add<JsonArray>(); t.add(targetZones[i].r); t.add(targetZones[i].g); t.add(targetZones[i].b); JsonObject z=zones.add<JsonObject>(); z["r"]=targetZones[i].r; z["g"]=targetZones[i].g; z["b"]=targetZones[i].b; }
  JsonArray segs=d["segments"].to<JsonArray>();
  for(uint8_t i=0;i<segmentCount && i<MAX_SEGMENTS;i++){ JsonObject o=segs.add<JsonObject>(); o["start"]=segments[i].start; o["count"]=segments[i].count; o["source"]=segments[i].source; o["brightness"]=segments[i].brightness; o["reverse"]=segments[i].reverse; }
  String out; serializeJson(d,out); server.send(200,"application/json",out);
}

// GET /api/realtime — backward-compatible REST snapshot for older browser clients
void apiRealtime(){
  if(!webAuthCheck())return;
  JsonDocument d;
  d["seq"]=smartFrameSeq; d["ts"]=millis(); d["tv"]=tvOnline;
  JsonArray z=d["zones"].to<JsonArray>();
  for(int i=0;i<4;i++){ JsonObject zz=z.add<JsonObject>(); zz["r"]=targetZones[i].r; zz["g"]=targetZones[i].g; zz["b"]=targetZones[i].b; }
  String out; serializeJson(d,out);
  server.send(200,"application/json",out);
}

// GET /api/capabilities — firmware/browser contract
void apiCapabilities(){
  if(!webAuthCheck())return; JsonDocument d;
  d["contract"]=FW_CONTRACT_VERSION; d["firmware"]=FIRMWARE_VERSION;
  JsonObject led=d["led"].to<JsonObject>(); led["count"]=LED_COUNT; led["pin"]=LED_PIN; led["type"]="WS2815";
  JsonObject mapper=d["mapper"].to<JsonObject>(); mapper["maxSegments"]=MAX_SEGMENTS; mapper["sourceCount"]=SOURCE_COUNT;
  mapper["fixed"]=true; mapper["sideCount"]=MAPPER_SIDE_COUNT; mapper["segmentsPerSide"]=MAPPER_SEGMENTS_PER_SIDE; mapper["ledsPerSegment"]=MAPPER_LEDS_PER_SEGMENT;
  JsonArray src=mapper["sources"].to<JsonArray>(); for(uint8_t i=0;i<SOURCE_COUNT;i++){ JsonObject x=src.add<JsonObject>(); x["id"]=i; x["name"]=SOURCE_NAMES[i]; }
  JsonObject mood=d["mood"].to<JsonObject>(); mood["effectCount"]=MOOD_FX_COUNT; mood["maxEffect"]=MOOD_EFFECT_MAX; mood["linkModes"]=4;
  JsonObject feat=d["features"].to<JsonObject>(); feat["mapper"]=true; feat["mapperPersistence"]=true; feat["gradient"]=true; feat["sideClone"]=true; feat["topology"]=true; feat["moodLink"]=true; feat["ota"]=true; feat["websocket"]=true; feat["apProvisioning"]=true;
  feat["ambilightSource"]=true; feat["tvStatus"]=true; feat["tvPowerState"]=false; feat["tvMeasuredProcessed"]=true;
  d["tv"]=JsonObject{}; d["tv"]["powerState"]="unknown"; d["tv"]["powerStateSupported"]=false;
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

// TV Master Sync + Side Clone is a dedicated physical-layout mode.
// In this mode the mapper remains authoritative only for BOTTOM 0..29
// and TOP 60..89. LEFT 30..59 and RIGHT 90..119 are supplied by the
// side-clone engine and therefore ignore mapper source/brightness/reverse.
static bool mapperSideBypassActive(){
  return tvMasterSyncEnabled && sideCloneEnabled;
}

static bool mapperLedIsClonedSide(uint16_t idx){
  return mapperSideBypassActive() && ((idx>=30 && idx<60) || (idx>=90 && idx<120));
}

static uint16_t fixedSegmentStart(uint8_t index){
  return (uint16_t)index * MAPPER_LEDS_PER_SEGMENT;
}

static bool fixedMapperSegment(uint8_t index,const LedSegment& sg){
  return index < MAX_SEGMENTS &&
         sg.start == fixedSegmentStart(index) &&
         sg.count == MAPPER_LEDS_PER_SEGMENT &&
         sg.source < SOURCE_COUNT;
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
/* ===== FORWARD DECLARATIONS =========================================== */
void setDefaultMapping();
void saveConfig(bool includeMapper=false);
bool detectTVTopology();

void loadMapper(bool defaultsIfMissing){
  prefs.begin("cfg",true); bool has=prefs.isKey("segcnt"); uint8_t n=prefs.getUChar("segcnt",0);
  if(has && n==MAX_SEGMENTS){ segmentCount=MAX_SEGMENTS; bool all=true; for(uint8_t i=0;i<MAX_SEGMENTS;i++){ char k[8];
      snprintf(k,sizeof(k),"s%ust",i); segments[i].start=prefs.getUShort(k,0);
      snprintf(k,sizeof(k),"s%uct",i); segments[i].count=prefs.getUShort(k,0);
      snprintf(k,sizeof(k),"s%usc",i); segments[i].source=prefs.getUChar(k,SRC_BLACK);
      snprintf(k,sizeof(k),"s%ub",i); segments[i].brightness=prefs.getUChar(k,255);
      snprintf(k,sizeof(k),"s%ur",i); segments[i].reverse=prefs.getBool(k,false);
      if(!fixedMapperSegment(i,segments[i])) all=false;
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
    JsonArray a=d["segments"].as<JsonArray>(); if(a.size()!=MAX_SEGMENTS){server.send(400,"application/json","{\"ok\":false,\"err\":\"exactly 12 fixed segments required\"}");return;}
    LedSegment tmp[MAX_SEGMENTS]; bool ok=true; uint8_t i=0; for(JsonVariant v:a){
      tmp[i].start=fixedSegmentStart(i);
      tmp[i].count=MAPPER_LEDS_PER_SEGMENT;
      tmp[i].source=v["source"]|SRC_BLACK;
      tmp[i].brightness=v["brightness"]|255;
      tmp[i].reverse=v["reverse"]|false;
      if(!fixedMapperSegment(i,tmp[i]))ok=false;
      i++;
    }
    if(!ok){server.send(400,"application/json","{\"ok\":false,\"err\":\"invalid fixed mapper segment\"}");return;}
    segmentCount=MAX_SEGMENTS; for(i=0;i<segmentCount;i++)segments[i]=tmp[i]; saveMapper(); server.send(200,"application/json","{\"ok\":true}"); return;
  }
  server.send(400,"application/json","{\"ok\":false,\"err\":\"expected JSON segments\"}");
}

// POST /api/config  (általános beállítások)
void apiConfig(){
  if(!webAuthCheck())return;
  if(server.hasArg("brightness")) globalBrightness=clampU8(server.arg("brightness").toInt());
  if(server.hasArg("smoothing"))  smoothing=clampU8(server.arg("smoothing").toInt());
  if(server.hasArg("blackThreshold")) blackThreshold=clampU8(server.arg("blackThreshold").toInt());
  if(server.hasArg("dyn_on")) dynBrightEnabled=server.arg("dyn_on").toInt()!=0;
  if(server.hasArg("dyn_min")) dynBrightMin=clampU8(server.arg("dyn_min").toInt());
  if(server.hasArg("dyn_max")) dynBrightMax=clampU8(server.arg("dyn_max").toInt());
  if(server.hasArg("dyn_resp")) dynBrightResp=clampU8(server.arg("dyn_resp").toInt());
  if(server.hasArg("mood_dyn")) moodDynEnabled=server.arg("mood_dyn").toInt()!=0;
  if(server.hasArg("mood_dep")) moodDynDepth=clampU8(server.arg("mood_dep").toInt());
  if(server.hasArg("tv_sync")) tvMasterSyncEnabled=server.arg("tv_sync").toInt()!=0;
  if(server.hasArg("tv_bsync")) tvMasterBrightnessEnabled=server.arg("tv_bsync").toInt()!=0;
  if(server.hasArg("amb_src")) ambilightSource=(server.arg("amb_src").toInt()!=0)?AMB_SRC_PROCESSED:AMB_SRC_MEASURED;
  if(dynBrightMin>dynBrightMax){uint8_t t=dynBrightMin;dynBrightMin=dynBrightMax;dynBrightMax=t;}
  saveConfig(false);
  server.send(200,"application/json","{\"ok\":true}");
}

// POST /api/tv  (TV IP beállítása)
void apiTV(){
  if(!webAuthCheck())return;
  if(server.hasArg("ip")){
    IPAddress ip;
    if(ip.fromString(server.arg("ip"))){ tvIP=ip; tvConsecutiveFailures=0; tvOnline=false;
      if(tvClient.connected())tvClient.stop();
      saveConfig(false);
      server.send(200,"application/json","{\"ok\":true}"); return; }
  }
  server.send(400,"application/json","{\"ok\":false,\"err\":\"invalid ip\"}");
}

// GET/POST /api/auth  (webes jelszó beállítása / állapot)
void apiAuth(){
  if(server.method()==HTTP_GET){
    addCorsHeaders();
    if(!hostAllowed()){ server.send(403,"application/json","{\"ok\":false,\"err\":\"bad host\"}"); return; }
    // enabled = van jelszó; setup = friss (se jelszó, se tudatos lemondás);
    // optOut = a felhasználó tudatosan lemondott a jelszóról.
    bool setup = (!webAuthConfigured && !webAuthOptOut);
    char buf[80];
    snprintf(buf,sizeof(buf),"{\"enabled\":%s,\"setup\":%s,\"optOut\":%s}",
             webAuthConfigured?"true":"false", setup?"true":"false", webAuthOptOut?"true":"false");
    server.send(200,"application/json",buf);
    return;
  }
  addCorsHeaders();
  if(!hostAllowed()){ server.send(403,"application/json","{\"ok\":false,\"err\":\"bad host\"}"); return; }
  // Ha már van jelszó, a módosításhoz (újraírás VAGY kikapcsolás) a jelenlegit
  // igazolni kell (ne lehessen feltörés nélkül felülírni/kikapcsolni).
  if(webAuthConfigured && !verifyBasicAuth()){
    server.requestAuthentication(BASIC_AUTH,"Ambilight Bridge");
    return;
  }
  if(server.hasArg("password")){
    String p=server.arg("password");
    if(p.length()<64){
      if(p.length()==0){
        // Üres jelszó = tudatos lemondás az auth-ról (DISABLED állapot).
        setWebAuthPassword("");
        webAuthOptOut=true;
      } else {
        setWebAuthPassword(p.c_str());
        webAuthOptOut=false;
      }
      saveConfig(false);
      char buf[64];
      snprintf(buf,sizeof(buf),"{\"ok\":true,\"enabled\":%s}",webAuthConfigured?"true":"false");
      server.send(200,"application/json",buf);
      return;
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
  saveConfig(false); server.send(200,"application/json","{\"ok\":true}");
}

// GET /api/topology
void apiTopology(){ if(!webAuthCheck())return; JsonDocument d; d["detected"]=tvTopoDetected; d["left"]=tvTopoLeft; d["top"]=tvTopoTop; d["right"]=tvTopoRight; d["bottom"]=tvTopoBottom; d["layers"]=tvTopoLayers; String o;serializeJson(d,o);server.send(200,"application/json",o);}

/* ===== MOOD REST ======================================================== */

void apiMood(){
  if(!webAuthCheck())return;
  auto readSide=[&](const String& p,MoodConfig& m,const char* legacyEffect,const char* legacyHue,const char* legacySat,const char* legacyVal){
    if(server.hasArg(p+"Mode")) m.mode=server.arg(p+"Mode").toInt()!=0;
    if(server.hasArg(p+"Effect")) m.effect=(uint8_t)constrain(server.arg(p+"Effect").toInt(),0,(int)MOOD_EFFECT_MAX);
    else if(server.hasArg(legacyEffect)) m.effect=(uint8_t)constrain(server.arg(legacyEffect).toInt(),0,(int)MOOD_EFFECT_MAX);
    if(server.hasArg(p+"Hue")) m.hue=(uint16_t)constrain(server.arg(p+"Hue").toInt(),0,359);
    else if(server.hasArg(legacyHue)) m.hue=(uint16_t)constrain(server.arg(legacyHue).toInt(),0,359);
    if(server.hasArg(p+"Sat")) m.saturation=clampU8(server.arg(p+"Sat").toInt());
    else if(server.hasArg(legacySat)) m.saturation=clampU8(server.arg(legacySat).toInt());
    if(server.hasArg(p+"Val")) m.brightness=clampU8(server.arg(p+"Val").toInt());
    else if(server.hasArg(legacyVal)) m.brightness=clampU8(server.arg(legacyVal).toInt());
    if(server.hasArg(p+"Speed")) m.speed=clampU8(server.arg(p+"Speed").toInt());
    if(server.hasArg(p+"Palette")) m.palette=clampU8(server.arg(p+"Palette").toInt());
    if(server.hasArg(p+"Scale")) m.scale=clampU8(server.arg(p+"Scale").toInt());
    if(server.hasArg(p+"Motion")) m.motion=clampU8(server.arg(p+"Motion").toInt());
    if(server.hasArg(p+"Glow")) m.glow=clampU8(server.arg(p+"Glow").toInt());
    if(server.hasArg(p+"Density")) m.density=clampU8(server.arg(p+"Density").toInt());
    if(server.hasArg(p+"Turbulence")) m.turbulence=clampU8(server.arg(p+"Turbulence").toInt());
    if(server.hasArg(p+"ColorMode")) m.colorMode=clampU8(server.arg(p+"ColorMode").toInt());
    if(server.hasArg(p+"Auto")) m.autoColor=server.arg(p+"Auto").toInt()!=0;
    if(server.hasArg(p+"Reverse")) m.reverse=server.arg(p+"Reverse").toInt()!=0;
  };
  readSide("left",leftMood,"leftMode","leftHue","leftSat","leftVal");
  readSide("right",rightMood,"rightMode","rightHue","rightSat","rightVal");
  if(server.hasArg("linkMode")) moodLinkMode=(MoodLinkMode)constrain(server.arg("linkMode").toInt(),0,3);
  saveConfig(false);
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
  server.on("/setup",          HTTP_GET,  [](){
    // Explicit setup/recovery portal. Temporarily expose the provisioning UI
    // even in STA mode; /api/wifi still requires auth outside provisioning.
    bool savedProvisioning=provisioningMode;
    provisioningMode=true;
    handleConfigPortal();
    provisioningMode=savedProvisioning;
  });
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
  server.on("/api/auth",     HTTP_GET,  apiAuth);
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
  // A manuális Basic-auth ellenőrzéshez és a CORS Origin-tükrözéshez
  // össze kell gyűjteni ezeket a fejléceket.
  static const char* COLLECT_HEADERS[]={"Authorization","Origin"};
  server.collectHeaders(COLLECT_HEADERS,2);
  server.begin();
  wsServer.begin();
  wsServer.enableHeartbeat(15000,3000,2);
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
  WiFi.setAutoReconnect(true);            // az stack is próbálkozzon önmagától
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(),pass.c_str());
  WiFi.setSleep(false);              // Realtime WS: modem-sleep kikapcsolva
  wifiConnectInProgress=true; wifiAttemptStarted=millis();
    FastLED.setBrightness(effectiveMoodBrightness());

uint8_t moodSceneLuminance(){
  uint32_t sum=0; uint16_t count=0;
  const uint16_t starts[2]={moodLeftStart,moodRightStart};
  const uint16_t counts[2]={moodLeftCount,moodRightCount};
  for(uint8_t s=0;s<2;s++){
    for(uint16_t i=0;i<counts[s];i++){
      uint16_t idx=starts[s]+i; if(idx>=LED_COUNT)continue;
      const CRGB &c=leds[idx]; sum+=max(max(c.r,c.g),c.b); count++;
    }
  }
  return count?(uint8_t)(sum/count):0;
}

uint8_t effectiveMoodBrightness(){
  int br=globalBrightness;
  if(dynBrightEnabled){
    uint8_t lum=moodSceneLuminance();
    int dyn=dynBrightMin+((int)(dynBrightMax-dynBrightMin)*lum)/255;
    br=(br*(255-dynBrightResp)+dyn*dynBrightResp)/255;
  }
  if(moodDynEnabled){
    uint8_t lum=moodSceneLuminance();
    int factor=255-(int)moodDynDepth+((int)moodDynDepth*lum)/255;
    br=(br*factor)/255;
  }
  return (uint8_t)constrain(br,0,255);
}
  tvStatusSlot=(uint8_t)((tvStatusSlot+1u)&3u);
  tvMasterBrightnessAvailable=false;
  int n=-1; JsonDocument d;
  switch(tvStatusSlot){
    case 0: {
      n=tvGet("/1/sources/current",tvJsonBuf,sizeof(tvJsonBuf));
      if(n>0 && !deserializeJson(d,tvJsonBuf)){ const char* id=d["id"]|""; if(id[0]) tvStatSource=id; }
      break;
    }
    case 1: {
      n=tvGet("/1/channels/current",tvJsonBuf,sizeof(tvJsonBuf));
      if(n>0 && !deserializeJson(d,tvJsonBuf)){
        if(d["id"].is<const char*>()) tvStatChannel=(const char*)d["id"];
        else if(d["id"].is<int>()) tvStatChannel=String(d["id"].as<int>());
      }
      break;
    }
    case 2: {
      n=tvGet("/1/audio/volume",tvJsonBuf,sizeof(tvJsonBuf));
      if(n>0 && !deserializeJson(d,tvJsonBuf)){
        if(d["current"].is<int>()) tvStatVolume=d["current"].as<int>();
        if(d["max"].is<int>()) tvStatVolMax=d["max"].as<int>();
        tvStatMuted=d["muted"]|false;
      }
      break;
    }
    default: {
      n=tvGet("/1/ambilight/mode",tvJsonBuf,sizeof(tvJsonBuf));
      if(n>0 && !deserializeJson(d,tvJsonBuf)){ const char* m=d["current"]|""; if(m[0]) tvStatMode=m; }
      break;
    }
// [5.6.1] TV státusz lekérdezés: egyetlen JointSPACE v1 végpont/ciklus.
// A TV-n a keep-alive nem stabil, ezért a meglévő connect/close marad;
// a négy státusz-végpont rotációban fut (~1.25 s-onként egy kérés).