/* ============================================================================
   AMBILIGHT BRIDGE v5.6.3-C3-CHAOSWAVE-MOOD
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
     [CFG] CONFIG_SCHEMA_VERSION 11 retained; v5.6.2 adds the complete 23-effect Mood engine and wires all Mood controls.
     [MOOD 5.6.3] Turbulence now modulates ALL noise-based effects (FIRE, FIRE2,
           ORGANIC, LAVA, STARFIELD, TWINKLE) via moodTurb8() spatial-frequency
           chaos, in addition to NEBULA/PLASMA_X. No NVS/schema/API change.

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

#define FIRMWARE_VERSION        "5.6.3-C3-CHAOSWAVE-MOOD"
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
  static unsigned long lastReconnectAttempt=0;
  static uint32_t reconnectBackoffMs=5000;   // 5s -> ... -> 60s cap
  wl_status_t s=WiFi.status();

  if(wifiConnectInProgress){
    if(s==WL_CONNECTED){
      onWiFiConnected();
      reconnectBackoffMs=5000;
    }else if((long)(millis()-staConnectDeadline) >= 0){   // overflow-biztos
      Serial.println("[WiFi] időtúllépés → provisioning AP");
      wifiConnectInProgress=false;
      startProvisioningAP();
    }
  }else if(!provisioningMode){
    if(s==WL_CONNECTED){
      reconnectBackoffMs=5000;             // egészséges kapcsolat: backoff reset
    }else{
      // Nem csak az átmenetkor, hanem periodikusan is próbálunk újracsatlakozni,
      // növekvő backoff-fal. Így egy sikertelen reconnect nem "ragad be" reboot-ig.
      unsigned long now=millis();
      if(last==WL_CONNECTED){
        Serial.println("[WiFi] kapcsolat elveszett — újracsatlakozás");
        WiFi.reconnect();
        lastReconnectAttempt=now;
      }else if(now-lastReconnectAttempt>=reconnectBackoffMs){
        Serial.printf("[WiFi] újracsatlakozási kísérlet (backoff=%lums)\n",(unsigned long)reconnectBackoffMs);
        WiFi.reconnect();
        lastReconnectAttempt=now;
        reconnectBackoffMs = (reconnectBackoffMs>=60000)?60000:(reconnectBackoffMs*2);
      }
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
  tvClient.setTimeout(TV_SOCKET_TIMEOUT_MS);   // [E2] konstansból, nem magic number
  tvSocketAlive=true;
  return true;
}

// Rövid HTTP GET a JointSPACE-hez; a válasz JSON-ját buf-ba olvassuk.
// Puffer-alapú fejléc-parse (nincs String — nincs O(n²)/heap-fragmentáció).
int tvGet(const char* path, char* buf, size_t bufLen){
  if(!tvConnect())return -1;
  tvClient.printf("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  path, tvIP.toString().c_str());
  unsigned long t0=millis(); size_t n=0; bool headersDone=false; int contentLen=-1;
  char hdr[512]; size_t hlen=0; int mi=0; static const char MARK[]="\r\n\r\n";
  while(millis()-t0<TV_BODY_READ_TIMEOUT_MS){
    while(tvClient.available()){
      char c=tvClient.read();
      if(!headersDone){
        if(hlen<sizeof(hdr)-1) hdr[hlen++]=c;
        // \r\n\r\n határ detektálása állapotgéppel
        if(c==MARK[mi]) mi++; else mi=(c==MARK[0])?1:0;
        if(mi==4){
          headersDone=true; hdr[hlen]='\0';
          char* ci=strstr(hdr,"Content-Length:");
          if(!ci) ci=strstr(hdr,"content-length:");
          if(ci) contentLen=atoi(ci+15);
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
  // [5.6.0] a forrás (measured/processed) konfigurálható; a JSON-struktúra
  //         (layer1.left/right.0/1) mindkét végponton azonos → a parse változatlan.
  const char* ambPath = (ambilightSource==AMB_SRC_PROCESSED)
                          ? "/1/ambilight/processed"
                          : "/1/ambilight/measured";
  tvClient.printf("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  ambPath, tvIP.toString().c_str());
  unsigned long t0=millis(); size_t n=0; bool headersDone=false;
  int depth=0; bool bodyStarted=false, inString=false, escape=false;
  int mi=0; static const char MARK[]="\r\n\r\n";   // fejléc-határ detektor (nincs String)
  // [M4] a törőjel-számlálóval korrekt módon észleljük a JSON végét → korai kilépés
  while(millis()-t0<TV_AMBILIGHT_READ_TIMEOUT_MS){
    while(tvClient.available()){
      char c=tvClient.read();
      if(!headersDone){
        if(c==MARK[mi]) mi++; else mi=(c==MARK[0])?1:0;
        if(mi==4) headersDone=true;
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
  // A sikeres valódi Ambilight-keret bizonyítja, hogy a TV aktívan szolgáltat
  // képadatot, ezért az esetleg korábban beragadt power-state tiltást feloldjuk.
  tvOutputOff=false;
  // [FIX] Az ONLINE állapot KIZÁRÓLAG tényleges sikeres keretből származik
  //       (esemény-vezérelt), nem időzítőből → nincs többé hamis "[TV] online"
  //       közvetlenül boot után (a korábbi lastSuccessfulPoll=0 sentinel-bug).
  if(!tvOnline){ tvOnline=true; Serial.println("[TV] online"); }
  return true;
}

// [5.6.1] TV státusz lekérdezés: egyetlen JointSPACE v1 végpont/ciklus.
// A TV-n a keep-alive nem stabil, ezért a meglévő connect/close marad;
// a négy státusz-végpont rotációban fut (~1.25 s-onként egy kérés).
void pollTVStatus(){
  if(!tvOnline){ tvMasterBrightnessAvailable=false; return; }
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
  }
  tvStatusSlot=(uint8_t)((tvStatusSlot+1u)&3u);
  tvMasterBrightnessAvailable=false;
}

void handleTVWatchdog(){
  unsigned long now=millis();
  // [FIX] A watchdog CSAK OFFLINE-ra vált — az ONLINE mindig eseményből jön
  //       (readAmbilight() sikeres keret). Így megszűnik a boot utáni hamis
  //       "[TV] online" → "[TV] offline" páros, amit a lastSuccessfulPoll=0
  //       kezdőérték okozott (now-0 < TV_STALE_TIMEOUT_MS igaz volt induláskor).
  if(tvOnline){
    bool tooManyFails = (tvConsecutiveFailures>=TV_FAILURES_BEFORE_OFFLINE);
    // stale csak akkor értelmezhető, ha már VOLT sikeres poll (lastSuccessfulPoll!=0)
    bool stale = (lastSuccessfulPoll!=0 && (now-lastSuccessfulPoll)>=TV_STALE_TIMEOUT_MS);
    if(tooManyFails || stale){ tvOnline=false; tvOutputOff=true; Serial.println("[TV] offline"); }
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
  // [FIX] blackThreshold tényleges alkalmazása: küszöb alatti zónák feketére
  if(blackThreshold>0){
    for(int i=0;i<4;i++){
      uint8_t mx=max(max(targetZones[i].r,targetZones[i].g),targetZones[i].b);
      if(mx<blackThreshold) targetZones[i]=ZoneRGB{0,0,0};
    }
  }
  uint8_t sm=smoothing; for(int i=0;i<4;i++){int k=255-sm;currentZones[i].r+=(targetZones[i].r-currentZones[i].r)*k/255;currentZones[i].g+=(targetZones[i].g-currentZones[i].g)*k/255;currentZones[i].b+=(targetZones[i].b-currentZones[i].b)*k/255;}
  fill_solid(leds,LED_COUNT,CRGB::Black); bool written[LED_COUNT]={false};
  // Mapper owns the physical LED output unless TV Master Sync + Side Clone
  // is active. In that combined mode only BOTTOM and TOP remain mapper-driven.
  for(uint8_t s=0;s<segmentCount && s<MAX_SEGMENTS;s++){
    LedSegment &sg=segments[s];
    if(!mapperValid(sg)) continue;
    for(uint16_t i=0;i<sg.count;i++){
      uint16_t idx=sg.reverse?(sg.start+sg.count-1-i):(sg.start+i);
      if(mapperLedIsClonedSide(idx)) continue;
      float t=sg.count>1?(float)i/(float)(sg.count-1):0.0f;
      CRGB col=(sg.source>=SRC_GRADIENT_TOP&&sg.source<=SRC_GRADIENT_LEFT)?gradientSource(sg.source,t):zoneFromSource(sg.source);
      col.nscale8(sg.brightness); leds[idx]=col; written[idx]=true;
    }
  }

  if(sideCloneEnabled){
    // With the combined TV Master Sync + Side Clone mode the clone targets
    // are fixed to LEFT 30..59 and RIGHT 90..119. This deliberately ignores
    // mapper settings and user-defined clone ranges for these physical sides.
    const uint16_t leftStart  = mapperSideBypassActive() ? 30 : cloneLeftStart;
    const uint16_t leftCount  = mapperSideBypassActive() ? 30 : cloneLeftCount;
    const uint16_t rightStart = mapperSideBypassActive() ? 90 : cloneRightStart;
    const uint16_t rightCount = mapperSideBypassActive() ? 30 : cloneRightCount;

    CRGB l=zoneFromSource(SRC_L_AVG); CRGB r=zoneFromSource(SRC_R_AVG);
    l.nscale8(sideCloneBrightness); r.nscale8(sideCloneBrightness);

    for(uint16_t i=0;i<leftCount;i++){
      uint16_t idx=cloneLeftRev?(leftStart+leftCount-1-i):(leftStart+i);
      if(idx<LED_COUNT&&!written[idx])leds[idx]=l;
    }
    for(uint16_t i=0;i<rightCount;i++){
      uint16_t idx=cloneRightRev?(rightStart+rightCount-1-i):(rightStart+i);
      if(idx<LED_COUNT&&!written[idx])leds[idx]=r;
    }
  }
}

// [FIX] Effektív fBényerő: globális × (opcionális) TV-master × (opcionális) dinamikus.
// Korábban a dyn_* és a tv master brightness holt beállítások voltak — most élnek.
uint8_t effectiveBrightness(){
  int br=globalBrightness;
  if(tvMasterBrightnessEnabled && tvMasterBrightnessAvailable){
    br=(br*(int)tvMasterBrightness)/255;
  }
  if(dynBrightEnabled){
    uint8_t lum=sceneLuminance();                       // 0..255 jelenet-fényesség
    int dyn=dynBrightMin + ((int)(dynBrightMax-dynBrightMin)*lum)/255;
    // dynBrightResp: 0 = statikus (globális), 255 = teljesen jelenetkövető
    br=(br*(255-dynBrightResp) + dyn*dynBrightResp)/255;
  }
  return (uint8_t)constrain(br,0,255);
}

void showLeds(){
  if(ledTestActive){
    if(millis()<ledTestUntil){ fill_solid(leds,LED_COUNT,ledTestColor); FastLED.setBrightness(255); FastLED.show(); return; }
    ledTestActive=false;
  }
  if(tvOutputOff){ FastLED.clear(); FastLED.show(); return; }
  FastLED.setBrightness(effectiveBrightness());
  FastLED.show();
}

/* ===== MOOD ENGINE ====================================================== */

