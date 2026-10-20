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

    private final Context ctx;
    private PowerManager.WakeLock lock;
    private volatile boolean held = false;
    private volatile long acquires = 0L;
    private volatile long releases = 0L;

    public AndroidWakeLock(Context ctx) {
        this.ctx = ctx == null ? null : ctx.getApplicationContext();
    }

    @Override
    public synchronized void acquire() {
        if (held) {
            return;
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

    @Override
    public boolean isHeld() {
        return held;
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
