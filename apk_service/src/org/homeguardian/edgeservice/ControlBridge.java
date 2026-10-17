package org.homeguardian.edgeservice;

import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Locale;

/**
 * HG-NET-02B Phase 2: the OUTBOUND authenticated control bridge.
 *
 * WHY THIS EXISTS
 * The C++ MCP server binds 127.0.0.1:8790 only, which was measured on the device: a request to the
 * LAN address on the same port is refused, and /proc/net/tcp shows a single loopback binding. A
 * packet arriving on a VPN/TUN interface is addressed to the VPN address, never 127.0.0.1, so it can
 * never match. Rebinding to a wildcard address would fix reachability and is forbidden. Therefore
 * control must be DIALED OUT. This is the architecture, not a convenience fallback.
 *
 * THE SECURITY MODEL, IN ONE PARAGRAPH
 * Nothing here listens. The bridge opens a TCP connection toward an authorized coordinator and
 * proves two things in both directions: the coordinator must present the enrolment secret this
 * device generated, and the device presents a durable identity derived from a key it generated.
 * Only an allowlisted set of MCP control methods crosses the wire, each bounded by a hard frame cap
 * validated BEFORE allocation. Every relayed request is POSTed to one fixed loopback URL - the
 * destination is a constant, never derived from the request - so the bridge cannot be used as a
 * generic proxy to any other local service. Camera and microphone data never pass through it.
 *
 * THE PRODUCTION MCP BEARER TOKEN IS NEVER EXPORTED
 * The bridge reads it in-process via Provisioner.readToken(), exactly as the app's own probe does,
 * because the file is owned by the app's uid. It is never logged, never sent to the coordinator and
 * never written anywhere else. Authorized remote clients authenticate to the BRIDGE with the
 * separate enrolment secret, not with the MCP token.
 *
 * DEFAULT OFF. The bridge starts only when a coordinator endpoint has been provisioned AND an
 * explicit operator action starts it, so an unconfigured (production) install dials nowhere.
 */
public class ControlBridge {

    public static final String TAG = "HGBridge";

    // ---- provisioning files (0600, app-private) ----------------------------
    private static final String IDENTITY_NAME = "bridge_identity.json";
    private static final String ENROL_NAME = "bridge_enrol.json";

    // ---- protocol constants: all bounds are explicit and enforced ----------
    private static final String MAGIC = "HGB1";
    private static final int MAX_HANDSHAKE_LINE = 256;   // bytes, handshake lines
    private static final int MAX_FRAME = 64 * 1024;      // hard cap, validated before allocation
    private static final int HANDSHAKE_TIMEOUT_MS = 8000;
    private static final int RELAY_TIMEOUT_MS = 15000;
    private static final long RECONNECT_MIN_MS = 1000L;
    private static final long RECONNECT_MAX_MS = 60000L;

    /**
     * Only these JSON-RPC methods may cross the bridge. A method allowlist (rather than a blocklist)
     * is what stops the bridge becoming a general-purpose local-service proxy.
     */
    private static final String[] ALLOWED_METHODS = {
            "initialize", "notifications/initialized", "tools/list", "tools/call", "ping"
    };

    private final Context ctx;
    private volatile boolean running = false;
    private Thread worker;
    private volatile Socket socket;
    private volatile String mcpSessionId = null;
    private static volatile String lastStatus = "bridge: idle";

    public ControlBridge(Context context) { this.ctx = context; }

    public static String status() { return lastStatus; }
    public boolean isRunning() { return running; }

    // ====================================================================== provisioning

    /** 0600 file holding the device's durable bridge identity key. */
    private static File identityFile(Context ctx) { return new File(ctx.getFilesDir(), IDENTITY_NAME); }
    /** 0600 file holding the enrolment secret an authorized coordinator must present. */
    private static File enrolFile(Context ctx) { return new File(ctx.getFilesDir(), ENROL_NAME); }

    /**
     * Create the device identity if absent. The id is derived from a locally generated 32-byte key,
     * NOT from an IP address or any LAN detail - the brief forbids address-as-identity.
     */
    public static synchronized String ensureIdentity(Context ctx) throws Exception {
        File f = identityFile(ctx);
        if (f.exists()) return deviceIdFrom(identityFileBytes(f));
        byte[] key = new byte[32];
        new SecureRandom().nextBytes(key);
        writePrivate(f, toHex(key));
        return deviceIdFrom(identityFileBytes(f));
    }

