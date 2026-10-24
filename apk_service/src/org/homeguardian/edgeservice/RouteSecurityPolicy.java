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
 * ROUTE SPECIFICITY, NOT "ANY COVERING ROUTE"
 * An earlier version of this class refused a destination whenever ANY non-VPN network's routes
 * covered it. That is wrong for a split-tunnel VPN on a normal phone: Wi-Fi advertises a default
 * route 0.0.0.0/0, so Wi-Fi "covers" every IPv4 destination, including the approved WireGuard peer
 * that the VPN reaches through a /32 host route. The measured Mi A3 state (API 30, WireGuard
 * already up) is exactly that shape: network100 carries 0.0.0.0/0 while the VPN carries
 * 192.168.77.1/32. The old rule rejected the peer and the service could not dial.
 *
 * The fix is to compare specificity. For each resolved destination address:
 *   - a VPN network must route it (otherwise it is not on the approved tunnel), and
 *   - the VPN's best matching prefix must be longer than the best matching prefix of any non-VPN
 *     network. A non-VPN route at equal or greater length wins the comparison and the destination
 *     is refused, so LAN and equal-prefix overlap negatives are retained rather than traded away
 *     for the fix. The single exception is the default-route tie described next.
 *
 * The one tie that is admitted is the default-route tie (/0 vs /0), which is the normal
 * full-tunnel state: the VPN claims the default route exactly as Wi-Fi does. It is admitted only
 * because the socket is then pinned to the VPN network, and a failed pin is a failed dial. A tie
 * at any SPECIFIC prefix (for example a VPN /24 equal to the LAN /24) remains refused: that is the
 * adversarial overlap where route metadata does not establish which link owns the address.
 * THE CHECK IS STILL MADE BINDING BY PINNING THE SOCKET
 * Route metadata says where a packet should go; binding the socket to the selected VPN Network says
 * where it can go. On API 30 this is the strongest app-level guarantee available: after
 * Network.bindSocket() the kernel uses that network's table for the socket, so if the VPN has gone
 * away the bind or the subsequent connect fails instead of falling back to Wi-Fi or cellular.
 * There is no getter on API 30 that reads back which network a socket is bound to, so the bind call
 * succeeding is the observable evidence; this is stated as a limitation rather than claimed as
 * packet-level proof.
 *
 * The decision is a pure function over an injected {@link PlatformRouting} view, so every branch is
 * testable off-device against synthetic network states - including the /32 shape, and including
 * states a test host cannot produce (no network, VPN down, overlapping routes, two VPNs).
 *
 * FAIL-CLOSED WHEN THE PLATFORM CANNOT ANSWER
 * Reading ConnectivityManager requires ACCESS_NETWORK_STATE. That permission IS present in the
 * manifest - it was operator-approved, widening the approved set from three to four - so
 * {@link AndroidPlatformRouting} reads real platform state and route decisions are made from it.
 * The refusal below still applies when the permission is not granted at runtime (revoked, or a
 * build without it): the policy REFUSES every non-loopback destination rather than silently
 * falling back to the containment logic that is known to be wrong.
 *
 * ANDROID 11 / API 30 COMPATIBILITY
 * The target device runs API 30 and the app compiles against android-34 with --release 8.
 *   - android.net.RouteInfo.getDestination() returns an IpPrefix from API 21 onward (verified in
 *     the AOSP android11-release source and in this project's android.jar); it has never returned
 *     an InetAddress on a released platform. It is still read reflectively so a future signature
 *     change degrades to "unreadable route, therefore no coverage" instead of a crash, and the
 *     older InetAddress shape is handled defensively.
 *   - android.net.Network.bindSocket(Socket) exists since API 23 and is present on API 30. It
 *     throws SocketException/EPERM when the app may not use that network (for example an app
 *     excluded from the VPN); that becomes a refusal here, never a fallback.
 *   - No API newer than 23 is used on the production path.
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
        /**
         * Opaque platform handle for this network (an android.net.Network in production). The
         * admission decision never interprets it; the socket binder uses it to bind the exact
         * network that won the route comparison. Synthetic tests may use any token, including null.
         */
        public final Object token;

        public NetInfo(String label, boolean vpn, List<Route> routes) {
            this(label, vpn, routes, null);
        }
        public NetInfo(String label, boolean vpn, List<Route> routes, Object token) {
            this.label = label; this.vpn = vpn; this.routes = routes; this.token = token;
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

    // ------------------------------------------------------------------ route decision

    /**
     * The result of comparing one destination against the platform's per-network routes.
     *
     * Exposed as a value object so tests can assert WHICH VPN network was selected, not merely that
     * some network was acceptable. Selection is the second half of the deployed defect: the old
     * binder took the first VPN it enumerated, which need not be the VPN that routes the peer.
     */
    public static final class Admission {
        /** null when admitted, otherwise the human-readable refusal reason. */
        public final String refusal;
        /** The VPN network selected when admitted (null for loopback). */
        public final NetInfo vpnNetwork;
        /** Longest VPN prefix covering the address, or -1. */
        public final int vpnPrefixLength;
        /** Longest non-VPN prefix covering the address, or -1. */
        public final int competingPrefixLength;

        private Admission(String refusal, NetInfo vpnNetwork, int vpnPrefixLength,
                          int competingPrefixLength) {
            this.refusal = refusal;
            this.vpnNetwork = vpnNetwork;
            this.vpnPrefixLength = vpnPrefixLength;
            this.competingPrefixLength = competingPrefixLength;
        }

        public boolean admitted() { return refusal == null; }
    }

    private static Admission admitted(NetInfo selected, int vpnPrefix, int competingPrefix) {
        return new Admission(null, selected, vpnPrefix, competingPrefix);
    }

    private static Admission refused(String reason) {
        return new Admission(reason, null, -1, -1);
    }

    /**
     * Compares one destination address against a platform routing snapshot.
     *
     * Pure, with no I/O: the same function is used by the pre-dial check and by the socket binder,
     * so the network that is admitted is necessarily the network that gets pinned.
     */
    public static Admission evaluate(List<NetInfo> nets, InetAddress addr) {
        if (addr == null) return refused("refused: destination did not resolve to an address");
        if (addr.isLoopbackAddress()) return admitted(null, -1, -1);   // never leaves the device

        String special = specialUseRefusal(addr);
        if (special != null) return refused(special);

        List<NetInfo> all = (nets == null) ? new ArrayList<NetInfo>() : nets;

        boolean anyVpn = false;
        int vpnBest = -1;
        int competingBest = -1;
        String competingLabel = null;
        NetInfo vpnChoice = null;
        boolean vpnAmbiguous = false;

        for (NetInfo n : all) {
            if (n == null) continue;
            if (n.vpn) anyVpn = true;
            int p = bestPrefix(n, addr);
            if (p < 0) continue;
            if (n.vpn) {
                if (p > vpnBest) {
                    vpnBest = p;
                    vpnChoice = n;
                    vpnAmbiguous = false;
                } else if (p == vpnBest) {
                    // Two distinct VPNs are equally specific. Choosing either would be a guess, and
                    // guessing is how the old "first VPN" binder could pin the wrong tunnel.
                    vpnAmbiguous = true;
                }
            } else if (p > competingBest) {
                competingBest = p;
                competingLabel = n.label;
            }
        }

        if (!anyVpn) {
            return refused("refused: no VPN network is active, and "
                    + addr.getHostAddress() + " is not loopback");
        }

        if (vpnBest < 0) {
            // A specific non-VPN route, or a private destination covered only by a default route,
            // means this is a LAN/other-link address rather than an unrouted public one. Distinct
            // wording because the operator action differs.
            if (competingBest > 0 || addr.isSiteLocalAddress()) {
                String via = (competingLabel == null)
                        ? "the unprotected default path"
                        : "non-VPN network '" + competingLabel + "' (/" + competingBest + ")";
                return refused("refused: " + addr.getHostAddress()
                        + " is reachable via " + via
                        + " and no VPN network routes it - it would egress unprotected");
            }
            return refused("refused: no VPN network routes " + addr.getHostAddress()
                    + " - it is not reachable over the approved tunnel");
        }

        if (vpnAmbiguous) {
            return refused("refused: multiple VPN networks route " + addr.getHostAddress()
                    + " at equal specificity (/" + vpnBest
                    + ") - cannot select an approved tunnel unambiguously");
        }

        if (competingBest >= vpnBest) {
            // The default-route tie is the normal full-tunnel state and is admitted because the
            // socket binder pins the connection to this VPN. Any SPECIFIC-prefix tie (or a
            // more-specific non-VPN route) is an overlap and stays refused.
            boolean defaultTie = (competingBest == 0 && vpnBest == 0);
            if (!defaultTie) {
                String how = (competingBest == vpnBest) ? "equally specific" : "more specific";
                return refused("refused: " + addr.getHostAddress()
                        + " is routed by non-VPN network '" + competingLabel + "' with a " + how
                        + " prefix (/" + competingBest + " vs the VPN's /" + vpnBest
                        + ") - it would egress unprotected");
            }
        }

        return admitted(vpnChoice, vpnBest, competingBest);
    }

    // ------------------------------------------------------------------ socket binding

    /**
     * Destination-blind binder, retained for callers that only have a socket.
     *
     * A binder implementing this cannot know which VPN routes the peer, so production never uses it
     * to decide: {@link AndroidSocketBinder#bind(java.net.Socket)} refuses instead of picking the
     * first VPN network it finds.
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

    /**
     * Destination-aware binder: resolves the host, re-runs admission against live platform state,
     * and pins the socket to the VPN network that actually routes that destination.
     */
    public interface DestinationSocketBinder {
        String bind(java.net.Socket s, String host) throws Exception;
    }

    private static volatile SocketBinder binder = null;
    private static volatile DestinationSocketBinder destinationBinder = null;

    /** Test hook. Production wiring supplies the Android implementation from EdgeService. */
    public static void setBinderForTest(SocketBinder b) { binder = b; }
    public static void setDestinationBinderForTest(DestinationSocketBinder b) { destinationBinder = b; }

    /**
     * Production wiring. Called once by EdgeService so the policy reads the real platform state and
     * the bridge can pin its socket to the real VPN network.
     *
     * Deliberately an explicit call rather than a static initialiser: it keeps the decision logic
     * constructible in tests, and it makes the production path visible at the one place it is set up.
     */
    public static void wireAndroidPlatform(android.content.Context ctx) {
        platform = new AndroidPlatformRouting(ctx);
        AndroidSocketBinder b = new AndroidSocketBinder(ctx);
        destinationBinder = b;
        binder = b;      // destination-blind calls are refused by this binder, never guessed
    }

    /**
     * As {@link #bindSocketToVpn(java.net.Socket)}, but aware of the destination.
     *
     * A loopback destination is skipped: it never traverses a network interface, so there is no
     * tunnel to pin it to and demanding a binder for 127.0.0.1 would break local use for no
     * security gain. Everything else must be pinned.
     */
    public static String bindSocketToVpn(java.net.Socket s, String host) {
        if (isLoopbackLiteral(host)) return null;
        DestinationSocketBinder db = destinationBinder;
        if (db != null) {
            try {
                return db.bind(s, host);
            } catch (Throwable t) {
                return "refused: could not bind the socket to the VPN ("
                        + t.getClass().getSimpleName() + ")";
            }
        }
        return bindSocketToVpn(s);
    }

    /**
     * True only for a loopback LITERAL (127.0.0.0/8, ::1) or the exact name "localhost".
     *
     * Deliberately not a general DNS test: the bridge connects by re-resolving the host, so a NAME
     * that happens to resolve to loopback now could resolve elsewhere at connect time. Exempting
     * such a name from pinning would turn a DNS change into an unprotected dial. A numeric literal
     * cannot change, so only literals are exempt.
     */
    public static boolean isLoopbackLiteral(String host) {
        if (host == null) return false;
        String h = host.trim();
        if (h.startsWith("[") && h.endsWith("]")) h = h.substring(1, h.length() - 1);
        if (h.equalsIgnoreCase("localhost")) return true;
        if (!isNumericLiteral(h)) return false;
        try {
            return InetAddress.getByName(h).isLoopbackAddress();
        } catch (Throwable t) {
            return false;   // unparseable: treat as a real destination and require pinning
        }
    }

    /** Digits and dots for IPv4 text, hex/colon/dot for IPv6 text; anything else is a name. */
    private static boolean isNumericLiteral(String h) {
        if (h.length() == 0) return false;
        boolean colon = h.indexOf(':') >= 0;
        for (int i = 0; i < h.length(); i++) {
            char c = h.charAt(i);
            boolean ok = (c >= '0' && c <= '9') || c == '.';
            if (colon) {
                ok = ok || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':';
            }
            if (!ok) return false;
        }
        return true;
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
            return "refused: could not bind the socket to the VPN ("
                    + t.getClass().getSimpleName() + ")";
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
            resolved = resolve(host);
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
        if (allLoopback) {
            // A literal cannot change; a NAME can re-resolve between this check and connect(), so
            // exempting a name here would let DNS turn the loopback exemption into an unprotected
            // dial. Require the operator to write the loopback literal.
            if (isLoopbackLiteral(host)) return null;
            return "refused: '" + host
                    + "' resolves only to loopback; use a loopback IP literal so DNS cannot"
                    + " change the exempted destination";
        }

        PlatformRouting pr = platform;
        if (pr == null || !pr.available()) {
            // Fail closed. Not a fallback to the wrong mechanism: refusing is the only safe answer
            // when we cannot establish where the traffic would go.
            String why = (pr == null) ? "platform routing view not wired" : pr.unavailableReason();
            return "refused: cannot verify routing (" + why + ")";
        }

        List<NetInfo> nets = pr.networks();
        if (nets == null) nets = new ArrayList<NetInfo>();

        NetInfo selectedNetwork = null;
        for (InetAddress a : resolved) {
            if (a.isLoopbackAddress()) continue;      // never leaves the device
            Admission admission = evaluate(nets, a);
            if (!admission.admitted()) return admission.refusal;
            if (admission.vpnNetwork != null) {
                if (selectedNetwork == null) {
                    selectedNetwork = admission.vpnNetwork;
                } else if (!sameNetwork(selectedNetwork, admission.vpnNetwork)) {
                    return "refused: " + host
                            + " resolves to addresses on different VPN networks"
                            + " - one socket cannot be pinned to both";
                }
            }
        }

        Log.i(TAG, "endpoint accepted: " + host
                + " (VPN route selected; the socket must still be pinned to it)");
        return null;
    }

    // ------------------------------------------------------------------ helpers

    private static List<InetAddress> resolve(String host) throws Exception {
        HostResolver r = resolver;
        if (r != null) return r.resolve(host);
        return new ArrayList<InetAddress>(
                java.util.Arrays.asList(InetAddress.getAllByName(host)));
    }

    /** Length of the longest route in this network that covers addr, or -1. */
    private static int bestPrefix(NetInfo n, InetAddress addr) {
        if (n.routes == null) return -1;
        int maxBits = addr.getAddress().length * 8;
        int best = -1;
        for (Route r : n.routes) {
            if (r == null || r.network == null || r.prefixLength < 0) continue;
            int p = Math.min(r.prefixLength, maxBits);
            if (inPrefix(addr, r.network, p) && p > best) best = p;
        }
        return best;
    }

    /** Same platform network? Prefer the opaque token; fall back to identity in synthetic tests. */
    private static boolean sameNetwork(NetInfo a, NetInfo b) {
        if (a == b) return true;
        if (a == null || b == null) return false;
        if (a.token != null && b.token != null) return a.token.equals(b.token);
        return false;
    }

    /**
     * Address classes that can never be a coordinator endpoint, refused before any route is
     * considered. 169.254.0.0/16 is the link-local range that carries cloud instance metadata
     * (169.254.169.254); 100.64/10-style shared space is not listed because it is routable, but
     * link-local, unspecified, multicast and broadcast are not legitimate dial targets whatever the
     * platform claims to route.
     */
    private static String specialUseRefusal(InetAddress a) {
        if (a.isAnyLocalAddress()) {
            return "refused: " + a.getHostAddress()
                    + " is the unspecified address - never a valid coordinator endpoint";
        }
        if (a.isMulticastAddress()) {
            return "refused: " + a.getHostAddress()
                    + " is multicast - never a valid coordinator endpoint";
        }
        byte[] b = a.getAddress();
        if (a instanceof Inet4Address) {
            int b0 = b[0] & 0xFF, b1 = b[1] & 0xFF;
            if (b0 == 169 && b1 == 254) {
                return "refused: " + a.getHostAddress()
                        + " is IPv4 link-local (cloud metadata range) - never a valid coordinator endpoint";
            }
            boolean allOnes = true;
            for (byte x : b) if ((x & 0xFF) != 0xFF) { allOnes = false; break; }
            if (allOnes) {
                return "refused: " + a.getHostAddress()
                        + " is the broadcast address - never a valid coordinator endpoint";
            }
        } else if (a instanceof Inet6Address) {
            // fe80::/10
            if ((b[0] & 0xFE) == 0xFE && (b[1] & 0xC0) == 0x80) {
                return "refused: " + a.getHostAddress()
                        + " is IPv6 link-local - never a valid coordinator endpoint";
            }
        }
        return null;
    }

    /**
     * True when both addresses are the same family and share the first prefixLength bits.
     * prefixLength of 0 matches any address of the same family, which is how a default route is
     * represented. It is still the WEAKEST possible match: the policy compares lengths, so a /0
     * never beats a specific VPN route and never masks a LAN route.
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
     * ACCESS_NETWORK_STATE is present in the manifest (operator-approved, widening the approved set
     * from three to four), so in production this reads the real platform state. If the permission is
     * not granted at runtime this reports unavailable and the policy refuses - fail-closed, never a
     * silent downgrade to the unreliable containment path.
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

        /**
         * Every network with its routes, each carrying its android.net.Network as the opaque token
         * so the binder can pin exactly the network this decision selected.
         */
        @SuppressWarnings("deprecation")   // getAllNetworks is the API 30 way; no replacement there
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
                    out.add(new NetInfo(label, vpn, routes, n));
                }
            } catch (Throwable t) {
                Log.w(TAG, "could not enumerate networks: " + t);
            }
            return out;
        }

        /**
         * Extracts (destination, prefix) from a RouteInfo reflectively.
         *
         * On Android API 21+ - including the target API 30 - getDestination() returns an IpPrefix.
         * Reading it reflectively keeps the class loadable and the decision fail-closed if a future
         * platform changes the return type; an unrecognised route shape is skipped, and an address
         * no readable route covers is refused. The older InetAddress shape is handled too, because
         * the cost is a few lines and the alternative is guessing a /32 from a wider route.
         */
        private static Route routeOf(android.net.RouteInfo ri) {
            try {
                java.lang.reflect.Method gd = android.net.RouteInfo.class.getMethod("getDestination");
                Object d = gd.invoke(ri);
                Route fromPrefix = fromIpPrefix(d);
                if (fromPrefix != null) return fromPrefix;
                if (d instanceof InetAddress) {
                    // Defensive legacy shape: without a companion prefix-length accessor a host
                    // route is the only safe reading, and a host route cannot over-claim coverage.
                    return new Route((InetAddress) d, 32);
                }
            } catch (Throwable ignored) { }
            return null;   // unreadable: skip rather than guess a destination
        }

        /** Builds a Route from an android.net.IpPrefix, by reflection. */
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
     * Real socket binder. Re-runs the route decision against LIVE platform state, selects the one
     * VPN network that routes the destination without a more-specific non-VPN route shadowing it,
     * and pins the socket to it with Network.bindSocket.
     *
     * Why the destination is required: the previous implementation took the first network with
     * TRANSPORT_VPN. With two VPN apps installed that can be a VPN which does not route the peer at
     * all, so "bound" would not have meant "on the approved overlay". Here an ambiguous selection
     * refuses instead of guessing, and the destination-blind entry point refuses too.
     *
     * API 30 note: Network.bindSocket has no read-back accessor, so success of this call is the
     * available proof that the kernel has the network's table for the socket. If the VPN is removed
     * after the bind, the following connect fails rather than silently re-routing, which is the
     * no-fallback property the bridge relies on.
     */
    public static final class AndroidSocketBinder implements SocketBinder, DestinationSocketBinder {
        private final android.content.Context ctx;
        public AndroidSocketBinder(android.content.Context c) { this.ctx = c; }

        /** Destination-blind calls are refused: selecting a VPN without the peer is a guess. */
        public String bind(java.net.Socket s) {
            return "refused: no destination supplied - cannot select the VPN network that routes it";
        }

        public String bind(java.net.Socket s, String host) {
            if (host == null || host.trim().length() == 0) {
                return "refused: no destination supplied - cannot select the VPN network that routes it";
            }
            if (isLoopbackLiteral(host)) return null;
            try {
                List<InetAddress> resolved;
                try {
                    resolved = resolve(host);
                } catch (Throwable t) {
                    return "refused: cannot resolve '" + host + "' ("
                            + t.getClass().getSimpleName() + ")";
                }
                if (resolved == null || resolved.isEmpty()) {
                    return "refused: '" + host + "' resolved to no addresses";
                }

                AndroidPlatformRouting pr = new AndroidPlatformRouting(ctx);
                if (!pr.available()) {
                    return "refused: cannot verify routing (" + pr.unavailableReason() + ")";
                }
                List<NetInfo> nets = pr.networks();

                android.net.Network selected = null;
                boolean anyRouted = false;
                for (InetAddress a : resolved) {
                    if (a.isLoopbackAddress()) continue;
                    anyRouted = true;
                    Admission admission = evaluate(nets, a);
                    if (!admission.admitted()) return admission.refusal;

                    Object token = admission.vpnNetwork == null ? null : admission.vpnNetwork.token;
                    if (!(token instanceof android.net.Network)) {
                        return "refused: selected VPN network is not bindable on this platform";
                    }
                    android.net.Network candidate = (android.net.Network) token;
                    if (selected == null) {
                        selected = candidate;
                    } else if (!selected.equals(candidate)) {
                        return "refused: '" + host
                                + "' resolves to addresses on different VPN networks"
                                + " - one socket cannot be pinned to both";
                    }
                }
                if (!anyRouted) {
                    // Every address was loopback but the host was not a loopback literal, so the
                    // exemption could be invalidated by a DNS change before connect().
                    return "refused: '" + host
                            + "' resolves only to loopback but is not a loopback literal";
                }
                // Re-read capabilities immediately before binding: the enumeration and this call
                // are separated by real time, and a VPN can disappear in between.
                android.net.ConnectivityManager cm = (android.net.ConnectivityManager)
                        ctx.getSystemService(android.content.Context.CONNECTIVITY_SERVICE);
                if (cm == null) return "refused: ConnectivityManager unavailable";
                android.net.NetworkCapabilities nc = cm.getNetworkCapabilities(selected);
                if (nc == null
                        || !nc.hasTransport(android.net.NetworkCapabilities.TRANSPORT_VPN)) {
                    return "refused: selected VPN network is no longer present";
                }

                selected.bindSocket(s);   // kernel now uses this network's table for s
                Log.i(TAG, "socket bound to the VPN network that routes " + host);
                return null;
            } catch (SecurityException se) {
                return "refused: ACCESS_NETWORK_STATE not granted - cannot pin the socket to the VPN";
            } catch (Throwable t) {
                // Includes SocketException/EPERM when the app may not use that VPN, and IOException
                // when the network has gone away. The correct outcome is a refusal, never a retry
                // on another interface or another VPN.
                return "refused: could not bind the socket to the VPN ("
                        + t.getClass().getSimpleName() + ")";
            }
        }
    }
}
