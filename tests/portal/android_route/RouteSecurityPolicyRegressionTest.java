import org.homeguardian.edgeservice.RouteSecurityPolicy;
import org.homeguardian.edgeservice.RouteSecurityPolicy.Admission;
import org.homeguardian.edgeservice.RouteSecurityPolicy.NetInfo;
import org.homeguardian.edgeservice.RouteSecurityPolicy.PlatformRouting;
import org.homeguardian.edgeservice.RouteSecurityPolicy.Route;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.IpPrefix;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.RouteInfo;

import java.net.InetAddress;
import java.net.Socket;
import java.net.SocketException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Regression suite for the deployed Android route-admission defect.
 *
 * THE DEFECT: the previous policy refused a destination when ANY non-VPN network's routes covered
 * it. On the target device Wi-Fi advertises 0.0.0.0/0, so every IPv4 coordinator endpoint was
 * covered by Wi-Fi and every approved WireGuard peer was refused even though the VPN carried a /32
 * route for that exact peer. The policy now compares route specificity: the VPN route must be
 * strictly more specific than the best non-VPN route.
 *
 * FIXTURES COME FROM THE FAILING POPULATION: the first case is the measured device shape
 * (Wi-Fi default /0 alongside a VPN /32 peer route), not a convenient example. The public,
 * cloud-metadata and LAN-overlap cases keep their refusals.
 *
 * Run only through tests/portal/test_android_route_security_policy.py, which compiles the
 * PRODUCTION class and this file and executes the result.
 */
public class RouteSecurityPolicyRegressionTest {

    private static int checks = 0;
    private static int failures = 0;

    // ------------------------------------------------------------------ fixtures

    static Route route(String cidr) throws Exception {
        String[] p = cidr.split("/");
        return new Route(InetAddress.getByName(p[0]), Integer.parseInt(p[1]));
    }