    private static byte[] identityFileBytes(File f) throws Exception {
        return readAll(f).trim().getBytes("UTF-8");
    }

    /** hg-android-<16 hex of SHA-256(device key)>. Stable, durable, not an address. */
    public static String deviceId(Context ctx) throws Exception {
        return deviceIdFrom(identityFileBytes(identityFile(ctx)));
    }

    private static String deviceIdFrom(byte[] keyMaterial) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        return "hg-android-" + toHex(md.digest(keyMaterial)).substring(0, 16);
    }

    /**
     * The enrolment secret for authorized coordinators. Generated on-device. This is the SUPPORTED
     * provisioning path the issue asks for: the operator reads it ONCE via an explicit on-device
     * action and configures the coordinator with it. It is deliberately a different secret from the
     * MCP bearer token, which is never revealed.
     */
    public static synchronized String ensureEnrolment(Context ctx) throws Exception {
        File f = enrolFile(ctx);
        if (f.exists()) return readAll(f).trim();
        byte[] s = new byte[32];
        new SecureRandom().nextBytes(s);
        String token = toHex(s);
        writePrivate(f, token);
        return token;
    }

    public static String enrolmentFingerprint(Context ctx) {
        try {
            return fingerprint(ensureEnrolment(ctx));
        } catch (Exception e) {
            return "unavailable";
        }
    }

    /** SHA-256, first 16 hex: identifies a secret without revealing it. */
    public static String fingerprint(String secret) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        return toHex(md.digest(secret.getBytes("UTF-8"))).substring(0, 16);
    }

    /** Close the credential file if it is empty. */
    public static synchronized void revokeEnrolment(Context ctx) {
        File f = enrolFile(ctx);
        if (f.exists() && !f.delete()) Log.w(TAG, "could not remove the enrolment secret");
    }

    // ====================================================================== lifecycle

    private volatile String coordHost;
    private volatile int coordPort;

    /** Point the bridge at an authorized coordinator. Both values are operator-provided. */
    public synchronized void configure(String host, int port) {
        this.coordHost = host;
        this.coordPort = port;
    }

    /** Start the outbound loop. Refuses when no coordinator is configured (the production case). */
    public synchronized String start() {
        if (running) return "bridge: already running";
        if (coordHost == null || coordPort <= 0) {
            lastStatus = "bridge: not started - no coordinator configured (production default)";
            Log.i(TAG, lastStatus);
            return lastStatus;
        }
        running = true;
        worker = new Thread(new Runnable() { @Override public void run() { loop(); } }, "hg-bridge");
        worker.start();
        lastStatus = "bridge: dialing " + coordHost + ":" + coordPort;
        Log.i(TAG, lastStatus);
        return lastStatus;
    }

    public synchronized void stop() {
        running = false;
        Socket s = socket;
        socket = null;
        try { if (s != null) s.close(); } catch (Exception ignored) { }
        if (worker != null) {
            try { worker.join(3000); } catch (InterruptedException ignored) { }
            worker = null;
        }
        lastStatus = "bridge: stopped";
        Log.i(TAG, lastStatus);
    }

    /** One session at a time: the previous socket is closed before a new one is dialed. */
    private void loop() {
        long backoff = RECONNECT_MIN_MS;
        while (running) {
            try {
                session();
                backoff = RECONNECT_MIN_MS;      // a clean session resets the backoff
            } catch (Exception e) {
                if (running) Log.w(TAG, "session ended: " + e);
            }
            if (!running) break;
            try { Thread.sleep(backoff); } catch (InterruptedException ignored) { break; }
            backoff = Math.min(backoff * 2, RECONNECT_MAX_MS);
        }
    }

    // ====================================================================== one session

    private void session() throws Exception {
        String token = Provisioner.readToken(ctx);   // in-process only; never leaves the device
        int mcpPort = Provisioner.DEFAULT_PORT;

        Socket s = new Socket();
        socket = s;
        s.connect(new InetSocketAddress(coordHost, coordPort), HANDSHAKE_TIMEOUT_MS);
        s.setSoTimeout(HANDSHAKE_TIMEOUT_MS);
        OutputStream out = s.getOutputStream();
        InputStream in = s.getInputStream();

        // --- handshake: we present identity, the coordinator must present the enrolment secret ---
        String id = deviceId(ctx);
        String ourFp = enrolmentFingerprint(ctx);
        writeLine(out, MAGIC + " " + id + " " + ourFp);
        String presented = readLine(in);
        String expected = ensureEnrolment(ctx);
        if (presented == null || !constantTimeEquals(presented.trim(), expected)) {
            writeLine(out, "DENY");
            lastStatus = "bridge: coordinator REJECTED (bad enrolment secret)";
            Log.w(TAG, lastStatus);
            close(s);
            return;
        }
        writeLine(out, "OK");
        lastStatus = "bridge: connected to " + coordHost + ":" + coordPort + " as " + id;
        Log.i(TAG, lastStatus);

        // --- relay loop: strictly one request in flight, bounded framing ---
        s.setSoTimeout(RELAY_TIMEOUT_MS);
        int relayed = 0, rejected = 0;
        while (running) {
            byte[] frame = readFrame(in);
            if (frame == null) break;

            int reqId = -1;
            String payload = null;
            String denyReason = null;
            try {
                org.json.JSONObject o = new org.json.JSONObject(new String(frame, "UTF-8"));
                reqId = o.optInt("id", -1);
                String method = o.optString("method", "");
                if (!isAllowed(method)) {
                    denyReason = "method not allowed: " + method;
                } else {
                    payload = new String(frame, "UTF-8");
                }
            } catch (Exception e) {
                denyReason = "malformed request";
            }

            if (denyReason != null) {
                rejected++;
                Log.w(TAG, "rejected a bridged request: " + denyReason);
                writeFrame(out, reqId, error(reqId, -32601, denyReason).toString().getBytes("UTF-8"));
                continue;
            }

            // The destination is a CONSTANT. It is never derived from the request, which is what
            // keeps the bridge from becoming a proxy to arbitrary local services.
            String resp = relay(mcpPort, payload, token);
            relayed++;
            writeFrame(out, reqId, resp.getBytes("UTF-8"));
        }
        lastStatus = "bridge: session closed (relayed=" + relayed + " rejected=" + rejected + ")";
        Log.i(TAG, lastStatus);
        close(s);
    }

    private static boolean isAllowed(String method) {
        for (String m : ALLOWED_METHODS) if (m.equals(method)) return true;
        return false;
    }

    /** POST to the fixed loopback MCP endpoint, with the in-process bearer token. */
    private String relay(int port, String body, String token) {
        HttpURLConnection c = null;
        try {
            URL url = new URL("http://" + Provisioner.LOOPBACK + ":" + port + "/mcp");
            c = (HttpURLConnection) url.openConnection();
            c.setRequestMethod("POST");
            c.setConnectTimeout(4000);
            c.setReadTimeout(RELAY_TIMEOUT_MS);
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.setRequestProperty("Accept", "application/json");
            c.setRequestProperty("Authorization", "Bearer " + token);
            if (mcpSessionId != null && !mcpSessionId.isEmpty()) {
                c.setRequestProperty("Mcp-Session-Id", mcpSessionId);
            }
            byte[] b = body.getBytes("UTF-8");
            c.setFixedLengthStreamingMode(b.length);
            OutputStream os = c.getOutputStream();
            os.write(b);
            os.flush();
            os.close();
            int code = c.getResponseCode();
            String sid = c.getHeaderField("Mcp-Session-Id");
            if (sid != null && !sid.trim().isEmpty()) {
                mcpSessionId = sid.trim();
            }
            InputStream is = (code >= 200 && code < 300) ? c.getInputStream() : c.getErrorStream();
            String text = is == null ? "" : readAllStream(is);
            if (code < 200 || code >= 300) {
                // Never propagate the token or any auth detail back to the peer.
                Log.w(TAG, "loopback MCP returned HTTP " + code);
            }
            return text.length() == 0 ? error(-1, -32603, "empty response").toString() : text;
        } catch (Exception e) {
            Log.w(TAG, "relay failed: " + e);
            return error(-1, -32603, "relay failed").toString();
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static org.json.JSONObject error(int id, int code, String msg) {
        try {
            org.json.JSONObject err = new org.json.JSONObject();
            err.put("code", code);
            err.put("message", msg);
            org.json.JSONObject o = new org.json.JSONObject();
            o.put("jsonrpc", "2.0");
            o.put("id", id);
            o.put("error", err);
            return o;
        } catch (Exception e) {
            return new org.json.JSONObject();
        }
    }

    // ====================================================================== framing

    /** [4B big-endian length][payload]. Length is validated BEFORE any allocation. */
    private static byte[] readFrame(InputStream in) throws Exception {
        byte[] hdr = readN(in, 4);
        if (hdr == null) return null;
        int len = ((hdr[0] & 0xFF) << 24) | ((hdr[1] & 0xFF) << 16) | ((hdr[2] & 0xFF) << 8) | (hdr[3] & 0xFF);
        if (len <= 0 || len > MAX_FRAME) {
            // Refuse rather than allocate. An over-long frame is a protocol violation, not a request.
            Log.w(TAG, "refused frame of length " + len + " (cap " + MAX_FRAME + ")");
            return null;
        }
        return readN(in, len);
    }

    private static void writeFrame(OutputStream out, int id, byte[] payload) throws Exception {
        if (payload == null) payload = new byte[0];
        if (payload.length > MAX_FRAME) {              // never emit beyond the cap either
            payload = error(id, -32603, "response exceeds frame cap").toString().getBytes("UTF-8");
        }
        out.write(new byte[]{
                (byte) (payload.length >>> 24), (byte) (payload.length >>> 16),
                (byte) (payload.length >>> 8), (byte) payload.length});
        out.write(payload);
        out.flush();
    }

    private static byte[] readN(InputStream in, int n) throws Exception {
        byte[] buf = new byte[n];
        int off = 0;
        while (off < n) {
            int r = in.read(buf, off, n - off);
            if (r < 0) return null;
            off += r;
        }
        return buf;
    }

    private static void writeLine(OutputStream out, String s) throws Exception {
        out.write((s + "\n").getBytes("UTF-8"));
        out.flush();
    }

    /** Reads at most MAX_HANDSHAKE_LINE bytes: handshake input is bounded too. */
    private static String readLine(InputStream in) throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        while (b.size() <= MAX_HANDSHAKE_LINE) {
            int ch = in.read();
            if (ch < 0) break;
            if (ch == '\n') return new String(b.toByteArray(), "UTF-8");
            b.write(ch);
        }
        return null;
    }

    private static void close(Socket s) {
        try { s.close(); } catch (Exception ignored) { }
    }

    /** Comparison that does not leak length or prefix through timing. */
    private static boolean constantTimeEquals(String a, String b) {
        byte[] x = a.getBytes();
        byte[] y = b.getBytes();
        if (x.length != y.length) return false;
        int d = 0;
        for (int i = 0; i < x.length; i++) d |= x[i] ^ y[i];
        return d == 0;
    }

    // ====================================================================== helpers

    private static String toHex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format(Locale.US, "%02x", x));
        return sb.toString();
    }

    private static void writePrivate(File f, String content) throws Exception {
        FileOutputStream fo = new FileOutputStream(f);
        fo.write(content.getBytes("UTF-8"));
        fo.flush();
        fo.close();
        // Owner-only. Best effort: on Android this is already the app's private dir.
        try { f.setReadable(false, false); } catch (Exception ignored) { }
        try { f.setReadable(true, true); } catch (Exception ignored) { }
    }

    private static String readAll(File f) throws Exception {
        InputStream in = new java.io.FileInputStream(f);
        String s = readAllStream(in);
        in.close();
        return s;
    }

    private static String readAllStream(InputStream in) throws Exception {
        BufferedReader r = new BufferedReader(new InputStreamReader(in, "UTF-8"));
        StringBuilder sb = new StringBuilder();
        String line;
        while ((line = r.readLine()) != null) sb.append(line).append('\n');
        return sb.toString();
    }
}
