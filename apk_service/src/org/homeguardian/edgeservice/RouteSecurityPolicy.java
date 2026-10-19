package org.homeguardian.edgeservice;

import android.util.Log;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.List;

/**
 * HG-NET-02B route-security policy — platform routing information, not interface inference.
 *
 * WHY THIS REPLACES TransportPolicy
 * TransportPolicy tried to prove "this destination travels over the tunnel" by subnet containment
 * over NetworkInterface. That fails against the deployed overlay, which addresses each peer with a
 * /32 host route: a tunnel interface's own address then contains only itself, so containment can
 * never reach the peer. It also could not see per-peer routes at all, since Android's VpnService
 * installs them in the VPN's own routing table rather than on the interface.
 *
 * This class instead asks the platform, which knows the actual answer: it works from per-network
 * routes as Android reports them via LinkProperties. That representation carries thin-prefix routes
 * natively, which is the case that broke.
 *
 * The decision is a pure function over an injected {@link PlatformRouting} view, so every branch is
 * testable off-device against synthetic network states - including the /32 shape, and including
 * states a test host cannot produce (no network, VPN down, overlapping routes).
 *
 * FAIL-CLOSED ON ABSENT PERMISSION
 * Reading ConnectivityManager requires ACCESS_NETWORK_STATE. That permission is NOT yet in the
 * manifest: it takes the operator-approved set from three to four, and adding it is an operator
 * decision. Until it is granted, {@link AndroidPlatformRouting} reports the platform as unavailable
 * and this policy REFUSES every non-loopback destination. That is deliberate - it refuses rather
 * than silently falling back to the containment logic that is known to be wrong.
 */
public final class RouteSecurityPolicy {

    private static final String TAG = "HGRoutePolicy";

    // ------------------------------------------------------------------ platform view

    /** One route: a network address and its prefix length. */
    public static final class Route {
        public final InetAddress network;
        public final int prefixLength;
        public Route(InetAddress n, int p) { this.network = n; this.prefixLength = p; }
    }

    /** One network as the platform describes it. */
    public static final class NetInfo {
        public final String label;
        public final boolean vpn;
        public final List<Route> routes;
        public NetInfo(String label, boolean vpn, List<Route> routes) {
            this.label = label; this.vpn = vpn; this.routes = routes;
        }
    }

    /** Read-only view of the platform's routing state. Real on device, synthetic in tests. */
    public interface PlatformRouting {
        /** All known networks and their routes. */
        List<NetInfo> networks();
        /** False when the platform cannot be read (e.g. required permission absent). */
        boolean available();
        /** Human-readable reason for unavailability, for the refusal message. */
        String unavailableReason();
    }

    /** Hostname resolution, injectable so DNS-change cases are tested rather than asserted. */
    public interface HostResolver {
        List<InetAddress> resolve(String host) throws Exception;
    }

    private static volatile PlatformRouting platform = null;
    private static volatile HostResolver resolver = null;

    /** Test hooks. Production wiring supplies the Android implementation from EdgeService. */
    public static void setPlatformForTest(PlatformRouting p) { platform = p; }
    public static void setResolverForTest(HostResolver r) { resolver = r; }

    /**
     * Production wiring. Called once by EdgeService so the policy reads the real platform state and
     * the bridge can pin its socket to the real VPN network.
     *
     * Deliberately an explicit call rather than a static initialiser: it keeps the decision logic
     * constructible in tests, and it makes the production path visible at the one place it is set up.
     */
    public static void wireAndroidPlatform(android.content.Context ctx) {
        platform = new AndroidPlatformRouting(ctx);
        binder = new AndroidSocketBinder(ctx);
    }

    /**
     * Binds an outbound socket to the VPN network, or reports why it could not.
     *
     * This is what makes the route checks binding rather than advisory. Evaluating routes proves
     * where a packet SHOULD go; binding the socket proves where it CAN go, because the kernel then
     * uses that network's routing table for this socket. If the VPN has gone away the bind fails or
     * the subsequent connect fails - it does not silently fall back to Wi-Fi or cellular, which is
     * the requirement. Without this, a routing change between the check and connect() would send the
     * credential over whatever interface won the lookup.
     */
    public interface SocketBinder {
        /**
         * @return null on success, otherwise a human-readable refusal reason.
         * @throws Exception permitted: a binder may fail with a checked exception (a lost network
         *         surfaces as IOException). {@link #bindSocketToVpn} catches everything, so a
         *         throwing binder becomes a refusal rather than an escape.
         */
        String bind(java.net.Socket s) throws Exception;
    }

    private static volatile SocketBinder binder = null;

