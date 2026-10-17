package org.homeguardian.edgeservice;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * In-process MCP health probe over loopback.
 *
 * This is a REAL MCP client: it performs initialize (capturing Mcp-Session-Id),
 * notifications/initialized, tools/list and a tools/call, exactly as an external
 * client would. It runs inside the app process, which is what makes "MCP is
 * available with no ADB attached" provable: the device answers its own loopback
 * call, and each attempt is timestamped into a log the host can read later.
 *
 * The bearer token is read from the app's own private credential file, so it is
 * never displayed, never logged and never leaves the sandbox.
 */
public final class McpProbe {

    public static final String TAG = "HGEdge";

    public static final class Result {
        public final boolean ok;
        public final String detail;
        public final long latencyMs;
        public final int httpCode;

        Result(boolean ok, String detail, long latencyMs, int httpCode) {
            this.ok = ok;
            this.detail = detail;
            this.latencyMs = latencyMs;
            this.httpCode = httpCode;
        }
    }

    private McpProbe() {}

    public static Result run(Context ctx, int port) {
        long t0 = System.currentTimeMillis();
        try {
            String token = Provisioner.readToken(ctx);
            if (token == null) return fail("no credential provisioned", t0, 0);

            String base = "http://" + Provisioner.LOOPBACK + ":" + port + "/mcp";

            // 1. initialize
            String initBody = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                    + "\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},"
                    + "\"clientInfo\":{\"name\":\"homeguardian-edge-service\",\"version\":\"1.0.0\"}}}";
            Http http = post(base, initBody, token, null);
            if (http.code != 200) return fail("initialize -> HTTP " + http.code, t0, http.code);
            String session = http.sessionId;
            if (session == null || session.length() == 0) {
                return fail("initialize returned no Mcp-Session-Id", t0, 200);
            }
            String proto = extract(initBody.length() > 0 ? http.body : "", "protocolVersion");

            // 2. notifications/initialized (no id => notification; expect 202)
            post(base, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", token, session);

            // 3. tools/list
            Http list = post(base, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", token, session);
            if (list.code != 200) return fail("tools/list -> HTTP " + list.code, t0, list.code);
            String toolNames = extractTools(list.body);

            // 4. tools/call on the read-only status tool
            Http call = post(base,
                    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                            + "\"params\":{\"name\":\"get_homeguardian_status\",\"arguments\":{}}}",
                    token, session);
            if (call.code != 200) return fail("tools/call -> HTTP " + call.code, t0, call.code);

            long ms = System.currentTimeMillis() - t0;
            return new Result(true,
                    "proto=" + proto + " tools=" + toolNames + " status=" + bodyText(call.body), ms, 200);
        } catch (Throwable t) {
            return fail(t.getClass().getSimpleName() + ": " + t.getMessage(), t0, 0);
        }
    }

    private static Result fail(String detail, long t0, int code) {
        return new Result(false, detail, System.currentTimeMillis() - t0, code);
    }

    // ---------------------------------------------------------------- HTTP ---

    private static final class Http {
        int code;
        String body = "";
        String sessionId;
    }

    private static Http post(String url, String body, String token, String session) throws Exception {
        HttpURLConnection c = null;
        Http out = new Http();
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod("POST");
            c.setConnectTimeout(3000);
            c.setReadTimeout(8000);
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.setRequestProperty("Accept", "application/json, text/event-stream");
            c.setRequestProperty("Authorization", "Bearer " + token);
            if (session != null) c.setRequestProperty("Mcp-Session-Id", session);
            byte[] b = body.getBytes("UTF-8");
            c.setFixedLengthStreamingMode(b.length);
            java.io.OutputStream os = c.getOutputStream();
            try {
                os.write(b);
                os.flush();
            } finally {
                os.close();
            }
            out.code = c.getResponseCode();
            out.sessionId = c.getHeaderField("Mcp-Session-Id");
            out.body = readStream(out.code >= 400 ? c.getErrorStream() : c.getInputStream());
            return out;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static String readStream(InputStream in) throws Exception {
        if (in == null) return "";
        BufferedReader r = new BufferedReader(new InputStreamReader(in, "UTF-8"));
        StringBuilder sb = new StringBuilder();
        String line;
        while ((line = r.readLine()) != null) sb.append(line);
        r.close();
        return sb.toString();
    }

    // ------------------------------------------------------------- parsing ---

    private static String extract(String json, String key) {
        try {
            return new JSONObject(json).optJSONObject("result").optString(key, "?");
        } catch (Throwable t) {
            return "?";
        }
    }

    private static String extractTools(String json) {
        try {
            org.json.JSONArray a = new JSONObject(json).getJSONObject("result").getJSONArray("tools");
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < a.length(); i++) {
                if (i > 0) sb.append(",");
                sb.append(a.getJSONObject(i).optString("name"));
            }
            return sb.length() == 0 ? "none" : sb.toString();
        } catch (Throwable t) {
            return "?";
        }
    }

    /** Pull the tool's text payload out of the MCP content envelope. */
    private static String bodyText(String json) {
        try {
            return new JSONObject(json).getJSONObject("result").getJSONArray("content")
                    .getJSONObject(0).optString("text", "?");
        } catch (Throwable t) {
            return "?";
        }
    }

    // ----------------------------------------------------------- probe log ---

    /**
     * Append one line to the probe log. Two copies of the same content are kept:
     *  - private : filesDir/probe.log
     *  - readable: getExternalFilesDir()/probe.log  (so the host can pull evidence
     *              with adb WITHOUT the app being debuggable and WITHOUT root;
     *              neither copy contains the bearer token or any other secret)
     */
    public static void appendLog(Context ctx, String line) {
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US).format(new Date());
        String row = stamp + " " + line + "\n";
        Log.i(TAG, "PROBE " + line);
        appendRaw(new File(ctx.getFilesDir(), "probe.log"), row, 0600);
        File ext = ctx.getExternalFilesDir(null);
        if (ext != null) {
            if (!ext.exists()) ext.mkdirs();
            appendRaw(new File(ext, "probe.log"), row, 0644);
        }
    }

    private static void appendRaw(File f, String row, int mode) {
        FileOutputStream os = null;
        try {
            os = new FileOutputStream(f, true);
            os.write(row.getBytes("UTF-8"));
            os.flush();
        } catch (Throwable t) {
            Log.w(TAG, "probe log append failed: " + t);
        } finally {
            if (os != null) try { os.close(); } catch (Throwable ignored) {}
        }
        try {
            android.system.Os.chmod(f.getAbsolutePath(), mode);
        } catch (Throwable ignored) {}
    }

    /** Truncate both probe logs (operator action, to start a clean measurement window). */
    public static void clearLog(Context ctx) {
        try { new FileOutputStream(new File(ctx.getFilesDir(), "probe.log"), false).close(); }
        catch (Throwable ignored) {}
        File ext = ctx.getExternalFilesDir(null);
        if (ext != null) {
            try { new FileOutputStream(new File(ext, "probe.log"), false).close(); }
            catch (Throwable ignored) {}
        }
    }
}
