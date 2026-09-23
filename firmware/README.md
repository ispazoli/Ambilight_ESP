# Ambilight Bridge firmware

## Canonical release

**v5.6.2-C3-MOOD-ENGINE**

Use `firmware/AMBILIGHT_BRIDGE_v5_6_2_C3_MOOD_ENGINE_FIXED.ino`.

Target: ESP32-C3 + WS2815 12 V, 120 logical LEDs.

v5.6.2 keeps the v5.6.1 security, networking, mapper, TV-sync and OTA architecture and adds the unified 23-effect Mood Engine with the corresponding browser controls.

Release package:

- `firmware/AMBILIGHT_BRIDGE_v5_6_2_C3_MOOD_ENGINE_FIXED.ino`
- `firmware/embedded_ui.h`
- `web/index.html`
- `firmware/OLVASS_EL_v5_6_2.txt`

`firmware/embedded_ui.h` is generated from `web/index.html`; do not edit it directly.

Older v5.x firmware files remain for rollback and comparison.

> Build status: source/static checks are prepared, but the target ESP32-C3 Arduino toolchain is not available in the local environment. The repository GitHub Actions build is the authoritative compile gate.
