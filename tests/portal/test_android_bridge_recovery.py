"""Host tests for the bridge recovery policy and its wiring into the edge service.

The retry arithmetic lives in a plain Java class with no Android dependency, so it can be
compiled and exercised on a host JVM. The Android wiring is checked statically: a policy that
is never called from the watchdog is a comment, not a recovery mechanism.

Run: python3 tests/portal/test_android_bridge_recovery.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SERVICE = ROOT / 'apk_service/src/org/homeguardian/edgeservice'

HARNESS = r'''
import org.homeguardian.edgeservice.BridgeSupervisor;

public final class Harness {
    static int fails = 0;

    static void ck(boolean condition, String what) {
        if (condition) System.out.println("  ok    " + what);
        else { fails++; System.out.println("  FAIL  " + what); }
    }

    public static void main(String[] args) {
        BridgeSupervisor s = new BridgeSupervisor();
        long[] expected = {5000L, 15000L, 45000L, 120000L, 300000L};
        for (int i = 0; i < expected.length; i++) {
            ck(s.nextDelayMs() == expected[i], "delay " + i + " is " + expected[i]);
        }
        // The cap must repeat rather than grow without bound or wrap.
        ck(s.nextDelayMs() == 300000L, "cap repeats");
        ck(s.nextDelayMs() == 300000L, "cap keeps repeating");
        ck(s.attempts() == 7, "attempts counted");
        s.reset();
        ck(s.attempts() == 0, "reset clears attempts");
        ck(s.nextDelayMs() == 5000L, "reset returns to the first delay");

        ck(BridgeSupervisor.shouldAttempt(true, false, false), "wanted, stopped bridge, live service -> try");
        ck(!BridgeSupervisor.shouldAttempt(true, true, false), "already running -> no second session");
        ck(!BridgeSupervisor.shouldAttempt(false, false, false), "not wanted -> never dial");
        ck(!BridgeSupervisor.shouldAttempt(true, false, true), "service stopping -> never dial");
        ck(!BridgeSupervisor.shouldAttempt(false, true, true), "nothing wanted or live -> never dial");

        ck("vpn_unavailable".equals(BridgeSupervisor.classify(
                "bridge: refused: no VPN network is active, and 192.168.77.1 is not loopback")),
           "tunnel-down refusal classified");
        ck("vpn_unavailable".equals(BridgeSupervisor.classify("vpn tunnel DOWN")), "vpn down classified");
        ck("auth_refused".equals(BridgeSupervisor.classify("auth failed: 401")), "401 classified");
        ck("auth_refused".equals(BridgeSupervisor.classify("unauthorized")), "unauthorized classified");
        ck("timeout".equals(BridgeSupervisor.classify("read timed out")), "timeout classified");
        ck("not_configured".equals(BridgeSupervisor.classify("refused - no coordinator configured")),
           "unconfigured classified");
        ck("refused".equals(BridgeSupervisor.classify("connection refused")), "refusal classified");
        ck("unknown".equals(BridgeSupervisor.classify("")), "empty classified as unknown");
        ck("unknown".equals(BridgeSupervisor.classify(null)), "null classified as unknown");

        ck(BridgeSupervisor.validTunnelName("21"), "plain tunnel name accepted");
        ck(BridgeSupervisor.validTunnelName("home gurdian"), "name with a space accepted");
        ck(BridgeSupervisor.validTunnelName("wg-prod_1.0"), "hyphen, underscore, dot accepted");
        ck(!BridgeSupervisor.validTunnelName(""), "empty name refused");
        ck(!BridgeSupervisor.validTunnelName(null), "null name refused");
        ck(!BridgeSupervisor.validTunnelName("line\nbreak"), "newline refused");
        ck(!BridgeSupervisor.validTunnelName("x".repeat(65)), "over-long name refused");
        ck(!BridgeSupervisor.validTunnelName("../../../etc/passwd"), "path-like name refused");
        ck(!BridgeSupervisor.validTunnelName("name;rm -rf /"), "shell-like name refused");
        // A category must never echo the input: it is logged, and the input can carry an address.
        ck(!BridgeSupervisor.classify("refused: no VPN network is active, and 10.1.2.3 is not loopback")
                .contains("10.1.2.3"), "category does not echo the input");

        if (fails != 0) System.exit(1);
        System.out.println("ALL PASS");
    }
}
'''


def find_jdk():
    candidates = []
    if os.environ.get('JAVA_HOME'):
        candidates.append(Path(os.environ['JAVA_HOME']) / 'bin')
    candidates.append(Path.home() / 'android-jdk' / 'bin')
    for directory in candidates:
        if (directory / 'javac').is_file() and (directory / 'java').is_file():
            return directory
    javac = shutil.which('javac')
    java = shutil.which('java')
    if javac and java:
        return Path(javac).parent
    return None


class BridgeRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.jdk = find_jdk()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    def test_policy_behaviour_on_a_host_jvm(self):
        if self.jdk is None:
            self.skipTest('no JDK available; set JAVA_HOME or install one')
        source = self.path / 'src'
        package = source / 'org/homeguardian/edgeservice'
        package.mkdir(parents=True)
        shutil.copy(SERVICE / 'BridgeSupervisor.java', package / 'BridgeSupervisor.java')
        (source / 'Harness.java').write_text(HARNESS)
        classes = self.path / 'classes'
        classes.mkdir()
        subprocess.run([str(self.jdk / 'javac'), '-Xlint:all', '-Werror',
                        '-d', str(classes),
                        str(package / 'BridgeSupervisor.java'), str(source / 'Harness.java')],
                       check=True, capture_output=True, text=True)
        run = subprocess.run([str(self.jdk / 'java'), '-cp', str(classes), 'Harness'],
                             check=True, capture_output=True, text=True)
        self.assertIn('ALL PASS', run.stdout)

    def test_policy_has_no_android_dependency(self):
        text = (SERVICE / 'BridgeSupervisor.java').read_text()
        for forbidden in ('import android.', 'import androidx.', 'Context', 'Handler'):
            self.assertNotIn(forbidden, text, forbidden)

    def test_service_retries_from_the_watchdog(self):
        text = (SERVICE / 'EdgeService.java').read_text()
        self.assertIn('BridgeSupervisor', text)
        # The retry must run on the periodic tick, and before the engine-dead early return,
        # otherwise a stopped native engine would also freeze the bridge recovery.
        tick = text.index('private void watchdogTick()')
        called = text.index('bridgeTick();', tick)
        engine_check = text.index('is the native engine still alive', tick)
        self.assertLess(called, engine_check)
        self.assertIn('TransportPolicy.vpnTunnelUp()', text)

    def test_operator_stop_still_wins(self):
        text = (SERVICE / 'EdgeService.java').read_text()
        body = text[text.index('private synchronized void stopBridge(boolean operator)'):]
        guarded = body[:body.index('EdgeState.bridgeRunning = false;')]
        self.assertIn('if (operator)', guarded)
        self.assertIn('bridgeWanted = false', guarded)
        self.assertIn('stopBridge(true)', text)     # operator stop path
        self.assertIn('stopBridge(false)', text)    # service teardown keeps the intent

    def test_restart_restores_wanted_state(self):
        text = (SERVICE / 'EdgeService.java').read_text()
        restore = text[text.index('private void restoreBridgeIfRequested()'):]
        restore = restore[:restore.index('private void bridgeTick()')]
        self.assertIn('desired_running', restore)
        self.assertIn('bridgeWanted = true', restore)

    def test_tunnel_control_is_narrow(self):
        """The app may ask for one existing tunnel; it may not manage VPNs."""
        manifest = (ROOT / 'apk_service/AndroidManifest.xml').read_text()
        self.assertIn('com.wireguard.android.permission.CONTROL_TUNNELS', manifest)
        for forbidden in ('BIND_VPN_SERVICE', 'WRITE_SECURE_SETTINGS'):
            self.assertNotIn(forbidden, manifest)

        service = (SERVICE / 'EdgeService.java').read_text()
        request = service[service.index('private void maybeRequestTunnelUp()'):]
        request = request[:request.index('private void scheduleBridgeRetry')]
        self.assertIn('setPackage(WG_PACKAGE)', request)      # explicit package, never implicit
        self.assertIn('checkSelfPermission(CONTROL_TUNNELS)', request)
        self.assertIn('BridgeSupervisor.validTunnelName(name)', request)
        self.assertIn('TUNNEL_REQUEST_INTERVAL_MS', request)  # rate limited
        self.assertIn('SET_TUNNEL_UP', request)
        self.assertNotIn('SET_TUNNEL_DOWN', service)          # never tears a tunnel down
        self.assertNotIn('prepare(', service)                 # never claims a VPN itself
        # The operator-configured name is the only value that may enter the request.
        self.assertIn('getString("tunnel", "")', request)

    def test_tunnel_name_is_configured_from_the_ui_only(self):
        activity = (SERVICE / 'MainActivity.java').read_text()
        self.assertIn('tunnelField', activity)
        self.assertIn('putString("tunnel"', activity)
        self.assertIn('requestPermissions(new String[]{CONTROL_TUNNELS}', activity)

    def test_status_exposes_retry_state(self):
        self.assertIn('bridgeAttempts', (SERVICE / 'EdgeState.java').read_text())
        self.assertIn('bridge retries', (SERVICE / 'MainActivity.java').read_text())


if __name__ == '__main__':
    unittest.main()
