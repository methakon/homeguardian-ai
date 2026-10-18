import org.homeguardian.edgeservice.TransportPolicy;
import org.homeguardian.edgeservice.TransportPolicy.Iface;
import org.homeguardian.edgeservice.TransportPolicy.IfaceAddr;
import java.net.InetAddress;
import java.util.*;

/** Evaluate the DEPLOYED HG-NET-02A overlay values against the real policy class. */
public class RealOverlayCheck {
    static IfaceAddr a(String ip,int p) throws Exception { return new IfaceAddr(InetAddress.getByName(ip),p); }
    static Iface ifc(String n, boolean up, IfaceAddr... ad) { return new Iface(n, up, new ArrayList<IfaceAddr>(Arrays.asList(ad))); }

    static void run(String label, final Iface... ifs) throws Exception {
        TransportPolicy.setViewForTest(new TransportPolicy.NetworkView() {
            public List<Iface> interfaces(){ return new ArrayList<Iface>(Arrays.asList(ifs)); }
        });
        System.out.println("  " + label);
        for (String u : new String[]{
                "ws://192.168.77.1:18994/",
                "ws://192.168.77.2:18994/",
                "ws://192.168.1.50:18994/" }) {
            String r = TransportPolicy.refusalReason(u);
            System.out.printf("    %-30s %s%n", u, r == null ? "ALLOW" : "REFUSE: " + r);
        }
    }

    public static void main(String[] x) throws Exception {
        System.out.println("=== REAL deployed overlay 192.168.77.x, tun addressed /32 (actual WG host-route pattern) ===");
        run("tun0 = 192.168.77.3/32 (phone peer) + wlan0 = 192.168.1.9/24",
                ifc("lo",true,a("127.0.0.1",8)), ifc("wlan0",true,a("192.168.1.9",24)), ifc("tun0",true,a("192.168.77.3",32)));
        System.out.println("\n=== same overlay, tun addressed /24 (the style my synthetic tests used) ===");
        run("tun0 = 192.168.77.3/24 + wlan0 = 192.168.1.9/24",
                ifc("lo",true,a("127.0.0.1",8)), ifc("wlan0",true,a("192.168.1.9",24)), ifc("tun0",true,a("192.168.77.3",24)));
    }
}
