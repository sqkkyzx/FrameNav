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

### 当前版本：0.19.9

修复正式证书链超出 TLS 发送缓冲导致 HTTPS 连接关闭的问题，并为 HTTP/HTTPS 设置
连接上限及空闲连接回收，避免手机联网探测占满连接后网页超时。TLS 动态释放缓冲和握手
凭据，配置页分块发送，避免并发 HTTPS 访问时出现 `No memory`。

Web 运行状态下方显示当前定位、来源、定位时间和观测距今，每 3 秒刷新。

GPS 连续搜星并始终优先使用有效卫星定位。室内无 GPS 时，可用手机连接设备 Wi-Fi，
在 HTTPS 网页点击“从手机获取一次定位”，允许定位后将位置保存在设备上作为兜底。
手机位置默认 **6 小时**过期，网页可设置 **1–720 小时**；到期且无 GPS 时停止发送位置。
也可以使用独立公网定位页 `https://framenav.igp.wiki/`，由 Android Chrome 获取手机位置并通过
BLE 写入设备；此方式不依赖设备本地 HTTPS 证书更新。公网容器部署说明见
[`public-service/README.md`](public-service/README.md)。

当前实测组合为 **XIAO ESP32-C6 + StarNav M10-K18 + 18350 电池 + Sony α7C II**。
`minimal-implementation` 分支保留 0.17.0 最小实现；`main` 是当前开发版本。
使用 PlatformIO Core CLI，无需 VS Code：

```sh
pio run -e xiao-c6-gnss-power
pio run -e xiao-c6-gnss-power -t upload
pio device monitor -b 115200
```

请显式指定上述环境；默认 `xiao-c6-debug` 是 Fake GPS 测试固件。
GNSS TX 接 D7/GPIO17，RX 接 D8/GPIO19，GND 共地；供电和串口电平应先按模块实物确认。
电池接入板载 BAT 正负端，不能接到 3V3；电芯保护、极性及 GNSS 电池供电路径需自行确认。
电池供电时不能假设 VBUS 有 5V 输出。
**不要将 VBUS 和 3V3 直接并联。**

XIAO 使用板载单色状态灯（不是下文上游参考硬件的 NeoPixel）：
未连接相机时闪烁，连接后常亮；常亮不代表相机已接受有效定位。
Wi-Fi 配置入口完全由 USB 线缆状态控制，USB 供电线和数据线都可以启动 Wi-Fi；拔出 USB 后关闭。当前 XIAO 没有接入 ESP32-C6 的 VBUS 检测 GPIO，供电线依赖已验证的 USB PHY 线状态；要覆盖所有不驱动 D+/D- 的线缆，需要补充 VBUS 硬件检测。
正式访问地址是 **https://framenav.device.igp.wiki/**。设备热点的本地 DNS 会把这个域名解析到
`10.254.254.1`，因此手机连设备热点时也可以使用同一个地址；若手机不接受 captive portal，仍可访问
**http://10.254.254.1/**。
手机使用 VPN 或代理时，应将 `framenav.device.igp.wiki` 和 `10.254.254.0/24` 设置为
通过设备 Wi-Fi 直连，避免本地网页请求被转发到外网。
设备接入外部 Wi‑Fi 后，域名解析由该 Wi‑Fi 的 DNS 或 hosts 规则负责，需把域名指向设备的 STA 地址；
否则公共 DNS 指向的服务器不会自动代理到设备。
默认热点密码为 `framenav1234`，请在配置页修改。

USB 开启配置网络后立即提供设备热点，同时连接网页中保存的外部 Wi-Fi；连接失败时降低
后台重试频率，避免持续扫频干扰手机访问热点。没有配置外部 Wi-Fi 时不主动扫描。
USB 插入后如果没有设备关联或近期网页活动，Wi-Fi 按网页设置在 5 或 15 分钟后关闭；关闭后拔出并重新插入 USB 才会再次开启。
正式构建把 `framenav.device.igp.wiki` 的默认证书和对应私钥内置到固件中。网页的“下载并更新证书和私钥”按钮从 `https://framenav.igp.wiki/download/certs.zip` 下载整套凭据，校验域名及证书与私钥匹配后成对保存；重新插拔 USB 后加载。
BLE 保留控制器 modem sleep 和搜索省电策略；GNSS 连续工作。电池物理断电再上电会重新启动设备。

