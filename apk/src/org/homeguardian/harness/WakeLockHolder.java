package org.homeguardian.harness;

/**
 * The wake lock, behind an interface.
 *
 * WHY THIS IS AN INTERFACE AND NOT A DIRECT PowerManager CALL
 * The lifecycle rules for the lock - acquire only when a link is actually wanted, release on every
 * exit path, never hold it while idle - are the part that can be WRONG. They are also the part that
 * needs a test. Calling PowerManager directly would drag Android into the test, so the rules would
 * end up asserted by reading code rather than by running it.
 *
 * With this seam the rules are exercised for real on a plain JVM: the test supplies a recording
 * holder and asserts the exact sequence of acquire/release across start, refusal, consent
 * withdrawal, failure and destroy.
 *
 * {@link AndroidWakeLock} is the production implementation. It is the ONLY class in this feature
 * that touches PowerManager.
 */
public interface WakeLockHolder {

    /**
     * Take the lock, or re-arm it if it has lapsed.
     *
     * Implementations must tolerate being asked twice and must not throw: a failure to take the lock
     * must not fail the link.
     *
     * RE-ARMING IS PART OF THE CONTRACT, not an optimisation. A held lock is not necessarily a live
     * lock - Android releases a TIMED wake lock by itself when the timeout elapses and reports
     * nothing - so `acquire()` on an already-held lock must establish whether it is still really held
     * and take a fresh one if not. It is called on every pump tick, so it must be cheap when the lock
     * is comfortably valid.
     */
    void acquire();

    /** Give the lock back. Must be idempotent and safe when nothing is held. */
    void release();

    /** Whether the lock is held right now. Used by tests and by the status panel. */
    boolean isHeld();

    /** How many times acquire() actually took the lock. Observability, not policy. */
    long acquireCount();

    /** How many times release() actually gave it back. Observability, not policy. */
    long releaseCount();
}
