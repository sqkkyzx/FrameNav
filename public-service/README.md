# FrameNav 公网定位页

这是一个只提供 HTTP 源站的静态页面。CDN 在公网边缘终止 HTTPS，并把请求回源到 Compose 暴露的端口。页面使用 Android Chrome 的 Web Bluetooth 连接 FrameNav，只写入手机位置，不访问设备的 HTTPS 证书。

## 发布

GitHub Actions 会构建并发布：

```text
ghcr.io/sqkkyzx/framenav-public:latest
```

服务器上将 `framenav.igp.wiki` 的 CDN 回源指向部署主机的 HTTP 端口，然后运行：

```bash
docker compose pull
docker compose up -d
docker compose ps
```

默认监听主机 `80` 端口。已有服务占用该端口时，可以设置 `FRAMENAV_ORIGIN_PORT=8080`，并让 CDN 回源到 `8080`。

## 使用条件

- 页面必须通过 CDN 的 HTTPS 地址打开，HTTP 源站不直接用于手机浏览器。
- Android Chrome 需要用户点击“连接设备”并选择 FrameNav；蓝牙连接期间设备需要处于 USB 唤醒窗口。
- 固件需要包含位置 BLE 服务：`b1f0b4d5-797b-5a9e-5b4f-4a1f02007ea1`。
