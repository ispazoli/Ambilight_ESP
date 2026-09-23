# Ambilight Bridge — v5.6.3-C3-CHAOSWAVE-MOOD

## Release

This release merges the corrected Mood Turbulence renderer into the v5.6.2 Mood Engine while keeping CONFIG_SCHEMA_VERSION = 11, REST parameters and Mood IDs compatible.

### Turbulence correction

The turbulence control now affects all eight noise-based Mood effects: FIRE, FIRE 2, ORGANIC FLOW, LAVA LAMP, STARFIELD, TWINKLE, NEBULA and PLASMA X. The moodTurb8() helper changes spatial noise frequency/chaos rather than amplitude, so threshold-driven effects retain their density semantics.

### Verification supplied with the release

- JavaScript syntax: OK (node --check)
- Browser/embedded UI parity: OK
- HTML IDs: 107, duplicates: 0
- Firmware braces: 329/329
- Mood cases: 23/23
- ESP32-C3 Arduino compilation was not available in the local environment; perform the hardware compile/flash test before OTA.

### Compatibility

- CONFIG_SCHEMA_VERSION: 11
- No NVS schema change
- No REST/Mood-ID change
- OTA-compatible partition scheme remains min_spiffs

The unique release identifier is 5.6.3-C3-CHAOSWAVE-MOOD.