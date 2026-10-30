package wiki.igp.framenav;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

/** Keeps phone location updates active while periodically sending them over the app's BLE link. */
public final class PhoneLocationService extends Service {
    public static final String ACTION_START = "wiki.igp.framenav.action.START_PHONE_LOCATION";
    public static final String ACTION_STOP = "wiki.igp.framenav.action.STOP_PHONE_LOCATION";

    private static final String CHANNEL_ID = "phone_location";
    private static final int NOTIFICATION_ID = 4101;
    private static final long LOCATION_INTERVAL_MS = 5_000L;
    private static final long MIN_PUSH_INTERVAL_MS = 10_000L;
    private static final long BLE_WARNING_DELAY_MS = 30_000L;
    private static final long BLE_STOP_DELAY_MS = 120_000L;

    public interface LocationWriter {
        boolean isReady();

        void write(Location location, ResultCallback callback);
    }

    public interface ResultCallback {
        void onResult(boolean success, String message);
    }

    public interface Listener {
        void onLocation(Location location);

        void onServiceState(String message, boolean error);
    }

    public final class LocalBinder extends Binder {
        public PhoneLocationService service() {
            return PhoneLocationService.this;
        }
    }

    private final IBinder binder = new LocalBinder();
    private final Handler main = new Handler(Looper.getMainLooper());
    private LocationManager locationManager;
    private LocationWriter writer;
    private Listener listener;
    private Location latestLocation;
    private long lastPushMs;
    private long disconnectedSinceMs;
    private boolean writeInFlight;
    private boolean locationUpdatesStarted;
    private boolean warningShown;

    private final Runnable pushLocation = new Runnable() {
        @Override
        public void run() {
            if (!locationUpdatesStarted) return;
            pushLatestLocation();
            main.postDelayed(this, MIN_PUSH_INTERVAL_MS);
        }
    };

    private final Runnable connectionWatch = new Runnable() {
        @Override
        public void run() {
            if (!locationUpdatesStarted || isBleReady()) {
                disconnectedSinceMs = 0;
                warningShown = false;
                return;
            }
            long now = android.os.SystemClock.elapsedRealtime();
            if (disconnectedSinceMs == 0) disconnectedSinceMs = now;
            long disconnectedFor = now - disconnectedSinceMs;
            if (!warningShown && disconnectedFor >= BLE_WARNING_DELAY_MS) {
                warningShown = true;
                notifyState("BLE 已断开，正在等待重新连接", true);
            }
            if (disconnectedFor >= BLE_STOP_DELAY_MS) {
                notifyState("BLE 长时间未连接，持续定位已停止", true);
                stopSelf();
                return;
            }
            main.postDelayed(this, 5_000L);
        }
    };

