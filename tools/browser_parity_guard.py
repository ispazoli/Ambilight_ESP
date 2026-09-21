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
if len(ids) != 105:
    fail(f"HTML ID count changed: expected 105, got {len(ids)}")

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

print("PASS: single-source UI parity")
print("  source/docs: identical")
print("  source/embedded header: identical")
print("  firmware: generated header included, duplicate UI absent")
print("  HTML IDs: 105/105")
print("  Mood effects: 23/23")
print("  API endpoint contract: present")
