import org.homeguardian.edgeservice.TransportPolicy;

import java.net.InetAddress;

/**
 * Off-device test for the bridge transport policy.
 *
 * The policy is the component that decides whether the device is allowed to present its auth token
 * to a destination, so it is tested against both directions: destinations that MUST be refused, and
 * destinations that MUST be allowed. A test containing only refusals would pass trivially if the
 * policy simply refused everything, so the allow cases are load-bearing.
 *
 * TransportPolicy touches only java.net plus android.util.Log, so it runs on the JVM against the
 * real class with a Log stub - not a reimplementation.
 *
 * Usage: TransportPolicyTest
 */
public class TransportPolicyTest {

    private static int pass = 0, fail = 0;

    private static void expectRefused(String label, String url) {
        String r = TransportPolicy.refusalReason(url);
        boolean ok = r != null;
        report(label, ok, ok ? "refused: " + r : "ALLOWED (should have been refused)");
    }

    private static void expectAllowed(String label, String url) {
        String r = TransportPolicy.refusalReason(url);
        boolean ok = r == null;
        report(label, ok, ok ? "allowed" : "REFUSED (should have been allowed): " + r);
    }

    private static void expectClass(String label, String ip, boolean wantPrivate) {
        try {
            boolean got = TransportPolicy.isPrivateOrVpn(InetAddress.getByName(ip));
            report(label, got == wantPrivate, ip + " -> private=" + got + " (wanted " + wantPrivate + ")");
        } catch (Throwable t) {
            report(label, false, "resolve failed: " + t);
        }
    }

    private static void report(String label, boolean ok, String detail) {
        if (ok) pass++; else fail++;
        System.out.printf("  %-46s %s  %s%n", label, ok ? "PASS" : "FAIL", detail);
    }

    public static void main(String[] args) {
        boolean vpnUp = TransportPolicy.vpnTunnelUp();
        System.out.println("  host VPN tunnel detected: " + vpnUp
                + "  (this host has no tunnel, so non-loopback private destinations must be refused)");

        System.out.println("\n--- MUST REFUSE ---");
        expectRefused("empty endpoint", "");
        expectRefused("null endpoint", null);
        expectRefused("public IPv4 destination", "ws://8.8.8.8:18994/");
        expectRefused("public DNS name", "ws://example.com:18994/");
        expectRefused("wss:// (no trust store in this build)", "wss://10.0.0.5:18994/");
        expectRefused("raw http scheme", "http://10.0.0.5:18994/");
        expectRefused("scheme-less host", "10.0.0.5:18994");

        System.out.println("\n--- MUST ALLOW (loopback never leaves the device) ---");
        expectAllowed("loopback IPv4", "ws://127.0.0.1:18995/");
        expectAllowed("loopback with path", "ws://127.0.0.1:18994/bridge");
        expectAllowed("loopback IPv6", "ws://[::1]:18994/");

        System.out.println("\n--- address classification ---");
        expectClass("10.0.0.5 site-local", "10.0.0.5", true);
        expectClass("192.168.1.1 site-local", "192.168.1.1", true);
        expectClass("172.16.9.9 site-local", "172.16.9.9", true);
        expectClass("100.64.0.1 CGNAT", "100.64.0.1", true);
        expectClass("169.254.1.1 link-local", "169.254.1.1", true);
        expectClass("fc00::1 ULA", "fc00::1", true);
        expectClass("fe80::1 link-local", "fe80::1", true);
        expectClass("8.8.8.8 public", "8.8.8.8", false);
        expectClass("172.32.0.1 outside 172.16/12", "172.32.0.1", false);
        expectClass("100.128.0.1 outside CGNAT", "100.128.0.1", false);
        expectClass("2001:4860::1 public v6", "2001:4860::1", false);

        System.out.println("\n--- private-but-not-loopback needs a tunnel (the credential rule) ---");
        // This is the load-bearing assertion: a private address is NOT sufficient on its own.
        // On a host with no tunnel this MUST be refused, or credentials could leave over Wi-Fi.
        String r = TransportPolicy.refusalReason("ws://192.168.1.50:18994/");
        boolean ok = (r == null) == vpnUp;
        report("private dest gated on tunnel state", ok,
                "vpnUp=" + vpnUp + " -> " + (r == null ? "allowed" : "refused (" + r + ")"));

        System.out.printf("%n=== transport policy: %d passed, %d failed ===%n", pass, fail);
        System.exit(fail == 0 ? 0 : 1);
    }
}
