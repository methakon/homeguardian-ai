package org.homeguardian.edgeservice;

import android.util.Log;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;

/**
 * HG-NET-02B transport policy for the outbound control bridge.
 *
 * WHY THIS IS STRUCTURAL RATHER THAN A FLAG CHECK
 * An earlier version accepted a destination when it was "private" and any tunnel interface was up.
 * HG-RELEASE-08 correctly identified that as insufficient: those two facts do not imply the traffic
 * to that address travels through WireGuard. A coordinator at 192.168.1.50 is private, and a tun0
 * belonging to some unrelated VPN can be up, yet the packet would leave over Wi-Fi - presenting the
 * device auth token on an unprotected link. "The tunnel is up" is a statement about the device, not
 * about this destination's path.
 *
 * An app without root cannot prove the kernel's chosen route, and cannot bind a socket to an
 * interface by name (SO_BINDTODEVICE needs CAP_NET_RAW). So the property is enforced by
 * CONTAINMENT, which is checkable from the interface list alone:
 *
 *   the destination must lie INSIDE a tunnel interface's own subnet   (it is on the overlay)
 *   and must lie OUTSIDE every non-tunnel interface's subnet          (no LAN egress)
 *
 * A destination that is on the overlay and on no directly-attached network can only be reached
 * through the tunnel. Neither rule hardcodes a subnet: the approved overlay is derived from
 * whatever the tunnel interface is actually configured with at check time, so the real deployed
 * instance is the source of truth rather than an example range compiled into the code.
 *
 * Loopback is exempt: it never leaves the device, so there is no link to protect.
 *
 * The interface view is injectable so the negative cases - LAN egress, off-overlay destinations,
 * tunnel down, a tunnel that shares the LAN's subnet - can be tested against synthetic topologies
 * instead of only whatever the test host happens to have.
 *
 * Enforced in OutboundMcpBridge on EVERY dial attempt (not just the first) and re-checked on each
 * heartbeat, so tunnel loss ends the session and prevents redial rather than leaking.
 */
public final class TransportPolicy {

    private static final String TAG = "HGBridgePolicy";

    /**
     * Tunnel interface name prefixes. WireGuard uses tun; some backends use wg or ppp. Written
     * without a closing star-slash sequence inside the text, which would end this comment early.
     */
    private static final String[] TUNNEL_PREFIXES = { "tun", "wg", "ppp" };

    // ---------------------------------------------------------------- network view (testable)

    /** One interface address with its prefix length. */
    public static final class IfaceAddr {
        public final InetAddress address;
        public final int prefixLength;      // CIDR bits, e.g. 24 for 10.42.0.1/24
        public IfaceAddr(InetAddress a, int p) { this.address = a; this.prefixLength = p; }
    }

    /** One interface and its addresses. */
    public static final class Iface {
        public final String name;
        public final boolean up;
        public final List<IfaceAddr> addrs;
        public Iface(String name, boolean up, List<IfaceAddr> addrs) {
            this.name = name; this.up = up; this.addrs = addrs;
        }
    }

    /** Read-only view of the device's interfaces. Real on device, synthetic in tests. */
    public interface NetworkView {
        List<Iface> interfaces();
    }

