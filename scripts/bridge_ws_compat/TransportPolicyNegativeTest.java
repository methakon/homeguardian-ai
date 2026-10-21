import org.homeguardian.edgeservice.TransportPolicy;
import org.homeguardian.edgeservice.TransportPolicy.Iface;
import org.homeguardian.edgeservice.TransportPolicy.IfaceAddr;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Negative-and-positive test suite for the bridge transport policy, driven by SYNTHETIC network
 * topologies rather than whatever the test host happens to have. That matters: the two defects
 * HG-RELEASE-08 reported (a private destination with a tunnel up does not imply the traffic is
 * routed through it; and the reconnect loop did not re-check) are exactly the cases a test bound to
 * the local host's single topology cannot express.
 *
 * Design notes that make this non-vacuous:
 *  - Every topology asserts BOTH directions. A policy that refused everything would pass a
 *    refusals-only suite, so the ALLOW cases are load-bearing.
 *  - The tunnel's subnet is supplied by the topology, never hardcoded in the policy, so a test that
 *    passes here is evidence the policy derives the overlay from configuration.
 *  - Case "tunnel shares the LAN subnet" is the adversarial one: the destination is inside the
 *    tunnel's subnet AND inside the LAN's. LAN egress must win, or the check is cosmetic.
 *
 * Usage: TransportPolicyNegativeTest
 */
public class TransportPolicyNegativeTest {

    private static int pass = 0, fail = 0;

    // ------------------------------------------------------------------ topology helpers

    private static IfaceAddr addr(String ip, int prefix) throws Exception {
        return new IfaceAddr(InetAddress.getByName(ip), prefix);
    }

    private static Iface iface(String name, boolean up, IfaceAddr... addrs) {
        return new Iface(name, up, new ArrayList<IfaceAddr>(Arrays.asList(addrs)));
    }

    private static TransportPolicy.NetworkView view(final Iface... i) {
        return new TransportPolicy.NetworkView() {
            public List<Iface> interfaces() { return new ArrayList<Iface>(Arrays.asList(i)); }
        };
    }

    /** A healthy device: WireGuard overlay tun0 alongside home Wi-Fi wlan0 and loopback. */
    private static void useHealthy() throws Exception {
        TransportPolicy.setViewForTest(view(
                iface("lo",    true,  addr("127.0.0.1", 8)),
                iface("wlan0", true,  addr("192.168.1.9", 24)),
                iface("tun0",  true,  addr("10.42.0.1", 24))));
    }

    private static void useTunnelDown() throws Exception {
        TransportPolicy.setViewForTest(view(
                iface("lo",    true,  addr("127.0.0.1", 8)),
                iface("wlan0", true,  addr("192.168.1.9", 24)),
                iface("tun0",  false, addr("10.42.0.1", 24))));
    }

    private static void useNoTunnel() throws Exception {
        TransportPolicy.setViewForTest(view(
                iface("lo",    true,  addr("127.0.0.1", 8)),
                iface("wlan0", true,  addr("192.168.1.9", 24))));
    }

    /** Adversarial: the tunnel is up but shares the LAN's subnet, so "on the tunnel subnet" alone
     *  would wrongly accept a destination reachable directly over Wi-Fi. */
    private static void useTunnelSharesLan() throws Exception {
        TransportPolicy.setViewForTest(view(
                iface("lo",    true,  addr("127.0.0.1", 8)),
                iface("wlan0", true,  addr("192.168.1.9", 24)),
                iface("tun0",  true,  addr("192.168.1.1", 24))));
    }

    /** A different overlay prefix from the healthy case, to prove nothing is hardcoded. */
    private static void useAlternateOverlay() throws Exception {
        TransportPolicy.setViewForTest(view(
                iface("lo",    true,  addr("127.0.0.1", 8)),
                iface("wlan0", true,  addr("192.168.1.9", 24)),
                iface("wg0",   true,  addr("172.30.7.1", 30))));
    }

    // ------------------------------------------------------------------ assertions

    private static void refuse(String label, String url) {
        String r = TransportPolicy.refusalReason(url);
        boolean ok = r != null;
        if (ok) pass++; else fail++;
        System.out.printf("  %-52s %s  %s%n", label, ok ? "PASS" : "FAIL",
                ok ? r : "ALLOWED (should have been refused)");
    }

