package org.homeguardian.harness;

import android.content.Context;
import android.os.PowerManager;
import android.util.Log;

/**
 * The production {@link WakeLockHolder}: a PARTIAL wake lock with a timeout backstop.
 *
 * The ONLY class in this feature that touches PowerManager. Keeping it isolated is what lets the
 * lifecycle rules be tested on a JVM (see {@link WakeLockHolder}).
 *
 * DESIGN NOTES, because a wake lock is the easiest way to ruin a phone's battery:
 *  - PARTIAL only. The CPU keeps running; the screen may still turn off. It is not a screen-on lock
 *    and it makes no claim about the network.
 *  - A TIMEOUT is always set. The explicit release is the normal mechanism; the timeout exists so a
 *    missed path bounds the damage instead of pinning the battery until reboot. An unreleased partial
 *    wake lock is the failure mode that matters, and a timeout is the cheapest guard against it.
 *  - Non-reference-counted. The host already tracks whether it holds the lock, so ref-counting here
 *    would only create a way for the two to disagree.
 *  - A failed acquire is reported, never fatal. Losing the lock must not lose the link.
 */
public final class AndroidWakeLock implements WakeLockHolder {

    private static final String TAG = "HGNodeWakeLock";

    /** See the class comment: bounds the damage of a missed release. */
    private static final long TIMEOUT_MS = 30L * 60L * 1000L;

    /**
     * Re-arm at half the timeout, not at the timeout.
     *
     * A timed wake lock is released BY THE PLATFORM when the timeout elapses, and nothing tells us.
     * Re-arming exactly at the boundary would race that; re-arming at half leaves a full half-window
     * of slack, so a missed or delayed tick cannot let the lock lapse.
     */
    private static final long RENEW_AFTER_MS = TIMEOUT_MS / 2L;

    private final Context ctx;
    private PowerManager.WakeLock lock;
    private volatile boolean held = false;
    private volatile long acquiredAtMs = 0L;
    private volatile long acquires = 0L;
    private volatile long releases = 0L;

    public AndroidWakeLock(Context ctx) {
        this.ctx = ctx == null ? null : ctx.getApplicationContext();
    }

    /**
     * Take the lock, or RE-ARM it if the platform has let it lapse.
     *
     * THE BUG THIS FIXES: the previous version did `if (held) return;`. Because Android releases a
     * timed lock on its own at the timeout, `held` stayed true after the platform had already let go,
     * so every later call was a silent no-op and the lock was gone for the rest of the session while
     * {@link #isHeld()} still reported it. On an unattended node that is the same silent-death shape
     * as the original screen-off defect, just at a 30-minute horizon - and no test could see it,
     * because a fake holder never expires.
     *
     * So a held lock is no longer assumed to be a live lock. Past {@link #RENEW_AFTER_MS} the stale
     * handle is dropped and a fresh lock taken, which is safe to call on every scheduler tick.
     */
    @Override
    public synchronized void acquire() {
        final long now = System.currentTimeMillis();
        if (held) {
            if (now - acquiredAtMs < RENEW_AFTER_MS) {
                return;   // comfortably inside the window; nothing to do
            }
            // Past the re-arm point. Do NOT trust `held`: release whatever the platform still has us
            // holding, then take a fresh lock below.
            try {
                if (lock != null && lock.isHeld()) {
                    lock.release();
                }
            } catch (RuntimeException e) {
                Log.w(TAG, "wake lock re-arm: releasing the stale handle threw: " + e);
            }
            lock = null;
            held = false;
            Log.i(TAG, "re-arming the partial wake lock before its timeout lapses");
        }
        if (ctx == null) {
            Log.w(TAG, "no context; continuing WITHOUT a wake lock");
            return;
        }
        try {
            final PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
            if (pm == null) {
                Log.w(TAG, "PowerManager unavailable; continuing WITHOUT a wake lock");
                return;
            }
            lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "homeguardian:node-transport");
            lock.setReferenceCounted(false);
            lock.acquire(TIMEOUT_MS);
            held = true;
            acquiredAtMs = now;
            acquires++;
            Log.i(TAG, "partial wake lock acquired (timeout " + (TIMEOUT_MS / 60000L) + "m)");
        } catch (RuntimeException e) {
            lock = null;
            held = false;
            Log.w(TAG, "wake lock acquire failed, continuing without it: " + e);
        }
    }

    @Override
    public synchronized void release() {
        if (!held || lock == null) {
            held = false;
            return;
        }
        try {
            if (lock.isHeld()) {
                lock.release();
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "wake lock release threw: " + e);
        } finally {
            lock = null;
            held = false;
            releases++;
            Log.i(TAG, "partial wake lock released");
        }
    }

    /**
     * Whether the lock is held RIGHT NOW, asked of the platform rather than of our own flag.
     *
     * The flag alone is not trustworthy: a timed lock is released by the system, and reporting `true`
     * for a lock the platform has already let go is the exact lie that hid the lapse. When there is
     * no handle we fall back to the flag, which is the no-context "continuing without a lock" case.
     */
    @Override
    public boolean isHeld() {
        final PowerManager.WakeLock l = lock;
        if (!held) {
            return false;
        }
        if (l == null) {
            return false;
        }
        try {
            return l.isHeld();
        } catch (RuntimeException e) {
            return false;
        }
    }

    @Override
    public long acquireCount() {
        return acquires;
    }

    @Override
    public long releaseCount() {
        return releases;
    }
}
