const SERVICE_UUID = "b1f0b4d5-797b-5a9e-5b4f-4a1f02007ea1";
const WRITE_UUID = "b1f0b4d5-797b-5a9e-5b4f-4a1f02107ea1";
const CLEAR_UUID = "b1f0b4d5-797b-5a9e-5b4f-4a1f02117ea1";

const $ = (id) => document.getElementById(id);
const state = { device: null, characteristic: null, clearCharacteristic: null, position: null };

function setMessage(text, error = false) {
  const node = $("message");
  node.textContent = text;
  node.classList.toggle("error", error);
}

function setStatus(id, text, kind = "neutral") {
  const node = $(id);
  node.textContent = text;
  node.className = `status status-${kind}`;
}

function secureContextReady() {
  if (!window.isSecureContext) {
    setStatus("secure-state", "需要 HTTPS", "error");
    setMessage("请通过 CDN 的 HTTPS 地址打开此页面。", true);
    return false;
  }
  setStatus("secure-state", "HTTPS 已启用", "ok");
  return true;
}

function bluetoothReady() {
  if (!navigator.bluetooth) {
    setStatus("device-state", "浏览器不支持", "error");
    setMessage("请使用 Android Chrome，并从 HTTPS 页面打开。", true);
    return false;
  }
  return true;
}

function setConnected(device) {
  state.device = device;
  $("device-name").textContent = device.name || "FrameNav";
  $("device-detail").textContent = "蓝牙已连接，可以写入位置。";
  setStatus("device-state", "已连接", "ok");
  $("write-location").disabled = !state.position;
  $("write-manual").disabled = false;
  $("clear-location").disabled = !state.clearCharacteristic;
}

function disconnected() {
  state.characteristic = null;
  state.clearCharacteristic = null;
  setStatus("device-state", "已断开", "warn");
  $("device-detail").textContent = "请重新连接设备。";
  $("write-location").disabled = true;
  $("write-manual").disabled = true;
  $("clear-location").disabled = true;
}

async function connectDevice() {
  if (!secureContextReady() || !bluetoothReady()) return;
  setMessage("请选择附近的 FrameNav…");
  const device = await navigator.bluetooth.requestDevice({
    filters: [{ services: [SERVICE_UUID] }],
    optionalServices: [SERVICE_UUID],
  });
  device.addEventListener("gattserverdisconnected", disconnected);
  const server = await device.gatt.connect();
  const service = await server.getPrimaryService(SERVICE_UUID);
  state.characteristic = await service.getCharacteristic(WRITE_UUID);
  state.clearCharacteristic = await service.getCharacteristic(CLEAR_UUID);
  setConnected(device);
  setMessage("设备已连接。", false);
}

function getPosition(options) {
  return new Promise((resolve, reject) => navigator.geolocation.getCurrentPosition(resolve, reject, options));
}

async function getPhonePosition() {
  if (!secureContextReady()) throw new Error("页面不是安全上下文");
  if (!navigator.geolocation) throw new Error("浏览器不支持定位");
  setStatus("location-state", "定位中", "warn");
  setMessage("正在请求手机定位权限…");
  try {
    return await getPosition({ enableHighAccuracy: false, maximumAge: 0, timeout: 15000 });
  } catch (error) {
    if (error.code !== 2 && error.code !== 3) throw error;
    setMessage("普通定位未完成，正在尝试高精度定位，最多等待 60 秒…");
    return getPosition({ enableHighAccuracy: true, maximumAge: 0, timeout: 60000 });
  }
}

function showPosition(position) {
  const { latitude, longitude, accuracy } = position.coords;
  state.position = { latitude, longitude, accuracy: Number.isFinite(accuracy) ? accuracy : 0, timestamp: position.timestamp };
  $("latitude").textContent = latitude.toFixed(7);
  $("longitude").textContent = longitude.toFixed(7);
  $("accuracy").textContent = `${Math.round(state.position.accuracy)} m`;
  setStatus("location-state", "已获取", "ok");
  $("write-location").disabled = !state.characteristic;
}

function encodePosition(position) {
  const buffer = new ArrayBuffer(28);
  const view = new DataView(buffer);
  const latitude = Math.round(position.latitude * 10000000);
  const longitude = Math.round(position.longitude * 10000000);
  const accuracyCm = Math.min(2000000000, Math.max(0, Math.round(position.accuracy * 100)));
  view.setInt32(0, latitude, true);
  view.setInt32(4, longitude, true);
  view.setUint32(8, accuracyCm, true);
  view.setBigUint64(12, BigInt(Math.trunc(position.timestamp)), true);
  view.setBigUint64(20, BigInt(Date.now()), true);
  return buffer;
}

async function writeValue(characteristic, value) {
  if (characteristic.writeValueWithResponse) return characteristic.writeValueWithResponse(value);
  return characteristic.writeValue(value);
}

async function requireConnection() {
  if (!state.characteristic || !state.device?.gatt?.connected) {
    throw new Error("请先连接 FrameNav");
  }
}

async function writeCurrentPosition(position, label = "手机位置") {
  await requireConnection();
  const capturedMs = Math.trunc(position.timestamp);
  if (!Number.isSafeInteger(capturedMs) || Date.now() < capturedMs || Date.now() - capturedMs > 60000) {
    throw new Error("定位数据已过期，请重新获取");
  }
  await writeValue(state.characteristic, encodePosition(position));
  setMessage(`${label}已写入设备。`);
}

async function readAndShowPosition() {
  try {
    const position = await getPhonePosition();
    showPosition(position);
    setMessage("手机定位已获取，可以写入设备。");
  } catch (error) {
    setStatus("location-state", "失败", "error");
    setMessage(error.code === 1 ? "定位权限被拒绝，请在浏览器设置中允许定位。" : "手机定位失败，请稍后重试。", true);
  }
}

function manualPosition() {
  const latitude = Number($("manual-latitude").value);
  const longitude = Number($("manual-longitude").value);
  if (!$("manual-latitude").value.trim() || !$("manual-longitude").value.trim() ||
      !Number.isFinite(latitude) || latitude < -90 || latitude > 90 ||
      !Number.isFinite(longitude) || longitude < -180 || longitude > 180) {
    throw new Error("请输入有效的纬度和经度");
  }
  return { latitude, longitude, accuracy: 0, timestamp: Date.now() };
}

$("connect").addEventListener("click", async () => {
  try { await connectDevice(); } catch (error) { setStatus("device-state", "连接失败", "error"); setMessage(error.message || "蓝牙连接失败。", true); }
});
$("get-location").addEventListener("click", readAndShowPosition);
$("write-location").addEventListener("click", async () => {
  try { await writeCurrentPosition(state.position); } catch (error) { setMessage(error.message || "写入失败。", true); }
});
$("write-manual").addEventListener("click", async () => {
  try { await writeCurrentPosition(manualPosition(), "手动位置"); } catch (error) { setMessage(error.message || "写入失败。", true); }
});
$("clear-location").addEventListener("click", async () => {
  try { await requireConnection(); await writeValue(state.clearCharacteristic, new Uint8Array([1])); setMessage("设备中的手机位置已清除。"); }
  catch (error) { setMessage(error.message || "清除失败。", true); }
});

secureContextReady();
if (!navigator.bluetooth) setStatus("device-state", "需 Android Chrome", "warn");
