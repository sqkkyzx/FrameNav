package wiki.igp.framenav;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ClipData;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.os.IBinder;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.DragEvent;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.FrameLayout;

import org.json.JSONObject;
import org.json.JSONArray;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Collections;

public final class MainActivity extends Activity implements BleManager.Listener {
    private static final int REQUEST_BLE_PERMISSIONS = 501;
    private static final int REQUEST_LOCATION_PERMISSIONS = 502;
    private static final int REQUEST_CONTINUOUS_PERMISSIONS = 504;
    private static final long MAX_CACHED_LOCATION_AGE_MS = 60_000L;
    private static final long LOCATION_TIMEOUT_MS = 20_000L;
    private static final String DEVICE_ADDRESS = "10.254.254.1";
    private static final int[] TIMEZONE_VALUES = {-720, -660, -600, -570, -540, -480, -420, -360,
            -300, -240, -210, -180, -120, -60, 0, 60, 120, 180, 210, 240, 270, 300,
            330, 345, 360, 390, 420, 480, 525, 540, 570, 600, 630, 660, 720, 765, 780, 840};
    private static final int[] DST_VALUES = {0, 30, 60, 120};

    private final Handler main = new Handler(Looper.getMainLooper());
    private BleManager bleManager;
    private WifiClient wifiClient;
    private LocationManager locationManager;
    private LocationListener legacyLocationListener;
    private Runnable locationTimeout;
    private Runnable locationSettle;
    private Location bestLocation;
    private CancellationSignal gpsCancellation;
    private CancellationSignal networkCancellation;
    private boolean locationBusy;
    private boolean bleReady;
    private boolean bleLocationAvailable;
    private boolean bleConfigAvailable;
    private Location currentLocation;
    private PhoneLocationService phoneLocationService;
    private boolean phoneLocationBound;
    private boolean phoneLocationStarting;
    private int selectedTab;

