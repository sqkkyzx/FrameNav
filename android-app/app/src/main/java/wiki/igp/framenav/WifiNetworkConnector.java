package wiki.igp.framenav;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;

/** Requests a local-only Wi-Fi connection to the FrameNav access point. */
public final class WifiNetworkConnector {
    public interface Listener {
        void onConnected(Network network);

        void onError(String message);

        void onDisconnected();
    }

    private final Context context;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private ConnectivityManager connectivityManager;
    private ConnectivityManager.NetworkCallback callback;
    private Network network;
    private Runnable timeout;

    public WifiNetworkConnector(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
        connectivityManager = (ConnectivityManager) this.context.getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    @SuppressLint("MissingPermission")
    public void connect(String ssid, String passphrase) {
        String networkName = ssid == null ? "" : ssid.trim();
        if (networkName.isEmpty()) {
            listener.onError("请输入设备热点名称");
            return;
        }
        disconnect(false);
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            try {
                context.startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                listener.onError("请在系统 Wi-Fi 设置中连接设备热点，再返回应用");
            } catch (Exception error) {
                listener.onError("无法打开系统 Wi-Fi 设置");
            }
            return;
        }

        WifiNetworkSpecifier.Builder specifier = new WifiNetworkSpecifier.Builder()
                .setSsid(networkName);
        String password = passphrase == null ? "" : passphrase;
        if (!password.isEmpty()) {
            specifier.setWpa2Passphrase(password);
        }
        NetworkRequest request = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(specifier.build())
                .build();
        callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network available) {
                main.post(() -> {
                    network = available;
                    cancelTimeout();
                    listener.onConnected(available);
                });
            }

            @Override
            public void onUnavailable() {
                main.post(() -> {
                    cancelTimeout();
                    listener.onError("未能连接设备 Wi-Fi；请确认热点名称和密码");
                });
            }

            @Override
            public void onLost(Network lost) {
                main.post(() -> {
                    if (network != null && network.equals(lost)) {
                        network = null;
                        listener.onDisconnected();
                    }
                });
            }
        };
        try {
            connectivityManager.requestNetwork(request, callback, 30_000);
            timeout = () -> listener.onError("等待系统 Wi-Fi 连接授权超时");
            main.postDelayed(timeout, 32_000);
        } catch (SecurityException error) {
            listener.onError("系统拒绝 Wi-Fi 连接请求");
        } catch (Exception error) {
            listener.onError(error.getMessage() == null ? "无法请求 Wi-Fi 连接" : error.getMessage());
        }
    }

    public Network getNetwork() {
        return network;
    }

    public void disconnect() {
        disconnect(true);
    }

    public void close() {
        disconnect(false);
    }

    private void disconnect(boolean notify) {
        cancelTimeout();
        if (callback != null && connectivityManager != null) {
            try {
                connectivityManager.unregisterNetworkCallback(callback);
            } catch (IllegalArgumentException ignored) {
                // The platform may already have removed a failed request.
            }
            callback = null;
        }
        network = null;
        if (notify) {
            listener.onDisconnected();
        }
    }

    private void cancelTimeout() {
        if (timeout != null) {
            main.removeCallbacks(timeout);
            timeout = null;
        }
    }
}
