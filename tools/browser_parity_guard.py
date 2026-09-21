#!/usr/bin/env python3
"""Deterministic firmware/browser UI parity guard."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "web" / "index.html"
DOCS = ROOT / "docs" / "index.html"
HEADER = ROOT / "firmware" / "embedded_ui.h"
FW = ROOT / "firmware" / "AMBILIGHT_BRIDGE_v5_5_1_C3_HARDENED.ino"

def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)

html = SOURCE.read_text(encoding="utf-8")
docs = DOCS.read_text(encoding="utf-8")
header = HEADER.read_text(encoding="utf-8")
fw = FW.read_text(encoding="utf-8")

if docs != html:
    fail("docs/index.html differs from web/index.html")

m = re.search(r'R"AMB_CC_HTML\(\n(.*?)\)AMB_CC_HTML";', header, re.S)
if not m:
    fail("firmware/embedded_ui.h has no valid generated UI payload")
if m.group(1) != html:
    fail("firmware/embedded_ui.h payload differs from web/index.html")

if '#include "embedded_ui.h"' not in fw:
    fail("firmware sketch does not include generated embedded_ui.h")
if 'static const char AMBILIGHT_CC_HTML[] PROGMEM = R"AMB_CC_HTML(' in fw:
    fail("firmware sketch still contains a duplicate embedded UI")

ids = re.findall(r'id="([^"]+)"', html)
if len(ids) != len(set(ids)):
    fail("duplicate HTML id detected")
if len(ids) != 103:
    fail(f"HTML ID count changed: expected 103, got {len(ids)}")

expected_endpoints = [
    "/api/auth", "/api/capabilities", "/api/config", "/api/ledtest",
    "/api/mapper", "/api/mood", "/api/ota", "/api/realtime",
    "/api/reboot", "/api/sideclone", "/api/state", "/api/tv", "/api/wifi",
]
for ep in expected_endpoints:
    if ep not in html:
        fail(f"missing browser endpoint: {ep}")

effects = re.search(r'const EFFECTS=\[(.*?)\];', html, re.S)
if not effects:
    fail("EFFECTS contract not found")
effect_count = len(re.findall(r'"(?:[^"\\]|\\.)*"', effects.group(1)))
if effect_count != 23:
    fail(f"effect count changed: expected 23, got {effect_count}")

# Firmware source inventory: these symbols are required by the current hardened build.
REQUIRED_FIRMWARE_SYMBOLS = [
    "setup()", "loop()", "readAmbilight", "renderZonesToLeds", "renderMood",
    "saveConfig", "loadConfig", "saveMapper", "loadMapper",
    "setupRoutes", "broadcastRealtime", "broadcastMoodStatus",
    "handleWiFi", "connectWiFi", "handleTVWatchdog", "effectiveBrightness",
    "zoneFromSource", "gradientSource", "webAuthCheck", "webAuthSilent",
]
for symbol in REQUIRED_FIRMWARE_SYMBOLS:
    if symbol not in fw:
        fail(f"firmware symbol missing: {symbol}")

# Endpoint inventory: firmware must still expose every browser-used API contract.
for ep in expected_endpoints:
    if ep not in fw:
        fail(f"firmware endpoint missing: {ep}")

# Mapper/source contracts must exist in both browser and firmware.
for token in ["GRAD_TOP", "GRAD_RIGHT", "GRAD_BOT", "GRAD_LEFT", "L0R0_BL", "L1R1_BL"]:
    if token not in html:
        fail(f"browser mapper token missing: {token}")
for token in ["SRC_GRADIENT_TOP", "SRC_GRADIENT_RIGHT", "SRC_GRADIENT_BOTTOM", "SRC_GRADIENT_LEFT", "SRC_L0_R0_BLEND", "SRC_L1_R1_BLEND"]:
    if token not in fw:
        fail(f"firmware mapper token missing: {token}")

# Fixed physical mapper contract: 4 sides × 3 segments × 10 LEDs = 120 LEDs.
for token in ["MAPPER_SIDE_COUNT", "MAPPER_SEGMENTS_PER_SIDE", "MAPPER_LEDS_PER_SEGMENT"]:
    if token not in fw:
        fail(f"fixed mapper constant missing: {token}")
if "segmentCount=MAX_SEGMENTS" not in fw or "exactly 12 fixed segments required" not in fw:
    fail("fixed 12-segment mapper contract missing")
if "fixedSegmentStart(i)" not in fw or "MAPPER_LEDS_PER_SEGMENT" not in fw:
    fail("fixed 3x10 LED addressing contract missing")
for token in ["FIXED_MAPPER_SEGMENTS=12", "FIXED_MAPPER_LEDS=10"]:
    if token not in html:
        fail(f"browser fixed mapper constant missing: {token}")

# Critical runtime invariants.
if "loadMapper(true);" not in fw or "saveConfig(true)" not in fw:
    fail("mapper persistence/migration invariant missing")
if "tvIP=DEFAULT_TV_IP" not in fw or 'prefs.getString("tvip"' not in fw:
    fail("TV-IP NVS persistence invariant missing")

print("  Firmware symbol inventory: present")
print("  Firmware endpoint inventory: present")
print("  Mapper source contract: present")
print("  Fixed mapper: 4 sides × 3 segments × 10 LEDs = 120")
print("  Persistence invariants: present")

print("PASS: single-source UI parity")
print("  source/docs: identical")
print("  source/embedded header: identical")
print("  firmware: generated header included, duplicate UI absent")
print("  HTML IDs: 103/103")
print("  Mood effects: 23/23")
print("  API endpoint contract: present")
