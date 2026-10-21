import org.homeguardian.edgeservice.TransportPolicy;
import org.homeguardian.edgeservice.TransportPolicy.Iface;
import org.homeguardian.edgeservice.TransportPolicy.IfaceAddr;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Evaluates TransportPolicy against a CONCRETE overlay addressing pattern supplied at run time.
 *
 * This exists because the containment rule passed its synthetic fixtures while failing against the
 * deployed overlay: the fixtures used a shared /24 prefix, whereas the deployment gives each peer a
 * /32 host route. This harness takes real values as arguments so that pattern can be reproduced
 * without committing any deployment address, subnet or key to the repository.
 *
 * Usage:
 *   RealOverlayCheck <peerAddr> <prefix> <hubAddr> <lanAddr> <lanPrefix> <port>
 *
 * Documentation example using RFC 5737 ranges (NOT a real deployment - they cannot be confused
 * with a live network, which is exactly why they are used here):
 *   RealOverlayCheck 203.0.113.9 32 203.0.113.1 198.51.100.7 24 18994
 */
public class RealOverlayCheck {

    static IfaceAddr a(String ip, int p) throws Exception {
        return new IfaceAddr(InetAddress.getByName(ip), p);
    }

    static Iface ifc(String n, boolean up, IfaceAddr... ad) {
        return new Iface(n, up, new ArrayList<IfaceAddr>(Arrays.asList(ad)));
    }

    static void run(String label, final Iface... ifs) throws Exception {
        TransportPolicy.setViewForTest(new TransportPolicy.NetworkView() {
            public List<Iface> interfaces() { return new ArrayList<Iface>(Arrays.asList(ifs)); }
        });
        System.out.println("  " + label);
    }

    static void probe(String url) {
        String r = TransportPolicy.refusalReason(url);
        System.out.printf("    %-34s %s%n", url, r == null ? "ALLOW" : "REFUSE: " + r);
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 6) {
            System.err.println("usage: RealOverlayCheck <peerAddr> <prefix> <hubAddr> <lanAddr> <lanPrefix> <port>");
            System.err.println("No defaults are compiled in, deliberately: see the file header.");
            System.exit(2);
        }
        int prefix = Integer.parseInt(args[1]);
        int lanPrefix = Integer.parseInt(args[4]);
        int port = Integer.parseInt(args[5]);

        System.out.println("=== thin-prefix tunnel (the deployed pattern) ===");
        run("tun0 = " + args[0] + "/" + prefix + " + wlan0 = " + args[3] + "/" + lanPrefix,
                ifc("lo", true, a("127.0.0.1", 8)),
                ifc("wlan0", true, a(args[3], lanPrefix)),
                ifc("tun0", true, a(args[0], prefix)));
        probe("ws://" + args[2] + ":" + port + "/");
        probe("ws://" + args[0] + ":" + port + "/");
        probe("ws://" + args[3] + ":" + port + "/");

        System.out.println("\n=== same tunnel with a broad prefix (what the synthetic tests assumed) ===");
        run("tun0 = " + args[0] + "/24 + wlan0 = " + args[3] + "/" + lanPrefix,
                ifc("lo", true, a("127.0.0.1", 8)),
                ifc("wlan0", true, a(args[3], lanPrefix)),
                ifc("tun0", true, a(args[0], 24)));
        probe("ws://" + args[2] + ":" + port + "/");
        probe("ws://" + args[0] + ":" + port + "/");

        System.out.println("\nIf the first block REFUSEs the hub while the second ALLOWs it, the"
                + " containment rule is confirmed to depend on a shared prefix and cannot serve a"
                + " thin-prefix overlay.");

        TransportPolicy.setViewForTest(null);
    }
}
