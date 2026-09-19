# Web UI

The browser control center provides:

- Dashboard
- LED Mapper
- Mood Studio
- Smart Engine
- TV Ambilight
- Settings
- Diagnostics
- WebSocket realtime communication
- HTTP polling fallback
- configuration persistence
- Wi-Fi/authentication controls
- OTA upload UI

## Current published UI

The current complete V5 UI is published as:

`docs/index.html`

It is intentionally kept as the immediately testable browser artifact while firmware/browser parity is being audited.

## Important

Do not split the monolithic HTML into `index.html`, `app.js` and `styles.css` until the behavior inventory has been completed. The current document contains CSS and JavaScript that are coupled to the page structure.

A split should preserve behavior, endpoint names, payloads, IDs, event handlers and initialization order.

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
