package android.util;

/**
 * Host-run stand-in for android.util.Log.
 *
 * The production class is compiled against the real android.jar but exercised on the JVM, where
 * android.jar's methods are "Stub!" throwers. This stub is placed FIRST on the runtime classpath so
 * the policy's diagnostic logging is inert instead of fatal. It records nothing on purpose: the
 * test's own assertions are the evidence, not log output.
 */
public final class Log {
    private Log() { }

    public static int i(String tag, String msg) { return 0; }
    public static int w(String tag, String msg) { return 0; }
    public static int e(String tag, String msg) { return 0; }
}
