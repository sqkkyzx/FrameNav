# FrameNav Android

Standalone Android app for connecting to FrameNav, editing device settings over its local IP API, and sending phone location over BLE. The device access point is fixed by firmware as `FrameNav-<last six AP MAC digits>` with password `framenav`.

## Build

Open this `android-app` directory in Android Studio. Install Android SDK Platform 35 and use JDK 17. Build the `app` configuration to produce `app/build/outputs/apk/debug/app-debug.apk`.

The project uses Android Gradle Plugin 8.6.1, which requires Gradle 8.7 or newer. Minimum supported Android version is API 26 (Android 8.0).

## Device connection

- BLE discovery looks for the `FrameNav` name or the phone-location service UUID.
- Location writes use the 28-byte little-endian location frame. The characteristic requires an encrypted BLE connection, so Android may show a pairing prompt on the first write.
- BLE service discovery silently reads configuration, compact telemetry, and paired-device information in the background. Configuration changes still use the fixed device AP at `10.254.254.1`; the AP SSID and password are fixed by firmware and are not editable in the app.
- The device AP is only used for explicit configuration changes, manual-location changes, and the opt-in location fallback. BLE is the default location transport.
- Continuous phone location runs as a foreground service and writes fresh fixes over BLE at the configured interval, warns after 30 seconds without BLE, and stops after 2 minutes without a connection.

The Wi-Fi fallback uses the device's local `/location/clock`, `/location/phone`, `/location/phone/clear`, `/` and `/save` endpoints. It sends requests through the selected Wi-Fi `Network` so Android does not route local device traffic over mobile data.

## Permissions

The app requests nearby-device access for BLE on Android 12+, coarse or precise location for phone positioning, and notification permission for the continuous location foreground service. Approximate location is supported through Android's network location provider.

## Validation

Building and device-level BLE/Wi-Fi testing require Android Studio or a local Android SDK, JDK 17, and an Android device.
