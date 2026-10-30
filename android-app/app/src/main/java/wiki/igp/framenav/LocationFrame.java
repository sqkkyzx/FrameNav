package wiki.igp.framenav;

import android.location.Location;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Encodes the 28-byte phone-location frame accepted by the FrameNav firmware. */
public final class LocationFrame {
    private LocationFrame() {
    }

    public static byte[] encode(Location location, long nowMs) {
        if (location == null) {
            throw new IllegalArgumentException("location is required");
        }
        double latitude = location.getLatitude();
        double longitude = location.getLongitude();
        if (!Double.isFinite(latitude) || !Double.isFinite(longitude)
                || latitude < -90.0 || latitude > 90.0
                || longitude < -180.0 || longitude > 180.0) {
            throw new IllegalArgumentException("invalid coordinates");
        }

        long capturedMs = location.getTime() > 0 ? location.getTime() : nowMs;
        float accuracy = location.hasAccuracy() && Float.isFinite(location.getAccuracy())
                ? Math.max(0.0f, location.getAccuracy()) : 0.0f;
        long accuracyCm = Math.min(2_000_000_000L, Math.round(accuracy * 100.0f));

        ByteBuffer buffer = ByteBuffer.allocate(28).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt((int) Math.round(latitude * 10_000_000.0));
        buffer.putInt((int) Math.round(longitude * 10_000_000.0));
        buffer.putInt((int) accuracyCm);
        buffer.putLong(capturedMs);
        buffer.putLong(nowMs);
        return buffer.array();
    }
}
