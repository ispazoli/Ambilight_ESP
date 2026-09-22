# Ambilight ESP

ESP32-C3 based Philips Ambilight bridge and WS2815 LED controller.

The project combines a Philips Ambilight TV integration with a configurable WS2815 LED strip, a browser-based Smart Control Center, configurable LED mapping, Mood Studio effects, Smart Engine scene analysis, TV synchronization, diagnostics and OTA firmware update support.

## Project status

This repository is the clean public project root for the Ambilight ESP development line.

Current known components:

- ESP32-C3 firmware
- WS2815 12 V LED output
- 120 LED logical target configuration
- Browser Smart Control Center
- WebSocket primary transport with HTTP fallback
- configurable 4-side / 12-segment mapper
- Mood Studio with 23 canonical effect IDs
- Smart Engine
- Philips TV Ambilight integration
- NVS configuration persistence
- OTA update interface
- diagnostics and health endpoints

**Important:** the browser UI currently published under `docs/index.html` is the V5 web UI supplied for this project. Firmware/browser API parity is being validated separately; do not assume every UI control is compatible with every firmware build until the regression gate passes.

## Hardware target

Reference target:

- ESP32-C3
- WS2815, 12 V
- 60 LED/m strip
- logical LED count: 120
- LED data GPIO: GPIO4 in the current reference firmware
- Philips Ambilight-compatible TV on the same LAN

Hardware wiring, power injection, grounding and level/protection considerations must be verified for the actual installation.

## Repository layout

```text
Ambilight_ESP/
├── config/
│   └── secrets.example.h
├── docs/
│   └── index.html
├── firmware/
│   └── README.md
├── tools/
│   └── README.md
├── web/
│   └── README.md
├── .gitignore
├── CONTRIBUTING.md
├── LICENSE
├── README.md
└── SECURITY.md
```

The firmware source is kept separate from the browser application so that firmware, API contract and UI changes can be reviewed independently.

## Canonical Mood IDs

The browser and firmware contract currently defines 23 effect IDs:

| ID | Effect |
|---:|---|
| 0 | Static |
| 1 | Breathe |
| 2 | Rainbow |
| 3 | Slow Color |
| 4 | Warm |
| 5 | Color Wave |
| 6 | Comet |
| 7 | Twinkle |
| 8 | Plasma |
| 9 | Fire |
| 10 | Palette Wave |
| 11 | Aurora |
| 12 | Ocean |
| 13 | Fire 2 |
| 14 | Energy Pulse |
| 15 | Meteor Shower |
| 16 | Nebula |
| 17 | Starfield |
| 18 | Organic Flow |
| 19 | Cyber Flow |
| 20 | Spectral |
| 21 | Lava Lamp |
| 22 | Plasma X |

Do not reorder these IDs casually. They are a wire/storage compatibility contract.

## Configuration and secrets

Never commit real Wi-Fi credentials, web authentication passwords, API keys or other private credentials.

Use:

`config/secrets.example.h`

as the template for a local `secrets.h`. The local secrets file is ignored by Git.

## Development gates

Before calling a firmware/browser release ready:

1. Firmware compiles cleanly for the target ESP32-C3 board.
2. Firmware starts without runtime initialization errors.
3. HTTP health/config endpoints respond.
4. Browser connects through WebSocket and HTTP fallback.
5. Configuration save/load round-trips correctly.
6. LED mapper edits persist correctly.
7. Mood IDs round-trip without remapping errors.
8. Smart Engine realtime bridge works.
9. TV synchronization works.
10. OTA is validated with a known-good image.
11. 3×10 smoke testing is completed.
12. Firmware ↔ browser parity/inventory is clean.
13. No real secrets are present in the repository or history.

## Public repository policy

This repository is intended to be public. Do not place:

- Wi-Fi SSIDs/passwords
- authentication passwords
- API keys
- tokens
- private certificates/keys
- firmware binaries containing embedded secrets
- personal network credentials

in committed source.

## License

MIT. See [LICENSE](LICENSE).


**Current release:** `v5.6.1-C3-HARDENED-MERGED` is now the canonical ESP32-C3 firmware. `web/index.html`, `docs/index.html` and `firmware/embedded_ui.h` are synchronized to this release; historical firmware files remain for traceability.
