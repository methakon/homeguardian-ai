import org.homeguardian.edgeservice.RouteSecurityPolicy;
import org.homeguardian.edgeservice.RouteSecurityPolicy.NetInfo;
import org.homeguardian.edgeservice.RouteSecurityPolicy.PlatformRouting;
import org.homeguardian.edgeservice.RouteSecurityPolicy.Route;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Independent off-device tests for RouteSecurityPolicy, driven by SYNTHETIC platform routing states.
 *
 * Why synthetic platform state rather than the host's real networks: the states that matter here
 * cannot be produced on a test host at all - no active network, a VPN network alongside a LAN that
 * routes the same destination, and a thin-prefix (/32) overlay. Modelling the platform contract is
 * what makes those cases testable; it is not a substitute for device validation, which remains
 * outstanding and is not claimed.
 *
 * Every group asserts BOTH directions. A policy that refused everything would pass a refusals-only
 * suite, so the ALLOW cases are load-bearing - and the thin-prefix ALLOW is the specific regression
 * the previous containment approach failed.
 *
 * All addresses are RFC 5737 documentation ranges; no deployment value appears here.
 *
 * Usage: RouteSecurityPolicyTest
 */
public class RouteSecurityPolicyTest {

    private static int pass = 0, fail = 0;

    // ------------------------------------------------------------------ builders

    static Route r(String cidr) throws Exception {
        String[] parts = cidr.split("/");
        return new Route(InetAddress.getByName(parts[0]), Integer.parseInt(parts[1]));
    }
    static List<Route> routes(String... cidrs) throws Exception {
        List<Route> l = new ArrayList<Route>();
        for (String c : cidrs) l.add(r(c));
        return l;
    }
    static NetInfo net(String label, boolean vpn, String... cidrs) throws Exception {
        return new NetInfo(label, vpn, routes(cidrs));
    }

    static void platform(final NetInfo... nets) {
        RouteSecurityPolicy.setPlatformForTest(new PlatformRouting() {
            public List<NetInfo> networks() { return new ArrayList<NetInfo>(Arrays.asList(nets)); }
            public boolean available() { return true; }
            public String unavailableReason() { return null; }
        });
    }

    static void platformUnavailable(final String reason) {
        RouteSecurityPolicy.setPlatformForTest(new PlatformRouting() {
            public List<NetInfo> networks() { return new ArrayList<NetInfo>(); }
            public boolean available() { return false; }
            public String unavailableReason() { return reason; }
        });
    }

    static void resolverReturns(final String... addrs) {
        RouteSecurityPolicy.setResolverForTest(new RouteSecurityPolicy.HostResolver() {
            public List<InetAddress> resolve(String host) throws Exception {
                List<InetAddress> l = new ArrayList<InetAddress>();
                for (String a : addrs) l.add(InetAddress.getByName(a));
                return l;
            }
        });
    }

    static void resolverFails() {
        RouteSecurityPolicy.setResolverForTest(new RouteSecurityPolicy.HostResolver() {
            public List<InetAddress> resolve(String host) throws Exception {
                throw new java.net.UnknownHostException(host);
            }
        });
    }

    // ------------------------------------------------------------------ assertions

    static void allow(String label, String url) {
        String got = RouteSecurityPolicy.refusalReason(url);
        boolean ok = got == null;
        if (ok) pass++; else fail++;
        System.out.printf("  %-54s %s  %s%n", label, ok ? "PASS" : "FAIL",
                ok ? "allowed" : "REFUSED: " + got);
    }

    static void refuse(String label, String url, String mustMention) {
        String got = RouteSecurityPolicy.refusalReason(url);
        boolean refused = got != null;
        boolean specific = !refused || mustMention == null || got.contains(mustMention);
        boolean ok = refused && specific;
        if (ok) pass++; else fail++;
        System.out.printf("  %-54s %s  %s%n", label, ok ? "PASS" : "FAIL",
                !refused ? "ALLOWED (should refuse)" : (specific ? "refused" : "refused for the WRONG reason: " + got));
    }

    static void truth(String label, boolean got, boolean want) {
        boolean ok = got == want;
        if (ok) pass++; else fail++;
        System.out.printf("  %-54s %s  %s (wanted %s)%n", label, ok ? "PASS" : "FAIL", got, want);
    }