    /** Test hook. Production wiring supplies the Android implementation from EdgeService. */
    public static void setBinderForTest(SocketBinder b) { binder = b; }

    /**
     * As {@link #bindSocketToVpn(java.net.Socket)}, but aware of the destination.
     *
     * A loopback destination is skipped: it never traverses a network interface, so there is no
     * tunnel to pin it to and demanding a binder for 127.0.0.1 would break local use for no
     * security gain. Everything else must be pinned.
     */
    public static String bindSocketToVpn(java.net.Socket s, String host) {
        if (isLoopbackLiteral(host)) return null;
        return bindSocketToVpn(s);
    }

    /** True for 127.0.0.0/8, ::1, and the usual textual forms. */
    public static boolean isLoopbackLiteral(String host) {
        if (host == null) return false;
        String h = host.trim();
        if (h.startsWith("[") && h.endsWith("]")) h = h.substring(1, h.length() - 1);
        if (h.equals("localhost")) return true;
        try {
            return InetAddress.getByName(h).isLoopbackAddress();
        } catch (Throwable t) {
            return false;   // unparseable: treat as a real destination and require pinning
        }
    }

    /**
     * @return null when the socket is now pinned to the VPN, otherwise the reason it is not.
     *         Absence of a binder is a refusal, not a pass: an unbound socket is an unproven path.
     */
    public static String bindSocketToVpn(java.net.Socket s) {
        SocketBinder b = binder;
        if (b == null) {
            return "refused: no VPN socket binder configured - cannot guarantee the path is the tunnel";
        }
        try {
            return b.bind(s);
        } catch (Throwable t) {
            return "refused: could not bind the socket to the VPN (" + t.getClass().getSimpleName() + ")";
        }
    }

    private RouteSecurityPolicy() {}

    // ------------------------------------------------------------------ the decision

    /**
     * @return null when the endpoint may be dialled, otherwise a human-readable refusal reason.
     */
    public static String refusalReason(String url) {
        if (url == null || url.trim().length() == 0) return "no coordinator endpoint configured";
        String u = url.trim();

        if (u.startsWith("wss://")) {
            return "wss:// is not available in this build (no TLS trust store configured)";
        }
        if (!u.startsWith("ws://")) {
            return "refused: only ws:// is supported (got '" + schemeOf(u) + "')";
        }

        String authority = u.substring(5);
        int slash = authority.indexOf('/');
        if (slash >= 0) authority = authority.substring(0, slash);
        if (authority.length() == 0) return "refused: endpoint has no host";
        int at = authority.lastIndexOf('@');
        if (at >= 0) authority = authority.substring(at + 1);

        String host;
        if (authority.startsWith("[")) {
            int close = authority.indexOf(']');
            if (close < 0) return "refused: malformed IPv6 endpoint (unclosed '[')";
            host = authority.substring(1, close);
        } else {
            int colon = authority.lastIndexOf(':');
            host = colon > 0 ? authority.substring(0, colon) : authority;
        }
        if (host.length() == 0) return "refused: endpoint has an empty host";

        List<InetAddress> resolved;
        try {
            resolved = (resolver != null)
                    ? resolver.resolve(host)
                    : new ArrayList<InetAddress>(java.util.Arrays.asList(InetAddress.getAllByName(host)));
        } catch (Throwable t) {
            return "refused: cannot resolve '" + host + "' (" + t.getClass().getSimpleName() + ")";
        }
        if (resolved == null || resolved.isEmpty()) {
            return "refused: '" + host + "' resolved to no addresses";
        }

        // Loopback is exempt from every platform check, including availability. It never leaves the
        // device, so it cannot egress unprotected whatever the routing state is - and requiring
        // routing metadata for 127.0.0.1 would refuse local development for no security gain.
        boolean allLoopback = true;
        for (InetAddress a : resolved) {
            if (!a.isLoopbackAddress()) { allLoopback = false; break; }
        }
        if (allLoopback) return null;

        PlatformRouting pr = platform;
        if (pr == null || !pr.available()) {
            // Fail closed. Not a fallback to the wrong mechanism: refusing is the only safe answer
            // when we cannot establish where the traffic would go.
            String why = (pr == null) ? "platform routing view not wired" : pr.unavailableReason();
            return "refused: cannot verify routing (" + why + ")";
        }

        List<NetInfo> nets = pr.networks();
        if (nets == null) nets = new ArrayList<NetInfo>();

        for (InetAddress a : resolved) {
            if (a.isLoopbackAddress()) continue;      // never leaves the device

            // P1: does any VPN network exist to carry this?
            boolean anyVpn = false;
            for (NetInfo n : nets) if (n.vpn) anyVpn = true;
            if (!anyVpn) {
                return "refused: no VPN network is active, and " + host + " is not loopback";
            }

            // P3 first: no non-VPN network may cover the destination. Checked before P2 so an
            // overlap is refused rather than trusted - the destination must not be reachable on
            // an unprotected link, whatever the VPN also says.
            for (NetInfo n : nets) {
                if (n.vpn) continue;
                if (covers(n, a)) {
                    return "refused: " + a.getHostAddress() + " is routed by non-VPN network '"
                            + n.label + "' - it would egress unprotected";
                }
            }

            // P2: the platform must say a VPN network routes this destination. This is the clause
            // that accepts a thin-prefix route, which interface-subnet containment could not.
            boolean vpnCovers = false;
            for (NetInfo n : nets) {
                if (!n.vpn) continue;
                if (covers(n, a)) { vpnCovers = true; break; }
            }
            if (!vpnCovers) {
                return "refused: no VPN network routes " + a.getHostAddress()
                        + " - it is not reachable over the approved tunnel";
            }
        }

        Log.i(TAG, "endpoint accepted: " + host + " (routes verified against platform state)");
        return null;
    }

