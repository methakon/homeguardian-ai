package android.os;

/**
 * JVM test stub for android.os.PowerManager.
 *
 * {@code AndroidWakeLock} is the only production class that touches PowerManager, and the tests
 * inject a fake {@code WakeLockHolder} instead - so this stub is never exercised. It exists so the
 * class can be RESOLVED off-device, which is what lets the lifecycle rules run on a plain JVM.
 *
 * TEST-ONLY: lives under apk/tests/, is never compiled into the APK.
 */
public final class PowerManager {

    public static final int PARTIAL_WAKE_LOCK = 1;

    private PowerManager() {
    }

    /** Mirrors the platform shape closely enough to resolve; never called by the tests. */
    public WakeLock newWakeLock(int levelAndFlags, String tag) {
        return new WakeLock();
    }

    public static final class WakeLock {
        WakeLock() {
        }

        public void setReferenceCounted(boolean value) {
        }

        public void acquire() {
        }

        public void acquire(long timeout) {
        }

        public void release() {
        }

        public boolean isHeld() {
            return false;
        }
    }
}
