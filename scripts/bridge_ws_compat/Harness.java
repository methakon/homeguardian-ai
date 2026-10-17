import android.content.Context;
import org.homeguardian.edgeservice.OutboundMcpBridge;
import org.homeguardian.edgeservice.Provisioner;

import java.io.File;

/**
 * Off-device harness for the REAL OutboundMcpBridge, run against the REAL coordinator
 * (web_mcp/outbound_bridge/coordinator.js) so protocol compatibility is demonstrated rather than
 * asserted.
 *
 * What is real here: OutboundMcpBridge, its RFC 6455 framing, its JSON message schemas, its
 * allowlists, its bounds, and Provisioner. What is stubbed: android.util.Log and
 * android.content.Context (getFilesDir only) - see the stub headers.
 *
 * Credentials live in a throwaway temp dir, so nothing touches the device's production files and
 * the production MCP bearer token is never involved.
 *
 * Usage: Harness <filesDir> <coordinatorWsUrl>
 */
public class Harness {

    public static void main(String[] args) throws Exception {
        File dir = new File(args[0]);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IllegalStateException("cannot create " + dir);
        String url = args[1];

        Context ctx = new Context(dir);

        // Provision with the REAL Provisioner so the loopback relay has a real bearer token.
        Provisioner.provisionIfNeeded(ctx);

        String deviceId = OutboundMcpBridge.deviceId(ctx);
        String authToken = OutboundMcpBridge.authToken(ctx);

        // Emitted for the test runner to register in the coordinator's authorizedDevices map.
        // Test credentials in a temp dir only; never the production values.
        System.out.println("DEVICE_ID=" + deviceId);
        System.out.println("AUTH_TOKEN=" + authToken);
        System.out.println("TOKEN_FP=" + OutboundMcpBridge.tokenFingerprint(ctx));
        System.out.flush();

        OutboundMcpBridge bridge = new OutboundMcpBridge(ctx);
        bridge.configure(url);
        System.out.println("START=" + bridge.start());
        System.out.flush();

        long deadline = System.currentTimeMillis() + Long.parseLong(System.getenv().getOrDefault("HARNESS_MS", "60000"));
        while (System.currentTimeMillis() < deadline) {
            Thread.sleep(2000);
            System.out.println("STATUS=" + OutboundMcpBridge.status());
            System.out.flush();
        }
        bridge.stop();
        System.out.println("DONE=" + OutboundMcpBridge.status());
    }
}