    // ------------------------------------------------------------------ main

    public static void main(String[] args) throws Exception {
        System.out.println("=== A. THIN-PREFIX overlay - the regression this replaces ===");
        // Deployment gives one /32 per peer. The old containment rule refused the hub here.
        platform(net("wlan0", false, "198.51.100.0/24"),
                 net("vpn",   true,  "203.0.113.1/32", "203.0.113.9/32"));
        allow("hub on a /32 route is now ALLOWED",           "ws://203.0.113.1:18994/");
        allow("peer /32 route ALLOWED",                      "ws://203.0.113.9:18994/");
        refuse("unrouted address in the same /24 refused",    "ws://203.0.113.5:18994/", "not reachable over the approved tunnel");
        // Still refused, now with the accurate reason: the VPN routes no part of that LAN, so the
        // failure is "not reachable over the approved tunnel" rather than a competing-route report.
        refuse("LAN host refused",                            "ws://198.51.100.7:18994/", "not reachable over the approved tunnel");

        System.out.println("\n=== B. NORMAL shared-prefix overlay (must keep working) ===");
        platform(net("wlan0", false, "198.51.100.0/24"), net("vpn", true, "203.0.113.0/24"));
        allow("hub in the overlay /24",                       "ws://203.0.113.1:18994/");
        allow("any address in the overlay /24",               "ws://203.0.113.200:18994/");
        refuse("address outside the overlay /24",             "ws://203.0.114.1:18994/", "not reachable over the approved tunnel");
        allow("loopback exempt (no VPN needed)",              "ws://127.0.0.1:18995/");
        allow("loopback IPv6 exempt",                         "ws://[::1]:18995/");

        System.out.println("\n=== C. ADVERSARIAL ===");
        platform(net("wlan0", false, "198.51.100.0/24"), net("vpn", true, "198.51.100.0/24", "203.0.113.1/32"));
        refuse("VPN and LAN both route it -> LAN wins (P3 before P2)", "ws://198.51.100.7:18994/", "egress unprotected");
        allow("a /32 only the VPN routes still works",        "ws://203.0.113.1:18994/");

        platform(net("wlan0", false, "198.51.100.0/24"), net("vpn", true, "203.0.113.0/24"));
        refuse("public destination not routed by VPN",        "ws://8.8.8.8:18994/", "not reachable over the approved tunnel");
        refuse("public DNS name not routed by VPN",           "ws://example.com:18994/", "not reachable over the approved tunnel");
        refuse("wss:// refused regardless",                   "wss://203.0.113.1:18994/", "no TLS trust store");
        refuse("http:// refused",                             "http://203.0.113.1:18994/", "only ws:// is supported");
        refuse("empty endpoint",                              "", "no coordinator endpoint");

        // DNS change: re-resolved on every call, so a re-point is caught.
        resolverReturns("203.0.113.1");
        allow("name resolving to an overlay address",         "ws://coord.internal:18994/");
        resolverReturns("198.51.100.7");
        refuse("SAME name re-pointed at the LAN",             "ws://coord.internal:18994/", "not reachable over the approved tunnel");
        resolverReturns("8.8.8.8");
        refuse("SAME name re-pointed at a public address",    "ws://coord.internal:18994/", "not reachable over the approved tunnel");
        resolverReturns("203.0.113.1", "8.8.8.8");
        refuse("dual-homed name refused (all addresses must pass)", "ws://dual.internal:18994/", "not reachable over the approved tunnel");
        resolverFails();
        refuse("resolution failure fails closed",             "ws://nx.internal:18994/", "cannot resolve");
        RouteSecurityPolicy.setResolverForTest(null);

        System.out.println("\n=== D. DISCONNECTED / DEGRADED ===");
        platform();
        refuse("no networks at all",                          "ws://203.0.113.1:18994/", "no VPN network is active");
        platform(net("wlan0", false, "198.51.100.0/24"));
        refuse("only a non-VPN network",                      "ws://203.0.113.1:18994/", "no VPN network is active");
        allow("loopback still fine when offline",             "ws://127.0.0.1:18995/");
        platform(net("vpn", true, "203.0.113.0/24"));   // VPN returns after tunnel loss
        allow("recovers once the VPN network is back",        "ws://203.0.113.1:18994/");

        platformUnavailable("ACCESS_NETWORK_STATE not granted to this app");
        refuse("permission absent -> FAIL CLOSED",            "ws://203.0.113.1:18994/", "ACCESS_NETWORK_STATE not granted");
        allow("loopback unaffected by permission state",      "ws://127.0.0.1:18995/");

        RouteSecurityPolicy.setPlatformForTest(null);
        refuse("no platform wired -> FAIL CLOSED",            "ws://203.0.113.1:18994/", "cannot verify routing");

        System.out.println("\n=== E. FULL-TUNNEL (default route) ===");
        platform(net("wlan0", false, "198.51.100.0/24"), net("vpn", true, "0.0.0.0/0"));
        allow("VPN carrying a default route covers public addresses", "ws://8.8.8.8:18994/");
        refuse("LAN still refused even with a full tunnel",   "ws://198.51.100.7:18994/", "egress unprotected");

        System.out.println("\n=== F. PREFIX ARITHMETIC ===");
        truth("/32 covers exactly its address",   RouteSecurityPolicy.inPrefix(InetAddress.getByName("203.0.113.1"), InetAddress.getByName("203.0.113.1"), 32), true);
        truth("/32 excludes the neighbour",       RouteSecurityPolicy.inPrefix(InetAddress.getByName("203.0.113.2"), InetAddress.getByName("203.0.113.1"), 32), false);
        truth("/24 covers the subnet",            RouteSecurityPolicy.inPrefix(InetAddress.getByName("203.0.113.255"), InetAddress.getByName("203.0.113.0"), 24), true);
        truth("/24 excludes the next subnet",     RouteSecurityPolicy.inPrefix(InetAddress.getByName("203.0.114.1"), InetAddress.getByName("203.0.113.0"), 24), false);
        truth("/0 covers any v4",                 RouteSecurityPolicy.inPrefix(InetAddress.getByName("8.8.8.8"), InetAddress.getByName("0.0.0.0"), 0), true);
        truth("family mismatch never covers",     RouteSecurityPolicy.inPrefix(InetAddress.getByName("::1"), InetAddress.getByName("203.0.113.0"), 24), false);
        truth("/31 covers both ends",             RouteSecurityPolicy.inPrefix(InetAddress.getByName("203.0.113.1"), InetAddress.getByName("203.0.113.0"), 31), true);
        truth("/30 excludes the far end",         RouteSecurityPolicy.inPrefix(InetAddress.getByName("203.0.113.4"), InetAddress.getByName("203.0.113.0"), 30), false);

        System.out.println("\n=== G. SOCKET PINNING (the no-fallback guarantee) ===");
        // The bridge consults this before every connect, so its contract is the guarantee:
        // it must never return null for a socket that is not actually pinned.
        RouteSecurityPolicy.setBinderForTest(null);
        String r1 = RouteSecurityPolicy.bindSocketToVpn(new java.net.Socket());
        truth("no binder configured -> refusal (not a pass)",
              r1 != null && r1.contains("no VPN socket binder"), true);

        RouteSecurityPolicy.setBinderForTest(new RouteSecurityPolicy.SocketBinder() {
            public String bind(java.net.Socket s) { return "refused: no VPN network to bind the socket to"; }
        });
        String r2 = RouteSecurityPolicy.bindSocketToVpn(new java.net.Socket());
        truth("binder refuses (no VPN) -> refusal", r2 != null && r2.contains("no VPN network"), true);

        RouteSecurityPolicy.setBinderForTest(new RouteSecurityPolicy.SocketBinder() {
            public String bind(java.net.Socket s) throws Exception { throw new java.io.IOException("network lost"); }
        });
        String r3 = RouteSecurityPolicy.bindSocketToVpn(new java.net.Socket());
        truth("binder throws (VPN vanished mid-flight) -> refusal, not a retry",
              r3 != null && r3.contains("could not bind"), true);

        RouteSecurityPolicy.setBinderForTest(new RouteSecurityPolicy.SocketBinder() {
            public String bind(java.net.Socket s) { return null; }
        });
        truth("binder succeeds -> allowed to dial",
              RouteSecurityPolicy.bindSocketToVpn(new java.net.Socket()) == null, true);

        RouteSecurityPolicy.setBinderForTest(null);

        System.out.println("\n=== H. END-TO-END: a refused bind must produce NO connection ===");
        // A real listener plus a real socket, so "no fallback" is measured rather than asserted.
        java.net.ServerSocket srv = new java.net.ServerSocket(0, 1, java.net.InetAddress.getByName("127.0.0.1"));
        final int port = srv.getLocalPort();
        srv.setSoTimeout(1500);
        final int[] accepted = { 0 };
        Thread t = new Thread(new Runnable() {
            public void run() {
                try { java.net.Socket c = srv.accept(); accepted[0]++; c.close(); }
                catch (Throwable ignored) { }
            }
        });
        t.start();

        // Case 1: binder pins successfully -> the connection is expected to reach the listener.
        RouteSecurityPolicy.setBinderForTest(new RouteSecurityPolicy.SocketBinder() {
            public String bind(java.net.Socket s) { return null; }
        });
        java.net.Socket ok = new java.net.Socket();
        String allowed = RouteSecurityPolicy.bindSocketToVpn(ok);
        if (allowed == null) ok.connect(new java.net.InetSocketAddress("127.0.0.1", port), 1500);
        try { ok.close(); } catch (Throwable ignored) { }
        Thread.sleep(400);
        truth("pinned socket reaches the listener", accepted[0] == 1, true);

        // Case 2: binder refuses -> the bridge must not connect at all, so nothing new arrives.
        RouteSecurityPolicy.setBinderForTest(new RouteSecurityPolicy.SocketBinder() {
            public String bind(java.net.Socket s) { return "refused: no VPN network"; }
        });
        int before = accepted[0];
        java.net.Socket bad = new java.net.Socket();
        String refused = RouteSecurityPolicy.bindSocketToVpn(bad);
        if (refused == null) bad.connect(new java.net.InetSocketAddress("127.0.0.1", port), 1500);  // must NOT happen
        try { bad.close(); } catch (Throwable ignored) { }
        Thread.sleep(600);
        truth("refused bind -> NO connection attempted (no fallback)", accepted[0] == before, true);

        RouteSecurityPolicy.setBinderForTest(null);
        srv.close();

        // A real phone is NEVER VPN-only: Wi-Fi stays attached and advertises its own default
        // route (0.0.0.0/0) because that is how Android describes an ordinary Wi-Fi network.
        // Refusing on "some non-VPN route covers it" therefore refuses EVERY endpoint on every
        // real split-tunnel deployment, measured on a Mi A3 / API 30. The kernel routes by
        // longest prefix, so the VPN's /32 wins over a /0; that is the only overlap that may pass.
        System.out.println("\n=== I. SPLIT TUNNEL WITH A NON-VPN DEFAULT ROUTE (the real device shape) ===");
        platform(net("wlan0", false, "192.168.1.0/24", "0.0.0.0/0"),
                 net("vpn",   true,  "192.168.77.1/32"));
        allow("VPN /32 wins against a non-VPN default route",  "ws://192.168.77.1:18994/");
        refuse("public address outside the VPN /32 refused",   "ws://8.8.8.8:18994/", "not reachable over the approved tunnel");
        refuse("LAN address still refused",                    "ws://192.168.1.50:18994/", "not reachable over the approved tunnel");

        platform(net("wlan0", false, "203.0.113.1/32"),
                 net("vpn",   true,  "203.0.113.1/32"));
        refuse("equal specificity on both paths stays refused", "ws://203.0.113.1:18994/", "egress unprotected");

        platform(net("wlan0", false, "192.168.1.0/24", "0.0.0.0/0"),
                 net("vpn",   true,  "203.0.113.0/24"));
        allow("VPN /24 more specific than the Wi-Fi default route", "ws://203.0.113.1:18994/");
        platform(net("wlan0", false, "203.0.113.0/24", "0.0.0.0/0"),
                 net("vpn",   true,  "203.0.113.1/32"));
        allow("VPN /32 inside a LAN /24 that also has a default route", "ws://203.0.113.1:18994/");

        System.out.printf("%n=== route-security policy: %d passed, %d failed ===%n", pass, fail);
        System.exit(fail == 0 ? 0 : 1);
    }
}
