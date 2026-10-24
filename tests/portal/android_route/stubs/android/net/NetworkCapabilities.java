package android.net;

/** Minimal android.net.NetworkCapabilities stand-in. */
public class NetworkCapabilities {
    public static final int TRANSPORT_CELLULAR = 0;
    public static final int TRANSPORT_WIFI = 1;
    public static final int TRANSPORT_VPN = 4;

    private final boolean vpn;

    public NetworkCapabilities(boolean vpn) { this.vpn = vpn; }

    public boolean hasTransport(int transportType) {
        return transportType == TRANSPORT_VPN && vpn;
    }
}