    static List<Route> routes(String... cidrs) throws Exception {
        List<Route> out = new ArrayList<Route>();
        for (String c : cidrs) out.add(route(c));
        return out;
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
                List<InetAddress> out = new ArrayList<InetAddress>();
                for (String a : addrs) out.add(InetAddress.getByName(a));
                return out;
            }
        });
    }

    // ------------------------------------------------------------------ fake Android platform

    static LinkProperties link(String... cidrs) throws Exception {
        LinkProperties lp = new LinkProperties();
        for (String cidr : cidrs) {
            String[] p = cidr.split("/");
            lp.addRoute(new RouteInfo(new IpPrefix(
                    InetAddress.getByName(p[0]), Integer.parseInt(p[1]))));
        }
        return lp;
    }

    static NetworkCapabilities caps(boolean vpn) {
        return new NetworkCapabilities(vpn);
    }

    static Context contextWith(ConnectivityManager cm) {
        Context c = new Context();
        c.setConnectivityService(cm);
        return c;
    }

    static String bindRefusal(String host) {
        return RouteSecurityPolicy.bindSocketToVpn(new Socket(), host);
    }

    // ------------------------------------------------------------------ assertions

    static void check(String label, boolean ok, String detail) {
        checks++;
        if (!ok) failures++;
        System.out.printf("  %-70s %s%s%n", label, ok ? "PASS" : "FAIL",
                ok || detail == null ? "" : "  [" + detail + "]");
    }

    static void allow(String label, String url) {
        String got = RouteSecurityPolicy.refusalReason(url);
        check(label, got == null, got);
    }

    static void refuse(String label, String url, String mustMention) {
        String got = RouteSecurityPolicy.refusalReason(url);
        boolean refused = got != null;
        check(label, refused && (mustMention == null || got.contains(mustMention)),
                !refused ? "ALLOWED (should refuse)" : "wrong reason: " + got);
    }

    // ------------------------------------------------------------------ main

    public static void main(String[] args) throws Exception {
        System.out.println("== A. THE MEASURED DEVICE SHAPE: Wi-Fi default /0 + VPN /32 peer ==");
        // Measured on the Mi A3 API30: network100 (Wi-Fi) carries 0.0.0.0/0 while the WireGuard
        // network carries a host route for the approved peer. The old policy refused this peer
        // purely because Wi-Fi's default route also "covered" it.
        platform(net("wifi100", false, "0.0.0.0/0", "192.168.1.0/24"),
                 net("vpn0",    true,  "192.168.77.1/32", "192.168.77.0/24"));
        allow("approved /32 peer is admitted despite Wi-Fi /0", "ws://192.168.77.1:18994/");
        allow("other peer in the VPN /24 is admitted",           "ws://192.168.77.9:18994/");

        // The tightest reproduction: Wi-Fi advertises ONLY the default route, as measured.
        platform(net("wifi100", false, "0.0.0.0/0"),
                 net("vpn0", true, "192.168.77.1/32"));
        allow("peer admitted with Wi-Fi /0 and VPN /32 as the only routes",
                "ws://192.168.77.1:18994/");

        System.out.println("\n== B. RETAINED NEGATIVES ==");
        refuse("public address not routed by the VPN is refused",
                "ws://8.8.8.8:18994/", "not reachable over the approved tunnel");
        refuse("cloud metadata 169.254.169.254 is refused",
                "ws://169.254.169.254:18994/", null);
        refuse("LAN host is refused", "ws://192.168.1.50:18994/", "egress unprotected");

        platform(net("wifi100", false, "0.0.0.0/0", "192.168.1.0/24"),
                 net("vpn0",    true,  "192.168.1.0/24"));   // equal specificity overlap
        refuse("VPN/LAN equal-prefix overlap is refused",
                "ws://192.168.1.50:18994/", "egress unprotected");

        platform(net("wifi100", false, "192.168.1.0/24"),
                 net("vpn0",    true,  "192.168.0.0/16"));   // LAN is more specific
        refuse("more-specific LAN route beats a wider VPN route",
                "ws://192.168.1.50:18994/", "egress unprotected");

        System.out.println("\n== C. FAIL-CLOSED STATES ==");
        platform(net("wifi100", false, "0.0.0.0/0", "192.168.1.0/24"));
        refuse("no VPN network at all", "ws://192.168.77.1:18994/", "no VPN network is active");
        allow("loopback is exempt", "ws://127.0.0.1:18995/");
        allow("IPv6 loopback is exempt", "ws://[::1]:18995/");

        platformUnavailable("ACCESS_NETWORK_STATE not granted to this app");
        refuse("platform unavailable fails closed",
                "ws://192.168.77.1:18994/", "ACCESS_NETWORK_STATE not granted");
        allow("loopback unaffected by permission state", "ws://127.0.0.1:18995/");

        RouteSecurityPolicy.setPlatformForTest(null);
        refuse("unwired platform fails closed",
                "ws://192.168.77.1:18994/", "cannot verify routing");

        System.out.println("\n== D. ENDPOINT FORM ==");
        platform(net("wifi100", false, "0.0.0.0/0"), net("vpn0", true, "192.168.77.1/32"));
        refuse("wss:// still refused", "wss://192.168.77.1:18994/", "no TLS trust store");
        refuse("http:// still refused", "http://192.168.77.1:18994/", "only ws:// is supported");
        refuse("empty endpoint refused", "", "no coordinator endpoint");
        check("loopback exemption is literal-only (IPv4)",
                RouteSecurityPolicy.isLoopbackLiteral("127.0.0.1"), null);
        check("loopback exemption is literal-only (IPv6)",
                RouteSecurityPolicy.isLoopbackLiteral("[::1]"), null);
        check("a hostname is never exempted as a loopback literal",
                !RouteSecurityPolicy.isLoopbackLiteral("coord.internal"), null);
        resolverReturns("127.0.0.1");
        refuse("a name resolving only to loopback is refused (DNS could change it)",
                "ws://loopname.internal:18994/", "only to loopback");
        RouteSecurityPolicy.setResolverForTest(null);

        resolverReturns("192.168.77.1");
        allow("name resolving onto the VPN is admitted", "ws://coord.internal:18994/");
        resolverReturns("192.168.1.50");
        refuse("same name re-pointed at the LAN is refused",
                "ws://coord.internal:18994/", "egress unprotected");
        RouteSecurityPolicy.setResolverForTest(null);

        System.out.println("\n== E. ROUTE SELECTION, FULL TUNNEL, IPV6 AND SPECIAL DESTINATIONS ==");
        NetInfo decoy = net("vpn-decoy", true, "10.99.0.0/24");
        NetInfo real = net("vpn-peer", true, "192.168.77.1/32");
        NetInfo wifi0 = net("wifi100", false, "0.0.0.0/0", "192.168.1.0/24");
        Admission selected = RouteSecurityPolicy.evaluate(
                Arrays.asList(decoy, wifi0, real), InetAddress.getByName("192.168.77.1"));
        check("selection returns the longest-prefix VPN, not the first enumerated one",
                selected.admitted() && selected.vpnNetwork == real
                        && selected.vpnPrefixLength == 32,
                "selected=" + (selected.vpnNetwork == null ? "none" : selected.vpnNetwork.label));

        Admission ambiguous = RouteSecurityPolicy.evaluate(
                Arrays.asList(net("vpn-a", true, "192.168.77.1/32"),
                        net("vpn-b", true, "192.168.77.1/32")),
                InetAddress.getByName("192.168.77.1"));
        check("two equally specific VPN networks are refused, not guessed",
                ambiguous.refusal != null && ambiguous.refusal.contains("multiple VPN"),
                ambiguous.refusal);

        platform(net("wifi100", false, "0.0.0.0/0", "192.168.1.0/24"),
                 net("vpn0", true, "0.0.0.0/0"));
        allow("full-tunnel default-route tie is admitted (binding enforces the tunnel)",
                "ws://8.8.8.8:18994/");
        refuse("in a full tunnel, a specific LAN route still beats the VPN default",
                "ws://192.168.1.50:18994/", "egress unprotected");

        platform(net("wifi100", false, "198.51.100.0/24"), net("vpn0", true, "0.0.0.0/0"));
        allow("VPN default route still admits a public destination when no route ties it",
                "ws://8.8.8.8:18994/");

        platform(net("wifi100", false, "::/0"), net("vpn0", true, "2001:db8::1/128"));
        allow("IPv6 /128 peer is admitted over an IPv6 default Wi-Fi route",
                "ws://[2001:db8::1]:18994/");
        refuse("IPv6 destination not routed by the VPN is refused",
                "ws://[2001:db8::2]:18994/", "not reachable over the approved tunnel");

        platform(net("wifi100", false, "0.0.0.0/0"),
                 net("vpn0", true, "0.0.0.0/0", "::/0"));
        refuse("cloud metadata is refused by address class, not by luck of routing",
                "ws://169.254.169.254:18994/", "cloud metadata");
        refuse("IPv6 link-local is refused",
                "ws://[fe80::1]:18994/", "IPv6 link-local");
        refuse("multicast is refused", "ws://224.0.0.1:18994/", "multicast");
        refuse("broadcast is refused", "ws://255.255.255.255:18994/", "broadcast");
        refuse("unspecified address is refused", "ws://0.0.0.0:18994/", "unspecified");

        System.out.println("\n== F. ANDROID ADAPTER: ROUTE PARSING + SOCKET PINNING (API30 SHAPE) ==");
        ConnectivityManager cm = new ConnectivityManager();
        Network fwifi = new Network("wifi100");
        Network fvpn = new Network("vpn0");
        cm.addNetwork(fwifi, caps(false), link("0.0.0.0/0", "192.168.1.0/24"));
        cm.addNetwork(fvpn, caps(true), link("192.168.77.1/32", "192.168.77.0/24"));
        Context ctx = contextWith(cm);
        RouteSecurityPolicy.wireAndroidPlatform(ctx);

        allow("adapter: measured device shape is admitted",
                "ws://192.168.77.1:18994/");
        Socket pinned = new Socket();
        String bound = RouteSecurityPolicy.bindSocketToVpn(pinned, "192.168.77.1");
        check("adapter: binder pins the routing VPN and touches no other network",
                bound == null && fvpn.bindAttempts == 1 && fwifi.bindAttempts == 0
                        && fvpn.boundSockets.size() == 1 && fvpn.boundSockets.get(0) == pinned,
                "result=" + bound + " vpnAttempts=" + fvpn.bindAttempts
                        + " wifiAttempts=" + fwifi.bindAttempts);
        refuse("adapter: public destination is refused", "ws://8.8.8.8:18994/",
                "not reachable over the approved tunnel");
        refuse("adapter: metadata destination is refused",
                "ws://169.254.169.254:18994/", "cloud metadata");
        refuse("adapter: LAN destination is refused", "ws://192.168.1.50:18994/",
                "egress unprotected");
        String blind = RouteSecurityPolicy.bindSocketToVpn(new Socket());
        check("adapter: destination-blind bind refuses instead of choosing a VPN",
                blind != null && blind.contains("no destination supplied")
                        && fvpn.bindAttempts == 1 && fwifi.bindAttempts == 0, blind);

        // Two VPNs installed: the FIRST enumerated VPN is a decoy that does not route the peer.
        ConnectivityManager cm2 = new ConnectivityManager();
        Network decoy2 = new Network("vpn-decoy");
        Network wifi2 = new Network("wifi100");
        Network real2 = new Network("vpn-peer");
        cm2.addNetwork(decoy2, caps(true), link("10.99.0.0/24"));
        cm2.addNetwork(wifi2, caps(false), link("0.0.0.0/0"));
        cm2.addNetwork(real2, caps(true), link("192.168.77.1/32"));
        RouteSecurityPolicy.wireAndroidPlatform(contextWith(cm2));
        allow("adapter: peer admitted with a decoy VPN enumerated first",
                "ws://192.168.77.1:18994/");
        String multi = RouteSecurityPolicy.bindSocketToVpn(new Socket(), "192.168.77.1");
        check("adapter: binder selects the VPN that routes the peer, not the first VPN",
                multi == null && real2.bindAttempts == 1 && decoy2.bindAttempts == 0,
                "result=" + multi + " decoyAttempts=" + decoy2.bindAttempts
                        + " realAttempts=" + real2.bindAttempts);

        // Two VPNs at equal specificity: fail closed rather than pick one.
        ConnectivityManager cm3 = new ConnectivityManager();
        Network ambA = new Network("vpn-a");
        Network ambB = new Network("vpn-b");
        cm3.addNetwork(new Network("wifi100"), caps(false), link("0.0.0.0/0"));
        cm3.addNetwork(ambA, caps(true), link("192.168.77.1/32"));
        cm3.addNetwork(ambB, caps(true), link("192.168.77.1/32"));
        RouteSecurityPolicy.wireAndroidPlatform(contextWith(cm3));
        refuse("adapter: ambiguous VPN pair is refused", "ws://192.168.77.1:18994/",
                "multiple VPN");
        String ambBind = bindRefusal("192.168.77.1");
        check("adapter: ambiguous VPN pair is never bound",
                ambBind != null && ambBind.contains("multiple VPN")
                        && ambA.bindAttempts == 0 && ambB.bindAttempts == 0, ambBind);

        // API 30 EPERM / lost network: refusal, and no fallback to another VPN or to Wi-Fi.
        ConnectivityManager cm4 = new ConnectivityManager();
        Network decoy4 = new Network("vpn-decoy");
        Network wifi4 = new Network("wifi100");
        Network vpn4 = new Network("vpn-peer");
        cm4.addNetwork(decoy4, caps(true), link("10.99.0.0/24"));
        cm4.addNetwork(wifi4, caps(false), link("0.0.0.0/0"));
        cm4.addNetwork(vpn4, caps(true), link("192.168.77.1/32"));
        vpn4.bindFailure = new SocketException(
                "Binding socket to network 200 failed: EPERM (Operation not permitted)");
        RouteSecurityPolicy.wireAndroidPlatform(contextWith(cm4));
        allow("adapter: admission still passes before the EPERM bind",
                "ws://192.168.77.1:18994/");
        String eperm = RouteSecurityPolicy.bindSocketToVpn(new Socket(), "192.168.77.1");
        check("adapter: EPERM bind refuses with no fallback to another network",
                eperm != null && eperm.contains("could not bind")
                        && vpn4.bindAttempts == 1 && wifi4.bindAttempts == 0
                        && decoy4.bindAttempts == 0,
                "result=" + eperm + " vpn=" + vpn4.bindAttempts
                        + " wifi=" + wifi4.bindAttempts + " decoy=" + decoy4.bindAttempts);

        // VPN removed between admission and bind: the binder re-reads live state.
        ConnectivityManager cm5 = new ConnectivityManager();
        Network wifi5 = new Network("wifi100");
        Network vpn5 = new Network("vpn-peer");
        cm5.addNetwork(wifi5, caps(false), link("0.0.0.0/0"));
        cm5.addNetwork(vpn5, caps(true), link("192.168.77.1/32"));
        RouteSecurityPolicy.wireAndroidPlatform(contextWith(cm5));
        allow("adapter: admitted while the VPN is present", "ws://192.168.77.1:18994/");
        cm5.removeNetwork(vpn5);
        String gone = bindRefusal("192.168.77.1");
        check("adapter: binder re-checks live state and never binds a vanished VPN",
                gone != null && gone.contains("no VPN network is active")
                        && vpn5.bindAttempts == 0, gone);

        // ACCESS_NETWORK_STATE revoked: both admission and binding fail closed.
        ConnectivityManager cm6 = new ConnectivityManager();
        Network wifi6 = new Network("wifi100");
        Network vpn6 = new Network("vpn-peer");
        cm6.addNetwork(wifi6, caps(false), link("0.0.0.0/0"));
        cm6.addNetwork(vpn6, caps(true), link("192.168.77.1/32"));
        cm6.activeFailure = new SecurityException("ACCESS_NETWORK_STATE denied");
        RouteSecurityPolicy.wireAndroidPlatform(contextWith(cm6));
        refuse("adapter: revoked permission fails admission closed",
                "ws://192.168.77.1:18994/", "ACCESS_NETWORK_STATE not granted");
        String denied = bindRefusal("192.168.77.1");
        check("adapter: revoked permission fails binding closed",
                denied != null && denied.contains("ACCESS_NETWORK_STATE")
                        && vpn6.bindAttempts == 0, denied);

        // The platform's IpPrefix route shape is parsed into concrete prefixes.
        List<NetInfo> parsed = new RouteSecurityPolicy.AndroidPlatformRouting(ctx).networks();
        NetInfo parsedVpn = null;
        for (NetInfo n : parsed) if (n.token == fvpn) parsedVpn = n;
        boolean has32 = false, has24 = false;
        if (parsedVpn != null && parsedVpn.routes != null) {
            for (Route r : parsedVpn.routes) {
                if (r.prefixLength == 32) has32 = true;
                if (r.prefixLength == 24) has24 = true;
            }
        }
        check("adapter: IpPrefix routes parse to /32 and /24 and keep the network token",
                parsed.size() == 2 && parsedVpn != null && parsedVpn.vpn
                        && has32 && has24,
                "networks=" + parsed.size() + " vpnFound=" + (parsedVpn != null));

        // A name resolving onto two different VPNs cannot be served by one pinned socket.
        ConnectivityManager cm8 = new ConnectivityManager();
        Network vpnA8 = new Network("vpn-a");
        Network vpnB8 = new Network("vpn-b");
        cm8.addNetwork(new Network("wifi100"), caps(false), link("0.0.0.0/0"));
        cm8.addNetwork(vpnA8, caps(true), link("10.99.0.0/24"));
        cm8.addNetwork(vpnB8, caps(true), link("192.168.77.1/32"));
        RouteSecurityPolicy.wireAndroidPlatform(contextWith(cm8));
        resolverReturns("192.168.77.1", "10.99.0.5");
        refuse("adapter: a name spanning two VPNs is refused at admission",
                "ws://multi.internal:18994/", "different VPN networks");
        String split = bindRefusal("multi.internal");
        check("adapter: one socket is never pinned across two VPN networks",
                split != null && split.contains("different VPN networks")
                        && vpnA8.bindAttempts == 0 && vpnB8.bindAttempts == 0, split);
        RouteSecurityPolicy.setResolverForTest(null);

        // A real full tunnel: both Wi-Fi and the VPN claim the default route. Admission is allowed
        // because the socket is pinned to the VPN, and the binding is asserted here.
        ConnectivityManager cm9 = new ConnectivityManager();
        Network wifi9 = new Network("wifi100");
        Network vpn9 = new Network("vpn-full");
        cm9.addNetwork(wifi9, caps(false), link("0.0.0.0/0", "192.168.1.0/24"));
        cm9.addNetwork(vpn9, caps(true), link("0.0.0.0/0"));
        RouteSecurityPolicy.wireAndroidPlatform(contextWith(cm9));
        allow("adapter: full-tunnel public destination is admitted", "ws://8.8.8.8:18994/");
        String full = RouteSecurityPolicy.bindSocketToVpn(new Socket(), "8.8.8.8");
        check("adapter: full-tunnel socket is pinned to the VPN, not Wi-Fi",
                full == null && vpn9.bindAttempts == 1 && wifi9.bindAttempts == 0,
                "result=" + full + " vpn=" + vpn9.bindAttempts
                        + " wifi=" + wifi9.bindAttempts);
        refuse("adapter: full tunnel still refuses the LAN",
                "ws://192.168.1.50:18994/", "egress unprotected");

        System.out.printf("%n=== route security regression: %d checks, %d failed ===%n",
                checks, failures);
        System.exit(failures == 0 ? 0 : 1);
    }
}