// [MOOD 5.6.2] Unified 23-effect engine. All animation paths use the same
// speed clock; palette/color-mode/reverse/scale/glow/turbulence are consumed
// here so the UI parameters are functional rather than persistence-only.
static const uint8_t MOOD_PALETTE_HUE8[24] = {
  0,4,16,24,32,42,64,85,96,110,120,128,140,160,176,192,208,220,232,240,248,252,255,0
};
static const uint8_t MOOD_PALETTE_SAT[24] = {
  255,255,255,255,245,235,230,220,215,210,205,200,190,220,245,235,220,225,230,220,210,245,255,0
};
static inline uint8_t hue360To8(uint16_t h){ return (uint8_t)(((uint32_t)h*256u)/360u); }
static inline uint8_t moodBpm(uint8_t speed){ return (uint8_t)(3u + scale8(speed,77)); } // 3..80 BPM
static inline uint8_t moodPhase(uint8_t speed,uint8_t sideOffset){ return beat8(moodBpm(speed),sideOffset); }
static inline uint8_t moodPos(uint16_t i,uint16_t count,uint8_t scale){
  uint8_t p=(count>1)?(uint8_t)(((uint32_t)i*255u)/(count-1)):0;
  uint8_t sc=(uint8_t)max<uint16_t>(1,scale);
  return scale8(p,sc);
}
static CRGB moodTVColor(bool right){
  ZoneRGB a=right?currentZones[2]:currentZones[0];
  ZoneRGB b=right?currentZones[3]:currentZones[1];
  CRGB c((uint8_t)(((uint16_t)a.r+b.r)/2),(uint8_t)(((uint16_t)a.g+b.g)/2),(uint8_t)(((uint16_t)a.b+b.b)/2));
  if(c.r==0 && c.g==0 && c.b==0) return CRGB(255,255,255);
  return c;
}
static inline void moodBase(const MoodConfig& m,uint16_t i,uint16_t count,bool right,uint8_t phase,uint8_t& h,uint8_t& sat){
  uint8_t p=(count>1)?(uint8_t)(((uint32_t)i*255u)/(count-1)):0;
  uint8_t cm=m.autoColor?2:m.colorMode;
  if(cm==2){
    CHSV tv=rgb2hsv_approximate(moodTVColor(right)); h=tv.h; sat=tv.s;
  }else if(cm==0){
    uint8_t pi=min<uint8_t>(m.palette,23); h=MOOD_PALETTE_HUE8[pi]; sat=MOOD_PALETTE_SAT[pi];
  }else{
    h=hue360To8(m.hue); sat=m.saturation;
  }
  if(cm==3) h += p;
  else if(cm!=2 && m.autoColor==false) h += 0;
  h += phase;
}
static inline uint8_t moodGlow(uint8_t v,const MoodConfig& m){
  uint8_t floor=scale8(m.brightness,scale8(m.glow,96));
  return qadd8(floor,scale8(v,(uint8_t)(255u-scale8(m.glow,80))));
}
static inline uint8_t moodNoise(uint16_t x,uint16_t y,uint8_t turb){
  uint8_t n=inoise8(x,y); return lerp8by8(128,n,turb);
}
// [5.6.3] Turbulencia mint térbeli frekvencia (káosz): 0 = sima/nagy foltok,
// 255 = sűrű/kaotikus mintázat. Az amplitúdó változatlan marad, ezért a
// küszöb-alapú effektek (TWINKLE/STARFIELD) sűrűsége is helyes marad, a
// folyam-effektek (FIRE/FIRE2/ORGANIC/LAVA) pedig részletesebbé/kaotikusabbá válnak.
static inline uint8_t moodTurb8(uint16_t coord,uint16_t t,uint8_t turb){
  uint16_t f=48u+(uint16_t)turb;                       // ~0.75x .. 4.7x térbeli frekvencia
  uint16_t x=(uint16_t)(((uint32_t)coord*f)>>6);
  return inoise8(x,t);
}
static CRGB moodColor(const MoodConfig& m,uint16_t i,uint16_t count,bool right,uint8_t sideOffset=0){
  const uint8_t phase=moodPhase(m.speed,sideOffset);
  const uint8_t p=moodPos(i,count,m.scale);
  const uint16_t motion=(uint16_t)m.motion*2u;
  const uint16_t x=(uint16_t)p*4u + phase + motion;
  const uint8_t noise=moodNoise(x,(uint16_t)phase*3u + i*17u,m.turbulence);
  uint8_t h=0,sat=m.saturation,v=m.brightness;
  moodBase(m,i,count,right,0,h,sat); // effect owns temporal hue shift
  uint8_t wave=sin8((uint8_t)(p + phase));
  uint8_t wave2=sin8((uint8_t)(p*2u + phase));
  switch(m.effect){
    case MOOD_STATIC: break;
    case MOOD_BREATHE: v=scale8(m.brightness, (uint8_t)(128u + scale8(sin8(phase),127))); break;
    case MOOD_RAINBOW: h += p + phase; sat=255; break;
    case MOOD_SLOW_COLOR: h += (uint8_t)(phase>>3); break;
    case MOOD_WARM: h=12; sat=235; v=qadd8(scale8(m.brightness,150),scale8(wave,scale8(m.glow,105))); break;
    case MOOD_COLOR_WAVE: h += scale8(p,96) + wave/3; break;
    case MOOD_COMET: {
      uint8_t head=phase, d=(head>=p)?head-p:p-head;
      uint8_t tail=(uint8_t)max<uint16_t>(12,24u+scale8(m.scale,80));
      uint8_t env=(d>=tail)?0:(uint8_t)(255u-((uint16_t)d*255u/tail));
      v=qadd8(scale8(m.brightness,35),scale8(m.brightness,env));
      h += phase/3;
    } break;
    case MOOD_TWINKLE: {
      uint8_t n=moodTurb8((uint16_t)i*137u,phase*5u,m.turbulence); uint8_t threshold=(uint8_t)(255u-scale8(m.density,210));
      v=(n>threshold)?m.brightness:scale8(m.brightness,scale8(m.glow,55));
      h += n>>5;
    } break;
    case MOOD_PLASMA: v=scale8(m.brightness,(uint8_t)(55u+scale8((uint8_t)(wave+wave2),200))); h += wave/2; break;
    case MOOD_FIRE: {
      uint8_t f=moodTurb8((uint16_t)i*max<uint8_t>(1,m.scale),phase*2u,m.turbulence);
      f=qadd8(f,scale8(m.density,90)); h=8u+scale8(f,20); sat=245; v=scale8(m.brightness,(uint8_t)(45u+scale8(f,210)));
    } break;
    case MOOD_PALETTE_WAVE: {
      uint8_t pi=(uint8_t)((m.palette + (p>>5))%24); h=MOOD_PALETTE_HUE8[pi] + wave/8; sat=MOOD_PALETTE_SAT[pi]; v=qadd8(scale8(m.brightness,120),scale8(wave,scale8(m.glow,120)));
    } break;
    case MOOD_AURORA: h=scale8(wave,42)+118+phase/4; sat=scale8(m.saturation,210); v=scale8(m.brightness,(uint8_t)(70u+scale8(wave2,170))); break;
    case MOOD_OCEAN: h=150+scale8(wave,28); sat=235; v=scale8(m.brightness,(uint8_t)(80u+scale8(wave2,160))); break;
    case MOOD_FIRE2: {
      uint8_t f=qadd8(moodTurb8((uint16_t)i*max<uint8_t>(1,m.scale/2+1),phase*3u,m.turbulence),scale8(wave,80));
      h=2+scale8(f,30); sat=255; v=scale8(m.brightness,(uint8_t)(35u+scale8(f,220)));
    } break;
    case MOOD_PULSE: v=scale8(m.brightness,(uint8_t)(55u+scale8(sin8(phase),200))); break;
    case MOOD_METEOR: {
      uint8_t head=phase, d=(head>=p)?head-p:p-head; uint8_t tail=(uint8_t)max<uint16_t>(20,30u+scale8(m.density,70));
      uint8_t env=(d>=tail)?0:(uint8_t)(255u-((uint16_t)d*255u/tail)); v=scale8(m.brightness,env); h += phase;
    } break;
    case MOOD_NEBULA: { uint8_t n=noise; h += 190u + scale8(n,35); sat=220; v=scale8(m.brightness,(uint8_t)(60u+scale8(n,190))); } break;
    case MOOD_STARFIELD: { uint8_t n=moodTurb8((uint16_t)i*91u,phase*7u,m.turbulence); bool star=n>(uint8_t)(240u-scale8(m.density,180)); v=star?m.brightness:scale8(m.brightness,scale8(m.glow,35)); h += n>>4; } break;
    case MOOD_ORGANIC: { uint8_t n=moodTurb8((uint16_t)i*max<uint8_t>(1,m.scale),phase+motion,m.turbulence); h += scale8(n,70); v=scale8(m.brightness,(uint8_t)(70u+scale8(n,170))); } break;
    case MOOD_CYBER: h=150u+scale8(p,45)+phase/6; sat=255; v=((p>>4)&1)?m.brightness:scale8(m.brightness,scale8(m.glow,55)); break;
    case MOOD_SPECTRAL: h=p+phase; sat=scale8(m.saturation,235); v=m.brightness; break;
    case MOOD_LAVA: { uint8_t n=moodTurb8((uint16_t)i*max<uint8_t>(1,m.scale/2+1),phase,m.turbulence); h=5+scale8(n,18); sat=255; v=scale8(m.brightness,(uint8_t)(55u+scale8(n,195))); } break;
    case MOOD_PLASMA_X: { uint8_t n=qadd8(wave,noise); h += (uint8_t)(wave2/2+n/3); sat=scale8(m.saturation,240); v=scale8(m.brightness,(uint8_t)(45u+scale8(n,210))); } break;
    default: break;
  }
  v=moodGlow(v,m);
  CHSV hsv(h,sat,v); CRGB c; hsv2rgb_rainbow(hsv,c); return c;
}
void renderMoodSide(const MoodConfig& m,uint16_t start,uint16_t count,bool rev,bool right,uint8_t sideOffset=0){
  for(uint16_t i=0;i<count;i++){
    uint16_t idx=rev?(start+count-1-i):(start+i); if(idx>=LED_COUNT)continue;
    leds[idx]=(m.mode==0)?CRGB::Black:moodColor(m,i,count,right,sideOffset);
  }
}
void renderMood(){
  MoodConfig L=leftMood,R=rightMood; bool revL=L.reverse,revR=R.reverse; uint8_t offR=0;
  bool rUsesOwnTVColor=true;
  if(moodLinkMode==MOOD_LINK_MIRROR){R=L; revR=L.reverse; rUsesOwnTVColor=false;}
  else if(moodLinkMode==MOOD_LINK_SYMMETRIC){R=L; revR=!L.reverse; R.hue=(uint16_t)((R.hue+180u)%360u); rUsesOwnTVColor=false;}
  else if(moodLinkMode==MOOD_LINK_FLOW){R=L; revR=L.reverse; offR=128; rUsesOwnTVColor=false;}
  renderMoodSide(L,moodLeftStart,moodLeftCount,revL,false,0);
  renderMoodSide(R,moodRightStart,moodRightCount,revR,rUsesOwnTVColor,offR);
}

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

