package wiki.igp.framenav;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.location.Location;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import org.json.JSONObject;
import org.json.JSONArray;

/**
 * Bluetooth transport for FrameNav. All GATT operations are serialized because
 * Android accepts only one read/write request at a time per connection.
 */
public final class BleManager {
    public interface Listener {
        void onBleState(String state);

        void onBleReady(boolean locationAvailable, boolean configAvailable);

        void onBleConfig(Map<String, String> values);

        void onBleError(String message);

        void onBleDisconnected();
    }

    public interface Callback {
        void onSuccess();

        void onError(String message);
    }

    private interface ReadCallback {
        void onComplete(boolean success, byte[] value, String error);
    }

    public static final UUID LOCATION_SERVICE = UUID.fromString(
            "b1f0b4d5-797b-5a9e-5b4f-4a1f02007ea1");
    public static final UUID LOCATION_WRITE = UUID.fromString(
            "b1f0b4d5-797b-5a9e-5b4f-4a1f02107ea1");
    public static final UUID LOCATION_CLEAR = UUID.fromString(
            "b1f0b4d5-797b-5a9e-5b4f-4a1f02117ea1");

    public static final UUID CONFIG_SERVICE = UUID.fromString(
            "b1f0b4d5-797b-5a9e-5b4f-4a1f01007ea1");
    public static final UUID WIFI_WAKE = uuid("11007ea1");
    public static final UUID SETTINGS = uuid("12007ea1");
    public static final UUID TELEMETRY = uuid("13007ea1");
    public static final UUID BONDS = uuid("14007ea1");
    private static final LinkedHashMap<String, UUID> CONFIG_CHARACTERISTICS = new LinkedHashMap<>();

    static {
        CONFIG_CHARACTERISTICS.put("camera_name", uuid("02007ea1"));
        CONFIG_CHARACTERISTICS.put("camera_mac", uuid("03007ea1"));
        CONFIG_CHARACTERISTICS.put("tz", uuid("04007ea1"));
        CONFIG_CHARACTERISTICS.put("dst", uuid("05007ea1"));
        CONFIG_CHARACTERISTICS.put("max_gps_age", uuid("0b007ea1"));
        CONFIG_CHARACTERISTICS.put("gps_lock", uuid("0c007ea1"));
        CONFIG_CHARACTERISTICS.put("gps_sats", uuid("0d007ea1"));
        CONFIG_CHARACTERISTICS.put("gps_constellations", uuid("0e007ea1"));
        CONFIG_CHARACTERISTICS.put("camera_connected", uuid("0f007ea1"));
        CONFIG_CHARACTERISTICS.put("camera_bonded", uuid("10007ea1"));
    }

    private static UUID uuid(String suffix) {
        return UUID.fromString("b1f0b4d5-797b-5a9e-5b4f-4a1f" + suffix);
    }

    private static final long SCAN_TIMEOUT_MS = 15_000L;
    private static final long OP_TIMEOUT_MS = 8_000L;
    private static final long BOND_TIMEOUT_MS = 45_000L;

    private final Context context;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Deque<GattOperation> operations = new ArrayDeque<>();
    private final Map<UUID, BluetoothGattCharacteristic> characteristics = new LinkedHashMap<>();

    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothDevice device;
    private GattOperation inFlight;
    private Runnable operationTimeout;
    private Runnable scanTimeout;
    private boolean scanning;
    private boolean ready;
    private boolean waitingForBond;
    private boolean bondReceiverRegistered;
    private boolean gattCacheRefreshAttempted;
    private boolean gattCacheRefreshSucceeded;
    private int authRetryCount;
    private boolean mtuRequested;
    private int negotiatedMtu = 23;

    public BleManager(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
        BluetoothManager manager = (BluetoothManager) this.context.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = manager == null ? null : manager.getAdapter();
        IntentFilter filter = new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) {
            this.context.registerReceiver(bondReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            this.context.registerReceiver(bondReceiver, filter);
        }
        bondReceiverRegistered = true;
    }

