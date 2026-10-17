package org.homeguardian.edgeservice;

import android.util.Log;

import java.net.InetAddress;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;

/**
 * HG-NET-02B transport policy for the outbound control bridge.
 *
 * The rule this class exists to enforce: the bridge presents a device auth token, so it must never
 * present it over a path an attacker could be sitting on. Two properties are therefore required of
 * a coordinator endpoint before the bridge is allowed to dial it at all:
 *
 *   1. The destination resolves ONLY to private or VPN-scoped addresses. A public address is
 *      refused outright, because "the VPN is up" does not make a public destination private.
 *   2. A VPN tunnel interface is actually up - UNLESS the destination is loopback, in which case
 *      the traffic never leaves the device and there is nobody to sit in the middle.
 *
 * Both checks are implemented without adding an Android permission. In particular the VPN check
 * does NOT use ConnectivityManager (and so does not need ACCESS_NETWORK_STATE, which would change
 * the permission set the operator is asked to approve); it inspects the kernel interface list
 * directly, which the app can already see.
 *
 * This is enforced in OutboundMcpBridge.start() as well as in the UI. A UI-only check would be a
 * suggestion, not a policy: any future caller reaching the service directly would bypass it.
 */
public final class TransportPolicy {

    private static final String TAG = "HGBridgePolicy";

    /** Names a VPN tunnel takes on Android: WireGuard uses tun*, some backends use wg*. */
    private static final String[] VPN_IFACE_PREFIXES = { "tun", "wg", "ppp" };

    private TransportPolicy() {}

    /**
     * @return null when the endpoint may be dialled, otherwise a human-readable refusal reason.
     *         Returning the reason (rather than a boolean) means the operator sees WHY, which is
     *         the difference between a fixable misconfiguration and an unexplained refusal.
     */
    public static String refusalReason(String url) {
        if (url == null || url.trim().length() == 0) {
            return "no coordinator endpoint configured";
        }
        String u = url.trim();

        if (u.startsWith("wss://")) {
            // Not a policy choice: this build has no TLS trust store, so wss:// cannot actually be
            // established. Refusing is honest; claiming it is merely "not preferred" would not be.
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
        if (at >= 0) authority = authority.substring(at + 1);   // userinfo is not used and not wanted

        String host;
        if (authority.startsWith("[")) {
            // Bracketed IPv6 literal, e.g. [::1]:18994 . The port lives AFTER the closing bracket,
            // so splitting on the last colon first would corrupt the address.
            int close = authority.indexOf(']');
            if (close < 0) return "refused: malformed IPv6 endpoint (unclosed '[')";
            host = authority.substring(1, close);
        } else {
            int colon = authority.lastIndexOf(':');
            host = colon > 0 ? authority.substring(0, colon) : authority;
        }
        if (host.length() == 0) return "refused: endpoint has an empty host";

        List<InetAddress> addrs;
        try {
            addrs = new java.util.ArrayList<InetAddress>(
                    java.util.Arrays.asList(java.net.InetAddress.getAllByName(host)));
        } catch (Throwable t) {
            return "refused: cannot resolve '" + host + "' (" + t.getClass().getSimpleName() + ")";
        }
        if (addrs.isEmpty()) return "refused: '" + host + "' resolved to no addresses";

        boolean allLoopback = true;
        for (InetAddress a : addrs) {
            if (!isPrivateOrVpn(a)) {
                return "refused: '" + host + "' resolves to a PUBLIC address " + a.getHostAddress()
                        + " - the bridge would offer its credentials on an unprotected path";
            }
            if (!a.isLoopbackAddress()) allLoopback = false;
        }

        // Loopback never leaves the device, so no tunnel is required for it. Anything else must
        // actually be travelling inside a tunnel before we hand over a credential.
        if (!allLoopback && !vpnTunnelUp()) {
            return "refused: no VPN tunnel interface is up (destination " + host
                    + " is not loopback) - start WireGuard first";
        }

        Log.i(TAG, "endpoint accepted: " + host + " (loopbackOnly=" + allLoopback
                + ", vpnUp=" + vpnTunnelUp() + ")");
        return null;
    }

    /** True when a tunnel interface is present and up, by kernel interface list. No permission needed. */
    public static boolean vpnTunnelUp() {
        try {
            Enumeration<NetworkInterface> ifs = NetworkInterface.getNetworkInterfaces();
            if (ifs == null) return false;
            for (NetworkInterface ni : Collections.list(ifs)) {
                if (!ni.isUp()) continue;
                String name = ni.getName();
                if (name == null) continue;
                String lower = name.toLowerCase(java.util.Locale.US);
                for (String p : VPN_IFACE_PREFIXES) {
                    if (lower.startsWith(p)) return true;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "could not enumerate network interfaces: " + t);
            return false;   // fail closed: unknown means "not proven up"
        }
        return false;
    }

    /**
     * Private, loopback, link-local, CGNAT (100.64/10) or IPv6 ULA (fc00::/7).
     *
     * Site-local and loopback come from the JDK. CGNAT and ULA are checked explicitly because
     * InetAddress has no predicate for them, and both are ranges WireGuard deployments commonly
     * use - treating them as public would refuse a legitimate peer.
     */
    public static boolean isPrivateOrVpn(InetAddress a) {
        if (a == null) return false;
        if (a.isLoopbackAddress()) return true;
        if (a.isSiteLocalAddress()) return true;     // 10/8, 172.16/12, 192.168/16, fec0::/10
        if (a.isLinkLocalAddress()) return true;     // 169.254/16, fe80::/10

        byte[] b = a.getAddress();
        if (a instanceof Inet4Address && b.length == 4) {
            int o0 = b[0] & 0xFF, o1 = b[1] & 0xFF;
            if (o0 == 100 && o1 >= 64 && o1 <= 127) return true;   // 100.64.0.0/10 CGNAT
            return false;
        }
        if (a instanceof Inet6Address && b.length == 16) {
            int top = ((b[0] & 0xFF) << 8) | (b[1] & 0xFF);
            if ((top & 0xFE00) == 0xFC00) return true;              // fc00::/7 unique local
            return false;
        }
        return false;
    }

    private static String schemeOf(String u) {
        int i = u.indexOf("://");
        return i > 0 ? u.substring(0, i) : "(none)";
    }
}
