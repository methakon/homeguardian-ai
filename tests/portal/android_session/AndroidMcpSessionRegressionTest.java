import android.content.Context;
import com.sun.net.httpserver.HttpServer;
import org.homeguardian.edgeservice.OutboundMcpBridge;
import org.json.JSONObject;
import java.io.*;
import java.lang.reflect.*;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;

/** Actual production relay via reflection, real HTTP socket and actual outbound WS serialization. */
public class AndroidMcpSessionRegressionTest {
    static OutboundMcpBridge bridge = new OutboundMcpBridge(new Context());
    static Field session, ws;
    static Method relay, handle;
    static int status = 200, calls, passed, failed;
    static String response = "{}", responseSid, redirect, seenSid, seenBody, seenAuth, seenPath, seenAccept;
    static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    interface Case { void run() throws Exception; }
    static void test(String name, Case body) {
        try { session.set(bridge, "old-native-session"); body.run(); passed++; System.out.println("PASS " + name); }
        catch (Throwable t) { failed++; System.out.println("FAIL " + name + ": " + t); }
    }
    static String request(String method, String id) {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"method\":\"" + method
            + "\",\"params\":{\"name\":\"get_homeguardian_status\",\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"host\",\"version\":\"1\"}}}";
    }
    static JSONObject send(String body) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream(); ws.set(bridge, bytes);
        JSONObject message = new JSONObject().put("correlation_id", "stable-correlation").put("body", new JSONObject(body));
        handle.invoke(bridge, message);
        byte[] frame = bytes.toByteArray();
        check(frame.length >= 6, "missing response frame");
        int len = frame[1] & 127, pos = 2;
        if (len == 126) { len = ((frame[pos] & 255) << 8) | (frame[pos + 1] & 255); pos += 2; }
        byte[] mask = new byte[4]; System.arraycopy(frame, pos, mask, 0, 4); pos += 4;
        byte[] text = new byte[len];
        for (int i = 0; i < len; i++) text[i] = (byte)(frame[pos + i] ^ mask[i & 3]);
        JSONObject out = new JSONObject(new String(text, StandardCharsets.UTF_8));
        check("stable-correlation".equals(out.optString("correlation_id")), "correlation changed");
        return out;
    }
    static void reply(int code, String text, String sid) { status = code; response = text; responseSid = sid; }
    static void assertTransport() {
        check("Bearer host-test-bearer".equals(seenAuth), "bearer removed or changed");
        check("/mcp".equals(seenPath), "loopback target/path changed");
        check("application/json, text/event-stream".equals(seenAccept), "Accept changed");
    }
    public static void main(String[] args) throws Exception {
        session = OutboundMcpBridge.class.getDeclaredField("mcpSessionId"); session.setAccessible(true);
        ws = OutboundMcpBridge.class.getDeclaredField("wsOut"); ws.setAccessible(true);
        relay = OutboundMcpBridge.class.getDeclaredMethod("relayToLoopback", String.class); relay.setAccessible(true);
        handle = OutboundMcpBridge.class.getDeclaredMethod("handleMcpRequest", JSONObject.class); handle.setAccessible(true);
        // Bind ONLY the actual fixed production loopback target. Busy port is an error, never skipped.
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 8790), 0);
        server.createContext("/", exchange -> {
            calls++;
            seenSid = exchange.getRequestHeaders().getFirst("Mcp-Session-Id");
            seenAuth = exchange.getRequestHeaders().getFirst("Authorization");
            seenAccept = exchange.getRequestHeaders().getFirst("Accept");
            seenPath = exchange.getRequestURI().toString();
            seenBody = new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
            if (responseSid != null) exchange.getResponseHeaders().set("Mcp-Session-Id", responseSid);
            if (redirect != null) exchange.getResponseHeaders().set("Location", redirect);
            byte[] bytes = response.getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(status, bytes.length);
            exchange.getResponseBody().write(bytes); exchange.close();
        });
        server.start();
        try {
            test("initialize omits stale sid and tools/call uses fresh sid", () -> {
                reply(200, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}", "fresh-native-session");
                relay.invoke(bridge, request("initialize", "1"));
                check(seenSid == null, "initialize sent stale session header"); assertTransport();
                check("fresh-native-session".equals(session.get(bridge)), "fresh sid not cached");
                reply(200, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}", null);
                relay.invoke(bridge, request("tools/call", "2"));
                check("fresh-native-session".equals(seenSid), "tools/call lost fresh sid"); assertTransport();
            });
            for (int code : new int[]{404, 401, 403, 500}) {
                test("HTTP " + code + " is not fake success or replay", () -> {
                    reply(code, "{}", "untrusted-error-sid"); int before = calls;
                    JSONObject out = send(request("tools/call", "\"request-\\\"id\""));
                    check(calls == before + 1, "request replayed"); assertTransport();
                    check(out.optInt("status_code", 0) == code, "HTTP error relayed as 200");
                    JSONObject b = out.optJSONObject("body");
                    check(b.optJSONObject("error") != null && !b.has("result"), "missing JSON-RPC error");
                    check("request-\"id".equals(b.optString("id")), "request id changed");
                    check(code == 500 ? "old-native-session".equals(session.get(bridge)) : session.get(bridge) == null,
                        "session invalidation wrong");
                });
            }
            test("404 then explicit initialize recovers without replay", () -> {
                reply(404, "{}", null); int before = calls;
                send(request("tools/call", "42"));
                check(session.get(bridge) == null && calls == before + 1, "404 not invalidated exactly once");
                reply(200, "{\"jsonrpc\":\"2.0\",\"id\":43,\"result\":{}}", "restart-session");
                send(request("initialize", "43"));
                check(seenSid == null, "restart initialize carried a session");
                reply(200, "{\"jsonrpc\":\"2.0\",\"id\":44,\"result\":{}}", null);
                String body = request("tools/call", "44"); send(body);
                check("restart-session".equals(seenSid), "restart session not reused");
                check(new JSONObject(body).toString().equals(seenBody), "request body changed"); assertTransport();
            });
            test("redirect cannot change loopback target or replay bearer", () -> {
                reply(307, "{}", null); redirect = "http://127.0.0.1:8790/other-target";
                int before = calls;
                try {
                    JSONObject out = send(request("tools/call", "45"));
                    check(calls == before + 1, "redirect replayed"); assertTransport();
                    check(out.optInt("status_code", 0) == 307, "redirect status hidden");
                    check("old-native-session".equals(session.get(bridge)), "redirect reset grant");
                } finally { redirect = null; }
            });
            test("native JSON-RPC error and numeric id preserved", () -> {
                String nativeError = "{\"jsonrpc\":\"2.0\",\"id\":77,\"error\":{\"code\":-32602,\"message\":\"invalid arguments\"}}";
                reply(400, nativeError, null);
                JSONObject out = send(request("tools/call", "77"));
                check(out.optInt("status_code", 0) == 400, "lost HTTP status");
                check(new JSONObject(nativeError).toString().equals(out.optJSONObject("body").toString()), "native error changed");
                check("old-native-session".equals(session.get(bridge)), "400 reset session"); assertTransport();
            });
            test("malformed input never clears grant", () -> {
                reply(400, "{}", null);
                relay.invoke(bridge, "not JSON");
                check("old-native-session".equals(session.get(bridge)), "malformed input reset session");
            });
            test("failed initialize does not adopt response header", () -> {
                reply(400, "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"bad params\"}}", "bad-session");
                send("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
                check("old-native-session".equals(session.get(bridge)), "invalid initialize changed grant"); assertTransport();
            });
            test("noninitialize body text cannot reset or replace grant", () -> {
                reply(200, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"text\":\"initialize must be called first\"}}", "bad-session");
                send(request("tools/call", "2"));
                check("old-native-session".equals(session.get(bridge)), "tool content changed grant"); assertTransport();
            });
            test("mutation remains refused without HTTP or session changes", () -> {
                int before = calls;
                JSONObject out = send("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"set_capture_enabled\"}}");
                check(calls == before && out.optInt("status_code", 0) == 400, "mutation escaped gate");
                check("old-native-session".equals(session.get(bridge)), "refusal changed session");
            });
            test("JSON-looking strings stay strings in host serialization", () -> {
                JSONObject value = new JSONObject().put("text", "{}").put("number", 3).put("nested", new JSONObject().put("ok", true));
                check(value.toString().contains("\"text\":\"{}\""), "JSON stub string fidelity");
                check(value.toString().contains("\"number\":3"), "JSON stub number fidelity");
                check(new JSONObject(value.toString()).optJSONObject("nested") != null, "JSON stub nested fidelity");
            });
        } finally { server.stop(0); }
        System.out.println(passed + " passed, " + failed + " failed");
        if (failed != 0) throw new AssertionError("session regression failures");
    }
}