/* ===== PERSISTENCE (NVS) =============================================== */

void setDefaultMapping(){
  // 12 fixed physical segments: 3 × 10 LEDs on each of the 4 sides.
  // BOTTOM 0..29 | LEFT 30..59 | TOP 60..89 | RIGHT 90..119
  const uint8_t src[MAPPER_SIDE_COUNT]={SRC_L0,SRC_L1,SRC_R0,SRC_R1};
  segmentCount=MAX_SEGMENTS;
  for(uint8_t i=0;i<MAX_SEGMENTS;i++){
    uint8_t side=i/MAPPER_SEGMENTS_PER_SIDE;
    segments[i].start=fixedSegmentStart(i);
    segments[i].count=MAPPER_LEDS_PER_SEGMENT;
    segments[i].source=src[side];
    segments[i].brightness=255;
    segments[i].reverse=false;
  }
}

void saveConfig(bool includeMapper){
  prefs.begin("cfg",false); prefs.putUChar("cfg_ver",CONFIG_SCHEMA_VERSION); prefs.putUChar("bright",globalBrightness); prefs.putUChar("smooth",smoothing); prefs.putUChar("black",blackThreshold);
  prefs.putString("tvip",tvIP.toString());
  // Webes jelszó: csak sózott hash kerül NVS-be (a régi nyílt "webpw" kulcsot töröljük).
  prefs.remove("webpw");
  prefs.putBool("webauth",webAuthConfigured);
  prefs.putBool("webopt",webAuthOptOut);
  if(webAuthConfigured){ prefs.putBytes("webslt",webAuthSalt,16); prefs.putBytes("webhsh",webAuthHash,32); }
  else { prefs.remove("webslt"); prefs.remove("webhsh"); }
  prefs.putUChar("lm_eff",leftMood.effect);prefs.putUChar("rm_eff",rightMood.effect);prefs.putUShort("lm_hue",leftMood.hue);prefs.putUShort("rm_hue",rightMood.hue);
  prefs.putBool("lm_mode",leftMood.mode!=0); prefs.putBool("rm_mode",rightMood.mode!=0);
  prefs.putUChar("lm_sat",leftMood.saturation);prefs.putUChar("rm_sat",rightMood.saturation);prefs.putUChar("lm_val",leftMood.brightness);prefs.putUChar("rm_val",rightMood.brightness);
  prefs.putUChar("lm_sp",leftMood.speed);prefs.putUChar("rm_sp",rightMood.speed);
  prefs.putUChar("lm_pal",leftMood.palette);prefs.putUChar("rm_pal",rightMood.palette);
  prefs.putUChar("lm_sc",leftMood.scale);prefs.putUChar("rm_sc",rightMood.scale);
  prefs.putUChar("lm_mot",leftMood.motion);prefs.putUChar("rm_mot",rightMood.motion);
  prefs.putUChar("lm_gl",leftMood.glow);prefs.putUChar("rm_gl",rightMood.glow);
  prefs.putUChar("lm_den",leftMood.density);prefs.putUChar("rm_den",rightMood.density);
  prefs.putUChar("lm_tur",leftMood.turbulence);prefs.putUChar("rm_tur",rightMood.turbulence);
  prefs.putUChar("lm_cm",leftMood.colorMode);prefs.putUChar("rm_cm",rightMood.colorMode);
  prefs.putBool("lm_auto",leftMood.autoColor);prefs.putBool("rm_auto",rightMood.autoColor);
  prefs.putBool("lm_rev",leftMood.reverse);prefs.putBool("rm_rev",rightMood.reverse);
  prefs.putUChar("linkmode",(uint8_t)moodLinkMode);
  prefs.putBool("dyn_en",dynBrightEnabled);prefs.putUChar("dyn_min",dynBrightMin);prefs.putUChar("dyn_max",dynBrightMax);prefs.putUChar("dyn_resp",dynBrightResp);
  prefs.putBool("mood_dyn",moodDynEnabled);prefs.putUChar("mood_dep",moodDynDepth);
  prefs.putBool("tv_sync",tvMasterSyncEnabled);prefs.putBool("tv_bsync",tvMasterBrightnessEnabled);
  prefs.putUChar("amb_src",ambilightSource);
  prefs.putBool("clone_en",sideCloneEnabled);prefs.putUChar("clone_br",sideCloneBrightness);prefs.putUShort("cl_st",cloneLeftStart);prefs.putUShort("cl_ct",cloneLeftCount);prefs.putUShort("cr_st",cloneRightStart);prefs.putUShort("cr_ct",cloneRightCount);prefs.putBool("cl_rev",cloneLeftRev);prefs.putBool("cr_rev",cloneRightRev);
  prefs.end();
  if(includeMapper) saveMapper();
}

