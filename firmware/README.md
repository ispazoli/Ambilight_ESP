# Ambilight Bridge firmware

## Canonical release

**v5.6.3-C3-CHAOSWAVE-MOOD**

Use `firmware/AMBILIGHT_BRIDGE_v5_6_3_C3_CHAOSWAVE_MOOD.ino`.

Target: ESP32-C3 + WS2815 12 V, 120 logical LEDs.

The release uses Philips JointSPACE v1 HTTP/1.0 with explicit connection close, Measured Ambilight data as the default source, rotating TV status polling, explicit unknown TV power state where the TV API has no `/1/system/power` endpoint, Mood dynamic brightness, and the synchronized Control Center.

`firmware/embedded_ui.h` is generated from `web/index.html`; do not edit it directly.

Older v5.x firmware files remain for rollback and comparison.
