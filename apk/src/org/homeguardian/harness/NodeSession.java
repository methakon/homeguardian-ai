package org.homeguardian.harness;

import android.content.Context;

import java.io.File;

/**
 * H-03: the missing link. Native acquisition produces COMPACT numeric observations; the D-03
 * transport knows how to carry them; this class moves one into the other and nothing else.
 *
 * It parses the native drain (one JSON object per line, numeric scalars only), builds a
 * PendingObservation per line, and enqueues it. The transport owns queueing, retry
 * classification, TLS and acknowledgment; this class owns none of that.
 *
 * Credentials: the paths below are the ONLY place the app looks for its mutual-TLS material.
 * They are app-private files (filesDir), never public storage, never logged, and never sent
 * anywhere. Provisioning must place them there - see docs/ANDROID_WIFI_HARDWARE_ACCEPTANCE_H03.md.
 */
public final class NodeSession {

    public static final String DEVICE_P12 = "device.p12";
    public static final String DEVICE_P12_PASS = "device.p12.pass";
    public static final String CA_P12 = "ca.p12";
    public static final String CA_P12_PASS = "ca.p12.pass";

    private final ObservationTransport transport;
    private String deviceId = "";
    private long queuedTotal = 0;
    private long parseFailures = 0;
    private long lastSentSeen = 0;
    private long lastSentAtMs = 0;

    public NodeSession(ObservationTransport transport) {
        this.transport = transport;
    }

    public void setDeviceId(String deviceId) {
        this.deviceId = deviceId == null ? "" : deviceId;
    }

    public ObservationTransport transport() {
        return transport;
    }

    public long queuedTotal() {
        return queuedTotal;
    }

    public long parseFailures() {
        return parseFailures;
    }

    public long lastSentAtMs() {
        return lastSentAtMs;
    }

    /** Feed the native drain. Returns how many observations were handed to the transport. */
    public int pump(String ndjson) {
        if (ndjson == null || ndjson.length() == 0) {
            return 0;
        }
        int queued = 0;
        int start = 0;
        while (start < ndjson.length()) {
            int end = ndjson.indexOf('\n', start);
            if (end < 0) {
                end = ndjson.length();
            }
            final String line = ndjson.substring(start, end).trim();
            start = end + 1;
            if (line.length() == 0) {
                continue;
            }
            final String sourceType = stringField(line, "source_type");
            final String observationType = stringField(line, "observation_type");
            final String unit = stringField(line, "unit");
            final String state = stringField(line, "state");
            final String provenance = stringField(line, "provenance");
            final double value = numberField(line, "value");
            final double confidence = numberField(line, "confidence");
            final long acquiredAtMs = (long) numberField(line, "acquired_at_ms");
            if (sourceType == null || observationType == null || Double.isNaN(value)) {
                parseFailures++;
                continue;
            }
            final long sequence = transport.nextSequence();
            final String observationId = (deviceId.length() > 0 ? deviceId : "node") + "-" + sequence;
            transport.enqueue(new ObservationTransport.PendingObservation(
                    observationId, sourceType, observationType,
                    "live", acquiredAtMs > 0 ? acquiredAtMs : System.currentTimeMillis(), sequence,
                    value, unit == null ? "" : unit, state == null ? "" : state,
                    Double.isNaN(confidence) ? 0.0 : confidence,
                    provenance == null ? "real_device" : provenance));
            queued++;
        }
        queuedTotal += queued;
        if (transport.sentCount() > lastSentSeen) {
            lastSentSeen = transport.sentCount();
            lastSentAtMs = System.currentTimeMillis();
        }
        return queued;
    }

    /** Extract `"key":"value"` without a JSON dependency (the shape is produced by our own JNI). */
    static String stringField(String line, String key) {
        final String needle = "\"" + key + "\":\"";
        final int at = line.indexOf(needle);
        if (at < 0) {
            return null;
        }
        final int from = at + needle.length();
        final int to = line.indexOf('"', from);
        return to < 0 ? null : line.substring(from, to);
    }

    /** Extract `"key":number`. Returns NaN when absent or unparseable (never a silent zero). */
    static double numberField(String line, String key) {
        final String needle = "\"" + key + "\":";
        final int at = line.indexOf(needle);
        if (at < 0) {
            return Double.NaN;
        }
        int from = at + needle.length();
        int to = from;
        while (to < line.length()) {
            final char c = line.charAt(to);
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                to++;
            } else {
                break;
            }
        }
        try {
            return Double.parseDouble(line.substring(from, to));
        } catch (RuntimeException e) {
            return Double.NaN;
        }
    }

    /**
     * Build the transport configuration from app-private files. Missing material is left empty:
     * the transport then refuses to connect rather than falling back to an unverified link.
     */
    public static ObservationTransport.Config buildConfig(Context ctx, String deviceId,
                                                         String displayName, String host, int port) {
        final ObservationTransport.Config config = new ObservationTransport.Config();
        final File dir = ctx.getFilesDir();
        config.host = host;
        config.port = port;
        config.deviceId = deviceId == null ? "" : deviceId;
        config.displayName = displayName == null ? "" : displayName;
        config.pkcs12Path = new File(dir, DEVICE_P12).getAbsolutePath();
        config.pkcs12PasswordFile = new File(dir, DEVICE_P12_PASS).getAbsolutePath();
        config.trustStorePath = new File(dir, CA_P12).getAbsolutePath();
        config.trustStorePasswordFile = new File(dir, CA_P12_PASS).getAbsolutePath();
        return config;
    }

    /** True when the app appears to hold its provisioning material (existence only - never read). */
    public static boolean credentialsPresent(Context ctx) {
        final File dir = ctx.getFilesDir();
        return new File(dir, DEVICE_P12).isFile()
                && new File(dir, DEVICE_P12_PASS).isFile()
                && new File(dir, CA_P12).isFile()
                && new File(dir, CA_P12_PASS).isFile();
    }
}
