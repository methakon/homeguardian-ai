package android.net;

import java.net.InetAddress;

/** Minimal android.net.IpPrefix stand-in for host execution. */
public final class IpPrefix {
    private final InetAddress address;
    private final int prefixLength;

    public IpPrefix(InetAddress address, int prefixLength) {
        this.address = address;
        this.prefixLength = prefixLength;
    }

    public InetAddress getAddress() { return address; }
    public int getPrefixLength() { return prefixLength; }

    public boolean contains(InetAddress candidate) {
        byte[] c = candidate.getAddress();
        byte[] n = address.getAddress();
        if (c.length != n.length) return false;
        for (int i = 0; i < prefixLength; i++) {
            int mask = 1 << (7 - (i % 8));
            if ((c[i / 8] & mask) != (n[i / 8] & mask)) return false;
        }
        return true;
    }

    public String toString() { return address.getHostAddress() + "/" + prefixLength; }
}