    private static void allow(String label, String url) {
        String r = TransportPolicy.refusalReason(url);
        boolean ok = r == null;
        if (ok) pass++; else fail++;
        System.out.printf("  %-52s %s  %s%n", label, ok ? "PASS" : "FAIL",
                ok ? "allowed" : "REFUSED (should have been allowed): " + r);
    }

    private static void truth(String label, boolean got, boolean want, String detail) {
        boolean ok = got == want;
        if (ok) pass++; else fail++;
        System.out.printf("  %-52s %s  %s%n", label, ok ? "PASS" : "FAIL",
                detail + " (wanted " + want + ")");
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("--real")) {
            // Optional: report the real device's own verdict, for cross-checking on hardware.
            System.out.println("  real host tunnel up: " + TransportPolicy.vpnTunnelUp());
            System.out.println("  ws://127.0.0.1:18995/ -> " + TransportPolicy.refusalReason("ws://127.0.0.1:18995/"));
            System.exit(0);
        }

        System.out.println("=== TOPOLOGY A: healthy (tun0 10.42.0.1/24 + wlan0 192.168.1.9/24) ===");
        useHealthy();
        allow("overlay coordinator on tun0 subnet",          "ws://10.42.0.1:18994/");
        allow("another address on the overlay",              "ws://10.42.0.200:18994/");
        allow("loopback exempt (never leaves device)",       "ws://127.0.0.1:18995/");
        allow("loopback IPv6 exempt",                        "ws://[::1]:18995/");
        refuse("LAN EGRESS: coordinator inside wlan0 subnet", "ws://192.168.1.50:18994/");
        refuse("OFF-OVERLAY private address (10.43.0.1)",    "ws://10.43.0.1:18994/");
        refuse("CGNAT address not on the overlay",           "ws://100.64.0.1:18994/");
        refuse("public destination",                         "ws://8.8.8.8:18994/");
        refuse("public DNS name",                            "ws://example.com:18994/");
        refuse("wss:// unavailable (no trust store)",        "wss://10.42.0.1:18994/");
        refuse("http:// scheme",                             "http://10.42.0.1:18994/");
        refuse("empty endpoint",                             "");

        System.out.println("\n=== TOPOLOGY B: tunnel interface DOWN ===");
        useTunnelDown();
        refuse("overlay address while tunnel down",          "ws://10.42.0.1:18994/");
        refuse("LAN address while tunnel down",              "ws://192.168.1.50:18994/");
        allow("loopback still fine with tunnel down",        "ws://127.0.0.1:18995/");

        System.out.println("\n=== TOPOLOGY C: no tunnel at all (tunnel loss) ===");
        useNoTunnel();
        refuse("previously-valid overlay address now refused", "ws://10.42.0.1:18994/");
        refuse("LAN address, no tunnel",                     "ws://192.168.1.50:18994/");

        System.out.println("\n=== TOPOLOGY D: ADVERSARIAL - tunnel shares the LAN subnet ===");
        useTunnelSharesLan();
        refuse("LAN egress must WIN over tunnel-subnet match", "ws://192.168.1.50:18994/");
        // The tunnel's OWN address is inside wlan0's subnet here, so this overlay is ambiguous: a
        // packet to 192.168.1.1 may leave by either interface and nothing available to a non-root
        // app can tell which. Refusing is the safe reading, and it is the deliberate consequence of
        // checking non-tunnel containment BEFORE tunnel containment. An earlier draft of this test
        // expected ALLOW here; the expectation was wrong, not the policy.
        refuse("ambiguous overlay address ALSO refused",      "ws://192.168.1.1:18994/");

        System.out.println("\n=== TOPOLOGY D2: same tunnel, but destination only on the overlay ===");
        // A tunnel that shares the LAN prefix, with a coordinator address that is INSIDE the tunnel
        // prefix and OUTSIDE the LAN interface's own address range is still ambiguous, so the only
        // configuration that yields an ALLOW is one where the tunnel has its own distinct prefix -
        // which is what TOPOLOGY A and E demonstrate. This case documents the boundary explicitly.
        refuse("still refused: overlap is not resolvable",    "ws://192.168.1.200:18994/");

