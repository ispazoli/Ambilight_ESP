# Web UI

## Single source of truth

The complete Control Center UI lives in:

`web/index.html`

This is the **only hand-maintained UI source**. It contains the coupled HTML, CSS and JavaScript and must preserve all existing behavior, IDs, handlers, endpoint names, payloads and initialization order.

### Generated artifacts

Run:

```bash
python tools/build_web_ui.py
python tools/browser_parity_guard.py
```

The build generates:

- `docs/index.html` — browser/GitHub Pages artifact
- `firmware/embedded_ui.h` — PROGMEM firmware embedding

The firmware sketch:

`firmware/AMBILIGHT_BRIDGE_v5_5_1_C3_HARDENED.ino`

includes the generated header instead of containing a second copy of the UI.

**Do not edit `docs/index.html` or `firmware/embedded_ui.h` manually.** Changes belong in `web/index.html`, followed by the build.

## Browser regression gate

At minimum verify:

- connect/disconnect;
- health polling;
- WebSocket connection;
- HTTP fallback;
- settings save;
- Wi-Fi save;
- authentication save;
- mapper configuration;
- mapper preview;
- Mood Studio;
- all 23 effect IDs;
- Smart Engine;
- TV sync;
- LED tests;
- OTA;
- diagnostics.

The parity guard additionally checks the single-source invariant, firmware embedding, HTML ID parity, API endpoint contract and 23-effect contract.