    public boolean isConnected() {
        return ready && gatt != null;
    }

    @SuppressLint("MissingPermission")
    public void connect() {
        main.post(() -> {
            if (adapter == null) {
                listener.onBleError("此手机不支持蓝牙");
                return;
            }
            if (!adapter.isEnabled()) {
                listener.onBleError("请先打开手机蓝牙");
                return;
            }
            disconnectInternal(false);
            scanner = adapter.getBluetoothLeScanner();
            if (scanner == null) {
                listener.onBleError("无法启动蓝牙扫描");
                return;
            }
            try {
                scanner.startScan(scanCallback);
                scanning = true;
                listener.onBleState("正在搜索 FrameNav…");
                scanTimeout = () -> {
                    if (scanning) {
                        stopScan();
                        listener.onBleError("未找到 FrameNav。请确认设备处于 Wi-Fi/USB 唤醒窗口内");
                    }
                };
                main.postDelayed(scanTimeout, SCAN_TIMEOUT_MS);
            } catch (SecurityException error) {
                listener.onBleError("缺少蓝牙权限，请允许附近设备权限");
            }
        });
    }

    @SuppressLint("MissingPermission")
    public void disconnect() {
        main.post(() -> disconnectInternal(true));
    }

    public void close() {
        main.post(() -> {
            disconnectInternal(false);
            if (bondReceiverRegistered) {
                try {
                    context.unregisterReceiver(bondReceiver);
                } catch (IllegalArgumentException ignored) {
                    // The receiver may already have been removed.
                }
                bondReceiverRegistered = false;
            }
        });
    }

    public void readConfig(ConfigCallback callback) {
        main.post(() -> {
            if (!ready) {
                callback.onError("蓝牙尚未连接");
                return;
            }
            BluetoothGattCharacteristic settings = characteristics.get(SETTINGS);
            if (settings != null) {
                enqueueRead(settings, (success, value, error) -> {
                    if (!success) {
                        callback.onError("读取设备配置失败：" + error);
                        return;
                    }
                    try {
                        JSONObject json = new JSONObject(new String(value, StandardCharsets.UTF_8));
                        Map<String, String> values = parseConfigJson(json);
                        callback.onSuccess(values);
                    } catch (Exception parseError) {
                        callback.onError("蓝牙配置数据无法解析");
                    }
                });
                return;
            }
            final List<Map.Entry<String, UUID>> entries = new ArrayList<>(CONFIG_CHARACTERISTICS.entrySet());
            final Map<String, String> values = new LinkedHashMap<>();
            List<Map.Entry<String, UUID>> readableEntries = new ArrayList<>();
            readableEntries.addAll(entries);
            readConfigAt(readableEntries, 0, values, callback);
        });
    }

    private static Map<String, String> parseConfigJson(JSONObject json) {
        Map<String, String> values = new LinkedHashMap<>();
        JSONArray camera = json.optJSONArray("c");
        JSONArray location = json.optJSONArray("l");
        if (camera != null || location != null) {
            putArrayValue(values, camera, 0, "cam_name");
            putArrayValue(values, camera, 1, "cam_mac");
            putArrayValue(values, camera, 2, "tz");
            putArrayValue(values, camera, 3, "dst");
            putArrayValue(values, camera, 4, "wifi_idle_s");
            putArrayValue(values, camera, 5, "ble_mode");
            putArrayValue(values, camera, 6, "ble_boot");
            putArrayValue(values, camera, 7, "ble_period");
            putArrayValue(values, camera, 8, "ble_window");
            putArrayValue(values, location, 0, "source_order");
            putArrayValue(values, location, 1, "source_gnss");
            putArrayValue(values, location, 2, "source_phone");
            putArrayValue(values, location, 3, "source_manual");
            putArrayValue(values, location, 4, "track_enabled");
            putArrayValue(values, location, 5, "gnss_idle");
            putArrayValue(values, location, 6, "track_idle");
            putArrayValue(values, location, 7, "track_int_s");
            putArrayValue(values, location, 8, "phone_push_enabled");
            putArrayValue(values, location, 9, "phone_interval_s");
            putArrayValue(values, location, 10, "manual_auto_update");
            putArrayValue(values, location, 11, "manual_lat");
            putArrayValue(values, location, 12, "manual_lon");
            return values;
        }
        java.util.Iterator<String> keys = json.keys();
        while (keys.hasNext()) {
            String key = keys.next();
            Object item = json.opt(key);
            if (item != null && item != JSONObject.NULL) values.put(key, String.valueOf(item));
        }
        return values;
    }

