package org.homeguardian.edgeservice;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * HG-NET-02E — Android edge client for the Outbound Authenticated MCP Bridge.
 *
 * PROTOCOL CONTRACT
 * Implements the wire protocol Antigravity specified in
 * docs/HG_NET_02_OUTBOUND_BRIDGE_SPEC.md (branch feature/hg-net-02e-outbound-mcp-bridge):
 * RFC 6455 WebSocket carrying UTF-8 JSON, message types bridge_auth / bridge_auth_ok /
 * bridge_auth_err / mcp_request / mcp_response / heartbeat_ping / heartbeat_pong /
 * session_terminate. This REPLACES the earlier raw-TCP "HGB1" framing, which could not talk
 * to the WebSocket coordinator - that mismatch is the reason this class exists in this shape.
 *
 * Deliberate deviation from the spec's reference snippet: the spec suggests OkHttp. This project
 * builds with aapt/d8 and no third-party Java dependencies, so RFC 6455 framing is implemented
 * here directly (~150 lines) rather than adding a dependency to a security-sensitive component.
 * The wire behaviour is the spec's; only the plumbing differs.
 *
 * WHY OUTBOUND AT ALL
 * The C++ MCP server binds 127.0.0.1:8790 only (measured: a request to the LAN address on the
 * same port is refused; /proc/net/tcp shows a single loopback binding). A packet arriving on a
 * VPN interface is addressed to the VPN address and cannot match. A wildcard rebind would be
 * reachable and is forbidden. So control is dialed out. Nothing here listens.
 *
 * SECURITY INVARIANTS IMPLEMENTED (spec section 3)
 *   - method allowlist AND tool allowlist, not a blocklist
 *   - 64 KB frame cap, enforced BEFORE allocation; over-cap tears the socket down with 4413
 *   - at most 8 in-flight requests; excess is refused, never queued without bound
 *   - 5 s request timeout
 *   - heartbeat ping every 15 s (WebSocket control frame), inactive timeout 35 s
 *   - revocation (4403) HALTS the reconnect loop rather than retrying forever
 *   - the loopback destination is a CONSTANT, never derived from the request payload
 *   - the production MCP bearer token is read in-process and never transmitted
 *
 * DEFAULT OFF: an unconfigured install dials nowhere.
 */
public class OutboundMcpBridge {

    public static final String TAG = "HGBridge";

    /** Spec: "1.0.0". Sent in bridge_auth so the coordinator can gate on it. */
    private static final String BRIDGE_VERSION = "1.0.0";

    // ---- spec section 3 bounds --------------------------------------------
    private static final int MAX_FRAME = 64 * 1024;          // 64 KB, validated pre-allocation
    private static final int MAX_IN_FLIGHT = 8;              // concurrent local calls
    private static final int REQUEST_TIMEOUT_MS = 5000;      // 5 s
    private static final int HEARTBEAT_MS = 15000;           // ping every 15 s
    private static final int INACTIVE_TIMEOUT_MS = 35000;    // 35 s without a pong -> drop
    private static final long RECONNECT_MIN_MS = 1000L;
    private static final long RECONNECT_MAX_MS = 60000L;

    private static final int WS_CLOSE_PAYLOAD_TOO_LARGE = 4413;
    private static final int WS_CLOSE_REVOKED = 4403;

    /** Spec section 3.1: permitted JSON-RPC methods. */
    private static final String[] ALLOWED_METHODS = {
            "initialize", "notifications/initialized", "tools/list", "tools/call", "ping"
    };
    /** Spec section 3.1: permitted TOOLS. A tool allowlist is what stops a permitted method
     *  being used to reach a mutating tool such as set_capture_enabled. */
    private static final String[] ALLOWED_TOOLS = {
            "get_homeguardian_status", "get_alert_summary"
    };

    private static final String IDENTITY_NAME = "bridge_identity.json";
    private static final String TOKEN_NAME = "bridge_auth.json";

