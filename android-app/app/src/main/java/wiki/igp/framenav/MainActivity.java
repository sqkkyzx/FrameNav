package wiki.igp.framenav;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.ComponentName;
import android.content.Intent;
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
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;
import org.json.JSONArray;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class MainActivity extends Activity implements BleManager.Listener {
    private static final int REQUEST_BLE_PERMISSIONS = 501;
    private static final int REQUEST_LOCATION_PERMISSIONS = 502;
    private static final int REQUEST_CONTINUOUS_PERMISSIONS = 504;
    private static final long MAX_CACHED_LOCATION_AGE_MS = 60_000L;
    private static final long LOCATION_TIMEOUT_MS = 20_000L;
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
    private boolean settingContinuousSwitch;
    private int selectedTab;

    private TextView connectionStatus;
    private TextView wifiStatus;
    private TextView pairedDevices;
    private LinearLayout pairedDeviceRows;
    private TextView deviceStatus;
    private TextView deviceLocation;
    private TextView storageStatus;
    private TextView locationStatus;
    private TextView coordinates;
    private TextView accuracy;
    private TextView message;
    private EditText deviceIp;
    private EditText cameraName;
    private EditText cameraMac;
    private EditText stationSsid;
    private EditText stationPassword;
    private Spinner timezone;
    private Spinner dstOffset;
    private EditText locationTtl;
    private EditText gpsAge;
    private Spinner fixedPriority;
    private Spinner fixedDelete;
    private Spinner gnssIdle;
    private Spinner trackIdle;
    private Spinner bluetoothMode;
    private EditText bluetoothBoot;
    private EditText bluetoothPeriod;
    private EditText bluetoothWindow;
    private Spinner wifiIdle;
    private EditText trackInterval;
    private EditText manualLatitude;
    private EditText manualLongitude;
    private Switch continuousLocation;
    private LinearLayout tabContent;
    private final List<View> tabPages = new ArrayList<>();
    private final List<Button> tabButtons = new ArrayList<>();
    private Button connectBleButton;
    private Button connectWifiButton;
    private Button readConfigButton;
    private Button saveConfigButton;
    private Button getLocationButton;
    private Button writeLocationButton;
    private Button clearLocationButton;
    private Runnable bleReconnect;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(0xFF14231D);
        getWindow().setNavigationBarColor(0xFF14231D);
        locationManager = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        wifiClient = new WifiClient();
        bleManager = new BleManager(this, this);
        buildUi();
        deviceIp.setText(getPreferences(MODE_PRIVATE).getString("device_ip", "10.254.254.1"));
        phoneLocationStarting = getPreferences(MODE_PRIVATE).getBoolean("continuous_location", false);
        settingContinuousSwitch = true;
        continuousLocation.setChecked(phoneLocationStarting);
        settingContinuousSwitch = false;
        if (phoneLocationStarting) startPhoneLocationService();
        checkCurrentWifi();
        refreshControls();
    }

    @Override
    protected void onDestroy() {
        stopLocationRequest();
        if (bleManager != null && !phoneLocationStarting) {
            bleManager.close();
        }
        if (wifiClient != null) {
            wifiClient.close();
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
        if (deviceIp != null) {
            refreshDeviceStatus();
            refreshStorageStatus();
            refreshPairedDevices();
        }
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFFF5F7F5);
        root.setPadding(dp(16), dp(12), dp(16), dp(6));
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
        LinearLayout connectionCard = card(statusPage, "设备状态");
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
        deviceStatus = text("设备定位：—", 14, 0xFF14231D, true);
        deviceStatus.setPadding(0, dp(8), 0, 0);
        connectionCard.addView(deviceStatus, matchWrap());
        deviceLocation = text("来源、坐标与时间：—", 13, 0xFF68756D, false);
        connectionCard.addView(deviceLocation, matchWrap());
        storageStatus = text("轨迹存储：—", 13, 0xFF68756D, false);
        storageStatus.setPadding(0, dp(4), 0, 0);
        connectionCard.addView(storageStatus, matchWrap());
        LinearLayout locationCard = card(statusPage, "手机定位");
        locationStatus = text("尚未获取", 14, 0xFF68756D, false);
        locationCard.addView(locationStatus, matchWrap());
        coordinates = text("—", 21, 0xFF14231D, true);
        coordinates.setPadding(0, dp(9), 0, dp(2));
        locationCard.addView(coordinates, matchWrap());
        accuracy = text("精度 —", 13, 0xFF68756D, false);
        accuracy.setPadding(0, 0, 0, dp(10));
        locationCard.addView(accuracy, matchWrap());
        LinearLayout locationButtons = row(locationCard);
        getLocationButton = button("获取手机位置", 0xFF28704A);
        getLocationButton.setOnClickListener(view -> requestLocationPermissionsThenLocate());
        locationButtons.addView(getLocationButton, weighted());
        writeLocationButton = button("写入设备", 0xFF14231D);
        writeLocationButton.setOnClickListener(view -> writeLocation());
        locationButtons.addView(writeLocationButton, weighted());
        clearLocationButton = button("清除固定位置", 0xFF53625A);
        clearLocationButton.setOnClickListener(view -> clearLocation());
        locationCard.addView(clearLocationButton, matchWrap());
        addPage(statusPage);

        LinearLayout connectionPage = newPage();
        LinearLayout ipCard = card(connectionPage, "设备 IP 连接");
        deviceIp = addField(ipCard, "设备地址", "10.254.254.1", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        connectWifiButton = button("打开系统 Wi-Fi 设置", 0xFF28704A);
        connectWifiButton.setOnClickListener(view -> openWifiSettings());
        ipCard.addView(connectWifiButton, matchWrap());
        TextView wifiHint = text("连接设备热点 FrameNav-MAC 后六位，再返回应用。热点密码固定为 framenav。设置读取和保存使用 HTTP IP 直连。", 13, 0xFF68756D, false);
        wifiHint.setPadding(0, dp(6), 0, 0);
        ipCard.addView(wifiHint, matchWrap());
        LinearLayout configActions = row(ipCard);
        readConfigButton = button("读取配置", 0xFF53625A);
        readConfigButton.setOnClickListener(view -> readConfig());
        configActions.addView(readConfigButton, weighted());
        saveConfigButton = button("保存配置", 0xFF28704A);
        saveConfigButton.setOnClickListener(view -> saveConfig());
        configActions.addView(saveConfigButton, weighted());
        Button pairCamera = button("打开相机配对窗口", 0xFF53625A);
        pairCamera.setOnClickListener(v -> startCameraPairing());
        ipCard.addView(pairCamera, matchWrap());
        LinearLayout bondsCard = card(connectionPage, "已配对设备");
        pairedDevices = text("尚未读取", 13, 0xFF68756D, false);
        bondsCard.addView(pairedDevices, matchWrap());
        pairedDeviceRows = new LinearLayout(this);
        pairedDeviceRows.setOrientation(LinearLayout.VERTICAL);
        bondsCard.addView(pairedDeviceRows, matchWrap());
        Button refreshBonds = button("刷新设备列表", 0xFF53625A);
        refreshBonds.setOnClickListener(v -> refreshPairedDevices());
        bondsCard.addView(refreshBonds, matchWrap());
        addPage(connectionPage);

        LinearLayout locationPage = newPage();
        LinearLayout phoneCard = card(locationPage, "手机定位");
        continuousLocation = new Switch(this);
        continuousLocation.setText("持续获取并通过 BLE 推送");
        continuousLocation.setTextColor(0xFF14231D);
        continuousLocation.setTextSize(14);
        phoneCard.addView(continuousLocation, matchWrap());
        continuousLocation.setOnCheckedChangeListener((button, checked) -> setContinuousLocation(checked));
        manualLatitude = addField(phoneCard, "固定位置纬度", "-90 至 90", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        manualLongitude = addField(phoneCard, "固定位置经度", "-180 至 180", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        Button saveManual = button("保存固定坐标到设备", 0xFF28704A);
        saveManual.setOnClickListener(v -> saveManualLocation());
        phoneCard.addView(saveManual, matchWrap());
        TextView locationNote = text("持续定位需要通知权限、手机定位权限和 BLE 连接。停止后通知栏服务会退出。", 12, 0xFF68756D, false);
        locationNote.setPadding(0, dp(7), 0, 0);
        phoneCard.addView(locationNote, matchWrap());
        addPage(locationPage);

        LinearLayout advancedPage = newPage();
        LinearLayout cameraCard = card(advancedPage, "相机与网络");
        cameraName = addField(cameraCard, "相机名称前缀", "留空自动选择", InputType.TYPE_CLASS_TEXT);
        cameraMac = addField(cameraCard, "相机地址前缀", "选填", InputType.TYPE_CLASS_TEXT);
        stationSsid = addField(cameraCard, "设备连接的 Wi-Fi 名称", "仅支持 2.4 GHz", InputType.TYPE_CLASS_TEXT);
        Button scanNetworks = button("扫描附近 Wi-Fi", 0xFF53625A);
        scanNetworks.setOnClickListener(v -> scanWifiNetworks());
        cameraCard.addView(scanNetworks, matchWrap());
        stationPassword = addField(cameraCard, "Wi-Fi 密码", "留空保持不变", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        wifiIdle = addChoice(cameraCard, "无人使用时关闭设备 Wi-Fi", new String[]{"5 分钟", "15 分钟"});
        LinearLayout locationConfigCard = card(advancedPage, "定位与轨迹");
        locationTtl = addField(locationConfigCard, "固定位置有效期（小时）", "1 至 720", InputType.TYPE_CLASS_NUMBER);
        fixedPriority = addChoice(locationConfigCard, "GPS 定位优先级", new String[]{"有 GPS 时优先使用 GPS", "固定位置过期前优先使用固定位置"});
        fixedDelete = addChoice(locationConfigCard, "GPS 稳定后的固定位置处理", new String[]{
                "稳定 1 分钟后删除", "稳定 5 分钟后删除", "偏离超过 100 米后删除",
                "稳定 1 分钟且偏离后删除", "稳定 5 分钟且偏离后删除", "过期前不删除"});
        gpsAge = addField(locationConfigCard, "卫星位置最大年龄（秒）", "0 至 86400", InputType.TYPE_CLASS_NUMBER);
        gnssIdle = addChoice(locationConfigCard, "相机和手机都未连接时搜星", new String[]{"关闭", "开启"});
        trackIdle = addChoice(locationConfigCard, "相机和手机都未连接时记录位置", new String[]{"关闭", "开启"});
        trackInterval = addField(locationConfigCard, "轨迹记录间隔（秒）", "1 至 3600", InputType.TYPE_CLASS_NUMBER);
        timezone = addChoice(locationConfigCard, "照片时区", new String[]{
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
        dstOffset = addChoice(locationConfigCard, "夏令时", new String[]{"关闭", "增加 30 分钟", "增加 1 小时", "增加 2 小时"});
        LinearLayout bleCard = card(advancedPage, "蓝牙省电");
        bluetoothMode = addChoice(bleCard, "设备连接模式", new String[]{"省电", "平衡", "自定义"});
        bluetoothBoot = addField(bleCard, "开机后查找设备（秒）", "0 至 300", InputType.TYPE_CLASS_NUMBER);
        bluetoothPeriod = addField(bleCard, "再次查找间隔（秒）", "1 至 10.24", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        bluetoothWindow = addField(bleCard, "每次查找时长（毫秒）", "20 至 1000", InputType.TYPE_CLASS_NUMBER);
        bluetoothMode.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                boolean custom = position == 2;
                bluetoothBoot.setEnabled(custom); bluetoothPeriod.setEnabled(custom); bluetoothWindow.setEnabled(custom);
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) { }
        });
        addPage(advancedPage);
        showTab(0);
        deviceIp.setText(getPreferences(MODE_PRIVATE).getString("device_ip", "10.254.254.1"));
        setContentView(root);
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
                settingContinuousSwitch = true;
                continuousLocation.setChecked(false);
                settingContinuousSwitch = false;
                setMessage("持续定位需要手机定位权限和通知权限。", true);
            }
        }
    }

    private void openWifiSettings() {
        startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
    }

    private void startCameraPairing() {
        rememberDeviceIp();
        wifiClient.startCameraPairing(deviceIp.getText().toString(), new WifiClient.Callback() {
            @Override public void onSuccess(String response) {
                setMessage("设备已进入相机配对窗口（90 秒）。", false);
            }
            @Override public void onError(String error, Throwable cause) {
                setMessage(error + "。请先连接设备热点。", true);
            }
        });
    }

    private void refreshPairedDevices() {
        wifiClient.getPairedDevices(deviceIp.getText().toString(), new WifiClient.Callback() {
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
                                        deviceIp.getText().toString(), token, type, new WifiClient.Callback() {
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
        rememberDeviceIp();
        setMessage("正在请求设备扫描 Wi-Fi…", false);
        wifiClient.startWifiScan(deviceIp.getText().toString(), new WifiClient.Callback() {
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
        main.postDelayed(() -> wifiClient.getWifiScan(deviceIp.getText().toString(), new WifiClient.Callback() {
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
                wifiStatus.setText("手机已连接 Wi-Fi · HTTP IP 可用");
                refreshControls();
            } else {
                wifiClient.setNetwork(null);
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
                    if (state.contains("已停止") || state.contains("未启动")) {
                        phoneLocationStarting = false;
                        getPreferences(MODE_PRIVATE).edit().putBoolean("continuous_location", false).apply();
                        settingContinuousSwitch = true;
                        continuousLocation.setChecked(false);
                        settingContinuousSwitch = false;
                    }
                }
            });
            phoneLocationService.setLocationWriter(new PhoneLocationService.LocationWriter() {
                @Override
                public boolean isReady() {
                    return bleReady && bleLocationAvailable;
                }

                @Override
                public void write(Location location, PhoneLocationService.ResultCallback callback) {
                    bleManager.writeLocation(location, new BleManager.Callback() {
                        @Override
                        public void onSuccess() { callback.onResult(true, ""); }
                        @Override
                        public void onError(String error) { callback.onResult(false, error); }
                    });
                }
            });
            phoneLocationService.setBleConnected(bleReady && bleLocationAvailable);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            phoneLocationBound = false;
            phoneLocationService = null;
        }
    };

    private void setContinuousLocation(boolean enabled) {
        if (settingContinuousSwitch) return;
        if (!enabled) {
            phoneLocationStarting = false;
            getPreferences(MODE_PRIVATE).edit().putBoolean("continuous_location", false).apply();
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
            getPreferences(MODE_PRIVATE).edit().putBoolean("continuous_location", true).apply();
            requestBlePermissionsThenConnect();
            setMessage("正在连接 BLE，连接成功后开始持续定位。", false);
            return;
        }
        startPhoneLocationService();
    }

    private void startPhoneLocationService() {
        if (!bleReady || !bleLocationAvailable) {
            phoneLocationStarting = true;
            if (bleReconnect == null) requestBlePermissionsThenConnect();
            return;
        }
        Intent intent = new Intent(this, PhoneLocationService.class).setAction(PhoneLocationService.ACTION_START);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(intent);
            else startService(intent);
            phoneLocationBound = bindService(intent, phoneLocationConnection, Context.BIND_AUTO_CREATE);
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
        getLocationButton.setEnabled(false);
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
        getLocationButton.setEnabled(true);
        if (bestLocation != null) {
            currentLocation = new Location(bestLocation);
            showLocation(currentLocation, "已获取手机位置");
            refreshControls();
        } else {
            locationStatus.setText("没有获取到位置");
            setMessage("未获取到手机位置。请确认系统定位已开启并允许精确位置。", true);
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
        refreshControls();
    }

    private void writeLocation() {
        if (currentLocation == null) {
            setMessage("请先获取手机位置。", true);
            return;
        }
        long age = System.currentTimeMillis() - currentLocation.getTime();
        if (age < 0 || age > 60_000L) {
            currentLocation = null;
            refreshControls();
            setMessage("位置数据已过期，请重新获取。", true);
            return;
        }
        if (bleReady && bleLocationAvailable) {
            setMessage("正在通过 BLE 写入 FrameNav…", false);
            bleManager.writeLocation(currentLocation, new BleManager.Callback() {
                @Override
                public void onSuccess() {
                    setMessage("手机位置已通过 BLE 写入设备。", false);
                    refreshDeviceStatus();
                }

                @Override
                public void onError(String error) {
                    setMessage(error, true);
                }
            });
            return;
        }
        writeLocationWifi();
    }

    private void writeLocationWifi() {
        long timestamp = currentLocation.getTime() > 0 ? currentLocation.getTime() : System.currentTimeMillis();
        double accuracyMeters = currentLocation.hasAccuracy() ? currentLocation.getAccuracy() : 0;
        setMessage("正在通过 Wi-Fi REST 写入位置…", false);
        wifiClient.postLocation(deviceIp.getText().toString(), currentLocation.getLatitude(),
                currentLocation.getLongitude(), accuracyMeters, timestamp,
                System.currentTimeMillis(), new WifiClient.Callback() {
                    @Override
                    public void onSuccess(String response) {
                        setMessage(response.trim().isEmpty() ? "手机位置已通过 Wi-Fi 写入设备。" : response.trim(), false);
                        refreshDeviceStatus();
                    }

                    @Override
                    public void onError(String error, Throwable cause) {
                        setMessage(error + "。确认已连接设备热点并检查设备 IP。", true);
                    }
                });
    }

    private void clearLocation() {
        rememberDeviceIp();
        wifiClient.clearLocation(deviceIp.getText().toString(), new WifiClient.Callback() {
                @Override
                public void onSuccess(String response) {
                    setMessage(response.trim().isEmpty() ? "设备中的手机位置已清除。" : response.trim(), false);
                    refreshDeviceStatus();
                }

                @Override
                public void onError(String error, Throwable cause) {
                    setMessage(error + "。确认已连接设备热点并检查设备 IP。", true);
                }
            });
    }

    private void readConfig() {
        readConfigButton.setEnabled(false);
        rememberDeviceIp();
        setMessage("正在通过设备 IP 读取配置…", false);
        wifiClient.getConfigurationPage(deviceIp.getText().toString(), new WifiClient.Callback() {
            @Override public void onSuccess(String jsonText) {
                try {
                    JSONObject json = new JSONObject(jsonText);
                    Map<String, String> values = new LinkedHashMap<>();
                    java.util.Iterator<String> keys = json.keys();
                    while (keys.hasNext()) {
                        String key = keys.next();
                        Object value = json.opt(key);
                        if (value != null && value != JSONObject.NULL) values.put(key, String.valueOf(value));
                    }
                    applyDeviceConfig(values);
                    refreshDeviceStatus();
                    refreshStorageStatus();
                    setMessage("设备配置已读取。", false);
                } catch (Exception error) {
                    setMessage("设备配置响应无法解析。", true);
                }
                readConfigButton.setEnabled(true);
            }
            @Override public void onError(String error, Throwable cause) {
                readConfigButton.setEnabled(true);
                setMessage(error + "。请连接设备热点并确认 IP 地址。", true);
            }
        });
    }

    private void applyDeviceConfig(Map<String, String> values) {
        setIfPresent(cameraName, values, "cam_name");
        setIfPresent(cameraMac, values, "cam_mac");
        setIfPresent(stationSsid, values, "wifi_ssid");
        selectValue(timezone, TIMEZONE_VALUES, values.get("tz"));
        selectValue(dstOffset, DST_VALUES, values.get("dst"));
        setIfPresent(locationTtl, values, "phone_ttl_h");
        setIfPresent(gpsAge, values, "max_gps_age");
        selectSpinner(fixedPriority, values.get("fixed_prio"));
        selectSpinner(fixedDelete, values.get("fixed_delete"));
        selectSpinner(gnssIdle, values.get("gnss_idle"));
        selectSpinner(trackIdle, values.get("track_idle"));
        setIfPresent(trackInterval, values, "track_int_s");
        selectSpinner(bluetoothMode, values.get("ble_mode"));
        setIfPresent(bluetoothBoot, values, "ble_boot");
        setIfPresent(bluetoothPeriod, values, "ble_period");
        setIfPresent(bluetoothWindow, values, "ble_window");
        selectSpinner(wifiIdle, "900".equals(values.get("wifi_idle_s")) ? "1" : "0");
        stationPassword.setText("");
        stationPassword.setHint("留空保持设备当前密码");
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
            put(values, "phone_ttl_h", range(locationTtl, "位置有效期", 1, 720));
            put(values, "max_gps_age", range(gpsAge, "卫星位置最大年龄", 0, 86400));
            put(values, "fixed_prio", Integer.toString(fixedPriority.getSelectedItemPosition()));
            put(values, "fixed_delete", Integer.toString(fixedDelete.getSelectedItemPosition()));
            put(values, "gnss_idle", Integer.toString(gnssIdle.getSelectedItemPosition()));
            put(values, "track_idle", Integer.toString(trackIdle.getSelectedItemPosition()));
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
        rememberDeviceIp();
        setMessage("正在通过 HTTP IP 保存设备配置…", false);
        saveConfigButton.setEnabled(false);
        wifiClient.saveConfig(deviceIp.getText().toString(), values, new WifiClient.Callback() {
            @Override
            public void onSuccess(String response) {
                saveConfigButton.setEnabled(true);
                setMessage("设备配置已保存。", false);
                refreshDeviceStatus();
                refreshStorageStatus();
            }

            @Override
            public void onError(String error, Throwable cause) {
                saveConfigButton.setEnabled(true);
                setMessage(error + "。请连接设备热点并确认 IP 地址。", true);
            }
        });
    }

    private static void put(Map<String, String> values, String key, String value) {
        values.put(key, value == null ? "" : value);
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

    private void rememberDeviceIp() {
        getPreferences(MODE_PRIVATE).edit().putString("device_ip", value(deviceIp)).apply();
    }

    private void refreshStorageStatus() {
        wifiClient.getStorage(deviceIp.getText().toString(), new WifiClient.Callback() {
            @Override public void onSuccess(String response) {
                try {
                    JSONObject json = new JSONObject(response);
                    if (!json.optBoolean("available", false)) {
                        storageStatus.setText("轨迹存储：不可用");
                        return;
                    }
                    storageStatus.setText("轨迹 " + json.optLong("record_count", 0)
                            + " 条 · 已用 " + formatBytes(json.optLong("used_bytes", 0))
                            + " / " + formatBytes(json.optLong("capacity_bytes", 0)));
                } catch (Exception ignored) { storageStatus.setText("轨迹存储：读取失败"); }
            }
            @Override public void onError(String error, Throwable cause) {
                storageStatus.setText("轨迹存储：读取失败");
            }
        });
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
        long now = System.currentTimeMillis();
        wifiClient.postLocation(deviceIp.getText().toString(), latitude, longitude, 0, now, now,
                new WifiClient.Callback() {
                    @Override public void onSuccess(String response) {
                        setMessage(response.trim().isEmpty() ? "固定位置已保存。" : response.trim(), false);
                        refreshDeviceStatus();
                    }
                    @Override public void onError(String error, Throwable cause) {
                        setMessage(error + "。固定坐标需要通过设备 IP 保存。", true);
                    }
                });
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
        wifiClient.getStatus(deviceIp.getText().toString(), new WifiClient.Callback() {
            @Override
            public void onSuccess(String response) {
                try {
                    JSONObject json = new JSONObject(response);
                    String status = json.optString("status", "未知");
                    String phoneStatus = json.optString("phoneStatus", "");
                    String source = json.optString("source", "—");
                    String position = json.optString("coordinates", "—");
                    String time = json.optString("time", "—");
                    deviceStatus.setText("设备定位：" + status + (phoneStatus.isEmpty() ? "" : " · 固定位置 " + phoneStatus));
                    deviceLocation.setText("来源：" + source + "\n坐标：" + position + "\n时间：" + time
                            + "\n搜到卫星：" + json.optInt("satellitesVisible", 0)
                            + " · 使用：" + json.optInt("satellites", 0)
                            + " · GNSS：" + json.optString("gnss", "—"));
                    if ("—".equals(position)) locationStatus.setText("设备位置：" + status);
                } catch (Exception ignored) {
                    // A successful write already confirms the primary action.
                }
            }

            @Override
            public void onError(String error, Throwable cause) {
                // Status polling is supplementary; don't overwrite a successful write.
            }
        });
    }

    private void refreshControls() {
        boolean canWrite = currentLocation != null;
        writeLocationButton.setEnabled(canWrite);
        clearLocationButton.setEnabled(true);
        readConfigButton.setEnabled(true);
        saveConfigButton.setEnabled(true);
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
        if (phoneLocationStarting && phoneLocationService == null) startPhoneLocationService();
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
