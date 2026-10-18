package android.content;

import java.io.File;

/**
 * JVM test stub for android.content.Context.
 *
 * Only the surface the classes under test actually touch is implemented: Provisioner and
 * OutboundMcpBridge read the app-private files dir, and RouteSecurityPolicy's Android wiring reads
 * CONNECTIVITY_SERVICE. Nothing else is stubbed deliberately - if the real code needed more of
 * Context, this would fail to compile rather than silently pass against a fake.
 *
 * getSystemService returns null here. That is correct for the JVM: the Android wiring
 * (AndroidPlatformRouting / AndroidSocketBinder) is never instantiated in these tests, and a null
 * service makes it report unavailable, which the policy already treats as fail-closed. The stub is
 * present so the class COMPILES off-device, not so it can be exercised.
 */
public class Context {

    public static final String CONNECTIVITY_SERVICE = "connectivity";

    private final File filesDir;

    public Context(File filesDir) { this.filesDir = filesDir; }

    public File getFilesDir() { return filesDir; }

    public Object getSystemService(String name) {
        // No platform services off-device. Deliberately null rather than a fake that would let an
        // untested path look exercised.
        return null;
    }
}