    // ------------------------------------------------------------------ helpers

    private static boolean covers(NetInfo n, InetAddress addr) {
        if (n.routes == null) return false;
        for (Route r : n.routes) {
            if (inPrefix(addr, r.network, r.prefixLength)) return true;
        }
        return false;
    }

    /**
     * True when both addresses are the same family and share the first prefixLength bits.
     * prefixLength of 0 matches any address of the same family, which is how a default route is
     * represented - correct, and the reason a VPN carrying a default route covers everything.
     */
    public static boolean inPrefix(InetAddress candidate, InetAddress network, int prefixLength) {
        if (candidate == null || network == null) return false;
        byte[] c = candidate.getAddress();
        byte[] n = network.getAddress();
        if (c.length != n.length) return false;
        int bits = prefixLength < 0 ? 0 : Math.min(prefixLength, c.length * 8);
        for (int i = 0; i < bits; i++) {
            int mask = 1 << (7 - (i % 8));
            if ((c[i / 8] & mask) != (n[i / 8] & mask)) return false;
        }
        return true;
    }

    private static String schemeOf(String u) {
        int i = u.indexOf("://");
        return i > 0 ? u.substring(0, i) : "(none)";
    }

    // ------------------------------------------------------------------ Android wiring

    /**
     * Real platform view. Reads ConnectivityManager, which requires ACCESS_NETWORK_STATE.
     *
     * The permission is deliberately NOT in the manifest yet: it widens the operator-approved set
     * from three to four and that is an operator decision. Until it is granted this reports
     * unavailable and the policy refuses - fail-closed, never a silent downgrade to the unreliable
     * containment path.
     */
    public static final class AndroidPlatformRouting implements PlatformRouting {
        private final android.content.Context ctx;
        public AndroidPlatformRouting(android.content.Context c) { this.ctx = c; }

        public boolean available() { return unavailableReason() == null; }

        public String unavailableReason() {
            try {
                Object svc = ctx.getSystemService(android.content.Context.CONNECTIVITY_SERVICE);
                if (svc == null) return "ConnectivityManager unavailable";
                android.net.ConnectivityManager cm = (android.net.ConnectivityManager) svc;
                // Throws SecurityException when ACCESS_NETWORK_STATE is not granted.
                android.net.Network active = cm.getActiveNetwork();
                if (active == null) return null;      // no network: policy refuses on its own terms
                android.net.NetworkCapabilities nc = cm.getNetworkCapabilities(active);
                if (nc == null) return null;
                return null;
            } catch (SecurityException se) {
                return "ACCESS_NETWORK_STATE not granted to this app";
            } catch (Throwable t) {
                return "platform routing unreadable (" + t.getClass().getSimpleName() + ")";
            }
        }

        public List<NetInfo> networks() {
            List<NetInfo> out = new ArrayList<NetInfo>();
            try {
                android.net.ConnectivityManager cm = (android.net.ConnectivityManager)
                        ctx.getSystemService(android.content.Context.CONNECTIVITY_SERVICE);
                if (cm == null) return out;
                for (android.net.Network n : cm.getAllNetworks()) {
                    android.net.NetworkCapabilities nc = cm.getNetworkCapabilities(n);
                    android.net.LinkProperties lp = cm.getLinkProperties(n);
                    if (nc == null || lp == null) continue;

                    boolean vpn = nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_VPN);
                    List<Route> routes = new ArrayList<Route>();
                    for (android.net.RouteInfo ri : lp.getRoutes()) {
                        Route parsed = routeOf(ri);
                        if (parsed != null) routes.add(parsed);
                    }
                    String label = String.valueOf(n) + (vpn ? " (vpn)" : "");
                    out.add(new NetInfo(label, vpn, routes));
                }
            } catch (Throwable t) {
                Log.w(TAG, "could not enumerate networks: " + t);
            }
            return out;
        }

