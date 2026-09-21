/* ============================================================================
   AMBILIGHT BRIDGE v5.5.1-C3-HARDENED
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
     [CFG] CONFIG_SCHEMA_VERSION 8 -> 9.

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

#define FIRMWARE_VERSION        "5.5.1-C3-HARDENED"
#define CONFIG_SCHEMA_VERSION   10
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

IPAddress          tvIP = DEFAULT_TV_IP;
ZoneRGB            targetZones[4], currentZones[4];
WiFiClient         tvClient;
bool               tvOnline, tvOutputOff=true;
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
#define TV_AMBILIGHT_READ_TIMEOUT_MS 800   // teljes ambilight-keret olvasási plafon
#define MOOD_FRAME_INTERVAL_MS      40
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
static const char AMBILIGHT_CC_HTML[] PROGMEM = R"AMB_CC_HTML(
<!DOCTYPE html>
<html lang="hu">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#060d18">
<meta name="description" content="Ambilight Bridge — ESP32-C3 Smart Control Center. Philips Ambilight TV backlight + WS2815 LED control with Smart Engine scene analysis.">
<title>Ambilight Bridge · Smart Control Center</title>
<style>
/* ═══════════════════════════════════════════════════════════════════════════
   AMBILIGHT BRIDGE — COMPLETE DESIGN SYSTEM v5.0
   Glassmorphic · Dark · Responsive · 12-column grid · Professional
   ═══════════════════════════════════════════════════════════════════════ */
:root {
  --bg:       #060d18;
  --bg2:      #0a1322;
  --card:     rgba(12,22,38,0.85);
  --card2:    rgba(16,28,48,0.78);
  --line:     rgba(120,155,210,0.11);
  --line2:    rgba(120,155,210,0.18);
  --text:     #e3edfc;
  --soft:     #8ca0bc;
  --muted:    #54657d;
  --cyan:     #2ad4ff;
  --blue:     #489eff;
  --purple:   #a078ff;
  --green:    #38e8a0;
  --red:      #ff5c72;
  --yellow:   #f5c842;
  --pink:     #f070b0;
  --orange:   #ff9040;
  --r:        16px;
  --r-sm:     10px;
  --r-pill:   999px;
  --shadow:   0 20px 60px rgba(0,0,0,.45), 0 4px 12px rgba(0,0,0,.25);
  --shadow-sm:0 4px 16px rgba(0,0,0,.3);
  --glass:    blur(24px) saturate(140%);
}
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
html{scroll-behavior:smooth;-webkit-text-size-adjust:100%}
body{min-height:100vh;min-height:100dvh;background:var(--bg);color:var(--text);font-family:'Inter','Inter Variable',ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;font-size:14px;line-height:1.55;-webkit-font-smoothing:antialiased;overflow-x:hidden}
body::before{content:"";position:fixed;inset:0;pointer-events:none;z-index:-1;
  background:radial-gradient(820px 500px at 8% -5%,rgba(42,212,255,.10),transparent 68%),
             radial-gradient(760px 480px at 94% 3%,rgba(160,120,255,.08),transparent 65%),
             radial-gradient(700px 440px at 50% 108%,rgba(56,232,160,.05),transparent 70%)}
::-webkit-scrollbar{width:5px}::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:rgba(120,155,210,.18);border-radius:10px}
::-webkit-scrollbar-thumb:hover{background:rgba(120,155,210,.30)}
/* ── App Shell ───────────────────────────────────────────────── */
.app{max-width:1440px;margin:0 auto;padding:18px 22px 40px}
/* ── Top Bar ─────────────────────────────────────────────────── */
.topbar{display:flex;align-items:center;justify-content:space-between;gap:18px;
  padding:16px 22px;margin-bottom:18px;
  background:var(--card);border:1px solid var(--line);border-radius:var(--r);
  box-shadow:var(--shadow);backdrop-filter:var(--glass);-webkit-backdrop-filter:var(--glass)}
.brand{display:flex;align-items:center;gap:14px}
.brandIcon{font-size:30px;line-height:1;filter:drop-shadow(0 0 14px rgba(42,212,255,.35))}
.brand h1{font-size:19px;font-weight:850;letter-spacing:-.2px}
.brand h1 em{font-style:normal;color:var(--cyan);font-weight:700}
.brand p{font-size:10px;color:var(--muted);margin-top:1px;text-transform:uppercase;letter-spacing:1.4px;font-weight:500}
.statusBar{display:flex;align-items:center;gap:12px}
.statusPill{display:flex;align-items:center;gap:7px;padding:7px 15px;
  border-radius:var(--r-pill);border:1px solid var(--line2);background:rgba(10,18,30,.7);
  font-size:11px;font-weight:600;color:var(--soft);letter-spacing:.3px}
.statusDot{width:8px;height:8px;border-radius:50%;background:var(--red);
  box-shadow:0 0 7px var(--red);transition:all .4s}
.statusDot.on{background:var(--green);box-shadow:0 0 9px var(--green),0 0 22px rgba(56,232,160,.35)}
.statusDot.ws{background:var(--cyan);box-shadow:0 0 9px var(--cyan),0 0 22px rgba(42,212,255,.35)}
.fpsChip{padding:5px 11px;border-radius:var(--r-pill);background:rgba(42,212,255,.08);
  border:1px solid rgba(42,212,255,.15);font-size:10px;font-weight:700;color:var(--cyan);letter-spacing:.5px}
/* ── Navigation ──────────────────────────────────────────────── */
.nav{display:flex;gap:5px;margin-bottom:18px;flex-wrap:wrap}
.navBtn{padding:11px 20px;border-radius:var(--r-pill);border:1px solid transparent;
  background:rgba(16,28,48,.5);color:var(--soft);font-size:12px;font-weight:650;
  cursor:pointer;transition:all .22s;letter-spacing:.4px;white-space:nowrap}
.navBtn:hover{border-color:var(--line2);color:var(--text);background:rgba(20,34,54,.6)}
.navBtn.active{background:rgba(42,212,255,.10);border-color:rgba(42,212,255,.28);color:var(--cyan)}
.navBtn .badge{font-size:9px;background:rgba(56,232,160,.15);color:var(--green);padding:2px 7px;border-radius:var(--r-pill);margin-left:5px}
/* ── Page Sections ───────────────────────────────────────────── */
.page{display:none}.page.active{display:block}
/* ── Grid System ─────────────────────────────────────────────── */
.g2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:14px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}
.g12{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
.c6{grid-column:span 6}.c4{grid-column:span 4}.c3{grid-column:span 3}.c8{grid-column:span 8}.c12{grid-column:span 12}
/* ── Cards ───────────────────────────────────────────────────── */
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);
  padding:18px 20px;box-shadow:var(--shadow-sm);backdrop-filter:var(--glass);-webkit-backdrop-filter:var(--glass)}
