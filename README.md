# 帧迹 FrameNav

FrameNav is an ESP32 application that allows embedding GPS location data directly into Sony Alpha camera photos via Bluetooth Low Energy (BLE). It acts as a bridge between a standard GPS module and your Sony camera, providing real-time geolocation tagging.

> **开源来源 / Open-source origin**
> FrameNav is derived from [finfinack/alphaloc](https://github.com/finfinack/alphaloc).
> AlphaLoc and the FrameNav modifications are distributed under the
> [Apache License 2.0](LICENSE). The principal changes are documented in
> [MODIFICATIONS.md](MODIFICATIONS.md).

FrameNav is an independent community project. It is not affiliated with,
endorsed by, or sponsored by Sony Group Corporation.

## Features

### 当前版本：0.18.0-test4（实验性）

当前实测组合为 **XIAO ESP32-C6 + StarNav M10-K18 + 18350 电池 + BMI160 + Sony α7C II**。
`minimal-implementation` 分支保留引入加速度传感器前的 0.17.0 最小实现；`main` 是当前开发版本。
使用 PlatformIO Core CLI，无需 VS Code：

```sh
pio run -e xiao-c6-gnss-power
pio run -e xiao-c6-gnss-power -t upload
pio device monitor -b 115200
```

请显式指定上述环境；默认 `xiao-c6-debug` 是 Fake GPS 测试固件。
GNSS TX 接 D7/GPIO17，RX 接 D8/GPIO19，GND 共地；供电和串口电平应先按模块实物确认。
BMI160 SDA 接 D9/GPIO20，SCL 接 D10/GPIO18，INT1 接 D0/GPIO0。
传感器由 3.3V 电源供电且须在深睡期间保持供电；模块 VIN/3V3 接法应以实际稳压电路为准。
电池接入板载 BAT 正负端，不能接到 3V3；电芯保护、极性及 GNSS 电池供电路径需自行确认。
电池供电时不能假设 VBUS 有 5V 输出。
**不要将 VBUS 和 3V3 直接并联。**

XIAO 使用板载单色状态灯（不是下文上游参考硬件的 NeoPixel）：
未连接相机时闪烁，连接后常亮；常亮不代表相机已接受有效定位。
Wi-Fi 配置入口由 USB 供电来源检测控制，电池与已识别的热靴供电不自动启动 Wi-Fi，但可通过画圈手势开启。
热靴识别依据实测 USB 信号特征，是启发式判断，并非通用热靴协议识别。
连接热点后可尝试自动弹出的配置页，或访问 **http://10.254.254.1/**。
默认热点密码为 `framenav1234`，请在配置页修改。

未连接相机、无 Wi-Fi 客户端及近期配置页访问时，持续无动作约 5 分钟可进入深睡；运动中断唤醒。
连接电脑 USB 数据主机时不深睡。画圈开启的热点在 30 秒无 Web 页面活动后关闭，手势冷却 60 秒。
已在测试固件验证电池画圈、纯电池冷启动画圈、深睡和运动唤醒；深睡唤醒后画圈与步行误触率仍待验证。
详见 [运动与深睡说明](MOTION-V18.md)。

持久化诊断仅在 `pio run -e xiao-c6-motion-debug` 测试目标启用，使用独立分区；
普通 `xiao-c6-gnss-power` 不包含该日志写入功能，详见 [测试诊断说明](MOTION-DEBUG.md)。

轨迹存储与掉电恢复仍属实验功能，目前尚无 GPX 导出接口。
下文同时保留上游其他板型的参考说明；LED、配置窗口及默认参数不一定适用于 XIAO 环境。

### 功能概览

- **Automatic Geotagging**: Sends latitude, longitude, and UTC time to the camera. GNSS altitude is parsed for local use but is not currently encoded in the known Sony BLE payload.
- **BLE Connection**: Emulates a smartphone location provider for Sony cameras.
- **Local Track Fallback**: Stores a 16-byte binary position record every three
  seconds in a 1500 KiB circular internal-Flash partition. Each record contains
  UTC time, latitude, longitude, and altitude.
- **Dual Configuration**: Configurable via a Web Interface (WiFi) or BLE characteristics.
- **Status Indicators**: NeoPixel (RGB LED) feedback for Camera connection, GPS fix, Battery, and WiFi status.
- **Low Power**: Supports disabling external peripherals (like Stemma QT) to save power.

## Hardware Support

The project is set up for **PlatformIO** and currently supports:

| Board | Environment | Pinout |
|-------|-------------|--------|
| [**DFRobot Beetle ESP32-C6**](https://wiki.dfrobot.com/SKU_DFR1117_Beetle_ESP32_C6) | `esp32c6` | GPS TX: 5, GPS RX: 4, NeoPixel: 6 |
| [**Adafruit Feather ESP32-S3**](https://learn.adafruit.com/adafruit-esp32-s3-feather/overview)| `esp32s3` | GPS TX: 38, GPS RX: 39, NeoPixel: 6 |
| [**Seeed Studio XIAO ESP32-C6**](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) | `xiao-c6-gnss-power` | MCU TX: D8/GPIO19 → GNSS RX; MCU RX: D7/GPIO17 ← GNSS TX |

You can easily adapt it to other ESP32 boards by modifying `platformio.ini`.

### Upstream reference build

The following is a parts list of what I ended up with.

- [Adafruit ESP32-S3 Feather](https://www.adafruit.com/product/5885), [Overview](https://learn.adafruit.com/adafruit-esp32-s3-feather/overview)
- [Adafruit Ultimate GPS featherwing](https://learn.adafruit.com/adafruit-ultimate-gps-featherwing)
- [SparkFun RGB LED Breakout - WS2812B](https://www.sparkfun.com/sparkfun-rgb-led-breakout-ws2812b.html)
- [Adafruit WiFi Antenna with w.FL / MHF3 / IPEX3 Connector](https://www.adafruit.com/product/5445)
    - This is optional if you don't have an ESP32 with a PCB antenna.
- Mini sliding switch (8.5mm x 3.7mm, 3 pin, slider 4mm tall)
- The antenna of a GY-GPS6MV2
- 2x Panasonic NCR18650GA Battery (3450mAh)
- Wires, electrical tape and whatnot

### Models

- [**Holder**](models/holder.3mf): Holder for the batteries, ESP32, optional wifi antenna, ...
- [**Cover**](models/cover.3mf): Cover with a press fit for the holder and room for the LED board.
- [**Cover Large**](models/cover_large.3mf): Same as cover but taller and with room for a larger, external GPS antenna.

## Usage

### 1. Initial Setup

1. Connect your GPS module (e.g., GY-GPS6MV2 or an Adafruit Ultimate GPS Featherwing) to the defined TX/RX pins.

2. Flash the firmware using PlatformIO.

   ```bash
   pio run -e xiao-c6-gnss-power -t upload
   ```

3. Power on the device.

### 2. Pairing with Camera

1. On your Sony Alpha camera, go to **Network** -> **Bluetooth Settings**.
2. Enable **Bluetooth Function**.
3. Select **Pairing**.
4. The camera should discover a device named `FrameNav-XXXXXX`. Select it to pair.
5. Once paired, go to **Location Info. Link Set.** and enable it.
6. The camera should now show the GPS icon (solid when receiving data).

### 3. LED Status Codes

The NeoPixel LED provides a visual "heartbeat" every 5 seconds. It flashes multiple colors in sequence to report the status of different subsystems:

**Sequence:** [Camera Status] -> [GPS Status] -> [Battery Status] -> [WiFi Status] -> [Sleep]

1.  **First Flash: Camera Connection**
    *   🟢 **Green**: Connected to camera and bonded.
    *   🔵 **Blue**: Connected to camera but not bonded yet.
    *   🔴 **Red**: Not connected.
2.  **Second Flash: GPS Fix**
    *   🟢 **Green**: Valid 3D GPS Fix acquired.
    *   🟣 **Violet**: *Fake* GPS fix (injected when `ALPHALOC_FAKE_GPS=1`).
    *   🔴 **Red**: No fix (searching for satellites).
3.  **Third Flash: Battery Level (Optional)**
    *   🟢 **Green**: > 50%
    *   🟡 **Yellow**: > 30%
    *   🔴 **Red**: <= 30%
4.  **Fourth Flash: WiFi/Config (Optional)**
    *   Only appears during the startup "Config Window" (first 5 minutes).
    *   🔵 **Blue**: Web server is active.
    *   ⚫ **(Off)**: Config window closed, WiFi disabled to save power.

**Example**:
*   🔴-🔴-🟢-🔵: No camera, no GPS, battery good, config mode active (just turned on).
*   🔵-🟢-🟡-⚫: Camera connected (not bonded yet), GPS fixed, battery medium, normal operation (config closed).
*   🟢-🟢-🔴-⚫: Camera connected (bonded), GPS fixed, battery low, normal operation (config closed).
*   🔵-🟣-🟢-⚫: Camera connected (not bonded yet), fake GPS fixed, battery good, normal operation (config closed).

### 4. Configuration

> **⚠️ SECURITY WARNING**: The BLE configuration service is **DISABLED by default** for security reasons. When enabled, it allows **UNAUTHENTICATED access** to device settings including WiFi passwords. Only enable it (`ALPHALOC_BLE_CONFIG=1`) in trusted environments or for initial setup, then rebuild with it disabled.

On startup, FrameNav enters a **Configuration Window** (default 5 minutes). During this time, you can change settings via WiFi (if `ALPHALOC_WIFI_WEB=1`) or BLE (if `ALPHALOC_BLE_CONFIG=1`).

<img width="300" height="450" alt="web" src="https://github.com/user-attachments/assets/bd50c0e7-f0e7-41f6-8e6a-0d7c2ba5bdf6" />

#### Configuration Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| **Camera Name Prefix** | Filter which cameras to connect to. Useful if you have multiple Sonys. | `SonyA7` |
| **Camera MAC Prefix** | Connect only to a specific MAC address prefix. Empty matches any. | (Empty) |
| **TZ Offset** | Timezone offset in minutes (e.g., `60` for UTC+1). | `60` |
| **DST Offset** | Daylight Savings Time offset in minutes. | `60` |
| **WiFi SSID/Pass** | Credentials for connecting to your home WiFi. | `WiFi` / `changeme` |
| **AP SSID/Pass** | Credentials for the hotspot created by FrameNav. | `FrameNav` / `framenav1234` |
| **Max GPS Age** | How long (seconds) to reuse old coordinates if GPS signal is lost. | `300` |

#### Method A: WiFi Web Interface

FrameNav tries to connect to the configured WiFi as a client:

- **Default SSID**: `WiFi`
- **Default Password**: `changeme`
- **IP**: assigned via DHCP

If it can't connect to the WiFi, it starts an access point itself:

- **SSID**: `FrameNav`
- **Password**: `framenav1234`
- **Network**: `10.254.254.0/24`
- **Configuration URL**: http://10.254.254.1/

FrameNav advertises a captive portal through DHCP option 114, answers local DNS queries with
`10.254.254.1`, and redirects HTTP probe paths to the configuration page. Supported operating systems
will usually open the page automatically after joining; the direct URL remains the reliable fallback.

The Web interface is Chinese-first. Connection settings are shown first: it displays GNSS/camera status,
firmware version, current track-storage usage, and the earliest retained record time,
lists up to three saved camera bonds, can remove an individual local bond, and can open a 90-second
continuous scan for pairing a new camera. Location and Bluetooth power modes provide preset policies plus
collapsible advanced fields; preset fields are read-only, while Custom is editable. Runtime power changes
take effect immediately after saving. The pairing action disconnects the current camera but does not erase
other saved bonds.

To enable this, set the build flag (`ALPHALOC_WIFI_WEB=1` in `platformio.ini`).

#### Method B: BLE Configuration

You can use a generic BLE app (like nRF Connect) to write to the configuration service.

**Service UUID**: `B1F0B4D5-797B-5A9E-5B4F-4A1F01007EA1`

| Characteristic Name | UUID (Base `B1F0...`) | R/W | Type (Over BLE) | Description |
| ------------------- | --------------------- | --- | --------------- | ----------- |
| Camera Name Prefix  | `...02007EA1`         | R/W | String          | Camera name identifier |
| Camera MAC Prefix   | `...03007EA1`         | R/W | String          | MAC address filter |
| Timezone Offset     | `...04007EA1`         | R/W | String (Int)    | Minutes from UTC |
| DST Offset          | `...05007EA1`         | R/W | String (Int)    | Daylight Savings offset (minutes) |
| Wifi SSID           | `...07007EA1`         | R/W | String          | Station SSID |
| Wifi Password       | `...08007EA1`         | R/W | String          | Station Password |
| AP SSID             | `...09007EA1`         | R/W | String          | Access Point SSID |
| AP Password         | `...0A007EA1`         | R/W | String          | Access Point Password |
| Max GPS Age         | `...0B007EA1`         | R/W | String (Int)    | Max age of GPS lock in seconds |
| GPS Lock            | `...0C007EA1`         | R   | String (0/1)    | 1 when GPS lock is valid |
| GPS Satellites      | `...0D007EA1`         | R   | String (Int)    | Satellites in view (from GGA) |
| GPS Constellations  | `...0E007EA1`         | R   | String (Int)    | Bitmask: 1=GPS, 2=GLONASS |
| Camera Connected    | `...0F007EA1`         | R   | String (0/1)    | BLE camera link active |
| Camera Bonded       | `...10007EA1`         | R   | String (0/1)    | Link bonded (after pairing) |

#### Build-Time Secrets (Recommended)

If you want to keep WiFi credentials and camera filters out of git, place them in `platformio.secrets.ini` and keep the file ignored. The build merges these flags into every environment via `platformio.ini`.

Example `platformio.secrets.ini` with all supported defaults:

```ini
[secrets]
build_flags =
  -DALPHALOC_DEFAULT_WIFI_SSID="\"MyWifi\""
  -DALPHALOC_DEFAULT_WIFI_PASS="\"MyPass\""
  -DALPHALOC_DEFAULT_AP_SSID="\"FrameNav\""
  -DALPHALOC_DEFAULT_AP_PASS="\"framenav1234\""
  -DALPHALOC_DEFAULT_CAMERA_NAME_PREFIX="\"SonyA7\""
  -DALPHALOC_DEFAULT_CAMERA_MAC_PREFIX="\"AA:BB:CC\""
```

These values are used as defaults on first boot (or after a factory reset) and can still be changed later via the WiFi or BLE config UIs.

## BLE Client Details (Camera Link)

FrameNav acts as a BLE client to Sony cameras

### Advertisement Filters

- **Manufacturer data**: company ID `0x012D`, product ID `0x0003`
- **Optional name prefix filter**: `camera_name_prefix` (if set)
- **Optional MAC prefix filter**: `camera_mac_prefix` (if set)

### Services and Identifiers

FrameNav looks for two Sony services (UUIDs are built from 16-bit IDs using the Sony base UUID):

```
Sony base UUID (constructed):
  00000000-0000-8000-ffff-ffff-ffff-ffff
  with 16-bit words inserted at positions:
    first = 0xDD00 / 0xFF00
    second = 0xDD00 / 0xFF00
```

It accepts either 128-bit UUIDs or 16-bit UUIDs:

- **Location Service**: `0xDD00`
- **Remote Service**: `0xFF00`

### Characteristic Discovery

Within those services, FrameNav searches for:

**Location Service (DD00)**

- `0xDD21`: Flags read to determine if TZ/DST are required
- `0xDD30`: Lock location (0x00 - Off, 0x01 - On)
- `0xDD31`: Enable location updates (0x00 - Off, 0x01 - On)
- `0xDD11`: Location write characteristic; used for sending GPS payloads

Data is a 95 byte buffer

| Offset    | Description                                                  | Remark                                           |
|-----------|--------------------------------------------------------------|--------------------------------------------------|
| [0:1]     | Payload Length (exclude these two bytes)                     | 0x5D = 93 bytes                                  |
| [2:4]     | Fixed Data                                                   | 0x0802FC                                         |
| [5]       | Flag of transmitting timezone offset and DST offset required | 0x03 for transmit and 0x00 for do not transmit   |
| [6:10]    | Fixed Data                                                   | 0x0000101010                                     |
| [11:14]   | Latitude (multiplied by 10000000)                            | 0x0BF79E5E = 200777310 / 10000000 = 20.077731    |
| [15:18]   | Longitude (multiplied by 10000000) 	                       | 0x41C385A7 = 1103332775 / 10000000 = 110.3332775 |
| [19:20]   | 	UTC Year                                                   | 0x07E4 = 2020                                    |
| [21]      | 	UTC Month                                                  | 0x0B = 11                                        |
| [22]      | 	UTC Day                                                    | 0x05 = 5                                         |
| [23]      | 	UTC Hour                                                   | 0x04 = 4                                         |
| [24]      | 	UTC Minute                                                 | 0x02 = 2                                         |
| [25]      | 	UTC Second                                                 | 0x2A = 42                                        |
| [26:90]   | 	Zeros  	                                                   | 0x00                                             |
| *[91:92]  | 	Difference between UTC and current timezone in minutes     | 0x01E0 = 480min = 8h (UTC+8)                     |
| *[93:94]  | 	Difference for DST in current timezone in minutes          |                                                  |

**Remote Service (FF00)**

- `0xFF02` (notification characteristic; CCCD `0x2902`)

### Connection/Discovery Flow (simplified)

```
ADV (Sony) --> connect
  |
  +--> discover DD00 (Location Service)
  |      +--> find DD11, DD21, DD30, DD31
  |      +--> read DD21 flag byte
  |      +--> enable location writes on DD11
  |
  +--> discover FF00 (Remote Service)
         +--> find FF02
         +--> find/guess CCCD (0x2902)
         +--> enable notifications
```

### Sending Location

Once services and characteristics are resolved:
- Builds a Sony-compatible location payload
- Writes to `DD11`
- Enables FF02 notifications after first location write (if required)

## Configuration Options

- **Camera Name Prefix**: Limits connection to cameras starting with this name (Default: "SonyA7").
- **Timezone/DST**: Offset in minutes for timestamp correction.
- **Max GPS Age**: How long to keep sending the last known location if GPS signal is lost.

## Building

This project uses the Espressif IoT Development Framework (ESP-IDF) within PlatformIO.
Check `platformio.ini` for build flags and pin definitions.

```ini
; Example Override
build_flags =
  -DGPS_UART_TX_PIN=5
  -DGPS_UART_RX_PIN=4
```

### Build Environments

The `platformio.ini` file defines several build environments for different purposes:

*   **`env:esp32c6`**: The standard production build for the ESP32-C6. Logging is disabled for performance.
*   **`env:esp32c6-debug`**: A debugging environment.
    *   Enables verbose logging (`ALPHALOC_VERBOSE=1`).
    *   **Enables Fake GPS (`ALPHALOC_FAKE_GPS=1`)**: Simulates a stationary location (Munich) for testing without a GPS module or satellite lock.
*   **`env:esp32c6-debug-gps`**: Debugging environment using *real* GPS data but with verbose logging enabled.
*   **`env:esp32s3`**: The standard production build for the ESP32-S3. Logging is disabled for performance.
*   **`env:esp32s3-debug`**: A debugging environment.
    *   Enables verbose logging (`ALPHALOC_VERBOSE=1`).
    *   **Enables Fake GPS (`ALPHALOC_FAKE_GPS=1`)**: Simulates a stationary location (Munich) for testing without a GPS module or satellite lock.
*   **`env:esp32s3-debug-gps`**: Debugging environment using *real* GPS data but with verbose logging enabled.
*   **`env:xiao-c6-debug`**: XIAO ESP32-C6 Fake GPS compatibility target.
*   **`env:xiao-c6-gnss-debug`**: XIAO ESP32-C6 with a real UART GNSS receiver and verbose diagnostics.
*   **`env:xiao-c6-gnss-power`**: Current XIAO ESP32-C6 release target with the FrameNav Web UI and GNSS/BLE power policies.

### Build Flags

You can customize the firmware behavior using these compilation flags in `platformio.ini`:

| Flag | Description | Default |
|------|-------------|---------|
| `ALPHALOC_WIFI_WEB` | Enable internal settings web server. Set to `0` to remove WiFi stack and save power/flash. | `1` |
| `ALPHALOC_BLE_CONFIG` | **Enable BLE configuration service.** Note: This allows reading and writing the config via BLE unauthenticated! | `0` (DISABLED) |
| `ALPHALOC_VERBOSE` | Enable extensive debug logging to serial UART. | `0` |
| `ALPHALOC_FAKE_GPS` | Ignore UART GPS and output a static test location. | `0` |
| `ALPHALOC_NEOPIXEL_PIN`| GPIO pin number for the WS2812B NeoPixel. | (Board dependent) |
| `ALPHALOC_BATTERY_MONITOR` | Enable MAX17048 / LC709203F battery monitor over I2C. | `0` |
| `ALPHALOC_BATTERY_SDA_PIN` | I2C SDA pin for battery monitor. | (Board dependent) |
| `ALPHALOC_BATTERY_SCL_PIN` | I2C SCL pin for battery monitor. | (Board dependent) |
| `ALPHALOC_BATTERY_I2C_POWER_PIN` | Optional power-enable pin for I2C battery monitor. | (Unset) |
| `GPS_UART_TX_PIN` | TX Pin for GPS Serial (Connects to GPS RX). | (Board dependent) |
| `GPS_UART_RX_PIN` | RX Pin for GPS Serial (Connects to GPS TX). | (Board dependent) |
| `DALPHALOC_FACTORY_RESET` | If set to `1`, wipes NVS settings on boot. Dangerous. | Undefined |

**Security Best Practices:**
- Keep `ALPHALOC_BLE_CONFIG=0` in production builds
- Only enable BLE config for initial setup in controlled environments
- Use WiFi config (`ALPHALOC_WIFI_WEB=1`) with strong AP password instead
- Change default WiFi credentials immediately after first boot

## References

There is a bunch of work done by others before me which greatly helped figuring out how the BLE protocol works with the Sony Alpha cameras. I'm sure I'm forgetting a bunch but to at least name a few:

- https://github.com/dzwiedziu-nkg/esp32-a7iv-rc
- https://github.com/ekutner/camera-gps-link
- https://github.com/whc2001/ILCE7M3ExternalGps
- And many more (search GitHub for one of the unique identifiers such as the service UUIDs).

## License and attribution

FrameNav is open-source software licensed under the
[Apache License, Version 2.0](LICENSE).

This project is derived from
[AlphaLoc by finfinack](https://github.com/finfinack/alphaloc), which is also
licensed under Apache-2.0. FrameNav retains that license and documents its
changes in [MODIFICATIONS.md](MODIFICATIONS.md). Attribution and distribution
notices are provided in [NOTICE](NOTICE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Unless explicitly stated otherwise, contributions submitted to this repository
are accepted under Apache-2.0; see [CONTRIBUTING.md](CONTRIBUTING.md).