void loadConfig(){
  prefs.begin("cfg",true); bool fresh=!prefs.isKey("cfg_ver"); uint8_t stored=prefs.getUChar("cfg_ver",0);
  tvIP=DEFAULT_TV_IP;
  globalBrightness=prefs.getUChar("bright",DEFAULT_BRIGHTNESS); smoothing=prefs.getUChar("smooth",DEFAULT_SMOOTHING); blackThreshold=prefs.getUChar("black",DEFAULT_BLACK_THRESHOLD);
  String tvs=prefs.getString("tvip",""); IPAddress ip; if(tvs.length()&&ip.fromString(tvs))tvIP=ip;
  // Webes jelszó betöltése: elsődlegesen sózott hash (webslt/webhsh).
  webAuthConfigured=prefs.getBool("webauth",false);
  webAuthOptOut=prefs.getBool("webopt",false);
  if(webAuthConfigured){
    if(prefs.getBytes("webslt",webAuthSalt,16)!=16 || prefs.getBytes("webhsh",webAuthHash,32)!=32) webAuthConfigured=false;
  }
  // Migráció: ha még régi nyílt "webpw" van tárolva, hash-eljük és tovább viszük.
  bool legacyAuthMigrated=false;
  if(!webAuthConfigured){
    String legacy=prefs.getString("webpw","");
    if(legacy.length()){ setWebAuthPassword(legacy.c_str()); legacyAuthMigrated=true; }
  }
#ifdef SECRET_WEB_AUTH
  if(!webAuthConfigured && strlen(SECRET_WEB_AUTH)>0) setWebAuthPassword(SECRET_WEB_AUTH);
#endif
#ifdef SECRET_TV_IP
  if(tvIP==IPAddress(0,0,0,0))tvIP=IPAddress(SECRET_TV_IP);
#endif
  leftMood.mode=prefs.getBool("lm_mode",true); rightMood.mode=prefs.getBool("rm_mode",true);
  leftMood.effect=prefs.getUChar("lm_eff",0);rightMood.effect=prefs.getUChar("rm_eff",0);leftMood.hue=prefs.getUShort("lm_hue",0);rightMood.hue=prefs.getUShort("rm_hue",120);
  leftMood.saturation=prefs.getUChar("lm_sat",255);rightMood.saturation=prefs.getUChar("rm_sat",255);leftMood.brightness=prefs.getUChar("lm_val",200);rightMood.brightness=prefs.getUChar("rm_val",200);
  leftMood.speed=prefs.getUChar("lm_sp",28);rightMood.speed=prefs.getUChar("rm_sp",28);leftMood.palette=prefs.getUChar("lm_pal",0);rightMood.palette=prefs.getUChar("rm_pal",1);
  leftMood.scale=prefs.getUChar("lm_sc",70);rightMood.scale=prefs.getUChar("rm_sc",70);leftMood.motion=prefs.getUChar("lm_mot",65);rightMood.motion=prefs.getUChar("rm_mot",65);
  leftMood.glow=prefs.getUChar("lm_gl",75);rightMood.glow=prefs.getUChar("rm_gl",75);leftMood.density=prefs.getUChar("lm_den",55);rightMood.density=prefs.getUChar("rm_den",55);
  leftMood.turbulence=prefs.getUChar("lm_tur",45);rightMood.turbulence=prefs.getUChar("rm_tur",45);leftMood.colorMode=prefs.getUChar("lm_cm",2);rightMood.colorMode=prefs.getUChar("rm_cm",2);
  leftMood.autoColor=prefs.getBool("lm_auto",false);rightMood.autoColor=prefs.getBool("rm_auto",false);leftMood.reverse=prefs.getBool("lm_rev",false);rightMood.reverse=prefs.getBool("rm_rev",false);
  moodLinkMode=(MoodLinkMode)constrain(prefs.getUChar("linkmode",0),0,3);
  dynBrightEnabled=prefs.getBool("dyn_en",false);dynBrightMin=prefs.getUChar("dyn_min",0);dynBrightMax=prefs.getUChar("dyn_max",255);dynBrightResp=prefs.getUChar("dyn_resp",35);
  moodDynEnabled=prefs.getBool("mood_dyn",false);moodDynDepth=prefs.getUChar("mood_dep",25);
  tvMasterSyncEnabled=prefs.getBool("tv_sync",true);tvMasterBrightnessEnabled=prefs.getBool("tv_bsync",true);
  ambilightSource=(prefs.getUChar("amb_src",AMB_SRC_MEASURED)!=0)?AMB_SRC_PROCESSED:AMB_SRC_MEASURED;
  sideCloneEnabled=prefs.getBool("clone_en",true);sideCloneBrightness=prefs.getUChar("clone_br",255);cloneLeftStart=prefs.getUShort("cl_st",30);cloneLeftCount=prefs.getUShort("cl_ct",30);cloneRightStart=prefs.getUShort("cr_st",90);cloneRightCount=prefs.getUShort("cr_ct",30);cloneLeftRev=prefs.getBool("cl_rev",false);cloneRightRev=prefs.getBool("cr_rev",false);
  prefs.end();
  if(legacyAuthMigrated){
    prefs.begin("cfg",false);
    prefs.remove("webpw");
    prefs.end();
    Serial.println("[AUTH] legacy plaintext password migrated and removed");
  }
  // Mapper is loaded independently from the general configuration transaction.
  loadMapper(true);
  if(fresh){
    saveConfig(true);
    Serial.println("[CFG] friss telepítés — alapértelmezett konfig");
  } else {
    if(segmentCount!=MAX_SEGMENTS){ setDefaultMapping(); saveMapper(); }
    if(stored>0 && stored<CONFIG_SCHEMA_VERSION){
      Serial.printf("[CFG] konfig séma frissítve v%u -> v%u\n",stored,CONFIG_SCHEMA_VERSION);
      saveConfig(true);   // séma-migrációkor a helyesen betöltött mappert is újramentjük
    }
  }
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

  // [WDT] Task watchdog a loop()-ra: ha egy iteráció beragad (pl. hálózati
  // blokkolás), a chip újraindul. Az API core-verziónként eltér, ezért guardolt.
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_cfg = { .timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&twdt_cfg);   // IDF5-ben a WDT-t a core már inicializálta
#else
  esp_task_wdt_init(WDT_TIMEOUT_MS/1000, true);
#endif
  esp_task_wdt_add(NULL);                 // a jelenlegi (loop) taszk feliratkoztatása

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

  // TV státusz poll (forrás/csatorna/hangerő/Ambilight mód)
  if(now-lastTVMasterPoll>=TV_MASTER_POLL_MS){
    lastTVMasterPoll=now;
    if(WiFi.isConnected() && tvIP!=IPAddress(0,0,0,0)) pollTVStatus();
  }

  // TV watchdog
  if(now-lastTVWatchdog>=TV_WATCHDOG_INTERVAL_MS){
    lastTVWatchdog=now;
    handleTVWatchdog();
  }

  // ===== TV vs Mood LED-frissítés – kölcsönösen kizárólagos, csak egyszer/show() =====
  // FONTOS: az ambilight-keret lekérése (readAmbilight) az, ami detektálja a TV-t
  // (frissíti lastSuccessfulPoll-t → a watchdog ebből állítja tvOnline-t), ezért a
  // lekérést MINDIG futtatjuk, ha kapcsolódtunk – függetlenül a tvOnline-tól.
  // Csak a LED render+show() kölcsönösen kizárólagos a mood-dal.
  bool tvFetchAllowed = !provisioningMode && WiFi.isConnected()
                        && tvIP!=IPAddress(0,0,0,0) && !otaInProgress;

  if(tvFetchAllowed && now-lastFrameStart>=TV_FRAME_INTERVAL_MS){
    lastFrameStart=now;
    bool got=readAmbilight();
    if(!got) tvConsecutiveFailures++;
    // [E5] a keretszámlálót a broadcast ELŐTT növeljük, hogy a WS üzenet
    //      és a kliensklónok konzisztens seq-et lássanak
    smartFrameSeq++;
    // Csak akkor renderelünk+mutatunk TV-ből, ha tényleg aktív a jel.
    if(tvOnline && !tvOutputOff){
      renderZonesToLeds();
      showLeds();
    }
    broadcastRealtime();
    smartLastFrameMs=now;
  }

  // Mood keret: csak ha nincs aktív TV-jel (kölcsönösen kizárólagos a fentivel)
  bool moodActive = !provisioningMode && (!tvOnline || tvOutputOff)
                    && !ledTestActive && !otaInProgress;
  if(moodActive && now-lastMoodFrame>=MOOD_FRAME_INTERVAL_MS){
    lastMoodFrame=now;
    renderMood();
    FastLED.setBrightness(effectiveMoodBrightness());
    FastLED.show();
  }

  static unsigned long lastMoodWsStatus=0;
  if(moodActive && now-lastMoodWsStatus>=MOOD_WS_STATUS_INTERVAL_MS){
    lastMoodWsStatus=now;
    broadcastMoodStatus();
  }

  // [WDT] watchdog karbantartás – ha a loop() beragad, a chip újraindul
  esp_task_wdt_reset();

  yield();
}