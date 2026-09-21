# Ambilight Regression Guard

## R6 — TV IP persistence across reboot

Purpose: verify that the configured TV IP is not overwritten by mapper loading during boot.

Procedure:

1. Set a non-default TV IP in the web UI.
2. Save the configuration.
3. Read `/api/state` and record `tvIP`.
4. Reboot the ESP32-C3.
5. Wait for WiFi/web services to become available.
6. Read `/api/state` again.
7. PASS when the post-reboot `tvIP` exactly matches the pre-reboot value.

Expected invariant:

`tvIP_after_reboot === tvIP_before_reboot`

Failure signature:

- TV IP returns to `DEFAULT_TV_IP` after reboot.
- `loadMapper()` must not modify `tvIP`.

This guard is intentionally independent of the mapper segment values so a mapper change cannot mask a TV-IP persistence regression.