    private static void putArrayValue(Map<String, String> values, JSONArray array,
                                      int index, String key) {
        if (array == null || index >= array.length()) return;
        Object item = array.opt(index);
        values.put(key, item == null || item == JSONObject.NULL ? "" : String.valueOf(item));
    }

    public void requestWifiWake(Callback callback) {
        main.post(() -> {
            if (!ready || gatt == null) {
                callback.onError("设备蓝牙尚未连接或蓝牙服务尚未就绪");
                return;
            }
            BluetoothGattCharacteristic wake = characteristics.get(WIFI_WAKE);
            if (wake == null) {
                callback.onError("已连接蓝牙，但设备固件未提供热点唤醒特征，请刷入支持蓝牙配置的固件");
                return;
            }
            enqueueWrite(wake, new byte[]{1}, callback);
        });
    }

    public interface ConfigCallback {
        void onSuccess(Map<String, String> values);

        void onError(String message);
    }

    public interface TelemetryCallback {
        void onSuccess(JSONObject value);

        default void onError(String message) { }
    }

    public interface BondsCallback {
        void onSuccess(JSONArray value);

        void onError(String message);
    }

    public void readTelemetry(TelemetryCallback callback) {
        main.post(() -> {
            BluetoothGattCharacteristic telemetry = characteristics.get(TELEMETRY);
            if (!ready || telemetry == null) {
                callback.onError("设备蓝牙遥测服务不可用");
                return;
            }
            enqueueRead(telemetry, (success, value, error) -> {
                if (!success) {
                    callback.onError("读取设备遥测失败：" + error);
                    return;
                }
                try {
                    callback.onSuccess(new JSONObject(new String(value, StandardCharsets.UTF_8)));
                } catch (Exception parseError) {
                    callback.onError("设备蓝牙遥测数据无法解析（" + value.length
                            + " 字节）");
                }
            });
        });
    }

    public void readBonds(BondsCallback callback) {
        main.post(() -> {
            BluetoothGattCharacteristic bonds = characteristics.get(BONDS);
            if (!ready || bonds == null) {
                callback.onError("设备蓝牙配对设备服务不可用");
                return;
            }
            enqueueRead(bonds, (success, value, error) -> {
                if (!success) {
                    callback.onError("读取已配对设备失败：" + error);
                    return;
                }
                try {
                    JSONArray items = new JSONObject(new String(value, StandardCharsets.UTF_8))
                            .optJSONArray("bonds");
                    if (items == null) {
                        callback.onError("蓝牙配对设备数据缺少 bonds 列表");
                    } else {
                        callback.onSuccess(items);
                    }
                } catch (Exception parseError) {
                    callback.onError("蓝牙配对设备数据无法解析");
                }
            });
        });
    }

    public void writeConfig(Map<String, String> values, Callback callback) {
        main.post(() -> {
            if (!ready) {
                callback.onError("蓝牙尚未连接");
                return;
            }
            final List<Map.Entry<String, String>> entries = new ArrayList<>();
            for (Map.Entry<String, String> entry : values.entrySet()) {
                if (CONFIG_CHARACTERISTICS.containsKey(entry.getKey())) {
                    entries.add(entry);
                }
            }
            writeConfigAt(entries, 0, callback);
        });
    }