    private TextView connectionStatus;
    private TextView wifiStatus;
    private TextView pairedDevices;
    private LinearLayout pairedDeviceRows;
    private TextView deviceStatus;
    private TextView deviceLocation;
    private TextView storageStatus;
    private TextView gnssSourceStatus;
    private TextView phoneSourceStatus;
    private TextView manualSourceStatus;
    private TextView activeSourceBadge;
    private TextView locationStatus;
    private TextView coordinates;
    private TextView accuracy;
    private TextView message;
    private EditText cameraName;
    private EditText cameraMac;
    private EditText stationSsid;
    private EditText stationPassword;
    private Spinner timezone;
    private Spinner dstOffset;
    private Switch gnssIdle;
    private Switch trackIdle;
    private Switch trackEnabled;
    private Switch phonePushEnabled;
    private Switch manualEnabled;
    private Switch apFallbackSwitch;
    private Spinner bluetoothMode;
    private EditText bluetoothBoot;
    private EditText bluetoothPeriod;
    private EditText bluetoothWindow;
    private Spinner wifiIdle;
    private EditText trackInterval;
    private EditText phoneInterval;
    private EditText manualLatitude;
    private EditText manualLongitude;
    private LinearLayout sourceOrderRows;
    private final List<String> locationOrder = new ArrayList<>(java.util.Arrays.asList("gnss", "phone", "manual"));
    private final Map<String, CheckBox> sourceChecks = new LinkedHashMap<>();
    private boolean sourceGnss = true;
    private boolean sourcePhone = true;
    private boolean sourceManual = true;
    private boolean trackingEnabled = true;
    private boolean phonePush = true;
    private boolean manualLocationEnabled = true;
    private boolean phoneContinuous;
    private boolean apFallback;
    private boolean updatingConfigControls;
    private int phoneLocationIntervalSeconds = 5;
    private boolean startupLocationRequested;
    private boolean configEditable;
    private boolean pendingHotspotWorkflow;
    private boolean deviceHotspotVerified;
    private Runnable afterHotspotConnected;
    private android.net.Network currentWifiNetwork;
    private android.net.Network verifiedHotspotNetwork;
    private android.net.ConnectivityManager connectivityManager;
    private final android.net.ConnectivityManager.NetworkCallback wifiNetworkCallback =
            new android.net.ConnectivityManager.NetworkCallback() {
                @Override public void onAvailable(android.net.Network network) {
                    runOnUiThread(() -> {
                        currentWifiNetwork = network;
                        wifiClient.setNetwork(network);
                        if (verifiedHotspotNetwork != null && !verifiedHotspotNetwork.equals(network)) {
                            verifiedHotspotNetwork = null;
                            deviceHotspotVerified = false;
                            if (phoneLocationService != null) {
                                phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
                            }
                        }
                        if ((pendingHotspotWorkflow || apFallback) && !deviceHotspotVerified) {
                            verifyDeviceHotspot();
                        }
                    });
                }

                @Override public void onLost(android.net.Network network) {
                    runOnUiThread(() -> {
                        if (network.equals(currentWifiNetwork)) {
                            currentWifiNetwork = null;
                            wifiClient.setNetwork(null);
                            checkCurrentWifi();
                        }
                        if (network.equals(verifiedHotspotNetwork)) {
                            verifiedHotspotNetwork = null;
                            deviceHotspotVerified = false;
                            if (phoneLocationService != null) {
                                phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
                            }
                        }
                    });
                }
            };
    private final Runnable configPoll = new Runnable() {
        @Override public void run() {
            if (!bleReady) return;
            if (bleConfigAvailable && !configEditable) readConfigOverBle();
            bleManager.readTelemetry(MainActivity.this::applyTelemetry);
            main.postDelayed(this, 30_000L);
        }
    };
    private LinearLayout tabContent;
    private final List<View> tabPages = new ArrayList<>();
    private final List<Button> tabButtons = new ArrayList<>();
    private Button connectBleButton;
    private Button connectWifiButton;
    private Button saveLocationButton;
    private Button saveConnectionButton;
    private Button editLocationButton;
    private Button editConnectionButton;
    private Button usePhoneLocationButton;
    private Button saveManualButton;
    private Button clearManualButton;
    private Runnable bleReconnect;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(0xFF14231D);
        getWindow().setNavigationBarColor(0xFF14231D);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) getWindow().setDecorFitsSystemWindows(false);
        locationManager = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        wifiClient = new WifiClient();
        bleManager = new BleManager(this, this);
        buildUi();
        connectivityManager = (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        if (connectivityManager != null) {
            android.net.NetworkRequest request = new android.net.NetworkRequest.Builder()
                    .addTransportType(android.net.NetworkCapabilities.TRANSPORT_WIFI).build();
            connectivityManager.registerNetworkCallback(request, wifiNetworkCallback);
        }
        phoneLocationStarting = false;
        phonePush = true;
        phoneContinuous = true;
        apFallback = getPreferences(MODE_PRIVATE).getBoolean("ap_fallback", false);
        manualLocationEnabled = true;
        setConfigEditable(false);
        requestLocationPermissionsThenLocate();
        checkCurrentWifi();
        refreshControls();
    }

    @Override
    protected void onDestroy() {
        main.removeCallbacks(configPoll);
        stopLocationRequest();
        if (bleManager != null && !phoneLocationStarting) {
            bleManager.close();
        }
        if (wifiClient != null) {
            wifiClient.close();
        }
        if (connectivityManager != null) {
            try { connectivityManager.unregisterNetworkCallback(wifiNetworkCallback); }
            catch (IllegalArgumentException ignored) { }
        }
        if (phoneLocationBound) {
            unbindService(phoneLocationConnection);
            phoneLocationBound = false;
        }
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        checkCurrentWifi();
        updateLocalPhoneStatus();
        if (pendingHotspotWorkflow && wifiClientHasNetwork()) {
            main.postDelayed(this::verifyDeviceHotspot, 700L);
        } else if (apFallback && wifiClientHasNetwork() && !deviceHotspotVerified) {
            main.postDelayed(this::verifyDeviceHotspot, 700L);
        }
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFFF5F7F5);
        root.setPadding(dp(16), dp(12), dp(16), dp(6));
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            int top = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    ? insets.getInsets(android.view.WindowInsets.Type.systemBars()).top
                    : insets.getSystemWindowInsetTop();
            int bottom = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    ? insets.getInsets(android.view.WindowInsets.Type.systemBars()).bottom
                    : insets.getSystemWindowInsetBottom();
            view.setPadding(dp(16), dp(12) + top, dp(16), dp(6) + bottom);
            return insets;
        });
        LinearLayout heading = row(root);
        TextView brand = text("FrameNav", 24, 0xFF14231D, true);
        heading.addView(brand, new LinearLayout.LayoutParams(0, dp(36), 1f));
        TextView firmware = text("设备管理", 13, 0xFF68756D, false);
        firmware.setGravity(Gravity.CENTER_VERTICAL | Gravity.RIGHT);
        heading.addView(firmware, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(36)));
        message = text("连接设备后读取状态和配置。", 13, 0xFF53625A, false);
        message.setPadding(dp(10), dp(8), dp(10), dp(8));
        message.setBackgroundColor(0xFFEAF0EB);
        LinearLayout.LayoutParams messageParams = matchWrap();
        messageParams.bottomMargin = dp(8);
        root.addView(message, messageParams);

        tabContent = new LinearLayout(this);
        tabContent.setOrientation(LinearLayout.VERTICAL);
        root.addView(tabContent, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        LinearLayout nav = new LinearLayout(this);
        nav.setOrientation(LinearLayout.HORIZONTAL);
        nav.setBackgroundColor(0xFFFFFFFF);
        nav.setPadding(0, dp(4), 0, dp(4));
        String[] tabs = {"当前状态", "连接配置", "定位配置", "高级"};
        for (int i = 0; i < tabs.length; i++) {
            final int index = i;
            Button tab = new Button(this);
            tab.setText(tabs[i]);
            tab.setTextSize(12);
            tab.setAllCaps(false);
            tab.setMinHeight(dp(48));
            tab.setPadding(dp(2), 0, dp(2), 0);
            tab.setOnClickListener(v -> showTab(index));
            nav.addView(tab, new LinearLayout.LayoutParams(0, dp(50), 1f));
            tabButtons.add(tab);
        }
        root.addView(nav, matchWrap());

        LinearLayout statusPage = newPage();
        LinearLayout connectionCard = card(statusPage, "连接信息");
        connectionStatus = text("未连接", 14, 0xFF68756D, false);
        connectionCard.addView(connectionStatus, matchWrap());
        wifiStatus = text("设备 IP 通道未连接", 13, 0xFF68756D, false);
        connectionCard.addView(wifiStatus, matchWrap());
        LinearLayout connectionButtons = row(connectionCard);
        connectBleButton = button("扫描并连接 BLE", 0xFF28704A);
        connectBleButton.setOnClickListener(view -> requestBlePermissionsThenConnect());
        connectionButtons.addView(connectBleButton, weighted());
        Button disconnect = button("断开", 0xFF53625A);
        disconnect.setOnClickListener(view -> {
            bleManager.disconnect();
            setMessage("BLE 已断开。", false);
        });
        connectionButtons.addView(disconnect, weighted());
        LinearLayout locationCard = card(statusPage, "定位信息");
        LinearLayout activeRow = row(locationCard);
        activeSourceBadge = text("当前使用：—", 12, 0xFFFFFFFF, true);
        activeSourceBadge.setPadding(dp(8), dp(5), dp(8), dp(5));
        activeSourceBadge.setBackgroundColor(0xFF28704A);
        activeRow.addView(activeSourceBadge, matchWrap());
        deviceStatus = text("设备定位：—", 13, 0xFF68756D, false);
        locationCard.addView(deviceStatus, matchWrap());
        gnssSourceStatus = text("GNSS 定位：—", 13, 0xFF53625A, false);
        gnssSourceStatus.setPadding(0, dp(8), 0, dp(2));
        locationCard.addView(gnssSourceStatus, matchWrap());
        phoneSourceStatus = text("手机定位：—", 13, 0xFF53625A, false);
        phoneSourceStatus.setPadding(0, dp(8), 0, dp(2));
        locationCard.addView(phoneSourceStatus, matchWrap());
        manualSourceStatus = text("手动设置：—", 13, 0xFF53625A, false);
        manualSourceStatus.setPadding(0, dp(8), 0, dp(2));
        locationCard.addView(manualSourceStatus, matchWrap());
        deviceLocation = text("—", 12, 0xFF68756D, false);
        locationCard.addView(deviceLocation, matchWrap());
        locationStatus = text("手机位置尚未获取", 12, 0xFF68756D, false);
        coordinates = text("—", 12, 0xFF68756D, false);
        accuracy = text("精度 —", 12, 0xFF68756D, false);
        storageStatus = text("轨迹存储：—", 13, 0xFF53625A, false);
        LinearLayout storageCard = card(statusPage, "存储轨迹");
        storageCard.addView(storageStatus, matchWrap());
        addPage(statusPage);

        LinearLayout connectionPage = newPage();
        LinearLayout ipCard = card(connectionPage, "设备 IP 连接");
        TextView deviceAddress = text("固定地址  " + DEVICE_ADDRESS, 14, 0xFF14231D, true);
        ipCard.addView(deviceAddress, matchWrap());
        connectWifiButton = button("打开系统 Wi-Fi 设置", 0xFF28704A);
        connectWifiButton.setOnClickListener(view -> openWifiSettings());
        ipCard.addView(connectWifiButton, matchWrap());
        TextView wifiHint = text("连接设备热点 FrameNav-MAC 后六位，再返回应用。热点密码固定为 framenav。设置读取和保存使用 HTTP IP 直连。", 13, 0xFF68756D, false);
        wifiHint.setPadding(0, dp(6), 0, 0);
        ipCard.addView(wifiHint, matchWrap());
        Button wakeWifi = button("通过蓝牙唤醒设备热点", 0xFF53625A);
        wakeWifi.setOnClickListener(view -> wakeDeviceWifi());
        ipCard.addView(wakeWifi, matchWrap());
        editConnectionButton = button("编辑连接配置", 0xFF53625A);
        editConnectionButton.setOnClickListener(v -> setConfigEditable(true));
        ipCard.addView(editConnectionButton, matchWrap());
        Button pairCamera = button("打开相机配对窗口", 0xFF53625A);
        pairCamera.setOnClickListener(v -> startCameraPairing());
        ipCard.addView(pairCamera, matchWrap());
        LinearLayout bondsCard = card(connectionPage, "已配对设备");
        pairedDevices = text("尚未读取", 13, 0xFF68756D, false);
        bondsCard.addView(pairedDevices, matchWrap());
        TextView pairedPhonesHeading = text("管理手机", 14, 0xFF14231D, true);
        pairedPhonesHeading.setPadding(0, dp(8), 0, dp(3));
        bondsCard.addView(pairedPhonesHeading, matchWrap());
        TextView pairedPhone = text("本机 · " + Build.MODEL + "（BLE 状态见当前状态）", 13, 0xFF53625A, false);
        bondsCard.addView(pairedPhone, matchWrap());
        Button managePhoneBond = button("在系统蓝牙设置中管理手机配对", 0xFF53625A);
        managePhoneBond.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)));
        bondsCard.addView(managePhoneBond, matchWrap());
        TextView pairedCamerasHeading = text("推送相机", 14, 0xFF14231D, true);
        pairedCamerasHeading.setPadding(0, dp(12), 0, dp(3));
        bondsCard.addView(pairedCamerasHeading, matchWrap());
        pairedDeviceRows = new LinearLayout(this);
        pairedDeviceRows.setOrientation(LinearLayout.VERTICAL);
        bondsCard.addView(pairedDeviceRows, matchWrap());
        Button refreshBonds = button("连接热点查看相机", 0xFF53625A);
        refreshBonds.setOnClickListener(v -> wakeDeviceWifi());
        bondsCard.addView(refreshBonds, matchWrap());
        addPage(connectionPage);

        LinearLayout locationPage = newPage();
        LinearLayout commonCard = card(locationPage, "通用");
        sourceOrderRows = new LinearLayout(this);
        sourceOrderRows.setOrientation(LinearLayout.VERTICAL);
        commonCard.addView(text("定位来源和优先级 · 拖动排序", 12, 0xFF53625A, false), matchWrap());
        commonCard.addView(sourceOrderRows, matchWrap());
        rebuildSourceRows();
        trackEnabled = addSwitch(commonCard, "开启轨迹记录", true, null);
        trackIdle = addSwitch(commonCard, "未连接相机时也记录轨迹", false, null);
        trackInterval = addField(commonCard, "轨迹记录频率（秒）", "15", InputType.TYPE_CLASS_NUMBER);
        trackInterval.setText("15");
        LinearLayout timeCard = card(locationPage, "照片时间");
        timezone = addChoice(timeCard, "照片时区", new String[]{
                "UTC−12:00 国际日期变更线西", "UTC−11:00 萨摩亚", "UTC−10:00 夏威夷",
                "UTC−09:30 马克萨斯群岛", "UTC−09:00 阿拉斯加", "UTC−08:00 太平洋时间",
                "UTC−07:00 山地时间", "UTC−06:00 中部时间", "UTC−05:00 东部时间",
                "UTC−04:00 大西洋时间", "UTC−03:30 纽芬兰", "UTC−03:00 巴西利亚",
                "UTC−02:00 南乔治亚", "UTC−01:00 亚速尔群岛", "UTC±00:00 UTC",
                "UTC+01:00 柏林 / 巴黎", "UTC+02:00 雅典 / 开罗", "UTC+03:00 莫斯科 / 利雅得",
                "UTC+03:30 德黑兰", "UTC+04:00 迪拜", "UTC+04:30 喀布尔", "UTC+05:00 伊斯兰堡",
                "UTC+05:30 印度", "UTC+05:45 尼泊尔", "UTC+06:00 达卡", "UTC+06:30 仰光",
                "UTC+07:00 曼谷 / 雅加达", "UTC+08:00 北京 / 上海 / 新加坡", "UTC+08:45 尤克拉",
                "UTC+09:00 东京 / 首尔", "UTC+09:30 阿德莱德", "UTC+10:00 悉尼",
                "UTC+10:30 豪勋爵岛", "UTC+11:00 所罗门群岛", "UTC+12:00 奥克兰 / 斐济",
                "UTC+12:45 查塔姆群岛", "UTC+13:00 汤加", "UTC+14:00 莱恩群岛"});
        dstOffset = addChoice(timeCard, "夏时令", new String[]{"关闭", "增加 30 分钟", "增加 1 小时", "增加 2 小时"});
        LinearLayout gnssCard = card(locationPage, "GNSS 定位");
        gnssIdle = addSwitch(gnssCard, "未连接任何设备时也持续搜星", false, null);
        LinearLayout phoneCard = card(locationPage, "手机定位");
        phonePushEnabled = addSwitch(phoneCard, "通过蓝牙持续推送手机定位", true,
                checked -> { phonePush = checked; updateLocationService(); });
        phoneInterval = addField(phoneCard, "手机定位获取频率（秒）", "5", InputType.TYPE_CLASS_NUMBER);
        phoneInterval.setText("5");
        apFallbackSwitch = addSwitch(phoneCard, "BLE 不可用时使用设备 AP 回退", false,
                checked -> {
                    apFallback = checked;
                    getPreferences(MODE_PRIVATE).edit().putBoolean("ap_fallback", checked).apply();
                    if (phoneLocationService != null) phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
                    if (checked) {
                        if (wifiClientHasNetwork()) verifyDeviceHotspot();
                        else wakeDeviceWifi();
                    }
                });
        apFallbackSwitch.setChecked(apFallback);
        LinearLayout manualCard = card(locationPage, "手动设置经纬度（兜底位置）");
        LinearLayout coordinateRow = row(manualCard);
        LinearLayout longitudeColumn = new LinearLayout(this);
        longitudeColumn.setOrientation(LinearLayout.VERTICAL);
        coordinateRow.addView(longitudeColumn, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout latitudeColumn = new LinearLayout(this);
        latitudeColumn.setOrientation(LinearLayout.VERTICAL);
        coordinateRow.addView(latitudeColumn, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        manualLongitude = addField(longitudeColumn, "经度", "-180 至 180", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        manualLatitude = addField(latitudeColumn, "纬度", "-90 至 90", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        usePhoneLocationButton = button("使用手机定位填入", 0xFF53625A);
        usePhoneLocationButton.setOnClickListener(v -> fillManualFromPhone());
        manualCard.addView(usePhoneLocationButton, matchWrap());
        manualEnabled = addSwitch(manualCard, "使用真实定位持续更新", true,
                checked -> { manualLocationEnabled = checked; phoneContinuous = checked; updateLocationService(); });
        saveManualButton = button("保存手动位置", 0xFF28704A);
        saveManualButton.setOnClickListener(v -> saveManualLocation());
        manualCard.addView(saveManualButton, matchWrap());
        clearManualButton = button("清除手动位置", 0xFF8B3D36);
        clearManualButton.setOnClickListener(v -> clearManualLocation());
        manualCard.addView(clearManualButton, matchWrap());
        saveLocationButton = button("保存定位配置", 0xFF28704A);
        saveLocationButton.setOnClickListener(v -> saveConfig());
        commonCard.addView(saveLocationButton, matchWrap());
        editLocationButton = button("编辑定位配置", 0xFF53625A);
        editLocationButton.setOnClickListener(v -> setConfigEditable(true));
        commonCard.addView(editLocationButton, matchWrap());
        TextView locationNote = text("手机位置自动获取；持续采集与蓝牙推送分别控制。保存设备配置需要连接 FrameNav 热点。", 12, 0xFF68756D, false);
        locationNote.setPadding(0, dp(7), 0, 0);
        manualCard.addView(locationNote, matchWrap());
        addPage(locationPage);

        LinearLayout advancedPage = newPage();
        LinearLayout cameraCard = card(connectionPage, "相机与网络");
        cameraName = addField(cameraCard, "相机名称前缀", "留空自动选择", InputType.TYPE_CLASS_TEXT);
        cameraMac = addField(cameraCard, "相机地址前缀", "选填", InputType.TYPE_CLASS_TEXT);
        stationSsid = addField(cameraCard, "设备连接的 Wi-Fi 名称", "仅支持 2.4 GHz", InputType.TYPE_CLASS_TEXT);
        Button scanNetworks = button("扫描附近 Wi-Fi", 0xFF53625A);
        scanNetworks.setOnClickListener(v -> scanWifiNetworks());
        cameraCard.addView(scanNetworks, matchWrap());
        stationPassword = addField(cameraCard, "Wi-Fi 密码", "留空保持不变", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        wifiIdle = addChoice(cameraCard, "无人使用时关闭设备 Wi-Fi", new String[]{"5 分钟", "15 分钟"});
        LinearLayout bleCard = card(connectionPage, "蓝牙省电");
        bluetoothMode = addChoice(bleCard, "设备连接模式", new String[]{"省电", "平衡", "自定义"});
        bluetoothBoot = addField(bleCard, "开机后查找设备（秒）", "0 至 300", InputType.TYPE_CLASS_NUMBER);
        bluetoothPeriod = addField(bleCard, "再次查找间隔（秒）", "1 至 10.24", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        bluetoothWindow = addField(bleCard, "每次查找时长（毫秒）", "20 至 1000", InputType.TYPE_CLASS_NUMBER);
        bluetoothBoot.setText("30");
        bluetoothPeriod.setText("3.0");
        bluetoothWindow.setText("100");
        bluetoothMode.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                boolean custom = position == 2;
                bluetoothBoot.setEnabled(custom); bluetoothPeriod.setEnabled(custom); bluetoothWindow.setEnabled(custom);
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) { }
        });
        addPage(advancedPage);
        saveConnectionButton = button("保存连接配置", 0xFF28704A);
        saveConnectionButton.setOnClickListener(v -> saveConfig());
        connectionPage.addView(saveConnectionButton, matchWrap());
        showTab(0);
        setContentView(root);
        storageStatus.setText("等待 BLE 设备遥测");
    }

    private LinearLayout newPage() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(0, dp(4), 0, dp(14));
        return page;
    }

    private void addPage(LinearLayout page) {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(page);
        tabPages.add(scroll);
        if (tabPages.size() == 1) tabContent.addView(scroll, matchWrap());
    }

    private void showTab(int index) {
        selectedTab = index;
        if (tabContent == null || tabPages.isEmpty()) return;
        tabContent.removeAllViews();
        tabContent.addView(tabPages.get(index), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        for (int i = 0; i < tabButtons.size(); i++) {
            tabButtons.get(i).setTextColor(i == index ? 0xFF28704A : 0xFF68756D);
            tabButtons.get(i).setBackgroundTintList(android.content.res.ColorStateList.valueOf(
                    i == index ? 0xFFEAF0EB : 0xFFFFFFFF));
        }
    }

    private Spinner addChoice(LinearLayout parent, String label, String[] options) {
        TextView title = text(label, 12, 0xFF53625A, false);
        LinearLayout.LayoutParams labelParams = matchWrap();
        labelParams.topMargin = dp(7);
        parent.addView(title, labelParams);
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, options);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        parent.addView(spinner, matchWrap());
        return spinner;
    }

    private interface SwitchListener { void onChanged(boolean checked); }

    private Switch addSwitch(LinearLayout parent, String label, boolean initial, SwitchListener listener) {
        Switch control = new Switch(this);
        control.setText(label);
        control.setTextColor(0xFF14231D);
        control.setTextSize(14);
        control.setChecked(initial);
        parent.addView(control, matchWrap());
        if (listener != null) control.setOnCheckedChangeListener((button, checked) -> {
            if (!updatingConfigControls) listener.onChanged(checked);
        });
        return control;
    }

    private void rebuildSourceRows() {
        if (sourceOrderRows == null) return;
        sourceOrderRows.removeAllViews();
        sourceChecks.clear();
        String[] ids = {"gnss", "phone", "manual"};
        String[] labels = {"GNSS 定位", "手机定位", "手动设置经纬度"};
        for (String id : locationOrder) {
            int index = java.util.Arrays.asList(ids).indexOf(id);
            if (index < 0) continue;
            LinearLayout line = new LinearLayout(this);
            line.setGravity(Gravity.CENTER_VERTICAL);
            line.setTag(id);
            TextView drag = text("↕", 20, 0xFF53625A, true);
            drag.setGravity(Gravity.CENTER);
            drag.setOnTouchListener((view, event) -> {
                if (event.getAction() == android.view.MotionEvent.ACTION_DOWN) {
                    View.DragShadowBuilder shadow = new View.DragShadowBuilder(line);
                    line.startDragAndDrop(ClipData.newPlainText("source", id), shadow, line, 0);
                    return true;
                }
                return false;
            });
            line.addView(drag, new LinearLayout.LayoutParams(dp(38), dp(48)));
            CheckBox check = new CheckBox(this);
            check.setText((locationOrder.indexOf(id) + 1) + ".  " + labels[index]);
            check.setTextColor(0xFF14231D);
            check.setTextSize(14);
            check.setChecked("gnss".equals(id) ? sourceGnss : "phone".equals(id) ? sourcePhone : sourceManual);
            sourceChecks.put(id, check);
            line.addView(check, new LinearLayout.LayoutParams(0, dp(48), 1f));
            check.setOnCheckedChangeListener((button, checked) -> {
                if (updatingConfigControls) return;
                if ("gnss".equals(id)) sourceGnss = checked;
                else if ("phone".equals(id)) sourcePhone = checked;
                else sourceManual = checked;
            });
            line.setOnDragListener((view, event) -> {
                if (event.getAction() == DragEvent.ACTION_DROP) {
                    String from = String.valueOf(event.getLocalState());
                    String to = String.valueOf(view.getTag());
                    int fromIndex = locationOrder.indexOf(from);
                    int toIndex = locationOrder.indexOf(to);
                    if (fromIndex >= 0 && toIndex >= 0) {
                        Collections.swap(locationOrder, fromIndex, toIndex);
                        rebuildSourceRows();
                    }
                    return true;
                }
                return true;
            });
            sourceOrderRows.addView(line, matchWrap());
        }
    }

    private void setConfigEditable(boolean editable) {
        configEditable = editable;
        for (EditText field : new EditText[]{cameraName, cameraMac, stationSsid, stationPassword,
                trackInterval, phoneInterval, manualLatitude, manualLongitude,
                bluetoothBoot, bluetoothPeriod, bluetoothWindow}) {
            if (field != null) field.setEnabled(editable);
        }
        for (Spinner field : new Spinner[]{timezone, dstOffset, wifiIdle, bluetoothMode}) {
            if (field != null) field.setEnabled(editable);
        }
        for (Switch field : new Switch[]{trackEnabled, trackIdle, gnssIdle, phonePushEnabled, manualEnabled}) {
            if (field != null) field.setEnabled(editable);
        }
        for (CheckBox field : sourceChecks.values()) field.setEnabled(editable);
        if (saveConnectionButton != null) saveConnectionButton.setVisibility(editable ? View.VISIBLE : View.GONE);
        if (saveLocationButton != null) saveLocationButton.setVisibility(editable ? View.VISIBLE : View.GONE);
        if (editConnectionButton != null) editConnectionButton.setVisibility(editable ? View.GONE : View.VISIBLE);
        if (editLocationButton != null) editLocationButton.setVisibility(editable ? View.GONE : View.VISIBLE);
        if (usePhoneLocationButton != null) usePhoneLocationButton.setEnabled(editable);
        if (saveManualButton != null) saveManualButton.setVisibility(editable ? View.VISIBLE : View.GONE);
        if (clearManualButton != null) clearManualButton.setVisibility(editable ? View.VISIBLE : View.GONE);
        if (editable) {
            setMessage("配置已进入编辑状态。保存前请连接设备热点。", false);
            wakeDeviceWifi();
        }
    }

    private LinearLayout card(LinearLayout parent, String title) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(15), dp(16), dp(15));
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xFFFFFFFF);
        background.setCornerRadius(dp(7));
        background.setStroke(dp(1), 0xFFD6DED8);
        card.setBackground(background);
        LinearLayout.LayoutParams params = matchWrap();
        params.bottomMargin = dp(12);
        parent.addView(card, params);
        TextView heading = text(title, 17, 0xFF14231D, true);
        heading.setPadding(0, 0, 0, dp(10));
        card.addView(heading, matchWrap());
        return card;
    }

    private EditText addField(LinearLayout parent, String label, String hint, int inputType) {
        TextView text = text(label, 12, 0xFF53625A, false);
        LinearLayout.LayoutParams labelParams = matchWrap();
        labelParams.topMargin = dp(7);
        parent.addView(text, labelParams);
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setTextSize(15);
        input.setTextColor(0xFF14231D);
        input.setHintTextColor(0xFF87928B);
        input.setHint(hint);
        input.setInputType(inputType);
        input.setPadding(dp(11), dp(8), dp(11), dp(8));
        input.setBackground(editBackground());
        LinearLayout.LayoutParams params = matchWrap();
        params.topMargin = dp(4);
        params.bottomMargin = dp(3);
        parent.addView(input, params);
        return input;
    }

    private GradientDrawable editBackground() {
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xFFF8FAF8);
        background.setCornerRadius(dp(5));
        background.setStroke(dp(1), 0xFFD6DED8);
        return background;
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        }
        return view;
    }

    private Button button(String label, int color) {
        Button button = new Button(this);
        button.setText(label);
        button.setTextSize(13);
        button.setAllCaps(false);
        button.setTextColor(0xFFFFFFFF);
        button.setMinHeight(dp(44));
        button.setMinimumHeight(dp(44));
        button.setBackgroundTintList(android.content.res.ColorStateList.valueOf(color));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(46));
        params.setMargins(dp(3), dp(7), dp(3), dp(7));
        button.setLayoutParams(params);
        return button;
    }

    private LinearLayout row(LinearLayout parent) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        parent.addView(row, matchWrap());
        return row;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams weighted() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, dp(48), 1f);
        params.setMargins(dp(2), dp(3), dp(2), dp(3));
        return params;
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void requestBlePermissionsThenConnect() {
        List<String> needed = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_SCAN);
            }
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                needed.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
        } else if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (needed.isEmpty()) {
            bleManager.connect();
        } else {
            requestPermissions(needed.toArray(new String[0]), REQUEST_BLE_PERMISSIONS);
        }
    }

    private void requestLocationPermissionsThenLocate() {
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
                || checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
            startLocationRequest();
        } else {
            requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION,
                    Manifest.permission.ACCESS_COARSE_LOCATION}, REQUEST_LOCATION_PERMISSIONS);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_BLE_PERMISSIONS) {
            boolean granted = grantResults.length > 0;
            for (int result : grantResults) {
                granted &= result == PackageManager.PERMISSION_GRANTED;
            }
            if (!granted) {
                setMessage("需要附近设备权限才能扫描和连接 FrameNav。", true);
                return;
            }
            bleManager.connect();
        } else if (requestCode == REQUEST_LOCATION_PERMISSIONS) {
            if (hasLocationPermission()) {
                startLocationRequest();
            } else {
                setMessage("需要定位权限才能读取手机位置。", true);
            }
        } else if (requestCode == REQUEST_CONTINUOUS_PERMISSIONS) {
            boolean notificationAllowed = Build.VERSION.SDK_INT < 33
                    || checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED;
            if (hasLocationPermission() && notificationAllowed) {
                startPhoneLocationService();
            } else {
                phoneLocationStarting = false;
                setMessage("持续定位需要手机定位权限和通知权限。", true);
            }
        }
    }

    private void openWifiSettings() {
        startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
    }

    private void wakeDeviceWifi() {
        pendingHotspotWorkflow = true;
        if (!bleReady) {
            setMessage("先连接 BLE，再唤醒设备热点。", false);
            requestBlePermissionsThenConnect();
            return;
        }
        bleManager.requestWifiWake(new BleManager.Callback() {
            @Override public void onSuccess() {
                setMessage("设备热点已唤醒，请连接 FrameNav 热点。", false);
                openWifiSettings();
            }
            @Override public void onError(String error) { setMessage(error, true); }
        });
    }

    private boolean wifiClientHasNetwork() {
        android.net.ConnectivityManager manager = (android.net.ConnectivityManager)
                getSystemService(CONNECTIVITY_SERVICE);
        if (manager == null) return false;
        for (android.net.Network network : manager.getAllNetworks()) {
            android.net.NetworkCapabilities caps = manager.getNetworkCapabilities(network);
            if (caps != null && caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)) return true;
        }
        return false;
    }

    private void verifyDeviceHotspot() {
        wifiClient.getConfigurationPage(DEVICE_ADDRESS, new WifiClient.Callback() {
            @Override public void onSuccess(String ignored) {
                pendingHotspotWorkflow = false;
                deviceHotspotVerified = true;
                verifiedHotspotNetwork = currentWifiNetwork;
                refreshPairedDevices();
                refreshStorageStatus();
                if (phoneLocationService != null) phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
                setMessage("已连接 FrameNav 设备热点。", false);
                Runnable action = afterHotspotConnected;
                afterHotspotConnected = null;
                if (action != null) main.post(action);
            }
            @Override public void onError(String error, Throwable cause) {
                deviceHotspotVerified = false;
                if (phoneLocationService != null) phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
                setMessage("当前 Wi-Fi 还不是 FrameNav 热点，请连接 FrameNav-MAC 后六位。", false);
            }
        });
    }

    private void startCameraPairing() {
        if (!deviceHotspotVerified) {
            afterHotspotConnected = this::startCameraPairing;
            wakeDeviceWifi();
            return;
        }
        wifiClient.startCameraPairing(DEVICE_ADDRESS, new WifiClient.Callback() {
            @Override public void onSuccess(String response) {
                setMessage("设备已进入相机配对窗口（90 秒）。", false);
            }
            @Override public void onError(String error, Throwable cause) {
                setMessage(error + "。请先连接设备热点。", true);
            }
        });
    }

    private void refreshPairedDevices() {
        wifiClient.getPairedDevices(DEVICE_ADDRESS, new WifiClient.Callback() {
            @Override public void onSuccess(String response) {
                try {
                    JSONArray bonds = new JSONObject(response).optJSONArray("bonds");
                    pairedDevices.setText(bonds == null || bonds.length() == 0 ? "没有已配对设备" : "");
                    if (bonds == null) return;
                    pairedDeviceRows.removeAllViews();
                    for (int i = 0; i < bonds.length(); i++) {
                        JSONObject bond = bonds.optJSONObject(i);
                        if (bond == null) continue;
                        String name = bond.optString("name", "相机");
                        String mac = bond.optString("mac", "");
                        String token = bond.optString("token", "");
                        String type = bond.optString("type", "0");
                        String status = bond.optBoolean("connected", false) ? " · 已连接" : "";
                        LinearLayout row = new LinearLayout(MainActivity.this);
                        row.setGravity(Gravity.CENTER_VERTICAL);
                        TextView label = text(name + "\n" + mac + status, 13, 0xFF14231D, false);
                        row.addView(label, new LinearLayout.LayoutParams(0, dp(56), 1f));
                        Button remove = button("移除", 0xFF8B3D36);
                        remove.setOnClickListener(v -> new AlertDialog.Builder(MainActivity.this)
                                .setTitle("移除已配对设备")
                                .setMessage("确定移除 " + name + "？")
                                .setNegativeButton("取消", null)
                                .setPositiveButton("移除", (dialog, which) -> wifiClient.deletePairedDevice(
                                        DEVICE_ADDRESS, token, type, new WifiClient.Callback() {
                                            @Override public void onSuccess(String ignored) { refreshPairedDevices(); }
                                            @Override public void onError(String error, Throwable cause) { setMessage(error, true); }
                                        })).show());
                        row.addView(remove, new LinearLayout.LayoutParams(dp(84), dp(48)));
                        pairedDeviceRows.addView(row, matchWrap());
                    }
                } catch (Exception error) {
                    pairedDevices.setText("设备列表无法解析");
                }
            }
            @Override public void onError(String error, Throwable cause) {
                pairedDevices.setText("无法读取设备列表：" + error);
            }
        });
    }

    private void scanWifiNetworks() {
        setMessage("正在请求设备扫描 Wi-Fi…", false);
        wifiClient.startWifiScan(DEVICE_ADDRESS, new WifiClient.Callback() {
            @Override public void onSuccess(String response) { pollWifiNetworks(0); }
            @Override public void onError(String error, Throwable cause) {
                setMessage(error + "。请确认设备 IP 通道可用。", true);
            }
        });
    }

    private void pollWifiNetworks(int attempt) {
        if (attempt >= 40) {
            setMessage("Wi-Fi 扫描等待超时，请重试。", true);
            return;
        }
        main.postDelayed(() -> wifiClient.getWifiScan(DEVICE_ADDRESS, new WifiClient.Callback() {
            @Override public void onSuccess(String response) {
                try {
                    JSONObject result = new JSONObject(response);
                    if (result.optBoolean("scanning", false)) {
                        pollWifiNetworks(attempt + 1);
                        return;
                    }
                    JSONArray networks = result.optJSONArray("networks");
                    if (networks == null || networks.length() == 0) {
                        setMessage("没有扫描到 Wi-Fi 网络。", false);
                        return;
                    }
                    List<String> names = new ArrayList<>();
                    List<String> ssids = new ArrayList<>();
                    for (int i = 0; i < networks.length(); i++) {
                        JSONObject network = networks.optJSONObject(i);
                        if (network == null) continue;
                        String ssid = network.optString("ssid", "");
                        if (ssid.isEmpty()) continue;
                        ssids.add(ssid);
                        names.add(ssid + (network.optBoolean("secure", true) ? " · 加密" : " · 开放")
                                + " · " + network.optInt("rssi", 0) + " dBm");
                    }
                    new AlertDialog.Builder(MainActivity.this)
                            .setTitle("选择设备要连接的 Wi-Fi")
                            .setItems(names.toArray(new String[0]), (dialog, which) -> {
                                if (which < ssids.size()) stationSsid.setText(ssids.get(which));
                            }).setNegativeButton("取消", null).show();
                    setMessage("扫描完成。", false);
                } catch (Exception error) {
                    setMessage("Wi-Fi 扫描响应无法解析。", true);
                }
            }
            @Override public void onError(String error, Throwable cause) {
                if (attempt < 2) pollWifiNetworks(attempt + 1);
                else setMessage(error, true);
            }
        }), 350L);
    }

    private void checkCurrentWifi() {
        try {
            android.net.ConnectivityManager manager =
                    (android.net.ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            android.net.Network wifi = null;
            if (manager != null) {
                for (android.net.Network network : manager.getAllNetworks()) {
                    android.net.NetworkCapabilities caps = manager.getNetworkCapabilities(network);
                    if (caps != null && caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)) {
                        wifi = network;
                        break;
                    }
                }
            }
            if (wifi != null) {
                wifiClient.setNetwork(wifi);
                currentWifiNetwork = wifi;
                if (verifiedHotspotNetwork != null && !verifiedHotspotNetwork.equals(wifi)) {
                    verifiedHotspotNetwork = null;
                    deviceHotspotVerified = false;
                    if (phoneLocationService != null) {
                        phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
                    }
                }
                wifiStatus.setText("手机已连接 Wi-Fi · HTTP IP 可用");
                refreshControls();
            } else {
                wifiClient.setNetwork(null);
                currentWifiNetwork = null;
                verifiedHotspotNetwork = null;
                deviceHotspotVerified = false;
                wifiStatus.setText("设备 IP 通道未连接");
            }
        } catch (Exception ignored) {
            wifiStatus.setText("设备 IP 通道未连接");
        }
    }

    private final ServiceConnection phoneLocationConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            phoneLocationBound = true;
            phoneLocationService = ((PhoneLocationService.LocalBinder) binder).service();
            phoneLocationService.setListener(new PhoneLocationService.Listener() {
                @Override
                public void onLocation(Location location) {
                    showLocation(location, "持续定位已更新");
                }

                @Override
                public void onServiceState(String state, boolean error) {
                    setMessage(state, error);
                    if (state.contains("已停止") || state.contains("未启动")) phoneLocationStarting = false;
                }
            });
            phoneLocationService.setLocationWriter(new PhoneLocationService.LocationWriter() {
                @Override
                public boolean isReady() {
                    return (bleReady && bleLocationAvailable) || (apFallback && deviceHotspotVerified);
                }

                @Override
                public boolean isBleReady() {
                    return bleReady && bleLocationAvailable;
                }

                @Override
                public void write(Location location, PhoneLocationService.ResultCallback callback) {
                    if (bleReady && bleLocationAvailable) {
                        bleManager.writeLocation(location, new BleManager.Callback() {
                            @Override public void onSuccess() { callback.onResult(true, ""); }
                            @Override public void onError(String error) {
                                if (apFallback && deviceHotspotVerified) {
                                    postLiveLocationOverWifi(location, callback);
                                } else {
                                    callback.onResult(false, error);
                                }
                            }
                        });
                        return;
                    }
                    if (!apFallback || !deviceHotspotVerified) {
                        callback.onResult(false, "BLE 未连接且 AP 回退未启用");
                        return;
                    }
                    postLiveLocationOverWifi(location, callback);
                }
            });
            phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
            phoneLocationService.configure(phoneContinuous || phonePush, phoneContinuous || phonePush,
                    phonePush, phoneLocationIntervalSeconds);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            phoneLocationBound = false;
            phoneLocationService = null;
        }
    };

    private void postLiveLocationOverWifi(Location location,
                                          PhoneLocationService.ResultCallback callback) {
        if (!apFallback || !deviceHotspotVerified) {
            callback.onResult(false, "设备 AP 回退未启用或热点未验证");
            return;
        }
        long timestamp = location.getTime() > 0 ? location.getTime() : System.currentTimeMillis();
        double accuracy = location.hasAccuracy() ? location.getAccuracy() : 0;
        wifiClient.postLiveLocation(DEVICE_ADDRESS, location.getLatitude(), location.getLongitude(),
                accuracy, timestamp, System.currentTimeMillis(), new WifiClient.Callback() {
                    @Override public void onSuccess(String ignored) { callback.onResult(true, ""); }
                    @Override public void onError(String error, Throwable cause) { callback.onResult(false, error); }
                });
    }

    private void setContinuousLocation(boolean enabled) {
        phoneContinuous = enabled;
        updateLocationService();
    }

    private void updateLocationService() {
        if (!phoneContinuous && !phonePush) {
            phoneLocationStarting = false;
            stopPhoneLocationService();
            return;
        }
        phoneLocationStarting = true;
        List<String> needed = new ArrayList<>();
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.ACCESS_FINE_LOCATION);
            needed.add(Manifest.permission.ACCESS_COARSE_LOCATION);
        }
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS);
        }
        if (!needed.isEmpty()) {
            requestPermissions(needed.toArray(new String[0]), REQUEST_CONTINUOUS_PERMISSIONS);
            return;
        }
        if (!bleReady || !bleLocationAvailable) {
            requestBlePermissionsThenConnect();
        }
        startPhoneLocationService();
    }

    private void startPhoneLocationService() {
        Intent intent = new Intent(this, PhoneLocationService.class).setAction(PhoneLocationService.ACTION_START);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(intent);
            else startService(intent);
            phoneLocationBound = bindService(intent, phoneLocationConnection, Context.BIND_AUTO_CREATE);
            if (phoneLocationBound && phoneLocationService != null) {
                phoneLocationService.configure(phoneContinuous || phonePush, phoneContinuous || phonePush,
                        phonePush, phoneLocationIntervalSeconds);
            }
            phoneLocationStarting = true;
            getPreferences(MODE_PRIVATE).edit().putBoolean("continuous_location", true).apply();
            setMessage("持续定位服务已启动。", false);
        } catch (SecurityException error) {
            phoneLocationStarting = false;
            setMessage("启动持续定位失败，请确认定位和通知权限。", true);
        }
    }

    private void stopPhoneLocationService() {
        Intent stop = new Intent(this, PhoneLocationService.class).setAction(PhoneLocationService.ACTION_STOP);
        startService(stop);
        if (phoneLocationBound) {
            unbindService(phoneLocationConnection);
            phoneLocationBound = false;
        }
        phoneLocationService = null;
        setMessage("持续定位已停止。", false);
    }

    private void startLocationRequest() {
        if (locationBusy) {
            return;
        }
        Location cached = freshestCachedLocation();
        if (cached != null && System.currentTimeMillis() - cached.getTime() <= MAX_CACHED_LOCATION_AGE_MS
                && (!cached.hasAccuracy() || cached.getAccuracy() <= 100f)) {
            showLocation(cached, "使用手机最近位置");
            return;
        }
        locationBusy = true;
        bestLocation = cached;
        locationStatus.setText("正在通过 Android 定位服务获取…");
        locationTimeout = () -> finishLocationRequest();
        main.postDelayed(locationTimeout, LOCATION_TIMEOUT_MS);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                    == PackageManager.PERMISSION_GRANTED) {
                requestModernLocation("gps");
            }
            requestModernLocation("network");
        } else {
            requestLegacyLocation();
        }
    }

    @SuppressLint("MissingPermission")
    private Location freshestCachedLocation() {
        if (locationManager == null || !hasLocationPermission()) {
            return null;
        }
        Location best = null;
        for (String provider : new String[]{LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
                LocationManager.PASSIVE_PROVIDER}) {
            try {
                Location candidate = locationManager.getLastKnownLocation(provider);
                if (candidate == null || candidate.getTime() <= 0
                        || System.currentTimeMillis() - candidate.getTime() > 10 * 60_000L) {
                    continue;
                }
                if (best == null || isBetterLocation(candidate, best)) {
                    best = candidate;
                }
            } catch (Exception ignored) {
                // A disabled provider may not return a cached fix.
            }
        }
        return best;
    }

    @SuppressLint({"MissingPermission", "NewApi"})
    private void requestModernLocation(String provider) {
        if (locationManager == null || !hasLocationPermission()) {
            return;
        }
        try {
            if (!locationManager.isProviderEnabled(provider)) {
                return;
            }
            CancellationSignal signal = new CancellationSignal();
            if (LocationManager.GPS_PROVIDER.equals(provider)) {
                gpsCancellation = signal;
            } else {
                networkCancellation = signal;
            }
            locationManager.getCurrentLocation(provider, signal, getMainExecutor(), location -> {
                if (!locationBusy || location == null) {
                    return;
                }
                considerLocation(location);
                if (location.hasAccuracy() && location.getAccuracy() <= 40f) {
                    finishLocationRequest();
                } else if (locationSettle == null) {
                    locationSettle = this::finishLocationRequest;
                    main.postDelayed(locationSettle, 2_000L);
                }
            });
        } catch (SecurityException ignored) {
            // Another provider may still return an approximate location.
        } catch (Exception ignored) {
            // The other provider or cached position may still succeed.
        }
    }

    @SuppressLint("MissingPermission")
    private void requestLegacyLocation() {
        if (locationManager == null || !hasLocationPermission()) {
            finishLocationRequest();
            return;
        }
        legacyLocationListener = new LocationListener() {
            @Override
            public void onLocationChanged(Location location) {
                considerLocation(location);
                if (!location.hasAccuracy() || location.getAccuracy() <= 60f) {
                    finishLocationRequest();
                }
            }
        };
        for (String provider : new String[]{LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER}) {
            try {
                if (locationManager.isProviderEnabled(provider)) {
                    locationManager.requestLocationUpdates(provider, 0L, 0f,
                            legacyLocationListener, Looper.getMainLooper());
                }
            } catch (Exception ignored) {
                // Continue with the other provider.
            }
        }
    }

    private boolean hasLocationPermission() {
        return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
                || checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    private void considerLocation(Location location) {
        if (location == null) {
            return;
        }
        if (bestLocation == null || isBetterLocation(location, bestLocation)) {
            bestLocation = new Location(location);
            showLocation(bestLocation, "已获取手机位置");
        }
    }

    private boolean isBetterLocation(Location candidate, Location current) {
        if (candidate.getTime() > current.getTime() + 15_000L) {
            return true;
        }
        if (candidate.getTime() + 30_000L < current.getTime()) {
            return false;
        }
        float candidateAccuracy = candidate.hasAccuracy() ? candidate.getAccuracy() : Float.MAX_VALUE;
        float currentAccuracy = current.hasAccuracy() ? current.getAccuracy() : Float.MAX_VALUE;
        return candidateAccuracy < currentAccuracy;
    }

    private void finishLocationRequest() {
        if (!locationBusy) {
            return;
        }
        locationBusy = false;
        stopLocationRequest();
        if (bestLocation != null) {
            currentLocation = new Location(bestLocation);
            showLocation(currentLocation, "已获取手机位置");
            refreshControls();
        } else {
            locationStatus.setText("没有获取到位置");
            setMessage("未获取到手机位置。请确认系统定位已开启并允许精确位置。", true);
            if (phoneContinuous || phonePush) updateLocationService();
        }
    }

    private void stopLocationRequest() {
        if (locationTimeout != null) {
            main.removeCallbacks(locationTimeout);
            locationTimeout = null;
        }
        if (locationSettle != null) {
            main.removeCallbacks(locationSettle);
            locationSettle = null;
        }
        if (gpsCancellation != null) {
            gpsCancellation.cancel();
            gpsCancellation = null;
        }
        if (networkCancellation != null) {
            networkCancellation.cancel();
            networkCancellation = null;
        }
        if (legacyLocationListener != null && locationManager != null) {
            try {
                locationManager.removeUpdates(legacyLocationListener);
            } catch (Exception ignored) {
                // The listener may already have been removed.
            }
            legacyLocationListener = null;
        }
    }

    private void showLocation(Location location, String status) {
        if (location == null) {
            return;
        }
        locationStatus.setText(status);
        coordinates.setText(String.format(java.util.Locale.US, "%.7f, %.7f",
                location.getLatitude(), location.getLongitude()));
        accuracy.setText("精度 " + (location.hasAccuracy() ? Math.round(location.getAccuracy()) + " m" : "未知"));
        currentLocation = new Location(location);
        updateLocalPhoneStatus();
        refreshControls();
        if (!startupLocationRequested) startupLocationRequested = true;
        if ((phoneContinuous || phonePush) && !phoneLocationStarting) updateLocationService();
    }

    private void readConfig() {
        readConfigOverBle();
    }

    private void readConfigOverBle() {
        if (!bleReady || !bleConfigAvailable || configEditable) return;
        bleManager.readConfig(new BleManager.ConfigCallback() {
            @Override public void onSuccess(Map<String, String> values) {
                applyDeviceConfig(values);
                setMessage("已通过 BLE 同步设备配置。", false);
            }
            @Override public void onError(String error) {
                if (configEditable) return;
                setMessage("BLE 配置读取失败：" + error, true);
            }
        });
    }

    private void applyTelemetry(JSONObject json) {
        String active = json.optString("activeSource", json.optString("active_source", "none"));
        String activeLabel = "gnss".equals(active) ? "GNSS 定位" : "phone".equals(active)
                ? "手机定位" : "manual".equals(active) ? "手动设置" : "无有效来源";
        activeSourceBadge.setText("当前使用：" + activeLabel);
        deviceStatus.setText("设备状态：" + json.optString("status", "BLE 实时遥测"));
        JSONObject gnss = json.optJSONObject("gnss");
        JSONObject phone = json.optJSONObject("phone");
        JSONObject manual = json.optJSONObject("manual");
        gnssSourceStatus.setText("GNSS 定位  " + sourceCoordinates(gnss)
                + "\n搜星 " + numberField(gnss, "satellitesVisible", "satellites_visible")
                + " · 使用 " + numberField(gnss, "satellitesUsed", "satellites_used")
                + " · " + stringField(gnss, "searchState", "search_state", "—"));
        phoneSourceStatus.setText("手机定位  " + sourceCoordinates(phone)
                + "\n精度 " + (phone != null && phone.has("accuracy") ? phone.optString("accuracy") + " m" : "—")
                + " · 推送 " + (phonePush ? "已开启" : "已关闭")
                + "\n获取时间 " + formatTimestamp(phone, "updatedAt", "updated_at")
                + " · 剩余有效期 " + formatRemaining(phone, "remainingSeconds", "remaining_seconds"));
        manualSourceStatus.setText("手动设置  " + sourceCoordinates(manual)
                + "\n设置时间 " + formatTimestamp(manual, "updatedAt", "updated_at")
                + " · 剩余有效期 " + formatRemaining(manual, "remainingSeconds", "remaining_seconds"));
        tintSource(gnssSourceStatus, "gnss".equals(active));
        tintSource(phoneSourceStatus, "phone".equals(active));
        tintSource(manualSourceStatus, "manual".equals(active));
        if (phone != null && phone.optBoolean("valid", false)) {
            currentLocation = new Location("device");
            currentLocation.setLatitude(phone.optDouble("latitude"));
            currentLocation.setLongitude(phone.optDouble("longitude"));
            currentLocation.setTime(System.currentTimeMillis());
            if (phone.has("accuracy")) currentLocation.setAccuracy((float) phone.optDouble("accuracy"));
        }
        JSONObject storage = json.optJSONObject("storage");
        if (storage != null) {
            if (storage.optBoolean("available", false)) {
                storageStatus.setText("轨迹 " + storage.optLong("record_count", 0)
                        + " 条 · 已用 " + formatBytes(storage.optLong("used_bytes", 0))
                        + " / " + formatBytes(storage.optLong("capacity_bytes", 0)));
            } else storageStatus.setText("轨迹存储不可用");
        }
    }

    private String sourceCoordinates(JSONObject source) {
        if (source == null || !source.optBoolean("valid", false)) return "无有效定位";
        return String.format(java.util.Locale.US, "%.6f, %.6f",
                source.optDouble("latitude"), source.optDouble("longitude"));
    }

    private String numberField(JSONObject json, String camel, String snake) {
        if (json == null) return "—";
        return json.has(camel) ? Integer.toString(json.optInt(camel))
                : json.has(snake) ? Integer.toString(json.optInt(snake)) : "—";
    }

    private String stringField(JSONObject json, String camel, String snake, String fallback) {
        if (json == null) return fallback;
        return json.optString(camel, json.optString(snake, fallback));
    }

    private String formatTimestamp(JSONObject json, String camel, String snake) {
        if (json == null) return "—";
        Object raw = json.has(camel) ? json.opt(camel) : json.opt(snake);
        if (!(raw instanceof Number)) return raw == null || raw == JSONObject.NULL ? "—" : String.valueOf(raw);
        long timestamp = ((Number) raw).longValue();
        if (timestamp <= 0) return "—";
        if (timestamp < 100_000_000_000L) timestamp *= 1000L;
        return new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.getDefault())
                .format(new java.util.Date(timestamp));
    }

    private String formatRemaining(JSONObject json, String camel, String snake) {
        if (json == null) return "—";
        long seconds = json.has(camel) ? json.optLong(camel, -1) : json.optLong(snake, -1);
        if (seconds < 0) return "—";
        if (seconds == 0) return "已过期";
        if (seconds < 60) return seconds + " 秒";
        if (seconds < 3600) return ((seconds + 59) / 60) + " 分钟";
        return (seconds / 3600) + " 小时 " + ((seconds / 60) % 60) + " 分钟";
    }

    private void tintSource(TextView view, boolean active) {
        view.setTextColor(active ? 0xFF28704A : 0xFF53625A);
        view.setTypeface(Typeface.DEFAULT, active ? Typeface.BOLD : Typeface.NORMAL);
    }

    private void applyDeviceConfig(Map<String, String> values) {
        if (configEditable) return;
        updatingConfigControls = true;
        setIfPresent(cameraName, values, "cam_name");
        setIfPresent(cameraMac, values, "cam_mac");
        setIfPresent(stationSsid, values, "wifi_ssid");
        selectValue(timezone, TIMEZONE_VALUES, values.get("tz"));
        selectValue(dstOffset, DST_VALUES, values.get("dst"));
        sourceGnss = configBoolean(values, "source_gnss", true);
        sourcePhone = configBoolean(values, "source_phone", true);
        sourceManual = configBoolean(values, "source_manual", true);
        trackingEnabled = configBoolean(values, "track_enabled", true);
        phonePush = configBoolean(values, "phone_push_enabled", true);
        manualLocationEnabled = configBoolean(values, "manual_auto_update", true);
        phoneContinuous = manualLocationEnabled;
        gnssIdle.setChecked(configBoolean(values, "gnss_idle", false));
        trackIdle.setChecked(configBoolean(values, "track_idle", false));
        trackEnabled.setChecked(trackingEnabled);
        phonePushEnabled.setChecked(phonePush);
        manualEnabled.setChecked(manualLocationEnabled);
        setIfPresent(manualLatitude, values, "manual_lat");
        setIfPresent(manualLongitude, values, "manual_lon");
        String order = values.get("source_order");
        if (order != null) {
            locationOrder.clear();
            for (String part : order.split(",")) {
                String id = part.trim().toLowerCase(java.util.Locale.ROOT);
                if (("gnss".equals(id) || "phone".equals(id) || "manual".equals(id)) && !locationOrder.contains(id)) locationOrder.add(id);
            }
            for (String id : new String[]{"gnss", "phone", "manual"}) if (!locationOrder.contains(id)) locationOrder.add(id);
            rebuildSourceRows();
        }
        setIfPresent(trackInterval, values, "track_int_s");
        setIfPresent(phoneInterval, values, "phone_interval_s");
        phoneLocationIntervalSeconds = parseInt(phoneInterval, 5);
        selectSpinner(bluetoothMode, values.get("ble_mode"));
        setIfPresent(bluetoothBoot, values, "ble_boot");
        setIfPresent(bluetoothPeriod, values, "ble_period");
        setIfPresent(bluetoothWindow, values, "ble_window");
        selectSpinner(wifiIdle, "900".equals(values.get("wifi_idle_s")) ? "1" : "0");
        for (String id : sourceChecks.keySet()) {
            sourceChecks.get(id).setChecked("gnss".equals(id) ? sourceGnss : "phone".equals(id) ? sourcePhone : sourceManual);
        }
        stationPassword.setText("");
        stationPassword.setHint("留空保持设备当前密码");
        updatingConfigControls = false;
        updateLocationService();
    }

    private static boolean configBoolean(Map<String, String> values, String key, boolean fallback) {
        String value = values.get(key);
        if (value == null) return fallback;
        return "1".equals(value) || "true".equalsIgnoreCase(value) || "on".equalsIgnoreCase(value);
    }

    private void setIfPresent(EditText input, Map<String, String> values, String key) {
        String value = values.get(key);
        if (value != null) {
            input.setText(value);
        }
    }

    private void saveConfig() {
        Map<String, String> values = new LinkedHashMap<>();
        try {
            put(values, "cam_name", value(cameraName));
            put(values, "cam_mac", value(cameraMac));
            put(values, "tz", Integer.toString(TIMEZONE_VALUES[timezone.getSelectedItemPosition()]));
            put(values, "dst", Integer.toString(DST_VALUES[dstOffset.getSelectedItemPosition()]));
            put(values, "wifi_ssid", value(stationSsid));
            putIfNotEmpty(values, "wifi_pass", stationPassword);
            put(values, "source_order", join(locationOrder));
            put(values, "source_gnss", sourceGnss ? "1" : "0");
            put(values, "source_phone", sourcePhone ? "1" : "0");
            put(values, "source_manual", sourceManual ? "1" : "0");
            put(values, "track_enabled", trackEnabled.isChecked() ? "1" : "0");
            put(values, "manual_auto_update", manualEnabled.isChecked() ? "1" : "0");
            put(values, "phone_push_enabled", phonePushEnabled.isChecked() ? "1" : "0");
            put(values, "phone_interval_s", range(phoneInterval, "手机定位获取频率", 1, 3600));
            put(values, "gnss_idle", gnssIdle.isChecked() ? "1" : "0");
            put(values, "track_idle", trackIdle.isChecked() ? "1" : "0");
            put(values, "track_int_s", range(trackInterval, "轨迹记录间隔", 1, 3600));
            put(values, "ble_mode", Integer.toString(bluetoothMode.getSelectedItemPosition()));
            put(values, "ble_boot", range(bluetoothBoot, "快速扫描秒数", 0, 300));
            put(values, "ble_period", decimalRange(bluetoothPeriod, "蓝牙扫描间隔", 1.0, 10.24));
            put(values, "ble_window", range(bluetoothWindow, "蓝牙扫描时长", 20, 1000));
            put(values, "wifi_idle_s", wifiIdle.getSelectedItemPosition() == 1 ? "900" : "300");
        } catch (IllegalArgumentException error) {
            setMessage(error.getMessage(), true);
            return;
        }
        if (!deviceHotspotVerified) {
            afterHotspotConnected = this::saveConfig;
            setMessage("连接 FrameNav 设备热点后会继续保存配置。", false);
            wakeDeviceWifi();
            return;
        }
        setMessage("正在通过固定 IP 保存设备配置…", false);
        if (saveConnectionButton != null) saveConnectionButton.setEnabled(false);
        if (saveLocationButton != null) saveLocationButton.setEnabled(false);
        wifiClient.saveConfig(DEVICE_ADDRESS, values, new WifiClient.Callback() {
            @Override
            public void onSuccess(String response) {
                if (saveConnectionButton != null) saveConnectionButton.setEnabled(true);
                if (saveLocationButton != null) saveLocationButton.setEnabled(true);
                setMessage("设备配置已保存。", false);
                configEditable = false;
                setConfigEditable(false);
                readConfigOverBle();
            }

            @Override
            public void onError(String error, Throwable cause) {
                if (saveConnectionButton != null) saveConnectionButton.setEnabled(true);
                if (saveLocationButton != null) saveLocationButton.setEnabled(true);
                setMessage(error + "。请连接 FrameNav 设备热点后重试。", true);
            }
        });
    }

    private static void put(Map<String, String> values, String key, String value) {
        values.put(key, value == null ? "" : value);
    }

    private static String join(List<String> values) {
        StringBuilder result = new StringBuilder();
        for (String value : values) {
            if (result.length() > 0) result.append(',');
            result.append(value);
        }
        return result.toString();
    }

    private void selectSpinner(Spinner spinner, String value) {
        if (value == null) return;
        try { spinner.setSelection(Integer.parseInt(value)); }
        catch (NumberFormatException ignored) { }
    }

    private void selectValue(Spinner spinner, int[] choices, String value) {
        if (value == null) return;
        try {
            int parsed = Integer.parseInt(value);
            for (int i = 0; i < choices.length; i++) if (choices[i] == parsed) spinner.setSelection(i);
        } catch (NumberFormatException ignored) { }
    }

    private void refreshStorageStatus() {
        refreshDeviceStatus();
    }

    private static String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format(java.util.Locale.US, "%.1f KB", bytes / 1024.0);
        return String.format(java.util.Locale.US, "%.1f MB", bytes / (1024.0 * 1024.0));
    }

    private void saveManualLocation() {
        double latitude, longitude;
        try {
            latitude = Double.parseDouble(value(manualLatitude));
            longitude = Double.parseDouble(value(manualLongitude));
            if (!Double.isFinite(latitude) || !Double.isFinite(longitude)
                    || latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
                throw new NumberFormatException();
            }
        } catch (NumberFormatException error) {
            setMessage("请输入有效的纬度和经度。", true);
            return;
        }
        if (!deviceHotspotVerified) {
            afterHotspotConnected = this::saveManualLocation;
            wakeDeviceWifi();
            return;
        }
        long now = System.currentTimeMillis();
        wifiClient.postManualLocation(DEVICE_ADDRESS, latitude, longitude, now, now,
                new WifiClient.Callback() {
                    @Override public void onSuccess(String response) {
                        setMessage(response.trim().isEmpty() ? "固定位置已保存。" : response.trim(), false);
                        refreshDeviceStatus();
                    }
                    @Override public void onError(String error, Throwable cause) {
                        setMessage(error + "。请连接 FrameNav 设备热点后重试。", true);
                        openWifiSettings();
                    }
                });
    }

    private void clearManualLocation() {
        if (!deviceHotspotVerified) {
            afterHotspotConnected = this::clearManualLocation;
            wakeDeviceWifi();
            return;
        }
        wifiClient.clearManualLocation(DEVICE_ADDRESS, new WifiClient.Callback() {
            @Override public void onSuccess(String response) {
                setMessage(response.trim().isEmpty() ? "手动兜底位置已清除。" : response.trim(), false);
                refreshDeviceStatus();
            }
            @Override public void onError(String error, Throwable cause) {
                setMessage(error + "。请连接 FrameNav 设备热点。", true);
            }
        });
    }

    private void fillManualFromPhone() {
        if (currentLocation == null) {
            setMessage("正在等待手机定位…", false);
            requestLocationPermissionsThenLocate();
            return;
        }
        manualLatitude.setText(String.format(java.util.Locale.US, "%.7f", currentLocation.getLatitude()));
        manualLongitude.setText(String.format(java.util.Locale.US, "%.7f", currentLocation.getLongitude()));
        setMessage("已填入当前手机位置，请保存手动位置。", false);
    }

    private void updateLocalPhoneStatus() {
        if (phoneSourceStatus == null) return;
        if (currentLocation == null) {
            phoneSourceStatus.setText("手机定位  等待获取");
            return;
        }
        String coordinates = String.format(java.util.Locale.US, "%.6f, %.6f",
                currentLocation.getLatitude(), currentLocation.getLongitude());
        String accuracyText = currentLocation.hasAccuracy()
                ? Math.round(currentLocation.getAccuracy()) + " m" : "未知";
        String timeText = new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss",
                java.util.Locale.getDefault()).format(new java.util.Date(currentLocation.getTime()));
        phoneSourceStatus.setText("手机定位  " + coordinates + "\n精度 " + accuracyText
                + " · 推送 " + (phonePush ? "已开启" : "已关闭")
                + "\n获取时间 " + timeText + " · 设备有效期等待 BLE 遥测");
    }

    private String number(EditText field, String label, int min, int max) {
        String text = value(field);
        try {
            int parsed = Integer.parseInt(text);
            if (parsed < min || parsed > max) {
                throw new NumberFormatException();
            }
            return Integer.toString(parsed);
        } catch (NumberFormatException error) {
            throw new IllegalArgumentException(label + "需在 " + min + " 至 " + max + " 之间");
        }
    }

    private String decimalRange(EditText field, String label, double min, double max) {
        try {
            double parsed = Double.parseDouble(value(field));
            if (!Double.isFinite(parsed) || parsed < min || parsed > max) {
                throw new NumberFormatException();
            }
            return Double.toString(parsed);
        } catch (NumberFormatException error) {
            throw new IllegalArgumentException(label + "需在 " + min + " 至 " + max + " 秒之间");
        }
    }

    private String range(EditText field, String label, int min, int max) {
        int parsed = parseInt(field, Integer.MIN_VALUE);
        if (parsed < min || parsed > max) {
            throw new IllegalArgumentException(label + "需在 " + min + " 至 " + max + " 之间");
        }
        return Integer.toString(parsed);
    }

    private int parseInt(EditText field, int fallback) {
        try {
            return Integer.parseInt(value(field));
        } catch (NumberFormatException error) {
            return fallback;
        }
    }

    private String value(EditText field) {
        return field.getText().toString().trim();
    }

    private void putIfNotEmpty(Map<String, String> values, String key, EditText field) {
        String value = value(field);
        if (!value.isEmpty()) {
            values.put(key, value);
        }
    }

    private void refreshDeviceStatus() {
        if (bleReady) bleManager.readTelemetry(this::applyTelemetry);
    }

    private void refreshControls() {
        if (saveConnectionButton != null) saveConnectionButton.setEnabled(true);
        if (saveLocationButton != null) saveLocationButton.setEnabled(true);
        connectBleButton.setEnabled(!bleReady);
    }

    private void setMessage(String value, boolean error) {
        if (message != null) {
            message.setText(value);
            message.setTextColor(error ? 0xFFB42318 : 0xFF53625A);
        }
        if (error && !isFinishing()) {
            Toast.makeText(this, value, Toast.LENGTH_LONG).show();
        }
    }

    @Override
    public void onBleState(String state) {
        connectionStatus.setText(state);
        refreshControls();
    }

    @Override
    public void onBleReady(boolean locationAvailable, boolean configAvailable) {
        if (bleReconnect != null) {
            main.removeCallbacks(bleReconnect);
            bleReconnect = null;
        }
        bleReady = locationAvailable || configAvailable;
        bleLocationAvailable = locationAvailable;
        bleConfigAvailable = configAvailable;
        connectionStatus.setText("已连接" + (locationAvailable ? " · 定位" : "")
                + (configAvailable ? " · 配置" : ""));
        if (locationAvailable) setMessage("BLE 定位已连接。配置读写通过设备 IP 完成。", false);
        else setMessage("BLE 已连接，但定位写入服务不可用。", true);
        if (phoneLocationService != null) phoneLocationService.setBleConnected(locationAvailable);
        if ((phoneContinuous || phonePush) && phoneLocationService == null) startPhoneLocationService();
        if (bleConfigAvailable) readConfigOverBle();
        if (bleReady) {
            main.removeCallbacks(configPoll);
            bleManager.readTelemetry(this::applyTelemetry);
            main.postDelayed(configPoll, 30_000L);
            if (pendingHotspotWorkflow) wakeDeviceWifi();
        }
        refreshControls();
    }

    @Override
    public void onBleConfig(Map<String, String> values) {
        // Configuration results are delivered through the explicit callback.
    }

    @Override
    public void onBleError(String error) {
        connectionStatus.setText("BLE 未就绪");
        setMessage(error, true);
        refreshControls();
    }

    @Override
    public void onBleDisconnected() {
        main.removeCallbacks(configPoll);
        bleReady = false;
        bleLocationAvailable = false;
        bleConfigAvailable = false;
        connectionStatus.setText("未连接");
        if (phoneLocationService != null) phoneLocationService.setBleConnected(false);
        if (phoneLocationStarting) {
            bleReconnect = () -> {
                bleReconnect = null;
                if (phoneLocationStarting && !bleReady) requestBlePermissionsThenConnect();
            };
            main.postDelayed(bleReconnect, 5_000L);
        }
        refreshControls();
    }
}
