package android.net;

public class NetworkCapabilities {
    public static final int TRANSPORT_VPN = 4;

    public boolean hasTransport(int transportType) {
        return false;
    }
}