.cardHead{display:flex;align-items:center;gap:10px;margin-bottom:14px}
.cardHead h3{font-size:12px;font-weight:750;color:var(--soft);text-transform:uppercase;letter-spacing:.6px}
.cardHead h3 b{color:var(--text);font-size:15px;text-transform:none;letter-spacing:0}
.cardHead .cardIcon{font-size:18px;opacity:.7}
/* ── Stats Grid ──────────────────────────────────────────────── */
.statBox{padding:15px 18px;border-radius:var(--r-sm);border:1px solid var(--line);
  background:var(--card2);text-align:center;transition:all .25s}
.statBox:hover{border-color:var(--line2)}
.statVal{font-size:24px;font-weight:850;letter-spacing:-.5px}
.statLabel{font-size:10px;color:var(--muted);margin-top:3px;text-transform:uppercase;letter-spacing:.7px;font-weight:600}
.good{color:var(--green)}.warn{color:var(--yellow)}.bad{color:var(--red)}.info{color:var(--cyan)}.accent{color:var(--purple)}
/* ── Zone Cards ──────────────────────────────────────────────── */
.zoneCard{display:flex;align-items:center;gap:12px;padding:12px 16px;
  border-radius:var(--r-sm);border:1px solid var(--line);background:var(--card2)}
.zoneSwatch{width:42px;height:42px;border-radius:var(--r-sm);border:1px solid rgba(255,255,255,.08);
  transition:background .12s;box-shadow:0 0 14px rgba(0,0,0,.3) inset}
