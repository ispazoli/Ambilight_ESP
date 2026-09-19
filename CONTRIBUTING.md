# Contributing

## Development principle

The primary rule for this project is:

> Add or repair functionality without silently removing an existing feature.

Changes affecting firmware, browser UI, API contracts, configuration storage or LED mapping should be reviewed for regression impact.

## Branches

Use focused branches such as:

```text
feat/<short-name>
fix/<short-name>
refactor/<short-name>
test/<short-name>
docs/<short-name>
```

Do not develop directly on `main) unless the change is trivial and intentionally committed there.

## Before submitting a change

For firmware:

- compile for the actual ESP32-C3 target;
- check serial startup output;
- verify HTTP endpoints;
- verify WebSocket behavior;
- verify NVS save/load;
- verify LED mapping;
- verify Mood Studio;
- verify TV synchronization;
- verify OTA if touched.

For browser changes:

- test desktop layout;
- test mobile layout;
- test connection/reconnection;
- test WebSocket fallback;
- test every save action;
- test mapper persistence;
- test all 23 Mood IDs;
- check browser console for errors;
- verify endpoint names and payloads against firmware.

## API compatibility

Treat endpoint names, JSON field names, configuration schema and Mood IDs as compatibility contracts.

Do not rename or reorder fields simply for cosmetic reasons.

## Regression testing

The project uses a firmware ↔ browser parity/inventory approach.

A change is not considered complete when it merely compiles. The affected feature must also be reachable from the browser and connected to the corresponding firmware endpoint/state.

## Pull requests

A pull request should describe:

- what changed;
- why it changed;
- files affected;
- compatibility impact;
- tests performed;
- known limitations.

Do not claim a test passed unless it was actually executed.

## Secrets

Never commit real credentials. Use the ignored local secrets template described in `SECURITY.md`.
