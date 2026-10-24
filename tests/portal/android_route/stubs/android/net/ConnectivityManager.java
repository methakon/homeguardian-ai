package android.net;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Minimal android.net.ConnectivityManager stand-in with injectable topology and failures. */
public class ConnectivityManager {
    private final List<Network> all = new ArrayList<Network>();
    private final Map<Network, NetworkCapabilities> caps =
            new HashMap<Network, NetworkCapabilities>();
    private final Map<Network, LinkProperties> links =
            new HashMap<Network, LinkProperties>();

    public Network active = null;
    public RuntimeException activeFailure = null;
    public RuntimeException enumerateFailure = null;

    public ConnectivityManager addNetwork(Network n, NetworkCapabilities c, LinkProperties l) {
        all.add(n);
        caps.put(n, c);
        links.put(n, l);
        if (active == null && !c.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) active = n;
        return this;
    }

    public void removeNetwork(Network n) {
        all.remove(n);
        caps.remove(n);
        links.remove(n);
        if (active == n) active = all.isEmpty() ? null : all.get(0);
    }

    public Network getActiveNetwork() {
        if (activeFailure != null) throw activeFailure;
        return active;
    }

    public Network[] getAllNetworks() {
        if (enumerateFailure != null) throw enumerateFailure;
        return all.toArray(new Network[0]);
    }

    public NetworkCapabilities getNetworkCapabilities(Network n) { return caps.get(n); }

    public LinkProperties getLinkProperties(Network n) { return links.get(n); }
}
