package android.net;

import java.net.InetAddress;

/**
 * Minimal android.net.RouteInfo stand-in. getDestination() returns IpPrefix, matching the real
 * Android API 21+/API 30 signature, so the production parser's reflective IpPrefix path is the one
 * exercised here.
 */
public final class RouteInfo {
    private final IpPrefix destination;

    public RouteInfo(IpPrefix destination) { this.destination = destination; }

    public IpPrefix getDestination() { return destination; }

    public boolean matches(InetAddress candidate) {
        return destination != null && destination.contains(candidate);
    }

    public String toString() { return String.valueOf(destination); }
}
