package org.homeguardian.edgeservice;

import android.content.Context;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStreamReader;

public class JavaBridgeRunner {
    public static void main(String[] args) {
        try {
            String coordHost = "127.0.0.1";
            int coordPort = 9443;
            String filesDir = "/tmp/hg-bridge-test";
            String mcpToken = null;
            String enrolSecret = null;

            for (int i = 0; i < args.length; i++) {
                if ("--coord-host".equals(args[i]) && i + 1 < args.length) coordHost = args[++i];
                else if ("--coord-port".equals(args[i]) && i + 1 < args.length) coordPort = Integer.parseInt(args[++i]);
                else if ("--files-dir".equals(args[i]) && i + 1 < args.length) filesDir = args[++i];
                else if ("--mcp-token".equals(args[i]) && i + 1 < args.length) mcpToken = args[++i];
                else if ("--enrol-secret".equals(args[i]) && i + 1 < args.length) enrolSecret = args[++i];
            }

            File dir = new File(filesDir);
            dir.mkdirs();
            Context ctx = new Context(dir);

            if (mcpToken != null) {
                File creds = Provisioner.credsFile(ctx);
                FileOutputStream fo = new FileOutputStream(creds);
                fo.write(("{\"" + mcpToken + "\":{\"role\":\"operator\"}}").getBytes("UTF-8"));
                fo.flush();
                fo.close();
            }

            if (enrolSecret != null) {
                File enrol = new File(ctx.getFilesDir(), "bridge_enrol.json");
                FileOutputStream fo = new FileOutputStream(enrol);
                fo.write(enrolSecret.getBytes("UTF-8"));
                fo.flush();
                fo.close();
            }

            // Ensure credentials are provisioned
            Provisioner.provisionIfNeeded(ctx);

            String deviceId = ControlBridge.ensureIdentity(ctx);
            String secret = ControlBridge.ensureEnrolment(ctx);
            String fp = ControlBridge.enrolmentFingerprint(ctx);

            // Print machine-readable ready signal
            System.out.println("READY device_id=" + deviceId + " enrolment_secret=" + secret + " fingerprint=" + fp);
            System.out.flush();

            // Wait for test runner signal to start dialing
            BufferedReader reader = new BufferedReader(new InputStreamReader(System.in));
            String cmd = reader.readLine();

            ControlBridge bridge = new ControlBridge(ctx);
            bridge.configure(coordHost, coordPort);
            bridge.start();

            Runtime.getRuntime().addShutdownHook(new Thread(new Runnable() {
                @Override
                public void run() {
                    bridge.stop();
                }
            }));

            // Keep running until bridge stops or process killed
            while (bridge.isRunning()) {
                Thread.sleep(200);
            }
        } catch (Throwable t) {
            System.err.println("JavaBridgeRunner fatal: " + t);
            t.printStackTrace();
            System.exit(1);
        }
    }
}
