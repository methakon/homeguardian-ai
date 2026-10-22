"""Execute actual UI action bodies with deterministic Android stand-ins.

Not instrumentation: policy decisions are covered separately by RouteSecurityPolicyTest.
No device, sockets, persistent credentials, or enrollment values are used here.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path.home() / ".hermes/cache/scratch/hg-phone-bridge-update"
JDK = Path.home() / "android-jdk/bin"


def action(source, name):
    match = re.search(r"(?:private|protected) void " + name + r"\(\)\s*\{", source)
    start, end, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class UiActionBehavior(unittest.TestCase):
    def test_action_bodies(self):
        source = (ROOT / "apk_service/src/org/homeguardian/edgeservice/MainActivity.java").read_text()
        methods = "\n".join(action(source, n) for n in
                            ("onBridgeStart", "onBridgeEnrollment", "dismissEnrollment", "onPause"))
        java = r'''
import java.util.*;
class Base { protected void onPause() {} }
public class UiActions extends Base {
    static final int MODE_PRIVATE = 0;
    AlertDialog enrollmentDialog;
    Handler ui = new Handler();
    Runnable ticker = new Runnable() { public void run() {} };
    Runnable enrollmentTimeout = new Runnable() { public void run() { dismissEnrollment(); } };
    Field endpointField = new Field();
    int started;
    Intent sent;
    Prefs getSharedPreferences(String n, int mode) { return new Prefs(); }
    void startService(Intent i) { started++; sent = i; }
    void render() {}
    static void check(boolean ok, String label) {
        if (!ok) throw new AssertionError(label);
    }
    static class Field {
        String text = " ws://203.0.113.1:9/ ";
        String getText() { return text; }
    }
    static class Prefs {
        Prefs edit() { return this; }
        Prefs putString(String k, String v) { return this; }
        void apply() {}
    }
    static class Intent {
        String action, endpoint;
        Intent(Object ctx, Class<?> cls) {}
        void setAction(String a) { action = a; }
        void putExtra(String k, String v) { endpoint = v; }
    }
    static class EdgeService { static String ACTION_BRIDGE_START = "start", EXTRA_BRIDGE_URL = "url"; }
    static class EdgeState {
        static boolean bridgeRunning;
        static String bridgeStatus, bridgeEndpoint;
        static void notifyChanged() {}
    }
    static class RouteSecurityPolicy {
        static boolean wired;
        static String refusal;
        static void wireAndroidPlatform(Object ctx) { wired = true; }
        static String refusalReason(String url) {
            check(wired, "platform must be wired first"); return refusal;
        }
    }
    static class OutboundMcpBridge {
        static boolean fail;
        static String deviceId(Object ctx) throws Exception {
            if (fail) throw new Exception("must not reach status");
            return "synthetic-device";
        }
        static String authToken(Object ctx) { return "synthetic-enrollment"; }
    }
    static class JSONObject {
        Map<String, String> fields = new LinkedHashMap<String, String>();
        void put(String k, String v) { fields.put(k, v); }
        public String toString() {
            StringJoiner j = new StringJoiner(",", "{", "}");
            for (String k : fields.keySet()) j.add("\""+k+"\":\""+fields.get(k)+"\"");
            return j.toString();
        }
    }
    interface DialogInterface {
        interface OnDismissListener { void onDismiss(DialogInterface dialog); }
    }
    static class WindowManager { static class LayoutParams { static final int FLAG_SECURE = 8192; } }
    static class Window {
        int flags;
        void addFlags(int f) { flags |= f; }
    }
    static class AlertDialog implements DialogInterface {
        String message;
        boolean showing, cancelable;
        Window window = new Window();
        DialogInterface.OnDismissListener listener;
        static class Builder {
            AlertDialog d = new AlertDialog();
            Builder(Object ctx) {}
            Builder setTitle(String t) { return this; }
            Builder setMessage(String m) { d.message = m; return this; }
            Builder setPositiveButton(String t, Object l) { return this; }
            Builder setCancelable(boolean c) { d.cancelable = c; return this; }
            AlertDialog create() { return d; }
        }
        void setMessage(String m) { message = m; }
        void setOnDismissListener(DialogInterface.OnDismissListener l) { listener = l; }
        Window getWindow() { return window; }
        void show() { check(window.flags == 8192, "secure flag before show"); showing = true; }
        // Android can dispatch OnDismiss later: tests explicitly deliver the callback.
        void dismiss() { showing = false; }
        void deliverDismiss() { listener.onDismiss(this); }
    }
    static class Handler {
        Runnable pending;
        long delay;
        void postDelayed(Runnable r, long d) { pending = r; delay = d; }
        void removeCallbacks(Runnable r) { if (pending == r) pending = null; }
    }
''' + methods + r'''
    public static void main(String[] args) {
        UiActions a = new UiActions();
        RouteSecurityPolicy.refusal = "refused: synthetic no VPN";
        a.onBridgeStart();
        check(a.started == 0 && !EdgeState.bridgeRunning, "refusal cannot start service");
        System.out.println("PASS refused route never starts service");
        RouteSecurityPolicy.refusal = null;
        a.onBridgeStart();
        check(a.started == 1 && "ws://203.0.113.1:9/".equals(a.sent.endpoint), "allowed route passes trimmed URL");
        System.out.println("PASS allowed route dispatches bridge action");
        a.onBridgeEnrollment();
        AlertDialog first = a.enrollmentDialog;
        check(first.showing && first.cancelable, "explicit dismissable display");
        check(first.message.equals("{\"device_id\":\"synthetic-device\",\"auth_token\":\"synthetic-enrollment\"}"), "exact enrollment schema");
        check(a.ui.delay == 30000L, "30 second lifetime");
        System.out.println("PASS secure dialog with exact two-field JSON");
        a.ui.pending.run();
        check(a.enrollmentDialog == null && !first.showing && first.message.isEmpty() && a.ui.pending == null, "timeout wipes dialog");
        System.out.println("PASS expiry dismisses and clears message");
        a.onBridgeEnrollment();
        AlertDialog second = a.enrollmentDialog;
        first.deliverDismiss();
        check(a.enrollmentDialog == second && second.showing && a.ui.pending != null, "old callback cannot clear new dialog");
        System.out.println("PASS stale dismiss preserves new dialog and timer");
        a.onPause();
        check(a.enrollmentDialog == null && !second.showing && second.message.isEmpty() && a.ui.pending == null, "pause wipes dialog");
        System.out.println("PASS backgrounding dismisses enrollment");
        a.onBridgeEnrollment();
        AlertDialog third = a.enrollmentDialog;
        third.dismiss(); third.deliverDismiss();
        check(a.enrollmentDialog == null && third.message.isEmpty() && a.ui.pending == null, "operator dismissal wipes dialog");
        System.out.println("PASS operator dismissal clears enrollment");
        OutboundMcpBridge.fail = true;
        a.onBridgeEnrollment();
        check(a.enrollmentDialog == null && "bridge: enrollment unavailable".equals(EdgeState.bridgeStatus), "generic error only");
        System.out.println("PASS enrollment failure reveals no exception details");
        System.out.println("UI actions: 8 passed, 0 failed");
    }
}
'''
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="ui-actions-", dir=SCRATCH) as temp:
            work = Path(temp)
            (work / "UiActions.java").write_text(java)
            subprocess.run([str(JDK / "javac"), "--release", "8", "-d", temp,
                            str(work / "UiActions.java")], check=True)
            result = subprocess.run([str(JDK / "java"), "-cp", temp, "UiActions"],
                                    check=True, capture_output=True, text=True)
            print(result.stdout, end="")
            self.assertEqual(len(re.findall(r"^PASS ", result.stdout, re.M)), 8)


if __name__ == "__main__":
    unittest.main()