    private final Context ctx;
    private volatile boolean running = false;
    private volatile boolean revoked = false;       // set on 4403; suppresses reconnect
    private Thread worker;
    private Thread heartbeat;
    private volatile Socket socket;
    private volatile OutputStream wsOut;
    private final AtomicInteger inFlight = new AtomicInteger(0);
    private volatile long lastInboundMs = 0L;
    private static volatile String lastStatus = "bridge: idle";

    public OutboundMcpBridge(Context context) { this.ctx = context; }

    public static String status() { return lastStatus; }
    public boolean isRunning() { return running; }

    // ====================================================================== credentials

    private static File identityFile(Context c) { return new File(c.getFilesDir(), IDENTITY_NAME); }
    private static File tokenFile(Context c) { return new File(c.getFilesDir(), TOKEN_NAME); }

    /**
     * Durable device identity derived from a locally generated key. Never an IP address: the
     * brief forbids using a development LAN address as device identity.
     */
    public static synchronized String deviceId(Context ctx) throws Exception {
        File f = identityFile(ctx);
        if (!f.exists()) {
            byte[] k = new byte[32];
            new SecureRandom().nextBytes(k);
            writePrivate(f, toHex(k));
        }
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        return "hg-android-" + toHex(md.digest(readAll(f).trim().getBytes("UTF-8"))).substring(0, 16);
    }

    /**
     * The 256-bit CSPRNG auth token this edge presents to the coordinator (spec frame 1). The
     * coordinator's device registry must hold it. Separate from the MCP bearer token, which is
     * never revealed to anyone. Shown once, on explicit operator action, for provisioning.
     */
    public static synchronized String authToken(Context ctx) throws Exception {
        File f = tokenFile(ctx);
        if (f.exists()) return readAll(f).trim();
        byte[] s = new byte[32];
        new SecureRandom().nextBytes(s);
        String t = toHex(s);
        writePrivate(f, t);
        return t;
    }

    public static String tokenFingerprint(Context ctx) {
        try { return fingerprint(authToken(ctx)); } catch (Exception e) { return "unavailable"; }
    }

