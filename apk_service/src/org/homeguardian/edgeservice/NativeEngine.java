package org.homeguardian.edgeservice;

/**
 * Java facade over the genuine C++ engine and MCP server.
 *
 * The native library is the REAL HomeGuardian code (Application, PersistenceAudit,
 * AlexaCredentialStore, McpTools, McpHttpServer) compiled for arm64-v8a and loaded
 * with System.loadLibrary. Nothing here is a stub or a re-implementation: this
 * class only carries arguments across the JNI boundary.
 *
 * Return codes are stable and mirrored from hg_edge_service_jni.cpp.
 */
public final class NativeEngine {

    public static final int OK = 0;
    public static final int ALREADY_RUNNING = 2;
    public static final int CREDENTIAL_STORE_FAILED = 3;
    public static final int START_REFUSED = 4;
    public static final int INTERNAL_ERROR = 5;
    public static final int NOT_RUNNING = 6;

    private static volatile boolean sLoaded = false;
    private static volatile String sLoadError = null;

    static {
        try {
            System.loadLibrary("hg_edge_service");
            sLoaded = true;
        } catch (Throwable t) {
            // Record it rather than crashing the class initialiser: the UI must be
            // able to REPORT that the native library is unusable.
            sLoadError = t.getClass().getSimpleName() + ": " + t.getMessage();
        }
    }

    private NativeEngine() {}

    public static boolean isLoaded() { return sLoaded; }

    public static String loadError() { return sLoadError; }

    private static native String nativeVersion();
    private static native int nativeStart(String dataDir, String configPath, String credsPath,
                                          String bindAddress, int port);
    private static native int nativeStop();
    private static native boolean nativeIsRunning();
    private static native int nativePort();
    private static native String nativeLastError();
    private static native String nativeStatsJson();
    private static native int nativeReloadCamera(boolean start);

    /** In-process only; never registered as an intent, bridge command or MCP tool. */
    public static int reloadCamera(boolean start) {
        return sLoaded ? nativeReloadCamera(start) : INTERNAL_ERROR;
    }

    public static String version() {
        return sLoaded ? nativeVersion() : "native library not loaded";
    }

    public static int start(String dataDir, String configPath, String credsPath,
                            String bindAddress, int port) {
        if (!sLoaded) return INTERNAL_ERROR;
        return nativeStart(dataDir, configPath, credsPath, bindAddress, port);
    }

    public static int stop() {
        if (!sLoaded) return INTERNAL_ERROR;
        return nativeStop();
    }

    public static boolean isRunning() {
        return sLoaded && nativeIsRunning();
    }

    public static int port() {
        return sLoaded ? nativePort() : 0;
    }

    public static String lastError() {
        return sLoaded ? nativeLastError() : (sLoadError == null ? "not loaded" : sLoadError);
    }

    public static String statsJson() {
        return sLoaded ? nativeStatsJson() : "{}";
    }

    /** Human-readable name for a native return code. */
    public static String describe(int code) {
        switch (code) {
            case OK: return "OK";
            case ALREADY_RUNNING: return "ALREADY_RUNNING";
            case CREDENTIAL_STORE_FAILED: return "CREDENTIAL_STORE_FAILED (fail-closed)";
            case START_REFUSED: return "START_REFUSED (bind/auth)";
            case INTERNAL_ERROR: return "INTERNAL_ERROR";
            case NOT_RUNNING: return "NOT_RUNNING";
            default: return "UNKNOWN(" + code + ")";
        }
    }
}
