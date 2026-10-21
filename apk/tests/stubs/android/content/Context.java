package android.content;

/**
 * JVM test stub for android.content.Context.
 *
 * Only the two members the production constructor's signature needs are declared. Tests never call
 * that constructor - they use the injectable {@code NodeTransportHost(ObservationLink,
 * WakeLockHolder)} form - so this exists purely so the class can be RESOLVED off-device.
 *
 * TEST-ONLY: lives under apk/tests/, is never compiled into the APK.
 */
public abstract class Context {

    public static final String POWER_SERVICE = "power";
    public static final String CONNECTIVITY_SERVICE = "connectivity";

    public abstract Context getApplicationContext();

    public abstract Object getSystemService(String name);
}
