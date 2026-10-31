package wiki.igp.framenav;

import android.os.Handler;
import android.os.Looper;
import android.net.Network;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Small dependency-free HTTP client for the device's local REST endpoints. */
public final class WifiClient {
    public interface Callback {
        void onSuccess(String response);

        void onError(String message, Throwable error);
    }

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile Network network;

    public void setNetwork(Network network) {
        this.network = network;
    }

    public void close() {
        executor.shutdownNow();
    }

    public static String normalizeBaseUrl(String value) {
        String input = value == null ? "" : value.trim();
        if (input.isEmpty()) {
            input = "10.254.254.1";
        }
        if (input.startsWith("https://")) input = input.substring("https://".length());
        else if (input.startsWith("http://")) input = input.substring("http://".length());
        input = "http://" + input;
        while (input.endsWith("/")) {
            input = input.substring(0, input.length() - 1);
        }
        return input;
    }

    public void postLocation(String baseUrl, double latitude, double longitude,
                             double accuracy, long timestampMs, long nowMs,
                             Callback callback) {
        String json = "{\"latitude\":" + Double.toString(latitude)
                + ",\"longitude\":" + Double.toString(longitude)
                + ",\"accuracy\":" + Double.toString(Math.max(0.0, accuracy))
                + ",\"timestamp_ms\":" + timestampMs
                + ",\"now_ms\":" + nowMs + "}";
        post(baseUrl, "/location/clock", "{\"now_ms\":" + nowMs + "}",
                new Callback() {
                    @Override
                    public void onSuccess(String ignored) {
                        post(baseUrl, "/location/phone", json, callback);
                    }

                    @Override
                    public void onError(String message, Throwable error) {
                        callback.onError(message, error);
                    }
                });
    }

    public void postLiveLocation(String baseUrl, double latitude, double longitude,
                                 double accuracy, long timestampMs, long nowMs,
                                 Callback callback) {
        String json = "{\"latitude\":" + Double.toString(latitude)
                + ",\"longitude\":" + Double.toString(longitude)
                + ",\"accuracy\":" + Double.toString(Math.max(0.0, accuracy))
                + ",\"timestamp_ms\":" + timestampMs
                + ",\"now_ms\":" + nowMs + "}";
        post(baseUrl, "/location/live", json, callback);
    }

    public void postManualLocation(String baseUrl, double latitude, double longitude,
                                   long timestampMs, long nowMs, Callback callback) {
        String json = "{\"latitude\":" + Double.toString(latitude)
                + ",\"longitude\":" + Double.toString(longitude)
                + ",\"timestamp_ms\":" + timestampMs
                + ",\"now_ms\":" + nowMs + "}";
        post(baseUrl, "/location/manual", json, callback);
    }

    public void clearManualLocation(String baseUrl, Callback callback) {
        post(baseUrl, "/location/manual/clear", "{}", callback);
    }

    public void clearLocation(String baseUrl, Callback callback) {
        post(baseUrl, "/location/phone/clear", "{}", callback);
    }

    public void saveConfig(String baseUrl, Map<String, String> values, Callback callback) {
        Map<String, String> copy = new LinkedHashMap<>(values);
        try {
            run("POST", normalizeBaseUrl(baseUrl) + "/save", formEncode(copy),
                    "application/x-www-form-urlencoded", callback);
        } catch (IOException error) {
            deliverError(callback, "无法编码设备配置", error);
        }
    }

    public void getStatus(String baseUrl, Callback callback) {
        run("GET", normalizeBaseUrl(baseUrl) + "/location", null, null, callback);
    }

    public void getConfigurationPage(String baseUrl, Callback callback) {
        run("GET", normalizeBaseUrl(baseUrl) + "/config", null, null, callback);
    }

    public void getStorage(String baseUrl, Callback callback) {
        run("GET", normalizeBaseUrl(baseUrl) + "/storage", null, null, callback);
    }

    public void startWifiScan(String baseUrl, Callback callback) {
        run("POST", normalizeBaseUrl(baseUrl) + "/wifi/scan/start", "",
                "application/x-www-form-urlencoded", callback);
    }

