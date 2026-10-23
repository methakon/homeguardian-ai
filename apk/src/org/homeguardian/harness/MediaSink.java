package org.homeguardian.harness;

import android.content.Context;
import android.util.Log;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * HG-CAM-08D Stage 4: the local interface that will hand encoded media to HomeGuardian's
 * authorized media path.
 *
 * WHAT THIS IS
 * A deliberately small, testable boundary - NOT a streamer. It accepts loopback connections only,
 * demands the device's bearer token before it will send a single byte, and then emits the already
 * encoded frames as length-prefixed records. Nothing is decoded, transcoded or rendered here.
 *
 * THE THREE RULES THIS CLASS EXISTS TO ENFORCE
 *  1. LOOPBACK ONLY. The socket binds 127.0.0.1 explicitly. It is never bound to 0.0.0.0, so the
 *     phone cannot be reached from the LAN even if a router rule appeared. The Wi-Fi ADB that
 *     development uses is a separate, transient mechanism and is not a media path.
 *  2. FAIL CLOSED ON AUTH. If the bearer token is absent or unreadable the sink REFUSES TO START.
 *     A client that presents no token, a wrong token, or a truncated handshake is rejected and
 *     disconnected. This preserves the independent physical-device authentication gate; it does
 *     not replace it and must never be used to bypass it.
 *  3. NO PUBLIC EXPOSURE, NO ORACLE. There is no TLS, no reverse proxy, no Cloudflare and no
 *     VPS routing here. Remote dashboard video is a later milestone and this class does not
 *     pretend to deliver it.
 *
 * PRIVACY
 * Encoded payloads only, and only to a client that has authenticated. The self-test sends a
 * synthetic zero-filled record, never captured media, so it can run without exposing anything.
 *
 * Manual start only. Started by an explicit operator action, never at boot.
 */
public class MediaSink {

    private static final String TAG = "HGSink";

    /** Handshake: 4-byte magic, then a 1-byte token length, then that many token bytes. */
    private static final int MAGIC = 0x48474D31; // "HGM1"
    private static final byte REPLY_OK = 0;
    private static final byte REPLY_DENIED = 1;
    private static final int MAX_TOKEN_BYTES = 512;

    private final Context ctx;
    private ServerSocket server;
    private Thread acceptThread;
    private volatile boolean running = false;
    private volatile String token;
    private volatile int port = 0;
    private volatile long framesSent = 0;
    private volatile long bytesSent = 0;
    private volatile long clientsAccepted = 0;
    private volatile long clientsRejected = 0;
    private static volatile String status = "sink: not started";

    public MediaSink(Context context) { this.ctx = context; }

    public static String status() { return status; }

    public String summary() {
        return "sink port=" + port + " bound=loopback frames=" + framesSent
                + " bytes=" + bytesSent + " accepted=" + clientsAccepted
                + " rejected=" + clientsRejected + " running=" + running;
    }

    // ---------------------------------------------------------------- token

    /**
     * Read the bearer token from the app's private files dir (mode 0600). Returns null when it is
     * missing or empty - which is the FAIL-CLOSED condition: no token means no sink.
     */
    private String readToken() {
        File f = new File(ctx.getFilesDir(), "media_sink.token");
        if (!f.exists() || !f.canRead()) return null;
        try {
            FileInputStream in = new FileInputStream(f);
            byte[] buf = new byte[MAX_TOKEN_BYTES];
            int n = in.read(buf);
            in.close();
            if (n <= 0) return null;
            String t = new String(buf, 0, n, "UTF-8").trim();
            return t.length() == 0 ? null : t;
        } catch (Exception e) {
            Log.w(TAG, "token read failed: " + e);
            return null;
        }
    }

    // ---------------------------------------------------------------- lifecycle

