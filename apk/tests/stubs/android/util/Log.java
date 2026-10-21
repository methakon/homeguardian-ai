package android.util;

/**
 * JVM test stub for android.util.Log.
 *
 * WHY THIS EXISTS
 * The classes under test call Log.i/w/e. On a real device that is the platform logger; on a plain
 * JVM there is no such class, and android.jar's version throws RuntimeException("Stub!") for every
 * call. This stub is compiled into a directory that is placed FIRST on the test runtime classpath so
 * the host's logging calls resolve here and the lifecycle assertions can run off-device.
 *
 * It is TEST-ONLY. It is not packaged into the APK: apk/build_apk.sh compiles apk/src, and this file
 * lives under apk/tests/. Nothing in the shipped application can reach it.
 */
public final class Log {

    /** Set true to make the stubs echo to stdout, for debugging a failing test. */
    public static boolean verbose = false;

    private Log() {
    }

    public static int v(String tag, String msg) { return print("V", tag, msg); }

    public static int d(String tag, String msg) { return print("D", tag, msg); }

    public static int i(String tag, String msg) { return print("I", tag, msg); }

    public static int w(String tag, String msg) { return print("W", tag, msg); }

    public static int e(String tag, String msg) { return print("E", tag, msg); }

    public static int e(String tag, String msg, Throwable tr) {
        return print("E", tag, msg + " | " + (tr == null ? "null" : tr.toString()));
    }

    private static int print(String level, String tag, String msg) {
        if (verbose) {
            System.out.println("    [" + level + "/" + tag + "] " + msg);
        }
        return 0;
    }
}