    public void writeLocation(Location location, Callback callback) {
        main.post(() -> {
            if (!ready || !characteristics.containsKey(LOCATION_WRITE)) {
                callback.onError("当前蓝牙设备没有定位写入服务");
                return;
            }
            try {
                enqueueWrite(characteristics.get(LOCATION_WRITE),
                        LocationFrame.encode(location, System.currentTimeMillis()), callback);
            } catch (IllegalArgumentException error) {
                callback.onError(error.getMessage());
            }
        });
    }

    public void clearLocation(Callback callback) {
        main.post(() -> {
            if (!ready || !characteristics.containsKey(LOCATION_CLEAR)) {
                callback.onError("当前蓝牙设备没有定位清除服务");
                return;
            }
            enqueueWrite(characteristics.get(LOCATION_CLEAR), new byte[]{1}, callback);
        });
    }

    private void readConfigAt(List<Map.Entry<String, UUID>> entries, int index,
                              Map<String, String> values, ConfigCallback callback) {
        if (index >= entries.size()) {
            callback.onSuccess(values);
            return;
        }
        Map.Entry<String, UUID> entry = entries.get(index);
        BluetoothGattCharacteristic characteristic = characteristics.get(entry.getValue());
        if (characteristic == null) {
            readConfigAt(entries, index + 1, values, callback);
            return;
        }
        enqueueRead(characteristic, (success, value, error) -> {
            if (success) {
                values.put(entry.getKey(), new String(value, StandardCharsets.UTF_8).trim());
                readConfigAt(entries, index + 1, values, callback);
            } else {
                callback.onError("读取 " + entry.getKey() + " 失败：" + error);
            }
        });
    }

    private void writeConfigAt(List<Map.Entry<String, String>> entries, int index,
                               Callback callback) {
        if (index >= entries.size()) {
            callback.onSuccess();
            return;
        }
        Map.Entry<String, String> entry = entries.get(index);
        BluetoothGattCharacteristic characteristic = characteristics.get(CONFIG_CHARACTERISTICS.get(entry.getKey()));
        if (characteristic == null) {
            callback.onError("设备不支持配置项：" + entry.getKey());
            return;
        }
        enqueueWrite(characteristic, entry.getValue().getBytes(StandardCharsets.UTF_8), new Callback() {
            @Override
            public void onSuccess() {
                writeConfigAt(entries, index + 1, callback);
            }

            @Override
            public void onError(String message) {
                callback.onError("写入 " + entry.getKey() + " 失败：" + message);
            }
        });
    }

    private void enqueueRead(BluetoothGattCharacteristic characteristic, ReadCallback callback) {
        operations.add(new GattOperation(characteristic, null, callback, null));
        runNextOperation();
    }

    private void enqueueWrite(BluetoothGattCharacteristic characteristic, byte[] value,
                               Callback callback) {
        operations.add(new GattOperation(characteristic, value, null, callback));
        runNextOperation();
    }

    @SuppressLint("MissingPermission")
    private void runNextOperation() {
        if (inFlight != null || operations.isEmpty() || gatt == null) {
            return;
        }
        inFlight = operations.removeFirst();
        authRetryCount = 0;
        boolean started;
        try {
            if (inFlight.value == null) {
                started = gatt.readCharacteristic(inFlight.characteristic);
            } else {
                inFlight.characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                inFlight.characteristic.setValue(inFlight.value);
                started = gatt.writeCharacteristic(inFlight.characteristic);
            }
        } catch (SecurityException error) {
            failConnection("缺少蓝牙连接权限");
            return;
        }
        if (!started) {
            if (inFlight.value == null) {
                completeOperation(false, null, "蓝牙读取未能启动");
            } else {
                failConnection("蓝牙操作未能启动，已重置连接，请重新连接");
            }
            return;
        }
        armOperationTimeout(OP_TIMEOUT_MS, "蓝牙操作超时，已重置连接，请重新连接");
    }