    public static String fingerprint(String s) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        return toHex(md.digest(s.getBytes("UTF-8"))).substring(0, 16);
    }

    public static synchronized void revokeAuthToken(Context ctx) {
        File f = tokenFile(ctx);
        if (f.exists() && !f.delete()) Log.w(TAG, "could not remove the auth token");
    }

    // ====================================================================== lifecycle

    private volatile String coordUrl;

    /** e.g. ws://127.0.0.1:18995/bridge . Operator-supplied; never compiled in. */
    public synchronized void configure(String url) { this.coordUrl = url; }

    public synchronized String start() {
        if (running) return "bridge: already running";
        if (revoked) return "bridge: refused - this device was REVOKED by the coordinator";
        if (coordUrl == null || coordUrl.length() == 0) {
            lastStatus = "bridge: not started - no coordinator configured (production default)";
            Log.i(TAG, lastStatus);
            return lastStatus;
        }
        running = true;
        worker = new Thread(new Runnable() { @Override public void run() { loop(); } }, "hg-bridge-ws");
        worker.start();
        lastStatus = "bridge: dialing " + coordUrl;
        Log.i(TAG, lastStatus);
        return lastStatus;
    }

    public synchronized void stop() {
        running = false;
        closeSocket(1000, "client stop");
        if (worker != null) { try { worker.join(3000); } catch (InterruptedException ignored) {} worker = null; }
        lastStatus = "bridge: stopped";
        Log.i(TAG, lastStatus);
    }

    private void loop() {
        long backoff = RECONNECT_MIN_MS;
        while (running && !revoked) {
            try {
                session();
                backoff = RECONNECT_MIN_MS;
            } catch (Exception e) {
                if (running && !revoked) Log.w(TAG, "session ended: " + e);
            }
            if (!running || revoked) break;
            try { Thread.sleep(backoff); } catch (InterruptedException ignored) { break; }
            backoff = Math.min(backoff * 2, RECONNECT_MAX_MS);
        }
        lastStatus = revoked ? "bridge: HALTED - device revoked (no further reconnect)"
                             : "bridge: stopped";
        Log.i(TAG, lastStatus);
    }

    // ====================================================================== one session

    private void session() throws Exception {
        String url = coordUrl;
        String host; int port; String path;
        if (url.startsWith("ws://"))       { url = url.substring(5);  }
        else if (url.startsWith("wss://")) { throw new Exception("wss not enabled: no TLS trust store configured on this build"); }
        int slash = url.indexOf('/');
        path = slash < 0 ? "/" : url.substring(slash);
        String hp = slash < 0 ? url : url.substring(0, slash);
        int colon = hp.lastIndexOf(':');
        host = colon < 0 ? hp : hp.substring(0, colon);
        port = colon < 0 ? 80 : Integer.parseInt(hp.substring(colon + 1));

        Socket s = new Socket();
        socket = s;
        s.connect(new InetSocketAddress(host, port), 8000);
        s.setSoTimeout(INACTIVE_TIMEOUT_MS);
        InputStream in = s.getInputStream();
        OutputStream out = s.getOutputStream();

        // --- RFC 6455 opening handshake -------------------------------------
        byte[] nonce = new byte[16];
        new SecureRandom().nextBytes(nonce);
        String key = java.util.Base64.getEncoder().encodeToString(nonce);
        String req = "GET " + path + " HTTP/1.1\r\n"
                + "Host: " + host + ":" + port + "\r\n"
                + "Upgrade: websocket\r\n"
                + "Connection: Upgrade\r\n"
                + "Sec-WebSocket-Key: " + key + "\r\n"
                + "Sec-WebSocket-Version: 13\r\n\r\n";
        out.write(req.getBytes("UTF-8"));
        out.flush();

        String statusLine = readHttpLine(in);
        if (statusLine == null || !statusLine.contains("101")) {
            throw new Exception("websocket upgrade refused: " + statusLine);
        }
        String expectAccept = wsAccept(key);
        boolean acceptOk = false;
        String h;
        while ((h = readHttpLine(in)) != null && h.length() > 0) {
            if (h.toLowerCase(Locale.US).startsWith("sec-websocket-accept:")) {
                acceptOk = h.substring(h.indexOf(':') + 1).trim().equals(expectAccept);
            }
        }
        if (!acceptOk) throw new Exception("Sec-WebSocket-Accept mismatch - refusing the session");

        wsOut = out;
        inFlight.set(0);
        lastInboundMs = System.currentTimeMillis();

        // --- frame 1: bridge_auth (spec section 2.B.1) ----------------------
        JSONObject auth = new JSONObject();
        auth.put("type", "bridge_auth");
        auth.put("device_id", deviceId(ctx));
        auth.put("auth_token", authToken(ctx));
        auth.put("bridge_version", BRIDGE_VERSION);
        auth.put("timestamp", System.currentTimeMillis());
        writeText(auth.toString());

        startHeartbeat();
        lastStatus = "bridge: connected to " + coordUrl + " as " + deviceId(ctx);
        Log.i(TAG, lastStatus);

        // --- frame loop -----------------------------------------------------
        int relayed = 0, refused = 0;
        try {
            while (running) {
                String text = readText();
                if (text == null) break;
                lastInboundMs = System.currentTimeMillis();

                JSONObject msg = new JSONObject(text);
                String type = msg.optString("type", "");

                if ("bridge_auth_ok".equals(type)) {
                    Log.i(TAG, "auth OK session=" + msg.optString("session_id")
                            + " proto=" + msg.optString("protocol_version")
                            + " hb=" + msg.optInt("heartbeat_interval_ms", HEARTBEAT_MS));
                } else if ("bridge_auth_err".equals(type)) {
                    throw new Exception("coordinator rejected auth: " + msg.optString("reason", "unspecified"));
                } else if ("session_terminate".equals(type)) {
                    Log.i(TAG, "coordinator terminated the session");
                    break;
                } else if ("mcp_request".equals(type)) {
                    if (handleMcpRequest(msg)) relayed++; else refused++;
                } else if ("heartbeat_pong".equals(type)) {
                    // liveness confirmed by lastInboundMs
                } else {
                    Log.w(TAG, "ignoring unknown frame type: " + type);
                }
            }
        } finally {
            stopHeartbeat();
            lastStatus = "bridge: session closed (relayed=" + relayed + " refused=" + refused + ")";
            Log.i(TAG, lastStatus);
            closeSocket(1000, "session end");
        }
    }

    // ====================================================================== request handling

    /** @return true if the request was relayed (or answered), false if it was refused. */
    private boolean handleMcpRequest(JSONObject msg) {
        String corrId = msg.optString("correlation_id", "");
        JSONObject body = msg.optJSONObject("body");
        if (body == null) {
            safeResponse(corrId, 400, errorBody(-1, -32600, "missing body"));
            return false;
        }
        String method = body.optString("method", "");

        if (!isAllowedMethod(method)) {
            Log.w(TAG, "refused method: " + method);
            safeResponse(corrId, 400, errorBody(body.optInt("id", -1), -32601, "UNAUTHORIZED_OPERATION"));
            return false;
        }
        // Tool-level check: a permitted method must not be used to reach a mutating tool.
        if ("tools/call".equals(method)) {
            JSONObject params = body.optJSONObject("params");
            String tool = params == null ? "" : params.optString("name", "");
            if (!isAllowedTool(tool)) {
                Log.w(TAG, "refused tool: " + tool);
                safeResponse(corrId, 400, errorBody(body.optInt("id", -1), -32601, "UNAUTHORIZED_TOOL"));
                return false;
            }
        }
        // Bounded concurrency: refuse rather than queue without limit.
        if (inFlight.get() >= MAX_IN_FLIGHT) {
            Log.w(TAG, "refusing request: " + inFlight.get() + " in flight (max " + MAX_IN_FLIGHT + ")");
            safeResponse(corrId, 503, errorBody(body.optInt("id", -1), -32603, "too many in-flight requests"));
            return false;
        }

        inFlight.incrementAndGet();
        try {
            String resp = relayToLoopback(body.toString());
            JSONObject parsed;
            try {
                parsed = new JSONObject(resp);
            } catch (Exception e) {
                // Never echo a non-JSON body back to the coordinator.
                parsed = errorBody(body.optInt("id", -1), -32603, "local response was not JSON");
            }
            safeResponse(corrId, 200, parsed);
            return true;
        } finally {
            inFlight.decrementAndGet();
        }
    }

    /**
     * Performs the local MCP call. The URL is a CONSTANT - never built from the request - which is
     * what stops this becoming a generic proxy to arbitrary local services.
     */
    private String relayToLoopback(String body) {
        HttpURLConnection c = null;
        try {
            String token = Provisioner.readToken(ctx);     // in-process; never transmitted
            URL url = new URL("http://" + Provisioner.LOOPBACK + ":" + Provisioner.DEFAULT_PORT + "/mcp");
            c = (HttpURLConnection) url.openConnection();
            c.setRequestMethod("POST");
            c.setConnectTimeout(1000);
            c.setReadTimeout(REQUEST_TIMEOUT_MS);
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.setRequestProperty("Authorization", "Bearer " + token);
            byte[] b = body.getBytes("UTF-8");
            c.setFixedLengthStreamingMode(b.length);
            OutputStream os = c.getOutputStream();
            os.write(b);
            os.flush();
            os.close();
            int code = c.getResponseCode();
            InputStream is = (code >= 200 && code < 300) ? c.getInputStream() : c.getErrorStream();
            String text = is == null ? "" : readAll(is);
            if (text.length() == 0) return errorBody(-1, -32603, "empty response").toString();
            return text;
        } catch (Exception e) {
            Log.w(TAG, "loopback relay failed: " + e);
            return errorBody(-1, -32603, "local relay failed").toString();
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private void safeResponse(String corrId, int statusCode, JSONObject bodyObj) {
        try {
            JSONObject o = new JSONObject();
            o.put("type", "mcp_response");
            o.put("correlation_id", corrId);
            o.put("status_code", statusCode);
            o.put("body", bodyObj == null ? new JSONObject() : bodyObj);
            writeText(o.toString());
        } catch (Exception e) {
            Log.w(TAG, "could not send response: " + e);
        }
    }

    private static JSONObject errorBody(int id, int code, String msg) {
        try {
            JSONObject err = new JSONObject();
            err.put("code", code);
            err.put("message", msg);
            JSONObject o = new JSONObject();
            o.put("jsonrpc", "2.0");
            o.put("id", id);
            o.put("error", err);
            return o;
        } catch (Exception e) {
            return new JSONObject();
        }
    }

    private static boolean isAllowedMethod(String m) {
        for (String a : ALLOWED_METHODS) if (a.equals(m)) return true;
        return false;
    }

    private static boolean isAllowedTool(String t) {
        for (String a : ALLOWED_TOOLS) if (a.equals(t)) return true;
        return false;
    }

    // ====================================================================== heartbeat

    private void startHeartbeat() {
        stopHeartbeat();
        heartbeat = new Thread(new Runnable() {
            @Override public void run() {
                while (running && socket != null && !socket.isClosed()) {
                    try { Thread.sleep(HEARTBEAT_MS); } catch (InterruptedException e) { return; }
                    try {
                        JSONObject o = new JSONObject();
                        o.put("type", "heartbeat_ping");
                        o.put("timestamp", System.currentTimeMillis());
                        writeText(o.toString());
                        if (System.currentTimeMillis() - lastInboundMs > INACTIVE_TIMEOUT_MS) {
                            Log.w(TAG, "no inbound frame for " + INACTIVE_TIMEOUT_MS + " ms - dropping the session");
                            closeSocket(1001, "inactive");
                            return;
                        }
                    } catch (Exception e) {
                        return;
                    }
                }
            }
        }, "hg-bridge-hb");
        heartbeat.start();
    }

    private void stopHeartbeat() {
        Thread h = heartbeat;
        heartbeat = null;
        if (h != null) h.interrupt();
    }

    // ====================================================================== RFC 6455 framing

    private static String wsAccept(String key) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-1");
        byte[] d = md.digest((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").getBytes("UTF-8"));
        return java.util.Base64.getEncoder().encodeToString(d);
    }

    private static String readHttpLine(InputStream in) throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        while (b.size() < 1024) {
            int ch = in.read();
            if (ch < 0) return null;
            if (ch == '\n') {
                String s = new String(b.toByteArray(), "UTF-8");
                return s.endsWith("\r") ? s.substring(0, s.length() - 1) : s;
            }
            b.write(ch);
        }
        return null;
    }

    /** Client->server frames MUST be masked (RFC 6455 section 5.3). */
    private synchronized void writeFrame(int opcode, byte[] payload) throws Exception {
        OutputStream out = wsOut;
        if (out == null) throw new Exception("no socket");
        ByteArrayOutputStream f = new ByteArrayOutputStream();
        f.write(0x80 | (opcode & 0x0F));                 // FIN + opcode
        int len = payload.length;
        if (len < 126) {
            f.write(0x80 | len);
        } else if (len <= 0xFFFF) {
            f.write(0x80 | 126);
            f.write((len >>> 8) & 0xFF); f.write(len & 0xFF);
        } else {
            f.write(0x80 | 127);
            for (int i = 7; i >= 0; i--) f.write((int) (((long) len >>> (8 * i)) & 0xFF));
        }
        byte[] mask = new byte[4];
        new SecureRandom().nextBytes(mask);
        f.write(mask);
        byte[] masked = new byte[len];
        for (int i = 0; i < len; i++) masked[i] = (byte) (payload[i] ^ mask[i & 3]);
        f.write(masked);
        out.write(f.toByteArray());
        out.flush();
    }

    private void writeText(String s) throws Exception { writeFrame(0x1, s.getBytes("UTF-8")); }

    /** Reads one data frame as text, answering control frames. Returns null at end of stream. */
    private String readText() throws Exception {
        while (true) {
            InputStream in = socket.getInputStream();
            int b0 = in.read();
            if (b0 < 0) return null;
            int opcode = b0 & 0x0F;
            int b1 = in.read();
            if (b1 < 0) return null;
            boolean masked = (b1 & 0x80) != 0;
            long len = b1 & 0x7F;
            if (len == 126) {
                len = ((long) in.read() << 8) | in.read();
            } else if (len == 127) {
                len = 0;
                for (int i = 0; i < 8; i++) len = (len << 8) | in.read();
            }
            // Cap BEFORE allocating. Spec 3.2: over 64 KB tears the connection down.
            if (len > MAX_FRAME) {
                Log.w(TAG, "frame of " + len + " bytes exceeds the 64 KB cap - closing " + WS_CLOSE_PAYLOAD_TOO_LARGE);
                closeSocket(WS_CLOSE_PAYLOAD_TOO_LARGE, "payload too large");
                return null;
            }
            byte[] mask = null;
            if (masked) {
                mask = new byte[4];
                readFully(in, mask);
            }
            byte[] payload = new byte[(int) len];
            readFully(in, payload);
            if (masked) for (int i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];

            if (opcode == 0x8) {                                  // close
                int code = payload.length >= 2 ? (((payload[0] & 0xFF) << 8) | (payload[1] & 0xFF)) : 1005;
                Log.i(TAG, "close frame received, code=" + code);
                if (code == WS_CLOSE_REVOKED) {
                    revoked = true;                               // halt reconnect (spec 3.4)
                    Log.e(TAG, "this device was REVOKED by the coordinator - no further reconnect");
                }
                return null;
            }
            if (opcode == 0x9) { writeFrame(0xA, payload); continue; }   // ping -> pong
            if (opcode == 0xA) continue;                                 // pong
            if (opcode == 0x1) return new String(payload, "UTF-8");
            // binary (0x2) and continuation are not used by this protocol
            Log.w(TAG, "ignoring unsupported opcode " + opcode);
        }
    }

    private static void readFully(InputStream in, byte[] buf) throws Exception {
        int off = 0;
        while (off < buf.length) {
            int r = in.read(buf, off, buf.length - off);
            if (r < 0) throw new Exception("stream closed mid-frame");
            off += r;
        }
    }

    private void closeSocket(int code, String reason) {
        try {
            Socket s = socket;
            if (s != null && !s.isClosed()) {
                try {
                    byte[] p = new byte[2 + reason.length()];
                    p[0] = (byte) (code >>> 8);
                    p[1] = (byte) code;
                    System.arraycopy(reason.getBytes("UTF-8"), 0, p, 2, reason.length());
                    writeFrame(0x8, p);
                } catch (Exception ignored) { }
                s.close();
            }
        } catch (Exception ignored) { }
        socket = null;
        wsOut = null;
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
        try { f.setReadable(false, false); } catch (Exception ignored) { }
        try { f.setReadable(true, true); } catch (Exception ignored) { }
    }

    private static String readAll(File f) throws Exception {
        FileInputStream in = new FileInputStream(f);
        String s = readAll(in);
        in.close();
        return s;
    }

    private static String readAll(InputStream in) throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return new String(b.toByteArray(), "UTF-8");
    }
}
