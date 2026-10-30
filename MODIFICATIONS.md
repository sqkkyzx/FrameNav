# FrameNav modifications to AlphaLoc

FrameNav is derived from [finfinack/alphaloc](https://github.com/finfinack/alphaloc)
and remains licensed under the Apache License, Version 2.0.

The initial FrameNav work in this repository is based on AlphaLoc commit
[`43f5ea52ec157c2f33e9831f8f91065f56f800ac`](https://github.com/finfinack/alphaloc/commit/43f5ea52ec157c2f33e9831f8f91065f56f800ac).

## Principal changes

- Sized TLS output for complete certificate chains and bounded concurrent HTTP
  and HTTPS sessions with idle-session eviction to prevent socket exhaustion.
  TLS releases temporary buffers and handshake credentials; the configuration
  page streams bounded fragments to avoid large allocations during HTTPS use.
  Suppressed favicon redirects to prevent redundant configuration-page downloads.

- Added manual phone geolocation over the HTTPS configuration page, with a
  configurable expiry in hours (six hours by default), continuously advancing
  camera UTC, and immediate priority for fresh GNSS. Camera delivery, Web status
  and track recording share this selector. The current receiver runs continuously.
  Phone acquisition first allows standard accuracy with a recent 30-second cache,
  then retries with high accuracy if the first request is unavailable or times out.

- Added USB-cable-gated Wi-Fi configuration with a configurable idle timeout.
- Start the local AP with USB configuration access and cap upstream retry
  backoff so failed router searches do not continuously interrupt the AP.
- Added a standalone HTTPS-hosted phone-location page that writes location over
  a dedicated BLE GATT service, plus Compose deployment and GHCR publishing.
- Added build targets and board integration for Seeed Studio XIAO ESP32-C6.
- Added StarNav M10-K18 UART/NMEA integration, GNSS rate control, standby
  protocol handling, and connection-aware GNSS duty cycling.
- Improved Sony camera discovery, pairing, bond management, reconnect behavior,
  and periodic location delivery.
- Added XIAO status-LED behavior and USB/power-source observation and policy.
- Added a 1500 KiB internal-Flash circular track store with three-second,
  16-byte UTC/latitude/longitude/altitude records and power-loss recovery.
- Reworked the Wi-Fi configuration interface as a Chinese-first FrameNav Web UI
  with captive-portal support, camera bond management, Bluetooth power profiles,
  phone-location controls, firmware version, and live track-storage status.
- Added host-side tests and diagnostic tools. Private hardware validation logs
  and experiment records are not included in the public repository.
- Renamed the user-facing project and device identity from AlphaLoc to FrameNav.

The internal `ALPHALOC_*` build-option and include-guard names are retained in
some places for source compatibility and to make the project's lineage clear.

## Change notice

Files carrying a `FrameNav modification of AlphaLoc` header contain changes to
files inherited from AlphaLoc. Newly added files are part of FrameNav and are
covered by the repository's Apache-2.0 license unless a file says otherwise.

This document is a summary, not a replacement for the Git history or LICENSE.