    private final LocationListener locationListener = new LocationListener() {
        @Override
        public void onLocationChanged(Location location) {
            if (location == null) return;
            latestLocation = new Location(location);
            if (listener != null) listener.onLocation(new Location(location));
            updateNotification(isBleReady()
                    ? "正在获取位置并通过 BLE 推送"
                    : "正在获取位置，等待 BLE 连接");
        }

        @Override
        public void onProviderEnabled(String provider) {
            notifyState("手机定位已开启", false);
        }

        @Override
        public void onProviderDisabled(String provider) {
            notifyState("手机定位服务已关闭", true);
        }

        @Override
        public void onStatusChanged(String provider, int status, Bundle extras) {
            // Provider status is informational and deprecated on newer Android releases.
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        locationManager = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopSelf();
            return START_NOT_STICKY;
        }
        Notification notification = buildNotification("正在启动持续定位");
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
        startLocationUpdates();
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override
    public void onDestroy() {
        stopLocationUpdates();
        if (locationManager != null) {
            try {
                locationManager.removeUpdates(locationListener);
            } catch (SecurityException ignored) {
                // Location permission may have been revoked while the service was active.
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(true);
        }
        if (listener != null) listener.onServiceState("持续定位已停止", false);
        super.onDestroy();
    }

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    public void setLocationWriter(LocationWriter writer) {
        this.writer = writer;
        updateConnectionWatch();
        if (isBleReady()) {
            notifyState("BLE 已连接，持续位置推送已就绪", false);
        }
    }

    public void setBleConnected(boolean connected) {
        if (connected) {
            disconnectedSinceMs = 0;
            warningShown = false;
            main.removeCallbacks(connectionWatch);
            notifyState("BLE 已连接，持续位置推送已就绪", false);
        } else {
            if (disconnectedSinceMs == 0) {
                disconnectedSinceMs = android.os.SystemClock.elapsedRealtime();
            }
            updateConnectionWatch();
        }
    }

    public boolean isRunning() {
        return locationUpdatesStarted;
    }

    private boolean isBleReady() {
        return writer != null && writer.isReady();
    }

    private void startLocationUpdates() {
        if (locationUpdatesStarted) return;
        if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
            notifyState("缺少手机定位权限，持续定位未启动", true);
            stopSelf();
            return;
        }
        if (locationManager == null) {
            notifyState("系统定位服务不可用", true);
            stopSelf();
            return;
        }
        boolean registered = false;
        try {
            if (locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
                locationManager.requestLocationUpdates(LocationManager.GPS_PROVIDER,
                        LOCATION_INTERVAL_MS, 0, locationListener, Looper.getMainLooper());
                registered = true;
            }
        } catch (SecurityException | IllegalArgumentException ignored) {
            // Try the network provider below when GPS is unavailable.
        }
        try {
            if (locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                locationManager.requestLocationUpdates(LocationManager.NETWORK_PROVIDER,
                        LOCATION_INTERVAL_MS, 0, locationListener, Looper.getMainLooper());
                registered = true;
            }
        } catch (SecurityException | IllegalArgumentException ignored) {
            // The device may not provide a network location provider.
        }
        if (!registered) {
            notifyState("没有可用的手机定位来源，请检查系统定位设置", true);
            stopSelf();
            return;
        }
        locationUpdatesStarted = true;
        Location cached = newestLastKnownLocation();
        long cachedAge = cached == null ? Long.MAX_VALUE
                : System.currentTimeMillis() - cached.getTime();
        if (cached != null && cachedAge >= 0 && cachedAge <= 60_000L) {
            latestLocation = cached;
            if (listener != null) listener.onLocation(new Location(cached));
        }
        main.removeCallbacks(pushLocation);
        main.post(pushLocation);
        updateConnectionWatch();
        notifyState("持续定位已启动", false);
    }

    private Location newestLastKnownLocation() {
        Location newest = null;
        String[] providers = {LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER};
        for (String provider : providers) {
            try {
                Location candidate = locationManager.getLastKnownLocation(provider);
                if (candidate != null && (newest == null || candidate.getTime() > newest.getTime())) {
                    newest = new Location(candidate);
                }
            } catch (SecurityException | IllegalArgumentException ignored) {
                // Providers can disappear while location is being initialized.
            }
        }
        return newest;
    }

    private void pushLatestLocation() {
        if (latestLocation == null || writeInFlight) return;
        long now = System.currentTimeMillis();
        long age = now - latestLocation.getTime();
        if (age < 0 || age > 60_000L) return;
        if (now - lastPushMs < MIN_PUSH_INTERVAL_MS || !isBleReady()) return;
        writeInFlight = true;
        Location sample = new Location(latestLocation);
        try {
            writer.write(sample, (success, message) -> main.post(() -> {
                writeInFlight = false;
                if (success) {
                    lastPushMs = System.currentTimeMillis();
                    updateNotification("位置已通过 BLE 更新 · " + formatCoordinates(sample));
                    if (listener != null) listener.onServiceState("位置已通过 BLE 更新", false);
                } else {
                    updateNotification("BLE 写入失败，等待重试");
                    if (listener != null) listener.onServiceState(
                            message == null ? "BLE 位置写入失败" : message, true);
                }
            }));
        } catch (RuntimeException error) {
            writeInFlight = false;
            String message = error.getMessage() == null
                    ? "BLE 位置写入无法启动" : error.getMessage();
            updateNotification("BLE 写入失败，等待重试");
            if (listener != null) listener.onServiceState(message, true);
        }
    }

    private void updateConnectionWatch() {
        main.removeCallbacks(connectionWatch);
        if (!locationUpdatesStarted || isBleReady()) return;
        if (disconnectedSinceMs == 0) disconnectedSinceMs = android.os.SystemClock.elapsedRealtime();
        main.post(connectionWatch);
    }

    private void stopLocationUpdates() {
        locationUpdatesStarted = false;
        main.removeCallbacks(pushLocation);
        main.removeCallbacks(connectionWatch);
    }

    private void notifyState(String message, boolean error) {
        updateNotification(message);
        if (listener != null) listener.onServiceState(message, error);
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                "持续手机定位", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("持续获取手机位置并通过蓝牙发送到 FrameNav");
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.createNotificationChannel(channel);
    }

    private Notification buildNotification(String text) {
        Intent stop = new Intent(this, PhoneLocationService.class).setAction(ACTION_STOP);
        PendingIntent stopIntent = PendingIntent.getService(this, 0, stop,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return builder.setSmallIcon(android.R.drawable.ic_menu_mylocation)
                .setContentTitle("FrameNav 持续定位")
                .setContentText(text)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .addAction(new Notification.Action.Builder(
                        android.R.drawable.ic_media_pause, "停止", stopIntent).build())
                .build();
    }

    private void updateNotification(String text) {
        NotificationManager manager = (NotificationManager) getSystemService(
                Context.NOTIFICATION_SERVICE);
        if (manager != null && locationUpdatesStarted) {
            manager.notify(NOTIFICATION_ID, buildNotification(text));
        }
    }

    private static String formatCoordinates(Location location) {
        return String.format(java.util.Locale.US, "%.5f, %.5f",
                location.getLatitude(), location.getLongitude());
    }
}
