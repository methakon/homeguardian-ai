package android.util;

/**
 * JVM test stub for android.util.Log.
 *
 * Exists ONLY so the REAL OutboundMcpBridge can be compiled and executed off-device against the
 * real coordinator. It is not shipped and is not part of the app: the app links the real
 * android.jar. Keeping the stub trivial is deliberate - any logic here would make the off-device
 * test less representative of the device run.
 */
public final class Log {
    public static int i(String tag, String msg) { System.out.println("I/" + tag + ": " + msg); return 0; }
    public static int w(String tag, String msg) { System.out.println("W/" + tag + ": " + msg); return 0; }
    public static int e(String tag, String msg) { System.out.println("E/" + tag + ": " + msg); return 0; }
    public static int d(String tag, String msg) { System.out.println("D/" + tag + ": " + msg); return 0; }
    public static int v(String tag, String msg) { System.out.println("V/" + tag + ": " + msg); return 0; }
    public static String getStackTraceString(Throwable t) { return String.valueOf(t); }
}