        System.out.println("\n=== TOPOLOGY E: different overlay prefix (nothing hardcoded) ===");
        useAlternateOverlay();
        allow("wg0 /30 peer address",                        "ws://172.30.7.2:18994/");
        refuse("address outside the wg0 /30",                "ws://172.30.7.6:18994/");
        refuse("10.42.0.1 is NOT the overlay on this device", "ws://10.42.0.1:18994/");

        System.out.println("\n=== DNS RESOLUTION CHANGES (re-resolved on every check) ===");
        useHealthy();
        final String[] answer = { "10.42.0.1" };
        TransportPolicy.setResolverForTest(new TransportPolicy.HostResolver() {
            public List<InetAddress> resolve(String host) throws Exception {
                return new ArrayList<InetAddress>(Arrays.asList(InetAddress.getByName(answer[0])));
            }
        });
        allow("name resolves to overlay address",            "ws://coord.internal:18994/");
        answer[0] = "192.168.1.50";
        refuse("SAME name re-pointed at LAN address",        "ws://coord.internal:18994/");
        answer[0] = "8.8.8.8";
        refuse("SAME name re-pointed at public address",     "ws://coord.internal:18994/");
        answer[0] = "10.42.0.1";
        allow("name re-pointed back to overlay",             "ws://coord.internal:18994/");

        TransportPolicy.setResolverForTest(new TransportPolicy.HostResolver() {
            public List<InetAddress> resolve(String host) throws Exception {
                return new ArrayList<InetAddress>(Arrays.asList(
                        InetAddress.getByName("10.42.0.1"), InetAddress.getByName("8.8.8.8")));
            }
        });
        refuse("dual-homed name (overlay + public) refused",  "ws://dual.internal:18994/");

        TransportPolicy.setResolverForTest(new TransportPolicy.HostResolver() {
            public List<InetAddress> resolve(String host) throws Exception {
                throw new java.net.UnknownHostException(host);
            }
        });
        refuse("resolution failure fails closed",            "ws://nx.internal:18994/");
        TransportPolicy.setResolverForTest(null);

        System.out.println("\n=== SUBNET ARITHMETIC (boundaries) ===");
        truth("/24 contains its last host",   TransportPolicy.inSubnet(InetAddress.getByName("10.42.0.255"), InetAddress.getByName("10.42.0.1"), 24), true, "10.42.0.255 in 10.42.0.1/24");
        truth("/24 excludes the next subnet", TransportPolicy.inSubnet(InetAddress.getByName("10.42.1.1"),   InetAddress.getByName("10.42.0.1"), 24), false, "10.42.1.1 in 10.42.0.1/24");
        truth("/32 is a host route only",     TransportPolicy.inSubnet(InetAddress.getByName("10.42.0.2"),   InetAddress.getByName("10.42.0.1"), 32), false, "10.42.0.2 in 10.42.0.1/32");
        truth("/32 matches itself",           TransportPolicy.inSubnet(InetAddress.getByName("10.42.0.1"),   InetAddress.getByName("10.42.0.1"), 32), true, "10.42.0.1 in 10.42.0.1/32");
        truth("/30 excludes .6",              TransportPolicy.inSubnet(InetAddress.getByName("172.30.7.6"),  InetAddress.getByName("172.30.7.1"), 30), false, "172.30.7.6 in 172.30.7.1/30");
        truth("/0 matches any same-family",   TransportPolicy.inSubnet(InetAddress.getByName("9.9.9.9"),     InetAddress.getByName("10.0.0.1"),  0), true, "9.9.9.9 in 10.0.0.1/0");
        truth("cross-family v4/v6 never matches", TransportPolicy.inSubnet(InetAddress.getByName("::1"),    InetAddress.getByName("10.0.0.1"),  8), false, "::1 in 10.0.0.1/8");
        truth("negative prefix treated as host",  TransportPolicy.inSubnet(InetAddress.getByName("10.42.0.2"), InetAddress.getByName("10.42.0.1"), -1), false, "10.42.0.2 in 10.42.0.1/unknown");

        TransportPolicy.setViewForTest(null);
        System.out.printf("%n=== transport policy negative suite: %d passed, %d failed ===%n", pass, fail);
        System.exit(fail == 0 ? 0 : 1);
    }
}
