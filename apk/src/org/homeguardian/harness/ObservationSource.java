package org.homeguardian.harness;

/**
 * Service-side access to the native observation source.
 *
 * WHY THIS EXISTS
 * The native engine is already process-global: every JNI entry point in {@code hg_harness_jni.cpp}
 * ignores its {@code thiz} and operates on one shared {@code node()}. Only the JAVA declaration
 * site made it look Activity-owned. Until now the drain was declared on {@code MainActivity}, which
 * is why the transport could only be pumped while the Activity was ticking.
 *
 * This class re-declares the three read-only accessors as {@code static} so the foreground service
 * can pull from the same engine with the Activity absent. The C++ side adds matching
 * {@code Java_org_homeguardian_harness_ObservationSource_*} entry points that call the existing
 * functions - it does NOT add a second engine, a second queue or a second consent gate.
 *
 * IDENTITY/ CONSENT IS NOT MOVED HERE. These are reads. Arming, disarming, granting and withdrawing
 * consent remain on {@code MainActivity}'s native surface, so this seam cannot become a way to
 * start a sensor without an operator action.
 *
 * Each of these three must be called off the main thread by its caller if the underlying call can
 * block; {@code CctvService} drives them from its own timer, at the observation interval the native
 * side already clamps to 100..60000 ms.
 */
public final class ObservationSource {

    private static volatile boolean sLoaded = false;
    private static volatile String sLoadError = null;

    static {
        try {
            System.loadLibrary("hg_harness");
            sLoaded = true;
        } catch (Throwable t) {
            // A service must be able to REPORT that the native layer is unusable rather than die
            // with an ExceptionInInitializerError. Same posture as NativeEngine on the Mi A3 side.
            sLoadError = t.getClass().getSimpleName() + ": " + t.getMessage();
            sLoaded = false;
        }
    }

    private ObservationSource() {
    }

    public static boolean isLoaded() {
        return sLoaded;
    }

    public static String loadError() {
        return sLoadError;
    }

    /** Drain the pending observations as NDJSON. Numeric scalars only - never pixels or PCM. */
    public static String drainObservations() {
        return sLoaded ? nativeDrainObservations() : "";
    }

    /** The native status line, for the panel and the acceptance log. */
    public static String stats() {
        return sLoaded ? nativeStats() : "native library not loaded: " + sLoadError;
    }

    /** Live consent state for each sensor, in the native gate's own vocabulary. */
    public static String consentState() {
        return sLoaded ? nativeConsentState() : "consent: unavailable";
    }

    private static native String nativeDrainObservations();

    private static native String nativeStats();

    private static native String nativeConsentState();
}
