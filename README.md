# Ambilight Bridge — Control Center preview

This **gh-pages** branch is the isolated browser preview line for the Soft-UI Control Center.

### Frozen reference
- **reference/ambilight_preview_v1.0.html** is the uploaded preview before this enhancement pass.

### Preview build
- **index.html** keeps the existing browser JavaScript and firmware API endpoints.
- **preview-demo.js** is the isolated demo/visual layer; it does not add firmware endpoints or change the API contract.
- Demo firmware display is aligned with the known baseline: **5.5.2-C3-OFF-FIX**, schema 10, contract 1.

### Added UI work
- Dashboard control-center hero and realtime TV visualization.
- Visual 12-segment TV Mapper.
- LEFT/RIGHT 30-LED Mood Studio live browser preview.
- Smart Engine controls bound to existing firmware configuration fields.
- Demo timers isolated to demo-owned handles.

### Main branch safety
This branch is independent from **main**. Firmware files, **web/index.html**, **docs/index.html** and embedded UI files are not modified by this preview build.