    public void getWifiScan(String baseUrl, Callback callback) {
        run("GET", normalizeBaseUrl(baseUrl) + "/wifi/scan", null, null, callback);
    }

    public void startCameraPairing(String baseUrl, Callback callback) {
        run("POST", normalizeBaseUrl(baseUrl) + "/pair/start", "",
                "application/x-www-form-urlencoded", callback);
    }

    public void getPairedDevices(String baseUrl, Callback callback) {
        run("GET", normalizeBaseUrl(baseUrl) + "/bonds", null, null, callback);
    }

    public void deletePairedDevice(String baseUrl, String token, String type, Callback callback) {
        Map<String, String> values = new LinkedHashMap<>();
        values.put("bond", token);
        values.put("type", type);
        try {
            run("POST", normalizeBaseUrl(baseUrl) + "/bond/delete", formEncode(values),
                    "application/x-www-form-urlencoded", callback);
        } catch (IOException error) {
            deliverError(callback, "无法编码设备信息", error);
        }
    }

    private void post(String baseUrl, String path, String body, Callback callback) {
        run("POST", normalizeBaseUrl(baseUrl) + path, body,
                "application/json; charset=utf-8", callback);
    }

    private void run(String method, String url, String body, String contentType,
                     Callback callback) {
        executor.execute(() -> {
            HttpURLConnection connection = null;
            try {
                Network selectedNetwork = network;
                connection = (HttpURLConnection) (selectedNetwork == null
                        ? new URL(url).openConnection()
                        : selectedNetwork.openConnection(new URL(url)));
                connection.setRequestMethod(method);
                connection.setConnectTimeout(8_000);
                connection.setReadTimeout(8_000);
                connection.setUseCaches(false);
                connection.setDoInput(true);
                if (body != null) {
                    connection.setDoOutput(true);
                    connection.setRequestProperty("Content-Type", contentType);
                    byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                    connection.setFixedLengthStreamingMode(bytes.length);
                    try (OutputStream output = connection.getOutputStream()) {
                        output.write(bytes);
                    }
                }
                int code = connection.getResponseCode();
                InputStream stream = code >= 400 ? connection.getErrorStream()
                        : connection.getInputStream();
                String response = stream == null ? "" : readLimited(stream, 64 * 1024);
                boolean actionRedirect = "POST".equals(method)
                        && (url.endsWith("/save") || url.endsWith("/pair/start")
                        || url.endsWith("/bond/delete"))
                        && code == HttpURLConnection.HTTP_SEE_OTHER;
                if ((code < 200 || code >= 300) && !actionRedirect) {
                    throw new IOException("设备返回 HTTP " + code
                            + (response.isEmpty() ? "" : ": " + response));
                }
                deliverSuccess(callback, response);
            } catch (Throwable error) {
                deliverError(callback, error.getMessage() == null
                        ? "Wi-Fi 请求失败" : error.getMessage(), error);
            } finally {
                if (connection != null) {
                    connection.disconnect();
                }
            }
        });
    }

    private void deliverSuccess(Callback callback, String response) {
        main.post(() -> callback.onSuccess(response));
    }

    private void deliverError(Callback callback, String message, Throwable error) {
        main.post(() -> callback.onError(message, error));
    }

    private static String readLimited(InputStream input, int limit) throws IOException {
        StringBuilder result = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                input, StandardCharsets.UTF_8))) {
            char[] buffer = new char[1024];
            int read;
            while ((read = reader.read(buffer)) != -1) {
                if (result.length() + read > limit) {
                    throw new IOException("设备响应过大");
                }
                result.append(buffer, 0, read);
            }
        }
        return result.toString();
    }

    private static String formEncode(Map<String, String> values) throws IOException {
        StringBuilder result = new StringBuilder();
        for (Map.Entry<String, String> entry : values.entrySet()) {
            if (result.length() > 0) {
                result.append('&');
            }
            result.append(URLEncoder.encode(entry.getKey(), "UTF-8"));
            result.append('=');
            result.append(URLEncoder.encode(entry.getValue() == null ? "" : entry.getValue(), "UTF-8"));
        }
        return result.toString();
    }
}