    private void armOperationTimeout(long delayMs, String message) {
        if (operationTimeout != null) {
            main.removeCallbacks(operationTimeout);
        }
        operationTimeout = () -> failConnection(message);
        main.postDelayed(operationTimeout, delayMs);
    }

    @SuppressLint("MissingPermission")
    private void beginBonding(BluetoothGatt bluetoothGatt) {
        if (authRetryCount > 1) {
            failConnection("蓝牙加密配对后写入仍被拒绝，请在系统蓝牙设置中移除并重新配对");
            return;
        }
        if (waitingForBond) {
            return;
        }
        BluetoothDevice target = device;
        if (target == null || bluetoothGatt != gatt) {
            failConnection("蓝牙设备已断开");
            return;
        }
        int bondState;
        try {
            bondState = target.getBondState();
        } catch (SecurityException error) {
            failConnection("缺少蓝牙连接权限");
            return;
        }
        if (bondState == BluetoothDevice.BOND_BONDED) {
            failConnection("设备已配对但拒绝加密写入，请在系统蓝牙设置中重新配对");
            return;
        }
        waitingForBond = true;
        armOperationTimeout(BOND_TIMEOUT_MS, "蓝牙配对超时，请确认系统配对提示并重试");
        if (bondState == BluetoothDevice.BOND_BONDING) {
            listener.onBleState("等待蓝牙配对完成…");
            return;
        }
        try {
            if (!target.createBond() && target.getBondState() != BluetoothDevice.BOND_BONDING) {
                waitingForBond = false;
                failConnection("无法启动蓝牙配对，请在系统蓝牙设置中配对设备");
            } else {
                listener.onBleState("请在系统提示中确认 FrameNav 蓝牙配对…");
            }
        } catch (SecurityException error) {
            waitingForBond = false;
            failConnection("缺少蓝牙连接权限");
        }
    }

    @SuppressLint("MissingPermission")
    private void retryAuthenticatedWrite() {
        if (inFlight == null || inFlight.value == null || gatt == null || waitingForBond) {
            return;
        }
        try {
            inFlight.characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            inFlight.characteristic.setValue(inFlight.value);
            if (!gatt.writeCharacteristic(inFlight.characteristic)) {
                failConnection("蓝牙配对完成，但重试写入未能启动，请重新连接");
                return;
            }
            authRetryCount++;
            armOperationTimeout(OP_TIMEOUT_MS, "蓝牙操作超时，已重置连接，请重新连接");
            listener.onBleState("蓝牙已配对，正在重试写入…");
        } catch (SecurityException error) {
            failConnection("缺少蓝牙连接权限");
        }
    }