.zoneData{font-size:11px;color:var(--soft);line-height:1.5}
.zoneData strong{display:block;font-size:14px;color:var(--text);font-weight:700}
/* ── LED Strip ───────────────────────────────────────────────── */
.ledStrip{display:flex;gap:1px;height:26px;border-radius:8px;overflow:hidden}
.ledStrip .px{flex:1;min-width:2px;transition:background .08s;border-radius:1px}
.ledLegend{display:flex;gap:14px;margin-top:8px;font-size:9px;color:var(--muted);flex-wrap:wrap}
.ledLegend span{display:flex;align-items:center;gap:4px}
.ledLegend i{display:inline-block;width:8px;height:8px;border-radius:2px}
/* ── Forms ───────────────────────────────────────────────────── */
label{display:block;font-size:10px;color:var(--muted);margin-bottom:3px;font-weight:650;text-transform:uppercase;letter-spacing:.6px}
input,select{width:100%;padding:10px 12px;border-radius:var(--r-sm);border:1px solid var(--line);background:rgba(10,18,30,.7);color:var(--text);font-size:13px;outline:none;font-family:inherit;transition:border .2s}
input:focus,select:focus{border-color:rgba(42,212,255,.45);box-shadow:0 0 0 3px rgba(42,212,255,.06)}
input[type=range]{padding:0;height:5px;-webkit-appearance:none;appearance:none;background:var(--line);border-radius:3px;cursor:pointer;border:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:17px;height:17px;border-radius:50%;background:var(--cyan);cursor:pointer;border:2px solid var(--bg);box-shadow:0 0 10px rgba(42,212,255,.25)}
input[type=color]{padding:3px;height:38px;cursor:pointer}
input[type=checkbox]{width:16px;height:16px;accent-color:var(--cyan);cursor:pointer;flex-shrink:0}
.frow{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.frow3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
.frow4{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
.checkRow{display:flex;align-items:center;gap:8px;padding:5px 0}
.checkRow label{margin:0;font-size:12px;color:var(--text);text-transform:none;letter-spacing:0}
.rangeRow{display:flex;align-items:center;gap:10px}
.rangeRow input[type=range]{flex:1}.rangeRow span{font-size:11px;color:var(--soft);min-width:32px;text-align:right;font-weight:600}
/* ── Buttons ─────────────────────────────────────────────────── */
.btn{display:inline-flex;align-items:center;gap:6px;padding:10px 22px;border-radius:var(--r-pill);
  border:1px solid rgba(42,212,255,.22);background:rgba(42,212,255,.07);color:var(--cyan);
  font-size:12px;font-weight:700;cursor:pointer;transition:all .2s;letter-spacing:.3px}
.btn:hover{background:rgba(42,212,255,.14);border-color:rgba(42,212,255,.4);transform:translateY(-1px)}
.btn:active{transform:translateY(0)}
.btnSave{background:rgba(56,232,160,.10);border-color:rgba(56,232,160,.25);color:var(--green)}
.btnSave:hover{background:rgba(56,232,160,.18);border-color:rgba(56,232,160,.45)}
.btnDanger{background:rgba(255,92,114,.08);border-color:rgba(255,92,114,.2);color:var(--red)}
.btnDanger:hover{background:rgba(255,92,114,.15);border-color:rgba(255,92,114,.4)}
.btnDim{background:rgba(120,155,210,.05);border-color:var(--line);color:var(--soft)}
.btnDim:hover{background:rgba(120,155,210,.10);color:var(--text)}
.btnSm{padding:7px 14px;font-size:10px}
.btnRow{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}
/* ── Segment Row ─────────────────────────────────────────────── */
.segBlock{margin-bottom:10px}
.segSide{font-size:10px;color:var(--cyan);font-weight:750;text-transform:uppercase;letter-spacing:1.2px;
  padding:6px 0 8px;border-bottom:1px solid var(--line);margin-bottom:8px}
.segRow{display:grid;grid-template-columns:55px 50px 1fr 45px 40px;gap:5px;align-items:end;
  padding:7px 10px;border-radius:var(--r-sm);border:1px solid transparent;transition:all .2s}
.segRow:hover{border-color:var(--line);background:rgba(16,28,48,.4)}
.segRow label{font-size:8px;margin-bottom:1px}
.segRow input,.segRow select{padding:5px 7px;font-size:11px}
.segRow input[type=checkbox]{width:14px;height:14px;margin-bottom:5px}
/* ── Smart Engine Panels ─────────────────────────────────────── */
.meterLabel{display:flex;justify-content:space-between;font-size:10px;color:var(--muted);margin-bottom:3px}
.meterBar{height:6px;border-radius:3px;background:var(--line);overflow:hidden;margin-bottom:10px}
.meterFill{height:100%;border-radius:3px;transition:width .3s}
.meterFill.brightness{background:linear-gradient(90deg,var(--blue),var(--cyan))}
.meterFill.saturation{background:linear-gradient(90deg,var(--purple),var(--pink))}
.meterFill.motion{background:linear-gradient(90deg,var(--orange),var(--yellow))}
.meterFill.energy{background:linear-gradient(90deg,var(--green),var(--cyan))}
.meterFill.speed{background:linear-gradient(90deg,var(--yellow),var(--green))}
/* ── Scene Type Badge ────────────────────────────────────────── */
.sceneBadge{display:inline-block;padding:5px 12px;border-radius:var(--r-pill);font-size:10px;font-weight:700;letter-spacing:.4px}
.sceneBadge.dark{background:rgba(120,155,210,.06);color:var(--muted);border:1px solid var(--line)}
.sceneBadge.normal{background:rgba(72,158,255,.08);color:var(--blue);border:1px solid rgba(72,158,255,.2)}
.sceneBadge.bright{background:rgba(245,200,66,.08);color:var(--yellow);border:1px solid rgba(245,200,66,.2)}
.sceneBadge.action{background:rgba(255,92,114,.08);color:var(--red);border:1px solid rgba(255,92,114,.2)}
.sceneBadge.calm{background:rgba(56,232,160,.08);color:var(--green);border:1px solid rgba(56,232,160,.2)}
/* ── Toast ───────────────────────────────────────────────────── */
.toast{position:fixed;top:22px;left:50%;transform:translateX(-50%);padding:11px 24px;
  border-radius:var(--r-pill);background:var(--card2);border:1px solid var(--line2);
  color:var(--text);font-size:12px;font-weight:700;z-index:9999;opacity:0;pointer-events:none;
  transition:opacity .3s,transform .3s;box-shadow:var(--shadow);backdrop-filter:var(--glass);
  letter-spacing:.3px}
.toast.show{opacity:1;transform:translateX(-50%) translateY(4px)}
.toast.ok{border-color:rgba(56,232,160,.4);color:var(--green)}
.toast.err{border-color:rgba(255,92,114,.4);color:var(--red)}
/* ── Modal ───────────────────────────────────────────────────── */
.modalOverlay{position:fixed;inset:0;background:rgba(3,8,18,.82);display:flex;
  align-items:center;justify-content:center;z-index:1000;backdrop-filter:blur(10px)}
.modalBox{background:var(--card);border:1px solid var(--line2);border-radius:var(--r);
  padding:28px 30px;max-width:440px;width:calc(100% - 36px);box-shadow:0 30px 80px rgba(0,0,0,.6)}
.modalBox h2{font-size:18px;margin-bottom:6px}
.modalBox p{font-size:12px;color:var(--soft);margin-bottom:16px}
.modalBox input{margin-bottom:12px}
/* ── Progress Bar ────────────────────────────────────────────── */
.progressWrap{margin-top:10px;display:none}
.progressBar{height:7px;border-radius:4px;background:var(--line);overflow:hidden}
.progressFill{height:100%;width:0;background:linear-gradient(90deg,var(--green),var(--cyan));border-radius:4px;transition:width .3s}
.progressText{font-size:10px;color:var(--soft);margin-top:4px}
/* ── Palette Grid ────────────────────────────────────────────── */
.paletteGrid{display:grid;grid-template-columns:repeat(8,1fr);gap:4px;margin-top:6px}
.paletteSwatch{height:22px;border-radius:4px;cursor:pointer;border:2px solid transparent;transition:all .15s}
.paletteSwatch:hover{transform:scale(1.12);border-color:var(--text)}
.paletteSwatch.sel{border-color:var(--cyan);box-shadow:0 0 10px rgba(42,212,255,.4)}
/* ── Responsive ──────────────────────────────────────────────── */
@media(max-width:1100px){.g12{grid-template-columns:1fr}.c6,.c4,.c3,.c8{grid-column:span 1}.g2,.g3,.g4{grid-template-columns:1fr}.frow3,.frow4{grid-template-columns:1fr 1fr}.segRow{grid-template-columns:45px 45px 1fr 40px 35px}}
@media(max-width:640px){.topbar{flex-direction:column;align-items:flex-start;gap:10px}.statusBar{flex-wrap:wrap}.brand h1{font-size:16px}.frow,.frow3,.frow4{grid-template-columns:1fr}.g2,.g3,.g4{grid-template-columns:1fr}.segRow{grid-template-columns:1fr 1fr;gap:4px}.nav{overflow-x:auto;flex-wrap:nowrap}.navBtn{flex-shrink:0}.paletteGrid{grid-template-columns:repeat(6,1fr)}}
</style>
</head><body>
<div class="app">
<!-- ═══════════════ TOP BAR ═══════════════ -->
<header class="topbar">
  <div class="brand"><div class="brandIcon">✦</div><div><h1>Ambilight <em>Bridge</em></h1><p>ESP32‑C3 · WS2815 · 120 LED · Smart Engine</p></div></div>
  <div class="statusBar">
    <div class="statusPill" id="tvPill"><span class="statusDot" id="tvDot"></span><span id="tvLabel">TV —</span></div>
    <div class="statusPill"><span class="statusDot" id="espDot"></span><span id="espLabel">ESP —</span></div>
    <div class="statusPill"><span class="statusDot ws" id="wsDot" style="display:none"></span><span id="wsLabel" style="display:none">WS</span></div>
    <div class="fpsChip" id="fpsChip">— FPS</div>
  </div>
</header>
<!-- ═══════════════ NAVIGATION ═══════════════ -->
<nav class="nav" id="mainNav">
  <button class="navBtn active" data-page="dash">🏠 Home</button>
  <button class="navBtn" data-page="mapper">🗺️ Mapper</button>
  <button class="navBtn" data-page="mood">🎨 Mood Studio</button>
  <button class="navBtn" data-page="smart">🧠 Smart Engine</button>
  <button class="navBtn" data-page="tv">📡 TV Ambilight</button>
  <button class="navBtn" data-page="settings">⚙️ Settings</button>
  <button class="navBtn" data-page="diag">📊 Diag</button>
</nav>

<!-- ═══════════════ DASHBOARD ═══════════════ -->
<section class="page active" id="page-dash">
  <div class="g4" id="dashStats"></div>
  <div class="g2" style="margin-top:14px">
    <div class="card"><div class="cardHead"><span class="cardIcon">📺</span><h3>TV <b>Zónák</b> · Valós idejű</h3></div><div class="g2" id="zoneGrid" style="gap:8px"></div></div>
    <div class="card"><div class="cardHead"><span class="cardIcon">💡</span><h3>LED <b>Szalag</b> · 120 px</h3></div><div class="ledStrip" id="ledBar"></div><div class="ledLegend"><span><i style="background:#ff4466"></i>L0</span><span><i style="background:#ff8844"></i>L1</span><span><i style="background:#4488ff"></i>R0</span><span><i style="background:#44ccff"></i>R1</span></div></div>
  </div>
  <div class="g2" style="margin-top:14px">
    <div class="card"><div class="cardHead"><span class="cardIcon">📈</span><h3>Smart <b>Engine</b> Áttekintés</h3></div><div id="dashSmart"></div></div>
    <div class="card"><div class="cardHead"><span class="cardIcon">🛠️</span><h3><b>Gyorsműveletek</b></h3></div>
      <div class="btnRow">
        <button class="btn btnDim btnSm" onclick="ledTest('red')">🔴 Piros</button>
        <button class="btn btnDim btnSm" onclick="ledTest('green')">🟢 Zöld</button>
        <button class="btn btnDim btnSm" onclick="ledTest('blue')">🔵 Kék</button>
        <button class="btn btnDim btnSm" onclick="ledTest('white')">⚪ Fehér</button>
        <button class="btn btnDanger btnSm" onclick="restartESP()">↻ Restart</button>
        <button class="btn btnSave btnSm" onclick="saveConfig()">💾 Mentés</button>
      </div>
    </div>
  </div>
</section>

<!-- ═══════════════ LED MAPPER ═══════════════ -->
<section class="page" id="page-mapper">
  <div class="card"><div class="cardHead"><span class="cardIcon">🗺️</span><h3>LED <b>Mapper</b> · 12 független szegmens · 4 oldal × 3</h3></div>
    <p style="font-size:11px;color:var(--muted);margin-bottom:12px">Minden szegmenshez rendelj TV zónát, gradiens irányt vagy keverési módot. A fizikai sorrend független a logikai zónáktól.</p>
    <div id="segList"></div>
    <div class="btnRow"><button class="btn btnDim" onclick="defaults()">↺ Gyári</button><button class="btn btnSave" onclick="saveConfig()">💾 Mentés</button></div>  </div>
  <div class="card" style="margin-top:14px"><div class="cardHead"><span class="cardIcon">👁️</span><h3><b>Előnézet</b> · forrás színek szerint</h3></div>
    <div class="ledStrip" id="mapperBar"></div>
    <div class="ledLegend"><span><i style="background:#f00"></i>L0</span><span><i style="background:#0f0"></i>L1</span><span><i style="background:#00f"></i>R0</span><span><i style="background:#ff0"></i>R1</span><span><i style="background:#f0f"></i>Gradiens</span><span><i style="background:#333"></i>Black</span></div>
  </div>
</section>

<!-- ═══════════════ MOOD STUDIO ═══════════════ -->
<section class="page" id="page-mood">
  <div class="g2">
    <div class="card"><div class="cardHead"><span class="cardIcon">🎨</span><h3><b>Bal Oldal</b> · LEFT 30‑59</h3></div><div id="moodLeftForm"></div></div>
    <div class="card"><div class="cardHead"><span class="cardIcon">🎨</span><h3><b>Jobb Oldal</b> · RIGHT 90‑119</h3></div><div id="moodRightForm"></div></div>
  </div>
  <div class="card" style="margin-top:14px"><div class="cardHead"><span class="cardIcon">🔗</span><h3><b>Link Mód</b></h3></div>
    <div class="frow"><select id="moodLinkSel"><option value="0">Független</option><option value="1">Tükrözés</option><option value="2">Szimmetrikus</option><option value="3">Flow (+60° hue)</option></select></div>
    <div class="btnRow"><button class="btn btnSave" onclick="saveConfig()">💾 Mentés</button></div>
  </div>
</section>

<!-- ═══════════════ SMART ENGINE ═══════════════ -->
<section class="page" id="page-smart">
  <div class="g2">
    <div class="card"><div class="cardHead"><span class="cardIcon">🎬</span><h3>Scene <b>Analyzer</b></h3></div><div id="scenePanel"></div></div>
    <div class="card"><div class="cardHead"><span class="cardIcon">🎛️</span><h3>Adaptive <b>Controller</b></h3></div><div id="adaptivePanel"></div></div>
  </div>
  <div class="g2" style="margin-top:14px">
    <div class="card"><div class="cardHead"><span class="cardIcon">📐</span><h3>Zóna <b>Analízis</b></h3></div><div class="g2" id="zoneAnalysis" style="gap:8px"></div></div>
    <div class="card"><div class="cardHead"><span class="cardIcon">🧠</span><h3>Smart Engine <b>Státusz</b></h3></div><div id="engineStatus"></div></div>
  </div>
</section>

<!-- ═══════════════ TV AMBILIGHT ═══════════════ -->
<section class="page" id="page-tv">
  <div class="g2">
    <div class="card"><div class="cardHead"><span class="cardIcon">📡</span><h3>TV <b>Kapcsolat</b></h3></div>
      <div class="frow"><div><label>TV IP-cím</label><input id="cfgTvIP" placeholder="192.168.1.x"></div><div><label>Port</label><input value="1925 (JointSPACE)" disabled></div></div>
      <div class="checkRow"><input type="checkbox" id="cfgTvSync"><label>TV Master Sync</label></div>
      <div class="checkRow"><input type="checkbox" id="cfgTvBSync"><label>TV fényerő követése</label></div>
      <p style="font-size:10px;color:var(--muted);margin-top:8px">IP módosítás után ments és indítsd újra az ESP32‑t.</p>
    </div>
    <div class="card"><div class="cardHead"><span class="cardIcon">🪞</span><h3>Side <b>Clone</b></h3></div>
      <div class="checkRow"><input type="checkbox" id="cfgCloneOn"><label>Side Clone engedélyezése</label></div>
      <div class="rangeRow"><label style="min-width:70px">Fényerő</label><input type="range" id="cfgCloneBri" min="0" max="255" value="255"><span id="cfgCloneBriV">255</span></div>
      <div class="frow"><div><label>Bal start</label><input type="number" id="cfgCloneLS" min="0" max="119" value="30"></div><div><label>Bal db</label><input type="number" id="cfgCloneLC" min="0" max="120" value="30"></div></div>
      <div class="frow"><div><label>Jobb start</label><input type="number" id="cfgCloneRS" min="0" max="119" value="90"></div><div><label>Jobb db</label><input type="number" id="cfgCloneRC" min="0" max="120" value="30"></div></div>
      <div class="checkRow"><input type="checkbox" id="cfgCloneLR"><label>Bal fordított</label></div>
      <div class="checkRow"><input type="checkbox" id="cfgCloneRR"><label>Jobb fordított</label></div>
    </div>
  </div>
  <div class="btnRow"><button class="btn btnSave" onclick="saveConfig()">💾 Mentés</button></div>
</section>

<!-- ═══════════════ SETTINGS ═══════════════ -->
<section class="page" id="page-settings">
  <div class="g2">
    <div class="card"><div class="cardHead"><span class="cardIcon">📶</span><h3><b>WiFi</b></h3></div>
      <div class="frow"><div><label>SSID</label><input id="cfgWifiSSID"></div><div><label>Jelszó</label><input id="cfgWifiPass" type="password"></div></div>
      <div class="btnRow"><button class="btn btnDim" onclick="saveWiFi()">💾 WiFi mentése</button></div>
    </div>
    <div class="card"><div class="cardHead"><span class="cardIcon">🔐</span><h3><b>Web Auth</b> <span id="authStateBadge" class="sceneBadge dark" style="margin-left:auto">— </span></h3></div>
      <div class="checkRow"><input type="checkbox" id="cfgAuthOn"><label>Jelszavas védelem engedélyezése</label></div>
      <div><label>Új jelszó (max 63 kar.)</label><input id="cfgAuthPass" type="password" placeholder="ambilight" autocomplete="new-password"></div>
      <div><label>Jelszó megerősítése</label><input id="cfgAuthPass2" type="password" placeholder="ismét" autocomplete="new-password"></div>
      <div class="btnRow"><button class="btn btnDim" onclick="saveAuth()">💾 Auth mentése</button><button class="btn btnDanger btnSm" onclick="disableAuth()">🔓 Kikapcsolás</button></div>
      <p style="font-size:10px;color:var(--muted);margin-top:8px">Üres jelszó vagy „Kikapcsolás” → az auth teljesen kikapcsol és az NVS kulcs törlődik. Módosítás után a böngésző újra bekérheti a belépést.</p>
    </div>
  </div>
  <div class="g2" style="margin-top:14px">
    <div class="card"><div class="cardHead"><span class="cardIcon">🔆</span><h3><b>Fényerő & Simítás</b></h3></div>
      <div class="rangeRow"><label style="min-width:80px">Fényerő</label><input type="range" id="cfgBri" min="0" max="255" value="160"><span id="cfgBriV">160</span></div>
      <div class="rangeRow"><label style="min-width:80px">Simítás</label><input type="range" id="cfgSmooth" min="0" max="100" value="70"><span id="cfgSmoothV">70</span></div>
      <div class="rangeRow"><label style="min-width:80px">Fekete küszöb</label><input type="range" id="cfgBlack" min="0" max="100" value="4"><span id="cfgBlackV">4</span></div>
    </div>
    <div class="card"><div class="cardHead"><span class="cardIcon">📡</span><h3><b>Dinamikus fényerő</b></h3></div>
      <div class="checkRow"><input type="checkbox" id="cfgDynOn"><label>Engedélyezve</label></div>
      <div class="frow3"><div><label>Min</label><input type="number" id="cfgDynMin" min="0" max="255" value="0"></div><div><label>Max</label><input type="number" id="cfgDynMax" min="0" max="255" value="255"></div><div><label>Válasz</label><input type="number" id="cfgDynResp" min="1" max="100" value="35"></div></div>
      <div class="checkRow"><input type="checkbox" id="cfgMoodDyn"><label>Mood dinamikus fényerő</label></div>
      <div class="rangeRow"><label style="min-width:80px">Mélység</label><input type="range" id="cfgMoodDep" min="0" max="100" value="25"><span id="cfgMoodDepV">25</span></div>
    </div>
  </div>
  <div class="card" style="margin-top:14px"><div class="cardHead"><span class="cardIcon">📦</span><h3><b>OTA Firmware Frissítés</b></h3></div>
    <div><input type="file" id="otaFile" accept=".bin"><div class="btnRow" style="margin-top:8px"><button class="btn" id="otaBtn" onclick="uploadOTA()">⬆ Feltöltés</button></div></div>
    <div class="progressWrap" id="otaProgress"><div class="progressBar"><div class="progressFill" id="otaBar"></div></div><div class="progressText" id="otaText"></div></div>
  </div>
  <div class="btnRow" style="margin-top:14px"><button class="btn btnSave" onclick="saveConfig()">💾 Összes beállítás mentése</button></div>
</section>

<!-- ═══════════════ DIAGNOSTICS ═══════════════ -->
<section class="page" id="page-diag">
  <div class="g4" id="diagStats"></div>
  <div class="card" style="margin-top:14px"><div class="cardHead"><span class="cardIcon">📋</span><h3><b>Frame Statisztika</b></h3></div>
    <div class="g4"><div class="statBox"><div class="statVal good" id="diagGood">0</div><div class="statLabel">Good Frames</div></div><div class="statBox"><div class="statVal bad" id="diagBad">0</div><div class="statLabel">Bad Frames</div></div><div class="statBox"><div class="statVal info" id="diagSeq">0</div><div class="statLabel">Last Seq</div></div><div class="statBox"><div class="statVal accent" id="diagFPS">0</div><div class="statLabel">Current FPS</div></div></div>
    <div style="margin-top:12px"><div><label>Utolsó hiba</label><div id="diagError" style="font-size:11px;color:var(--red);padding:6px 10px;border-radius:var(--r-sm);background:rgba(255,92,114,.05);border:1px solid rgba(255,92,114,.12);margin-top:4px;word-break:break-all">—</div></div></div>
  </div>
  <div class="card" style="margin-top:14px"><div class="cardHead"><span class="cardIcon">🔧</span><h3><b>ESP32 Rendszer</b></h3></div><div class="g3" id="sysInfo"></div></div>
</section>

<!-- ═══════════════ CONNECT MODAL ═══════════════ -->
<div class="modalOverlay" id="connectModal">
  <div class="modalBox">
    <h2>🔌 Kapcsolódás az ESP32‑höz</h2>
    <p>Az ESP32 közvetlenül HTTP-n szolgálja ki a vezérlőt. GitHub Pages HTTPS-ről a böngésző a helyi HTTP API-t blokkolhatja. Ilyenkor nyisd meg ezt a vezérlőt közvetlenül az ESP32-ről: <code>http://ESP:8080</code>.</p>
    <label>ESP32 IP-cím</label><input id="modalIP" placeholder="192.168.1.100 vagy ambilight.local">
    <label>Web Auth jelszó</label><input id="modalAuth" type="password" placeholder="ambilight">
    <div class="btnRow" style="justify-content:flex-end"><button class="btn" onclick="doConnect()">Kapcsolódás</button></div>
  </div>
</div>

<!-- ═══════════════ TOAST ═══════════════ -->
<div class="toast" id="toast"></div>
</div>

<!-- ═════════════════════════════════════════════════════════════════════════
   AMBILIGHT BRIDGE — JAVASCRIPT APPLICATION v5.0
   Smart Engine · Scene Analyzer · Adaptive Controller · Mapper · Full SPA
   ═══════════════════════════════════════════════════════════════════════ -->
<script>
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
let espHost="",espPort=8080,wsPort=81,ws=null,wsTO=null,pollTO=null,config=null,fps_f=0,fps_t=performance.now(),fps_v=0;
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
  if(!espIP)return;
  try{ws=new WebSocket(wsUrl())}catch(e){return scheduleWS()}
  ws.onopen=()=>{
    if(pollTO){clearTimeout(pollTO);pollTO=null;}
    const d=$("wsDot");if(d)d.style.display="inline-block";const l=$("wsLabel");if(l)l.style.display="inline";updateConn(true,config?.tv_online??false);
  };
  ws.onmessage=e=>{try{
    const f=JSON.parse(e.data);
    if(pipeline.accept(f,Date.now()).accepted){
      fps_f++;const n=performance.now();
      if(n-fps_t>=1000){fps_v=Math.round(fps_f*1000/(n-fps_t));fps_f=0;fps_t=n;const fc=$("fpsChip");if(fc)fc.textContent=fps_v+" FPS"}
      engine.process({zones:f.zones});updateLEDBar(f.zones);updateZones(f.zones);updateConn(true,!!f.tv);
    }
  }catch(err){}}
  ws.onclose=()=>{
    ws=null;const d=$("wsDot");if(d)d.style.display="none";const l=$("wsLabel");if(l)l.style.display="none";
    scheduleWS();startPoll();
  };
  ws.onerror=()=>{if(ws)ws.close()};
}
function scheduleWS(){if(wsTO)clearTimeout(wsTO);wsTO=setTimeout(startWS,2000)}
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
function canonicalMapperDefaults(){
  const out=Array.from({length:12},()=>({start:0,count:0,source:0,bri:255,brightness:255,rev:false,reverse:false}));
  const src=[1,2,3,4];
  for(let i=0;i<4;i++)out[i]={start:i*30,count:30,source:src[i],bri:255,brightness:255,rev:false,reverse:false};
  return out;
}
function renderSegs(segs){
  const el=$("segList");if(!el)return;
  if(!Array.isArray(segs)||!segs.length)segs=canonicalMapperDefaults();
  const names=mapperSources.length?mapperSources:SRC_FALLBACK;
  let h="";
  for(let side=0;side<4;side++){
    h+=`<div class="segSide">${SIDES[side]}</div>`;
    for(let s=0;s<3;s++){
      const i=side*3+s,seg=segs[i]||{start:0,count:0,source:0,brightness:255,reverse:false};
      const bri=seg.brightness??seg.bri??255,rev=seg.reverse??seg.rev??false;
      h+=`<div class="segRow"><div><label>Start</label><input type="number" id="s${i}st" value="${seg.start??0}" min="0" max="119" onchange="updateMapperPreview()"></div><div><label>DB</label><input type="number" id="s${i}co" value="${seg.count??0}" min="0" max="120" onchange="updateMapperPreview()"></div><div><label>Forrás</label><select id="s${i}src" onchange="updateMapperPreview()">${names.map((n,j)=>`<option value="${j}" ${(seg.source??0)===j?"selected":""}>${n}</option>`).join("")}</select></div><div><label>Fény</label><input type="number" id="s${i}bri" value="${bri}" min="0" max="255"></div><div><label>↔</label><input type="checkbox" id="s${i}rev" ${rev?"checked":""}></div></div>`;
    }
  }
  el.innerHTML=h;
}
function collectSegs(){
  const o=[];
  for(let i=0;i<Math.min(12,mapperMaxSegments);i++){
    const count=+($("s"+i+"co")?.value||0);
    if(count<=0)continue; // üres UI slot nem kerül POST-ba: firmware mapperValid() szerint a count=0 érvénytelen
    o.push({start:+($("s"+i+"st")?.value||0),count,source:+($("s"+i+"src")?.value||0),brightness:+($("s"+i+"bri")?.value||255),reverse:$("s"+i+"rev")?.checked||false});
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
  let step="CONFIG";
  try{
    const cfg=collectCfg();
    await apiFormPost("/api/config",{brightness:cfg.brightness,smoothing:cfg.smoothing,blackThreshold:cfg.black_threshold,
      dyn_on:cfg.dyn_on?1:0,dyn_min:cfg.dyn_min,dyn_max:cfg.dyn_max,dyn_resp:cfg.dyn_resp,
      mood_dyn:cfg.mood_dyn?1:0,mood_dep:cfg.mood_dep,tv_sync:cfg.tv_sync?1:0,tv_bsync:cfg.tv_bsync?1:0});
    step="TV";
    if($("cfgTvIP").value) await apiFormPost("/api/tv",{ip:$("cfgTvIP").value});
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

/* ── Init ───────────────────────────────────────────────────────── */
$("moodLeftForm").innerHTML=moodForm("left");$("moodRightForm").innerHTML=moodForm("right");renderSegs(canonicalMapperDefaults());renderScene({brightness:0,saturation:0,motion:0,energy:0,dominantColor:{r:0,g:0,b:0},dominantHue:0,warmCool:0,zones:ZN.map(n=>({name:n,r:0,g:0,b:0,luminance:0}))});renderAdaptive({brightness:50,speed:50,reaction:50,sceneType:"NORMAL"});renderZoneAnalysis({zones:ZN.map(n=>({name:n,r:0,g:0,b:0,luminance:0}))});
if(localESPPage){
  espHost=location.hostname; espPort=Number(location.port)||8080;
  espIP=espHost+(espPort!==8080?":"+espPort:"");
  $("connectModal").style.display="none";
  loadAll();
}else if(espIP && !securePage){
  try{setESPAddress(espIP);loadAll()}catch(e){$("connectModal").style.display="flex"}
}else $("connectModal").style.display="flex";
if(securePage){$("modalIP").value=localStorage.getItem("ab_ip")||"192.168.1.228";}
setInterval(async()=>{if(!espHost||securePage)return;try{const s=await apiGet("/api/state");config={...config,...normalizeState(s)};updateConn(true,config.tv_online);renderStats(config);renderDiag(config)}catch(e){updateConn(false)}},8000);
console.log("🚀 Ambilight Bridge v5.0 · Full Smart Engine · Ready");
</script>
</body>
)AMB_CC_HTML";

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
  d["dyn_on"]=dynBrightEnabled; d["dyn_min"]=dynBrightMin; d["dyn_max"]=dynBrightMax; d["dyn_resp"]=dynBrightResp;
  d["mood_dyn"]=moodDynEnabled; d["mood_dep"]=moodDynDepth;
  d["tv_sync"]=tvMasterSyncEnabled; d["tv_bsync"]=tvMasterBrightnessEnabled;
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
/* ===== FORWARD DECLARATIONS =========================================== */
void setDefaultMapping();
void saveConfig();
bool detectTVTopology();

void loadMapper(bool defaultsIfMissing){
  tvIP=DEFAULT_TV_IP;
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
  if(server.hasArg("dyn_on")) dynBrightEnabled=server.arg("dyn_on").toInt()!=0;
  if(server.hasArg("dyn_min")) dynBrightMin=clampU8(server.arg("dyn_min").toInt());
  if(server.hasArg("dyn_max")) dynBrightMax=clampU8(server.arg("dyn_max").toInt());
  if(server.hasArg("dyn_resp")) dynBrightResp=clampU8(server.arg("dyn_resp").toInt());
  if(server.hasArg("mood_dyn")) moodDynEnabled=server.arg("mood_dyn").toInt()!=0;
  if(server.hasArg("mood_dep")) moodDynDepth=clampU8(server.arg("mood_dep").toInt());
  if(server.hasArg("tv_sync")) tvMasterSyncEnabled=server.arg("tv_sync").toInt()!=0;
  if(server.hasArg("tv_bsync")) tvMasterBrightnessEnabled=server.arg("tv_bsync").toInt()!=0;
  if(dynBrightMin>dynBrightMax){uint8_t t=dynBrightMin;dynBrightMin=dynBrightMax;dynBrightMax=t;}
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
      saveConfig();
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
  saveConfig(); server.send(200,"application/json","{\"ok\":true}");
}

// GET /api/topology
void apiTopology(){ addCorsHeaders(); JsonDocument d; d["detected"]=tvTopoDetected; d["left"]=tvTopoLeft; d["top"]=tvTopoTop; d["right"]=tvTopoRight; d["bottom"]=tvTopoBottom; d["layers"]=tvTopoLayers; String o;serializeJson(d,o);server.send(200,"application/json",o);}

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
  tvClient.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
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
  tvClient.printf("GET /1/ambilight/processed HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  tvIP.toString().c_str());
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
  // [FIX] Az ONLINE állapot KIZÁRÓLAG tényleges sikeres keretből származik
  //       (esemény-vezérelt), nem időzítőből → nincs többé hamis "[TV] online"
  //       közvetlenül boot után (a korábbi lastSuccessfulPoll=0 sentinel-bug).
  if(!tvOnline){ tvOnline=true; Serial.println("[TV] online"); }
  return true;
}

// TV master állapot lekérdezése (power + best-effort brightness)
void pollTVMaster(){
  int n=tvGet("/1/system/power",tvJsonBuf,sizeof(tvJsonBuf));
  if(n>0){
    JsonDocument d;
    if(!deserializeJson(d,tvJsonBuf)){
      const char* pw=d["powerstate"]|"";
      tvOutputOff = (strcmp(pw,"On")!=0);
      // A TV elérhető → a master-fényerő szinkron alkalmazható (lásd effectiveBrightness).
      // Best-effort: ha a TV küld brightness értéket, azt átvesszük; különben
      // a beállított tvMasterBrightness marad (alapért. 255 = nincs változtatás).
      if(d["brightness"].is<int>()){
        int b=d["brightness"].as<int>();
        tvMasterBrightness=(uint8_t)constrain(b,0,255);
      }
      tvMasterBrightnessAvailable = tvMasterBrightnessEnabled;
    }
  } else {
    tvMasterBrightnessAvailable = false;
  }
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
    if(tooManyFails || stale){ tvOnline=false; Serial.println("[TV] offline"); }
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
  for(uint8_t s=0;s<segmentCount && s<MAX_SEGMENTS;s++){LedSegment &sg=segments[s];if(!mapperValid(sg))continue;for(uint16_t i=0;i<sg.count;i++){uint16_t idx=sg.reverse?(sg.start+sg.count-1-i):(sg.start+i);float t=sg.count>1?(float)i/(float)(sg.count-1):0.0f;CRGB col=(sg.source>=SRC_GRADIENT_TOP&&sg.source<=SRC_GRADIENT_LEFT)?gradientSource(sg.source,t):zoneFromSource(sg.source);col.nscale8(sg.brightness);leds[idx]=col;written[idx]=true;}}
  if(sideCloneEnabled){CRGB l=zoneFromSource(SRC_L_AVG);CRGB r=zoneFromSource(SRC_R_AVG);l.nscale8(sideCloneBrightness);r.nscale8(sideCloneBrightness);for(uint16_t i=0;i<cloneLeftCount;i++){uint16_t idx=cloneLeftRev?(cloneLeftStart+cloneLeftCount-1-i):(cloneLeftStart+i);if(idx<LED_COUNT&&!written[idx])leds[idx]=l;}for(uint16_t i=0;i<cloneRightCount;i++){uint16_t idx=cloneRightRev?(cloneRightStart+cloneRightCount-1-i):(cloneRightStart+i);if(idx<LED_COUNT&&!written[idx])leds[idx]=r;}}
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

// [E4] pontos 0..359 → 0..255 húe-átváltás: *256u/360u (nem *255/359)
static inline uint8_t hue360To8(uint16_t h){ return (uint8_t)(((uint32_t)h*256u)/360u); }

// [PERF] Az ESP32-C3 (RISC-V) NEM rendelkezik hardveres FPU-val, ezért a mood-motor
// teljesen fixpontos / egész számos FastLED-primitívekre (sin8, beat8, beatsin8,
// scale8, qadd8) épül — nincs többé lebegőpontos sin/fabs/fmod LED-enként.
static CRGB moodColor(const MoodConfig& m,uint16_t i,uint16_t count,uint8_t sideOffset=0){
  uint32_t now=millis(); uint8_t fx=m.effect; uint8_t h=hue360To8(m.hue);
  uint8_t p8 = (count>1) ? (uint8_t)(((uint32_t)i*255u)/(count-1)) : 0;   // pozíció 0..255
  uint8_t phase=(uint8_t)((now*(uint32_t)(m.speed+1)/30u)+sideOffset+i*(m.motion+1));
  uint8_t v=m.brightness;
  switch(fx){
    case MOOD_BREATHE:  v=beatsin8(19, scale8(m.brightness,115), m.brightness); break;   // ~3.1s ciklus
    case MOOD_RAINBOW:  h+=phase; break;
    case MOOD_SLOW_COLOR: h+=phase/8; break;
    case MOOD_WARM:     h=18; v=m.brightness; break;
    case MOOD_COLOR_WAVE: h+=scale8(p8,96)+phase; break;
    case MOOD_COMET: {
      h+=phase;
      uint8_t head=beat8(50);                      // 0..255 fűrészjel ~1.2s
      uint8_t dist=(head>=p8)?(head-p8):(p8-head);
      uint8_t env=(dist>=64)?0:(uint8_t)(255-dist*4);
      v=qadd8(scale8(m.brightness,64), scale8(m.brightness, scale8(env,191)));
    } break;
    case MOOD_TWINKLE: { uint8_t level=((((i*37u+phase*13u)%100u)<(20u+m.density))?255u:51u); v=scale8(m.brightness,level); } break;
    case MOOD_PULSE:    v=beatsin8(53, scale8(m.brightness,51), m.brightness); break;    // ~1.1s ciklus
    case MOOD_METEOR:   h+=phase; break;
    case MOOD_CYBER:    h=scale8(p8,40)+phase; break;
    case MOOD_SPECTRAL: h=p8+phase; break;
    default: break;
  }
  CHSV hsv(h,m.saturation,v); CRGB c; hsv2rgb_rainbow(hsv,c); return c;
}
void renderMoodSide(const MoodConfig& m,uint16_t start,uint16_t count,bool rev,uint8_t sideOffset=0){
  for(uint16_t i=0;i<count;i++){
    uint16_t idx=rev?(start+count-1-i):(start+i);
    if(idx>=LED_COUNT)continue;
    if(m.mode==0) leds[idx]=CRGB::Black;
    else leds[idx]=moodColor(m,i,count,sideOffset);
  }
}
void renderMood(){MoodConfig L=leftMood,R=rightMood;bool revR=false;uint8_t offR=0;if(moodLinkMode==MOOD_LINK_MIRROR){R=L;}else if(moodLinkMode==MOOD_LINK_SYMMETRIC){R=L;revR=true;R.hue=(R.hue+180)%360;}else if(moodLinkMode==MOOD_LINK_FLOW){R=L;offR=128;}renderMoodSide(L,moodLeftStart,moodLeftCount,false,0);renderMoodSide(R,moodRightStart,moodRightCount,revR,offR);}

/* ===== PERSISTENCE (NVS) =============================================== */

void setDefaultMapping(){
  segmentCount=4; uint16_t per=LED_COUNT/4; const uint8_t src[4]={SRC_L0,SRC_L1,SRC_R0,SRC_R1};
  for(uint8_t i=0;i<4;i++){segments[i].start=i*per;segments[i].count=per;segments[i].source=src[i];segments[i].brightness=255;segments[i].reverse=false;}
}

void saveConfig(){
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
  prefs.putBool("clone_en",sideCloneEnabled);prefs.putUChar("clone_br",sideCloneBrightness);prefs.putUShort("cl_st",cloneLeftStart);prefs.putUShort("cl_ct",cloneLeftCount);prefs.putUShort("cr_st",cloneRightStart);prefs.putUShort("cr_ct",cloneRightCount);prefs.putBool("cl_rev",cloneLeftRev);prefs.putBool("cr_rev",cloneRightRev);
  prefs.end(); saveMapper();
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
  sideCloneEnabled=prefs.getBool("clone_en",true);sideCloneBrightness=prefs.getUChar("clone_br",255);cloneLeftStart=prefs.getUShort("cl_st",30);cloneLeftCount=prefs.getUShort("cl_ct",30);cloneRightStart=prefs.getUShort("cr_st",90);cloneRightCount=prefs.getUShort("cr_ct",30);cloneLeftRev=prefs.getBool("cl_rev",false);cloneRightRev=prefs.getBool("cr_rev",false);
  prefs.end();
  if(legacyAuthMigrated){
    prefs.begin("cfg",false);
    prefs.remove("webpw");
    prefs.end();
    Serial.println("[AUTH] legacy plaintext password migrated and removed");
  }
  // [FIX] A mappert a séma-migráció ELŐTT kell betölteni! A migrációs saveConfig()
  //       a végén saveMapper()-t is hív, így ha a szegmensek még nincsenek
  //       memóriában (boot után nullázva), a tárolt LED-mapping felülíródna.
  //       A config és a mapper külön NVS-kulcsokon van, de a saveConfig()->saveMapper()
  //       lánc miatt EGYÜTT migrálódnak → EGYÜTT is kell tesztelni (Regression Guard).
  loadMapper(true);
  if(fresh){
    saveConfig();
    Serial.println("[CFG] friss telepítés — alapértelmezett konfig");
  } else {
    if(segmentCount==0){ setDefaultMapping(); saveMapper(); }
    if(stored>0 && stored<CONFIG_SCHEMA_VERSION){
      Serial.printf("[CFG] konfig séma frissítve v%u -> v%u\n",stored,CONFIG_SCHEMA_VERSION);
      saveConfig();   // most már a helyesen betöltött mappert menti tovább
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
    FastLED.setBrightness(globalBrightness);
    FastLED.show();
  }

  // [WDT] watchdog karbantartás – ha a loop() beragad, a chip újraindul
  esp_task_wdt_reset();

  yield();
}