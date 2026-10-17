package org.homeguardian.edgeservice;

import android.content.Context;
import android.system.Os;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.security.SecureRandom;

/**
 * Provisions the two files the native MCP server needs, inside the app's PRIVATE
 * files directory:
 *
 *   edge_config.json      - engine config (capture DISABLED, loopback, port 8790)
 *   mcp_credentials.json  - 0600, owned by the app uid, one bearer token
 *
 * Why this works without root or ADB: filesDir is owned by the app's own uid, and
 * AlexaCredentialStore::load() requires exactly that (regular file, no group/other
 * bits, st_uid == geteuid()). Neither condition needs a privileged caller.
 *
 * The token is generated with SecureRandom and is never displayed, never logged and
 * never written outside the app sandbox by this class.
 */
public final class Provisioner {

    private static final String TAG = "HGEdge";

    public static final String CONFIG_NAME = "edge_config.json";
    public static final String CREDS_NAME = "mcp_credentials.json";
    public static final int DEFAULT_PORT = 8790;
    public static final String LOOPBACK = "127.0.0.1";

    private Provisioner() {}

    public static File configFile(Context ctx) { return new File(ctx.getFilesDir(), CONFIG_NAME); }

    public static File credsFile(Context ctx) { return new File(ctx.getFilesDir(), CREDS_NAME); }

    /** True when both files exist and the credential file holds at least one token. */
    public static boolean isProvisioned(Context ctx) {
        File c = configFile(ctx);
        File k = credsFile(ctx);
        if (!c.isFile() || !k.isFile()) return false;
        try {
            JSONObject o = new JSONObject(readAll(k));
            return o.keys().hasNext();
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * Create the config and (if absent) the credential file. Never rotates an
     * existing token, because rotating silently would invalidate the operator's
     * paired client without telling anyone.
     */
    public static synchronized void provisionIfNeeded(Context ctx) throws Exception {
        writeConfigIfNeeded(ctx);
        if (!credsFile(ctx).isFile()) {
            writeCredentials(ctx, newTokenHex());
        } else {
            hardenPermissions(credsFile(ctx));
        }
    }

    /** Generate and persist a fresh token (explicit operator action: "Rotate token"). */
    public static synchronized void rotateToken(Context ctx) throws Exception {
        writeConfigIfNeeded(ctx);
        writeCredentials(ctx, newTokenHex());
    }

    public static String readToken(Context ctx) throws Exception {
        JSONObject o = new JSONObject(readAll(credsFile(ctx)));
        java.util.Iterator<String> it = o.keys();
        return it.hasNext() ? it.next() : null;
    }

    /** SHA-256 of the token, first 16 hex chars: identifies the principal without revealing it. */
    public static String tokenFingerprint(String token) {
        try {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
            byte[] d = md.digest(token.getBytes("UTF-8"));
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 8; i++) sb.append(String.format("%02x", d[i]));
            return sb.toString();
        } catch (Exception e) {
            return "unavailable";
        }
    }

    private static void writeConfigIfNeeded(Context ctx) throws Exception {
        File f = configFile(ctx);
        if (f.isFile()) return;
        JSONObject j = new JSONObject();
        j.put("log_level", "info");
        j.put("server_host", LOOPBACK);
        j.put("server_port", DEFAULT_PORT);
        j.put("data_dir", ctx.getFilesDir().getAbsolutePath());
        j.put("max_event_history", 1000);
        j.put("correlation_window_ms", 5000);
        j.put("alert_confidence_threshold", 0.5);
        // The edge node performs no physical capture. This is the honest value and
        // it makes the consent/capture gate refuse every observation.
        j.put("media_capture_enabled", false);
        j.put("cloud_processing_enabled", false);
        j.put("consent_policy_version", "1.0.0");
        j.put("routine_retention_days", 730);
        j.put("profile_retention_days", 1825);
        j.put("alert_retention_days", 30);
        writePrivate(f, j.toString(), 0600);
    }

    private static void writeCredentials(Context ctx, String token) throws Exception {
        JSONObject entry = new JSONObject();
        entry.put("principal", "edge-service");
        entry.put("allow_status", true);
        entry.put("allow_alert_summary", true);
        entry.put("expires_at_ms", 0);
        entry.put("revoked", false);
        JSONObject root = new JSONObject();
        root.put(token, entry);
        writePrivate(credsFile(ctx), root.toString(), 0600);
        Log.i(TAG, "credentials provisioned, principal=edge-service");
    }

    /** Write, then force owner-only permissions (the store rejects anything broader). */
    private static void writePrivate(File f, String content, int mode) throws Exception {
        FileOutputStream out = new FileOutputStream(f, false);
        try {
            out.write(content.getBytes("UTF-8"));
            out.flush();
            out.getFD().sync();
        } finally {
            out.close();
        }
        hardenPermissions(f);
        try {
            Os.chmod(f.getAbsolutePath(), mode);
        } catch (Throwable t) {
            // Fall back to the java.io API; the native store re-checks the mode and
            // fails closed if this did not take effect.
            f.setReadable(false, false);
            f.setWritable(false, false);
            f.setReadable(true, true);
            f.setWritable(true, true);
        }
    }

    private static void hardenPermissions(File f) {
        try {
            Os.chmod(f.getAbsolutePath(), 0600);
        } catch (Throwable ignored) {
            // Reported implicitly: the native store rejects a broad mode.
        }
    }

    private static String newTokenHex() {
        byte[] b = new byte[32];
        new SecureRandom().nextBytes(b);
        StringBuilder sb = new StringBuilder(64);
        for (byte x : b) sb.append(String.format("%02x", x));
        return sb.toString();
    }

    private static String readAll(File f) throws Exception {
        RandomAccessFile raf = new RandomAccessFile(f, "r");
        try {
            byte[] buf = new byte[(int) raf.length()];
            raf.readFully(buf);
            return new String(buf, "UTF-8");
        } finally {
            raf.close();
        }
    }

    /** The exact file mode the native store will enforce, for evidence. */
    public static String credsMode(Context ctx) {
        try {
            int m = Os.stat(credsFile(ctx).getAbsolutePath()).st_mode & 0777;
            return String.format("0%o", m);
        } catch (Throwable t) {
            return "unknown";
        }
    }
}