    private static final class SystemNetworkView implements NetworkView {
        public List<Iface> interfaces() {
            List<Iface> out = new ArrayList<Iface>();
            try {
                Enumeration<NetworkInterface> ifs = NetworkInterface.getNetworkInterfaces();
                if (ifs == null) return out;
                for (NetworkInterface ni : Collections.list(ifs)) {
                    List<IfaceAddr> addrs = new ArrayList<IfaceAddr>();
                    for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                        if (ia == null || ia.getAddress() == null) continue;
                        addrs.add(new IfaceAddr(ia.getAddress(), ia.getNetworkPrefixLength()));
                    }
                    out.add(new Iface(ni.getName(), ni.isUp(), addrs));
                }
            } catch (Throwable t) {
                // Returning what we have is safer than throwing: an empty list fails closed below,
                // because then no tunnel interface contains the destination.
                Log.w(TAG, "could not enumerate interfaces: " + t);
            }
            return out;
        }
    }

    private static volatile NetworkView view = new SystemNetworkView();

    /** Test hook. Production never calls this. */
    public static void setViewForTest(NetworkView v) {
        view = (v == null ? new SystemNetworkView() : v);
    }

    /**
     * Hostname resolution, injectable so the DNS-change case can be tested for real instead of
     * asserted: a name that re-points from an overlay address to a LAN or public address must be
     * re-evaluated. Resolution happens on EVERY refusalReason() call, with no caching, so the
     * reconnect loop's repeated checks cannot be satisfied by a stale answer.
     */
    public interface HostResolver {
        List<InetAddress> resolve(String host) throws Exception;
    }

    private static final class SystemResolver implements HostResolver {
        public List<InetAddress> resolve(String host) throws Exception {
            return new ArrayList<InetAddress>(
                    java.util.Arrays.asList(InetAddress.getAllByName(host)));
        }
    }

    private static volatile HostResolver resolver = new SystemResolver();

    /** Test hook. Production never calls this. */
    public static void setResolverForTest(HostResolver r) {
        resolver = (r == null ? new SystemResolver() : r);
    }

    private TransportPolicy() {}

    // ---------------------------------------------------------------- the policy

    /** True when at least one tunnel interface is up. Used for display and for early refusal. */
    public static boolean vpnTunnelUp() {
        for (Iface i : view.interfaces()) {
            if (i.up && isTunnelName(i.name)) return true;
        }
        return false;
    }

    /**
     * @return null when the endpoint may be dialled, otherwise a human-readable refusal reason.
     *         A reason rather than a boolean: the operator must be able to see why.
     */
    public static String refusalReason(String url) {
        if (url == null || url.trim().length() == 0) {
            return "no coordinator endpoint configured";
        }
        String u = url.trim();

        if (u.startsWith("wss://")) {
            // Not a preference: this build has no TLS trust store, so wss:// cannot be established.
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

        List<InetAddress> resolved;
        try {
            resolved = resolver.resolve(host);
        } catch (Throwable t) {
            return "refused: cannot resolve '" + host + "' (" + t.getClass().getSimpleName() + ")";
        }
        if (resolved == null || resolved.isEmpty()) return "refused: '" + host + "' resolved to no addresses";

        List<Iface> ifaces = view.interfaces();
        boolean tunUp = false;
        for (Iface i : ifaces) if (i.up && isTunnelName(i.name)) tunUp = true;

        for (InetAddress a : resolved) {
            if (a.isLoopbackAddress()) continue;             // never leaves the device

            // Resolved fresh on every call, so a DNS change that re-points a name at a different
            // address is re-evaluated here rather than trusted from an earlier check.
            if (!tunUp) {
                return "refused: no VPN tunnel interface is up, and " + host
                        + " is not loopback - start WireGuard first";
            }
            // 1) No LAN egress: the address must not be directly reachable on a non-tunnel link.
            for (Iface i : ifaces) {
                if (!i.up || isTunnelName(i.name)) continue;
                if (contains(i, a)) {
                    return "refused: " + a.getHostAddress() + " is inside " + i.name
                            + "'s subnet (" + describe(i)
                            + ") - it would egress over that link instead of the tunnel";
                }
            }
            // 2) Must actually be on an overlay: inside a tunnel interface's own subnet. This is
            //    the property "a tunnel is up" failed to establish, and it derives the approved
            //    overlay from the live interface config rather than any hardcoded range.
            boolean onOverlay = false;
            for (Iface i : ifaces) {
                if (!i.up || !isTunnelName(i.name)) continue;
                if (contains(i, a)) { onOverlay = true; break; }
            }
            if (!onOverlay) {
                return "refused: " + a.getHostAddress() + " is not inside any tunnel interface's"
                        + " subnet, so it is not reachable over the VPN overlay";
            }
        }

        List<String> tunNames = new ArrayList<String>();
        for (Iface i : ifaces) if (i.up && isTunnelName(i.name)) tunNames.add(i.name);
        Log.i(TAG, "endpoint accepted: " + host + " via " + tunNames);
        return null;
    }

    // ---------------------------------------------------------------- helpers

    private static boolean isTunnelName(String name) {
        if (name == null) return false;
        String n = name.toLowerCase(Locale.US);
        for (String p : TUNNEL_PREFIXES) if (n.startsWith(p)) return true;
        return false;
    }

    /** True when addr falls inside the subnet of one of the interface's addresses. */
    private static boolean contains(Iface i, InetAddress addr) {
        for (IfaceAddr ia : i.addrs) {
            if (inSubnet(addr, ia.address, ia.prefixLength)) return true;
        }
        return false;
    }

    /**
     * True when both addresses are the same family and share the first prefixLength bits.
     * A negative prefixLength (the JDK's "unknown") is treated as a host route, which is the
     * conservative reading: it can only ever match the exact address, never a wider range.
     */
    public static boolean inSubnet(InetAddress candidate, InetAddress network, int prefixLength) {
        byte[] c = candidate.getAddress();
        byte[] n = network.getAddress();
        if (c.length != n.length) return false;
        int bits = prefixLength < 0 ? c.length * 8 : Math.min(prefixLength, c.length * 8);
        for (int i = 0; i < bits; i++) {
            int mask = 1 << (7 - (i % 8));
            if ((c[i / 8] & mask) != (n[i / 8] & mask)) return false;
        }
        return true;
    }

    private static String describe(Iface i) {
        if (i.addrs.isEmpty()) return "no address";
        IfaceAddr a = i.addrs.get(0);
        return a.address.getHostAddress() + "/" + a.prefixLength;
    }

    private static String schemeOf(String u) {
        int i = u.indexOf("://");
        return i > 0 ? u.substring(0, i) : "(none)";
    }
}