        /**
         * Extracts (destination, prefix) from a RouteInfo reflectively.
         *
         * The accessor set is not stable across API levels: getDestination() returns InetAddress on
         * API 30 (the target device) but IpPrefix on newer builds, and getDestinationPrefix()
         * (IpPrefix) only exists later still. Calling either directly would bind the app to one
         * platform version, so both shapes are handled at runtime and an unrecognised one is
         * skipped - a route we cannot read is a route we must not claim, and the policy fails
         * closed on a destination no readable route covers.
         */
        private static Route routeOf(android.net.RouteInfo ri) {
            try {
                java.lang.reflect.Method gp = android.net.RouteInfo.class.getMethod("getDestinationPrefix");
                Object p = gp.invoke(ri);
                Route fromPrefix = fromIpPrefix(p);
                if (fromPrefix != null) return fromPrefix;
            } catch (Throwable ignored) { }
            try {
                java.lang.reflect.Method gd = android.net.RouteInfo.class.getMethod("getDestination");
                Object d = gd.invoke(ri);
                if (d instanceof InetAddress) {
                    int plen = 32;
                    try {
                        java.lang.reflect.Method pl = android.net.RouteInfo.class.getMethod("getDestinationPrefixLength");
                        Object v = pl.invoke(ri);
                        if (v instanceof Integer) plen = (Integer) v;
                    } catch (Throwable ignored) { }
                    return new Route((InetAddress) d, plen);
                }
                Route fromPrefix = fromIpPrefix(d);
                if (fromPrefix != null) return fromPrefix;
            } catch (Throwable ignored) { }
            return null;   // unreadable: skip rather than guess a destination
        }

        /** Builds a Route from an android.net.IpPrefix, by reflection (absent on older APIs). */
        private static Route fromIpPrefix(Object maybePrefix) {
            if (maybePrefix == null) return null;
            try {
                Class<?> c = maybePrefix.getClass();
                if (!"android.net.IpPrefix".equals(c.getName())) return null;
                java.lang.reflect.Method ga = c.getMethod("getAddress");
                java.lang.reflect.Method gl = c.getMethod("getPrefixLength");
                Object addr = ga.invoke(maybePrefix);
                Object plen = gl.invoke(maybePrefix);
                if (addr instanceof InetAddress && plen instanceof Integer) {
                    return new Route((InetAddress) addr, (Integer) plen);
                }
            } catch (Throwable ignored) { }
            return null;
        }
    }

    /**
     * Real socket binder. Finds the active network with TRANSPORT_VPN and pins the socket to it via
     * Network.bindSocket, so the kernel uses that network's table for this connection.
     *
     * Requires ACCESS_NETWORK_STATE, which is not in the manifest yet (operator decision). Until it
     * is granted the lookup throws SecurityException and this returns a refusal - it never returns
     * null on an exception, because an unbound socket would be an unpinned path.
     */
    public static final class AndroidSocketBinder implements SocketBinder {
        private final android.content.Context ctx;
        public AndroidSocketBinder(android.content.Context c) { this.ctx = c; }

        public String bind(java.net.Socket s) {
            try {
                android.net.ConnectivityManager cm = (android.net.ConnectivityManager)
                        ctx.getSystemService(android.content.Context.CONNECTIVITY_SERVICE);
                if (cm == null) return "refused: ConnectivityManager unavailable";

                android.net.Network vpn = null;
                for (android.net.Network n : cm.getAllNetworks()) {
                    android.net.NetworkCapabilities nc = cm.getNetworkCapabilities(n);
                    if (nc != null && nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_VPN)) {
                        vpn = n;
                        break;
                    }
                }
                if (vpn == null) return "refused: no VPN network to bind the socket to";

                vpn.bindSocket(s);      // kernel now uses this network's routing table for s
                Log.i(TAG, "socket bound to the VPN network; kernel cannot route it elsewhere");
                return null;
            } catch (SecurityException se) {
                return "refused: ACCESS_NETWORK_STATE not granted - cannot pin the socket to the VPN";
            } catch (Throwable t) {
                // Includes IOException when the network has gone away: the correct outcome is a
                // refusal, never a retry on another interface.
                return "refused: could not bind the socket to the VPN (" + t.getClass().getSimpleName() + ")";
            }
        }
    }
}