    private final BroadcastReceiver bondReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context receiverContext, Intent intent) {
            if (intent == null || !BluetoothDevice.ACTION_BOND_STATE_CHANGED.equals(intent.getAction())) {
                return;
            }
            BluetoothDevice bondedDevice;
            if (Build.VERSION.SDK_INT >= 33) {
                bondedDevice = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class);
            } else {
                bondedDevice = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            }
            if (bondedDevice == null || device == null || !device.equals(bondedDevice)
                    || !waitingForBond) {
                return;
            }
            int state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR);
            int previous = intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE,
                    BluetoothDevice.ERROR);
            if (state == BluetoothDevice.BOND_BONDED) {
                waitingForBond = false;
                retryAuthenticatedWrite();
            } else if (state == BluetoothDevice.BOND_NONE
                    && previous == BluetoothDevice.BOND_BONDING) {
                waitingForBond = false;
                failConnection("FrameNav 蓝牙配对失败或被取消");
            }
        }
    };

    private void completeOperation(boolean success, byte[] value, String error) {
        GattOperation operation = inFlight;
        if (operation == null) {
            return;
        }
        inFlight = null;
        if (operationTimeout != null) {
            main.removeCallbacks(operationTimeout);
            operationTimeout = null;
        }
        if (operation.readCallback != null) {
            operation.readCallback.onComplete(success, value == null ? new byte[0] : value, error);
        } else if (success) {
            operation.writeCallback.onSuccess();
        } else {
            operation.writeCallback.onError(error);
        }
        runNextOperation();
    }

    @SuppressLint("MissingPermission")
    private void discoverServices(BluetoothGatt bluetoothGatt) {
        try {
            if (!bluetoothGatt.discoverServices()) {
                failConnection("无法读取蓝牙服务");
            }
        } catch (SecurityException error) {
            failConnection("缺少蓝牙连接权限");
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        @SuppressLint("MissingPermission")
        public void onConnectionStateChange(BluetoothGatt bluetoothGatt, int status, int newState) {
            main.post(() -> {
                if (bluetoothGatt != gatt) {
                    return;
                }
                if (status != BluetoothGatt.GATT_SUCCESS
                        || newState != android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                    if (newState == android.bluetooth.BluetoothProfile.STATE_DISCONNECTED) {
                        if (status == BluetoothGatt.GATT_SUCCESS) {
                            disconnectInternal(true);
                        } else {
                            failConnection("蓝牙连接断开，状态码 " + status);
                        }
                    } else {
                        failConnection("蓝牙连接失败，状态码 " + status);
                    }
                    return;
                }
                listener.onBleState("已连接，正在读取服务…");
                try {
                    if (!gattCacheRefreshAttempted) {
                        gattCacheRefreshAttempted = true;
                        gattCacheRefreshSucceeded = refreshGattCache(bluetoothGatt);
                    }
                    negotiatedMtu = 23;
                    mtuRequested = bluetoothGatt.requestMtu(247);
                    if (!mtuRequested) {
                        discoverServices(bluetoothGatt);
                    }
                } catch (SecurityException error) {
                    failConnection("缺少蓝牙连接权限");
                }
            });
        }

        @Override
        @SuppressLint("MissingPermission")
        public void onMtuChanged(BluetoothGatt bluetoothGatt, int mtu, int status) {
            main.post(() -> {
                if (bluetoothGatt != gatt || !mtuRequested) {
                    return;
                }
                mtuRequested = false;
                negotiatedMtu = status == BluetoothGatt.GATT_SUCCESS ? mtu : 23;
                if (status == BluetoothGatt.GATT_SUCCESS && mtu >= 31) {
                    listener.onBleState("已连接，正在读取服务…");
                } else if (mtu < 31) {
                    listener.onBleState("设备只支持较小蓝牙数据包，定位写入可能不可用…");
                }
                discoverServices(bluetoothGatt);
            });
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt bluetoothGatt, int status) {
            main.post(() -> {
                if (bluetoothGatt != gatt) {
                    return;
                }
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    failConnection("蓝牙服务读取失败，状态码 " + status);
                    return;
                }
                characteristics.clear();
                addServiceCharacteristics(bluetoothGatt.getService(LOCATION_SERVICE));
                addServiceCharacteristics(bluetoothGatt.getService(CONFIG_SERVICE));
                boolean locationService = characteristics.containsKey(LOCATION_WRITE)
                        && characteristics.containsKey(LOCATION_CLEAR);
                boolean location = locationService && negotiatedMtu >= 31;
                boolean config = characteristics.containsKey(CONFIG_CHARACTERISTICS.get("camera_name"));
                ready = location || config;
                listener.onBleReady(location, config);
                if (locationService && !config) {
                    listener.onBleState(gattCacheRefreshSucceeded
                            ? "蓝牙已连接·定位；设备未提供配置服务，请检查固件配置"
                            : "蓝牙已连接·定位；未能刷新蓝牙服务缓存，请关闭并重新连接设备");
                }
                if (locationService && !location) {
                    listener.onBleState("蓝牙已连接；数据包大小不足，位置请走 Wi-Fi");
                } else if (!ready) {
                    listener.onBleError("已连接，但不是可用的 FrameNav 服务");
                }
            });
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt bluetoothGatt,
                                          BluetoothGattCharacteristic characteristic,
                                          int status) {
            byte[] value = characteristic.getValue();
            main.post(() -> {
                if (isCurrentOperation(bluetoothGatt, characteristic)) {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        completeOperation(true, value, "");
                    } else {
                        // A single optional telemetry/config characteristic may be
                        // unavailable (or transiently rejected) while the GATT
                        // connection itself remains usable. Complete just this
                        // queued read so the remaining device info requests can
                        // continue instead of being cancelled together.
                        completeOperation(false, value, gattStatus(status));
                    }
                }
            });
        }

        @Override
        public void onCharacteristicRead(BluetoothGatt bluetoothGatt,
                                          BluetoothGattCharacteristic characteristic,
                                          byte[] value, int status) {
            main.post(() -> {
                if (isCurrentOperation(bluetoothGatt, characteristic)) {
                    if (status == BluetoothGatt.GATT_SUCCESS) {
                        completeOperation(true, value, "");
                    } else {
                        completeOperation(false, value, gattStatus(status));
                    }
                }
            });
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt bluetoothGatt,
                                           BluetoothGattCharacteristic characteristic,
                                           int status) {
            main.post(() -> {
                if (!isCurrentOperation(bluetoothGatt, characteristic)) {
                    return;
                }
                if (status == BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION
                        || status == BluetoothGatt.GATT_INSUFFICIENT_ENCRYPTION) {
                    beginBonding(bluetoothGatt);
                    return;
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    completeOperation(true, null, "");
                } else {
                    failConnection(gattStatus(status));
                }
            });
        }
    };

    private static String gattStatus(int status) {
        return status == BluetoothGatt.GATT_SUCCESS ? "" : "蓝牙状态码 " + status;
    }

    @SuppressLint("MissingPermission")
    private boolean refreshGattCache(BluetoothGatt bluetoothGatt) {
        try {
            java.lang.reflect.Method refresh = bluetoothGatt.getClass().getMethod("refresh");
            Object result = refresh.invoke(bluetoothGatt);
            return result instanceof Boolean && (Boolean) result;
        } catch (ReflectiveOperationException | RuntimeException ignored) {
            // Android exposes refresh() only as a hidden API on some releases.
            return false;
        }
    }

    private boolean isCurrentOperation(BluetoothGatt callbackGatt,
                                       BluetoothGattCharacteristic characteristic) {
        return callbackGatt == gatt && inFlight != null
                && inFlight.characteristic.getUuid().equals(characteristic.getUuid());
    }

    @SuppressLint("MissingPermission")
    private void failConnection(String message) {
        BluetoothGatt failedGatt = gatt;
        ready = false;
        waitingForBond = false;
        mtuRequested = false;
        negotiatedMtu = 23;
        characteristics.clear();
        if (operationTimeout != null) {
            main.removeCallbacks(operationTimeout);
            operationTimeout = null;
        }
        List<GattOperation> failedOperations = new ArrayList<>();
        if (inFlight != null) {
            failedOperations.add(inFlight);
            inFlight = null;
        }
        failedOperations.addAll(operations);
        operations.clear();
        gatt = null;
        if (failedGatt != null) {
            try {
                failedGatt.disconnect();
            } catch (SecurityException ignored) {
                // Closing still releases the local GATT object.
            }
            failedGatt.close();
        }
        for (GattOperation operation : failedOperations) {
            if (operation.readCallback != null) {
                operation.readCallback.onComplete(false, new byte[0], message);
            } else if (operation.writeCallback != null) {
                operation.writeCallback.onError(message);
            }
        }
        listener.onBleError(message);
        listener.onBleDisconnected();
    }

    private void addServiceCharacteristics(BluetoothGattService service) {
        if (service == null) {
            return;
        }
        for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
            characteristics.put(characteristic.getUuid(), characteristic);
        }
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            if (!scanning || result == null || result.getDevice() == null) {
                return;
            }
            if (!isFrameNav(result)) {
                return;
            }
            main.post(() -> connectToDevice(result.getDevice()));
        }

        @Override
        public void onScanFailed(int errorCode) {
            main.post(() -> {
                stopScan();
                listener.onBleError("蓝牙扫描失败，状态码 " + errorCode);
            });
        }
    };

    @SuppressLint("MissingPermission")
    private boolean isFrameNav(ScanResult result) {
        String name = null;
        try {
            name = result.getScanRecord() == null ? null : result.getScanRecord().getDeviceName();
            if (name == null) {
                name = result.getDevice().getName();
            }
        } catch (SecurityException ignored) {
            // Service UUID matching below still works without device-name access.
        }
        if (name != null && name.startsWith("FrameNav")) {
            return true;
        }
        if (result.getScanRecord() == null || result.getScanRecord().getServiceUuids() == null) {
            return false;
        }
        for (ParcelUuid service : result.getScanRecord().getServiceUuids()) {
            if (LOCATION_SERVICE.equals(service.getUuid()) || CONFIG_SERVICE.equals(service.getUuid())) {
                return true;
            }
        }
        return false;
    }

    @SuppressLint("MissingPermission")
    private void connectToDevice(BluetoothDevice found) {
        if (!scanning) {
            return;
        }
        stopScan();
        device = found;
        gattCacheRefreshAttempted = false;
        gattCacheRefreshSucceeded = false;
        listener.onBleState("正在连接 " + safeDeviceName(found) + "…");
        try {
            gatt = found.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
            if (gatt == null) {
                listener.onBleError("无法创建蓝牙连接");
            }
        } catch (SecurityException error) {
            listener.onBleError("缺少蓝牙连接权限");
        }
    }

    @SuppressLint("MissingPermission")
    private String safeDeviceName(BluetoothDevice bluetoothDevice) {
        try {
            return bluetoothDevice.getName() == null ? "FrameNav" : bluetoothDevice.getName();
        } catch (SecurityException error) {
            return "FrameNav";
        }
    }

    @SuppressLint("MissingPermission")
    private void stopScan() {
        if (!scanning) {
            return;
        }
        scanning = false;
        if (scanTimeout != null) {
            main.removeCallbacks(scanTimeout);
            scanTimeout = null;
        }
        try {
            if (scanner != null) {
                scanner.stopScan(scanCallback);
            }
        } catch (SecurityException ignored) {
            // Permission errors are reported by the caller's next operation.
        }
    }

    @SuppressLint("MissingPermission")
    private void disconnectInternal(boolean notify) {
        stopScan();
        List<GattOperation> cancelledOperations = new ArrayList<>();
        if (inFlight != null) {
            cancelledOperations.add(inFlight);
            inFlight = null;
        }
        cancelledOperations.addAll(operations);
        operations.clear();
        ready = false;
        waitingForBond = false;
        mtuRequested = false;
        negotiatedMtu = 23;
        characteristics.clear();
        if (operationTimeout != null) {
            main.removeCallbacks(operationTimeout);
            operationTimeout = null;
        }
        if (gatt != null) {
            try {
                gatt.disconnect();
            } catch (SecurityException ignored) {
                // Closing still releases the local GATT object.
            }
            gatt.close();
            gatt = null;
        }
        if (notify) {
            listener.onBleDisconnected();
        }
        for (GattOperation operation : cancelledOperations) {
            if (operation.readCallback != null) {
                operation.readCallback.onComplete(false, new byte[0], "蓝牙已断开");
            } else if (operation.writeCallback != null) {
                operation.writeCallback.onError("蓝牙已断开");
            }
        }
    }

    private static final class GattOperation {
        final BluetoothGattCharacteristic characteristic;
        final byte[] value;
        final ReadCallback readCallback;
        final Callback writeCallback;

        GattOperation(BluetoothGattCharacteristic characteristic, byte[] value,
                      ReadCallback readCallback, Callback writeCallback) {
            this.characteristic = characteristic;
            this.value = value;
            this.readCallback = readCallback;
            this.writeCallback = writeCallback;
        }
    }
}
