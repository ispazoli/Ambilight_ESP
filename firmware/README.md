# Firmware

This directory contains the ESP32-C3 firmware for the Ambilight controller.

## Target

- ESP32-C3
- WS2815 12 V
- reference LED data GPIO: GPIO4
- reference logical LED count: 120

## Current reference firmware

The canonical firmware source should be placed here only after it has passed the compile and regression gates.

The browser UI already published in `docs/index.html` must not be considered automatically compatible with an arbitrary firmware build. Endpoint names, JSON payloads, configuration fields and Mood IDs must match.

## Local secrets

Create a local `secrets.h` from:

```text
../config/secrets.example.h
```

Keep the generated file outside Git.

## Build checklist

Before flashing:

1. Select the correct ESP32-C3 board.
2. Verify the LED GPIO.
3. Verify LED count.
4. Verify power and common ground.
5. Verify local Wi-Fi credentials.
6. Compile without warnings that indicate functional problems.
7. Review the serial boot log.
8. Confirm the HTTP port and WebSocket port.
9. Verify the device health endpoint.
10. Perform the browser smoke test.

Do not flash an unverified binary to production hardware.
