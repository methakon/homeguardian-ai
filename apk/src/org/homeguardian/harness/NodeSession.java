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

    /** Where the operator drops the four files for the one-time import. */
    public static final String IMPORT_DIR = "hg_provisioning";
    /** A credential file is small; anything larger is refused rather than copied. */
    private static final int MAX_CREDENTIAL_BYTES = 65536;

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
                    provenance == null ? "synthetic" : provenance));
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

    /**
     * One-time provisioning import, driven by an explicit operator action.
     *
     * WHY IT EXISTS: this app is NOT debuggable, so `run-as` cannot place files in its private
     * storage, and there is no root. The app's OWN external files directory
     * (`Android/data/<pkg>/files/`) is writable by adb and needs NO storage permission, so the
     * operator drops the four files there and the app COPIES them into private storage.
     *
     * The source files are DELETED after a successful import, because on API 27 (no scoped
     * storage) another app holding READ_EXTERNAL_STORAGE could read that directory - so the
     * exposure window is as short as this method can make it. Contents are never logged, never
     * returned, and never displayed.
     */
    public static String importCredentials(Context ctx) {
        final java.io.File src = new java.io.File(ctx.getExternalFilesDir(null), IMPORT_DIR);
        final java.io.File dst = ctx.getFilesDir();
        if (src == null || !src.isDirectory()) {
            return "no import directory; expected " + (src == null ? "(unavailable)" : src.getAbsolutePath());
        }
        final String[] names = {DEVICE_P12, DEVICE_P12_PASS, CA_P12, CA_P12_PASS};
        int copied = 0;
        final StringBuilder missing = new StringBuilder();
        for (int i = 0; i < names.length; i++) {
            final java.io.File from = new java.io.File(src, names[i]);
            final java.io.File to = new java.io.File(dst, names[i]);
            if (!from.isFile()) {
                missing.append(' ').append(names[i]);
                continue;
            }
            if (!copyFile(from, to)) {
                return "import failed while copying " + names[i] + " (nothing was changed)";
            }
            to.setReadable(false, false);
            to.setReadable(true, true);
            copied++;
        }
        if (copied != names.length) {
            return "import incomplete: copied " + copied + " of " + names.length + ", missing:" + missing;
        }
        // Shred the source: the private copies are the ones that count.
        int deleted = 0;
        for (int i = 0; i < names.length; i++) {
            final java.io.File from = new java.io.File(src, names[i]);
            if (from.isFile() && from.delete()) {
                deleted++;
            }
        }
        return "imported " + copied + " credential files into private storage; source files deleted="
                + deleted;
    }

    /** Bounded byte copy. Returns false on any failure; never logs or returns content. */
    private static boolean copyFile(java.io.File from, java.io.File to) {
        java.io.FileInputStream in = null;
        java.io.FileOutputStream out = null;
        try {
            in = new java.io.FileInputStream(from);
            out = new java.io.FileOutputStream(to);
            final byte[] buffer = new byte[8192];
            int total = 0;
            int read;
            while ((read = in.read(buffer)) > 0) {
                total += read;
                if (total > MAX_CREDENTIAL_BYTES) {
                    return false;   // bounded: a credential file is small
                }
                out.write(buffer, 0, read);
            }
            out.flush();
            return true;
        } catch (java.io.IOException e) {
            return false;
        } finally {
            try {
                if (in != null) in.close();
            } catch (java.io.IOException ignored) {
            }
            try {
                if (out != null) out.close();
            } catch (java.io.IOException ignored) {
            }
        }
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