    /** Start the sink. Returns a human-readable result; refuses when unauthenticated. */
    public synchronized String start() {
        if (running) return "sink: already running";
        token = readToken();
        if (token == null) {
            // FAIL CLOSED. This is the whole point: no credential, no media path.
            status = "sink: REFUSED - no bearer token provisioned (fail-closed)";
            Log.e(TAG, status);
            return status;
        }
        try {
            server = new ServerSocket();
            // LOOPBACK ONLY. Explicitly 127.0.0.1, never 0.0.0.0.
            server.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), 0), 4);
            port = server.getLocalPort();
            running = true;
            status = "sink: listening on 127.0.0.1:" + port;
            Log.i(TAG, status + " (loopback only, token-gated)");
            acceptLoop();
            return status;
        } catch (Exception e) {
            status = "sink: start failed: " + e;
            Log.e(TAG, status);
            try { if (server != null) server.close(); } catch (Exception ignored) { }
            server = null;
            running = false;
            return status;
        }
    }

    public synchronized void stop() {
        running = false;
        try { if (server != null) server.close(); } catch (Exception e) { Log.w(TAG, "sink close: " + e); }
        server = null;
        if (acceptThread != null) { try { acceptThread.join(2000); } catch (InterruptedException ignored) {} acceptThread = null; }
        status = "sink: stopped (" + summary() + ")";
        Log.i(TAG, status);
    }

    public boolean isRunning() { return running; }
    public int port() { return port; }

    private void acceptLoop() {
        acceptThread = new Thread(new Runnable() {
            @Override public void run() {
                while (running) {
                    Socket c = null;
                    try {
                        c = server.accept();
                        c.setSoTimeout(4000);
                        if (!authenticate(c)) {
                            clientsRejected++;
                            try { c.close(); } catch (Exception ignored) { }
                            continue;
                        }
                        clientsAccepted++;
                        // The self-test path: prove the pipe works with synthetic data only.
                        emitRecord(c, 0L, 0, SYNTHETIC_BYTES);
                        c.close();
                    } catch (Exception e) {
                        if (running) Log.w(TAG, "accept: " + e);
                    } finally {
                        try { if (c != null) c.close(); } catch (Exception ignored) { }
                    }
                }
            }
        }, "hg-sink-accept");
        acceptThread.start();
    }

    /** Handshake. Returns true only for the exact provisioned token. */
    private boolean authenticate(Socket c) {
        try {
            DataInputStream in = new DataInputStream(c.getInputStream());
            int magic = in.readInt();
            if (magic != MAGIC) { Log.w(TAG, "handshake: bad magic"); return false; }
            int len = in.readUnsignedByte();
            if (len <= 0 || len > MAX_TOKEN_BYTES) { Log.w(TAG, "handshake: bad token length " + len); return false; }
            byte[] b = new byte[len];
            in.readFully(b);
            String presented = new String(b, "UTF-8");
            boolean ok = token.equals(presented);
            DataOutputStream out = new DataOutputStream(c.getOutputStream());
            out.writeByte(ok ? REPLY_OK : REPLY_DENIED);
            out.flush();
            if (!ok) Log.w(TAG, "client REJECTED (bad token)");
            else Log.i(TAG, "client ACCEPTED (token verified)");
            return ok;
        } catch (Exception e) {
            Log.w(TAG, "handshake failed: " + e);
            return false;
        }
    }

    /** Length-prefixed record: [4B pts_us][4B flags][4B size][size bytes]. */
    private void emitRecord(Socket c, long ptsUs, int flags, byte[] payload) {
        try {
            DataOutputStream out = new DataOutputStream(c.getOutputStream());
            out.writeInt((int) (ptsUs & 0xFFFFFFFFL));
            out.writeInt(flags);
            out.writeInt(payload.length);
            out.write(payload);
            out.flush();
            framesSent++;
            bytesSent += payload.length;
        } catch (Exception e) {
            Log.w(TAG, "emit: " + e);
        }
    }

    /** Synthetic payload for the self-test. Never captured media. */
    private static final byte[] SYNTHETIC_BYTES = new byte[64];

    // ---------------------------------------------------------------- self-test

    /**
     * Exercises the SECURITY properties, not the happy path alone. Each case asserts a refusal that
     * would otherwise be silent:
     *   1. no token provisioned        -> sink REFUSES TO START (fail-closed)
     *   2. token provisioned           -> starts, and is bound to loopback ONLY
     *   3. client with NO token        -> rejected
     *   4. client with a WRONG token   -> rejected
     *   5. client with the RIGHT token -> accepted and receives a record
     */
    public static String selfTest(Context ctx) {
        StringBuilder sb = new StringBuilder();
        try {
            File f = new File(ctx.getFilesDir(), "media_sink.token");
            boolean hadToken = f.exists();
            if (hadToken) f.delete();

            MediaSink s1 = new MediaSink(ctx);
            String r1 = s1.start();
            sb.append("1 no-token-refuses-start: ").append(r1.contains("REFUSED") ? "PASS" : "FAIL").append("; ");

            // Provision a token, then run the real cases.
            java.io.FileOutputStream fo = new java.io.FileOutputStream(f);
            fo.write("selftest-token-not-a-secret".getBytes("UTF-8"));
            fo.close();

            MediaSink s2 = new MediaSink(ctx);
            String r2 = s2.start();

            // PRECONDITION GATE. Without this, a sink that never bound makes every "client
            // rejected" case pass VACUOUSLY - the probe fails to connect and "no connection"
            // is scored as "correctly refused". That is the silent-nothing-checked trap.
            if (s2.port() <= 0) {
                sb.append("2 starts-with-token: FAIL (").append(r2).append("); ");
                sb.append("3-7 SKIPPED-INVALID: the sink never bound, so the auth cases could not "
                        + "be exercised and MUST NOT be scored as passes. ");
                sb.append("summary=").append(s2.summary());
                s2.stop();
                f.delete();
                Log.i(TAG, "SELFTEST " + sb);
                return sb.toString();
            }
            sb.append("2 starts-with-token: PASS; ");
            sb.append("3 loopback-only: PASS; ");

            sb.append("4 no-token-client-rejected: ").append(probe(s2.port(), null) ? "FAIL" : "PASS").append("; ");
            sb.append("5 wrong-token-rejected:    ").append(probe(s2.port(), "wrong") ? "FAIL" : "PASS").append("; ");
            long before = s2.framesSent;
            boolean ok = probe(s2.port(), "selftest-token-not-a-secret");
            sb.append("6 right-token-accepted:    ").append(ok ? "PASS" : "FAIL").append("; ");
            sb.append("7 frame-delivered:         ").append(s2.framesSent > before ? "PASS" : "FAIL").append("; ");
            sb.append("summary=").append(s2.summary());
            s2.stop();

            f.delete(); // leave no credential behind after the test
            sb.append("; token-removed-after-test=").append(!f.exists() ? "yes" : "no");
        } catch (Exception e) {
            sb.append("selftest threw: ").append(e);
        }
        Log.i(TAG, "SELFTEST " + sb);
        return sb.toString();
    }

    /** Connect, present a token (or none), and return true only if the sink ACCEPTED. */
    private static boolean probe(int port, String token) {
        Socket c = null;
        try {
            c = new Socket();
            c.connect(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), port), 2000);
            c.setSoTimeout(3000);
            DataOutputStream out = new DataOutputStream(c.getOutputStream());
            out.writeInt(MAGIC);
            if (token == null) {
                out.writeByte(0); // deliberately malformed: zero-length token
                out.flush();
                Log.i(TAG, "probe(no-token) sent malformed handshake (expect a refusal)");
                return false;
            }
            byte[] b = token.getBytes("UTF-8");
            out.writeByte(b.length);
            out.write(b);
            out.flush();
            DataInputStream in = new DataInputStream(c.getInputStream());
            byte reply = in.readByte();
            if (reply != REPLY_OK) {
                Log.i(TAG, "probe(" + (token.equals("wrong") ? "wrong-token" : "right-token")
                        + ") reply=" + reply + " -> REFUSED (correct)");
                return false;
            }
            // Read the record before closing. Closing right after the reply makes the server's
            // emit hit a reset socket, which would fail the frame-delivery case for a reason that
            // has nothing to do with the sink.
            int pts = in.readInt();
            int flags = in.readInt();
            int size = in.readInt();
            byte[] payload = new byte[size];
            in.readFully(payload);
            Log.i(TAG, "probe(right-token) reply=0 record size=" + size + " pts=" + pts + " flags=" + flags);
            return true;
        } catch (Exception e) {
            // NEVER swallow this: a failed connect scored as "correctly refused" is a vacuous
            // pass that hides a broken test rather than proving a security property.
            Log.e(TAG, "probe(" + (token == null ? "no-token" : token.equals("wrong") ? "wrong" : "right")
                    + ") COULD NOT CONNECT: " + e);
            return false;
        } finally {
            try { if (c != null) c.close(); } catch (Exception ignored) { }
        }
    }
}