手机定位由手机操作系统提供，通过浏览器 Geolocation API 返回 WGS-84 经纬度与估算精度；
网页不会自动请求定位，只在用户点击时请求。使用手机系统浏览器打开受信任的 HTTPS 地址，
并允许定位权限。设备本身不需要联网即可接收手机位置；手机定位服务是否需要网络由手机决定。
浏览器最多等待 60 秒，允许使用手机最近 30 秒的位置；如果手机地图能定位但浏览器一直超时，
请尝试手机自带浏览器，并保持手机移动数据可用。
定位获取时间保持不变，设备用手机提供的 UTC 和单调运行时间推进相机时间，不会把相机时钟
停留在获取定位的那一刻。有效期从手机实际获取位置的时间开始计算；关闭 Wi-Fi 或拔插 USB
不会延长或清除位置。有效期设置保存在 NVS，位置只保存在本次开机内存中，整机断电后需重新获取。
GPS 恢复后立即优先使用 GPS，GPS 再次丢失时仍可使用未过期的手机位置。
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

**Sequence:** [Camera Status] -> [GPS Status] -> [Battery Status] -> [WiFi Status]

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
| **WiFi SSID/Pass** | Credentials for connecting to your home WiFi. | Empty (not configured) |
| **AP SSID/Pass** | Credentials for the hotspot created by FrameNav. | `FrameNav` / `framenav1234` |
| **WiFi Idle Timeout** | Turn WiFi off after no associated client or recent page activity. | `5 minutes` |
| **Max GPS Age** | How long (seconds) to reuse old coordinates if GPS signal is lost. | `300` |

#### Method A: WiFi Web Interface

FrameNav starts its hotspot immediately and also tries a saved WiFi network. The canonical page is
**https://framenav.device.igp.wiki/** and uses the embedded public certificate:

- **Default SSID/password**: empty; configure a 2.4 GHz network in the Web interface
- **IP**: assigned via DHCP

The hotspot remains available while the device connects to the external network.
Failed external-network attempts back off from 30 to 120 seconds to limit scan interference:

- **SSID**: `FrameNav`
- **Password**: `framenav1234`
- **Network**: `10.254.254.0/24`
- **Configuration URL**: https://framenav.device.igp.wiki/
- **Local fallback**: http://10.254.254.1/

FrameNav advertises a captive portal through DHCP option 114, resolves local DNS names to
`10.254.254.1`, and redirects HTTP probe paths to the HTTPS configuration page. Supported operating systems
will usually open the page automatically after joining; the canonical domain remains the reliable URL.

本地正式构建需要把证书放在被 Git 忽略的 `certs/server_cert.pem` 和
`certs/server_key.pem`。CMake 会调用 `scripts/embed_tls_cert.py` 生成同样被忽略的嵌入源文件。
在线更新需要设备连接可上网的外部 Wi-Fi。ZIP 可直接包含文件或多包一层目录，使用未加密的
stored/deflate 压缩；设备读取 `framenav.device.igp.wiki_bundle.pem`（缺失时使用同名 `_bundle.crt`）
和 `framenav.device.igp.wiki.key`，忽略 CSR 等其他文件。ZIP 最大 64 KiB，证书链最大 12 KiB，
私钥最大 8 KiB。下载服务器的 HTTPS 使用系统 CA 包验证；下载后的证书链需能解析、叶证书 SAN
需包含设备域名、私钥需匹配。设备尚无可信系统时钟，此处不额外校验证书有效期；手机仍会检查
HTTPS 服务的完整信任链和有效期。
`tls_cert` 分区以双槽保存证书和私钥，新数据完整写入并校验后才提交，更新中断时保留上一套；
没有有效保存记录时使用内置默认值。当前 HTTPS 会话保持原凭据，重新插拔 USB 后启用更新。
由于 HTTPS 服务使用内置私钥，拿到未启用 Flash Encryption/Secure Boot 的固件的人可以提取该私钥；
当前方案适合先做默认正式证书，后续生产发布应启用芯片安全启动和加密，或在泄露后立即换证书。

The Web interface is Chinese-first. Connection settings are shown first: it displays GNSS/camera status,
firmware version, current track-storage usage, and the earliest retained record time,
lists up to three saved camera bonds, can remove an individual local bond, and can open a 90-second
continuous scan for pairing a new camera. The location page provides a manual phone-location button,
accuracy, capture time, remaining lifetime and a configurable expiry in hours. It also accepts manual
WGS-84 latitude/longitude input when the phone browser cannot provide a location. GNSS runs continuously.
Phone acquisition first requests standard accuracy, then retries with high accuracy if unavailable or timed out.
If Chrome cannot obtain a position, use the phone's built-in browser. Once saved to the device,
the position remains available when switching browsers until it expires or the device loses power.
Bluetooth power modes provide preset policies and collapsible custom fields. Runtime Bluetooth changes
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
