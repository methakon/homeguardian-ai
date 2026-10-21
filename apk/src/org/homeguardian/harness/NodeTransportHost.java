package org.homeguardian.harness;

import android.content.Context;
import android.util.Log;

/**
 * Owns the observation transport's LIFECYCLE on behalf of the foreground service.
 *
 * THE DEFECT THIS CLASS FIXES
 * The transport was created, started and pumped by {@code NodePanel}, which the Activity builds.
 * {@code MainActivity.onPause()} removes its ticker, so {@code sample()} stopped being called the
 * moment the screen went off. Capture kept running (the camera HAL holds the SoC) but the transport
 * went idle, so observations stopped leaving the device exactly when a headless node needs them.
 * Ownership now lives here, driven by {@code CctvService}'s own timer, and the Activity only
 * commands and observes.
 *
 * WAKE-LOCK SAFETY (Stage 1 item 8). Read this before changing anything below.
 *  - The lock is PARTIAL only. It keeps the CPU running and the screen may still turn off. It is
 *    NOT a screen-on lock and NOT an "keep the network up" guarantee.
 *  - It is acquired ONLY while a link is actually wanted. It is never taken at construction, never
 *    taken by a UI callback, and never taken "just in case".
 *  - It carries a TIMEOUT as a backstop. An explicit release is the normal path; the timeout exists
 *    so that a bug or a killed code path cannot pin the battery down indefinitely. A host that
 *    leaks a wake lock is worse than a host that misses one tick.
 *  - It is released on EVERY exit path: operator stop, consent withdrawal, authorization
 *    withdrawal, link-start failure, missing credentials, service destroy, and a failed acquire.
 *  - Holding it while idle is a bug, not a conservative default. {@link #isHeldForTest()} exists so
 *    tests can assert that.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *  - It never starts capture. Arming the camera or microphone is an operator action owned by
 *    {@code CctvService}; nothing here can arm a sensor as a side effect of connecting.
 *  - It never re-arms anything after a consent withdrawal. {@link #onConsentWithdrawn()} only
 *    tears down, and leaves a flag that {@link #clearConsentBlock()} must clear explicitly.
 *  - It does not parse, inspect, store or log observation payloads. It moves a string to the link.
 */
public final class NodeTransportHost {

    public static final String TAG = "HGNodeHost";

    /** How the host currently stands. Mirrored into static fields for the status panel. */
    public enum State { IDLE, CONNECTING, CONNECTED, SUSPENDED, BLOCKED_NO_CREDENTIALS, STOPPED }

    // ---- observable state (read by NodePanel; commands nothing) --------------------------
    private static volatile State sState = State.IDLE;
    private static volatile String sDetail = "never started";
    private static volatile String sLinkHost = "";
    private static volatile int sLinkPort = 0;
    private static volatile boolean sConsentBlocked = false;
    private static volatile long sPumpedTotal = 0L;
    private static volatile long sLastPumpAtMs = 0L;
    private static volatile String sLastStopReason = "never_started";

    /**
     * Counter mirror. The panel used to read the transport instance directly because it OWNED it.
     * It no longer does - the service does - so the host publishes counts here for the UI to read.
     * Counts only: never a payload, a device id, a credential or any media. This is display data;
     * nothing gates on it.
     */
    private static volatile long sQueued = 0L;
    private static volatile long sSent = 0L;
    private static volatile long sRefused = 0L;
    private static volatile long sDropped = 0L;
    private static volatile long sPermanent = 0L;
    private static volatile long sQueuedTotal = 0L;
    private static volatile long sParseFailures = 0L;
    private static volatile long sLastSentAtMs = 0L;

    public static long queuedCountStatic() { return sQueued; }
    public static long sentCountStatic() { return sSent; }
    public static long refusedCountStatic() { return sRefused; }
    public static long droppedCountStatic() { return sDropped; }
    public static long permanentRejectionCountStatic() { return sPermanent; }
    public static long queuedTotalStatic() { return sQueuedTotal; }
    public static long parseFailuresStatic() { return sParseFailures; }
    public static long lastSentAtMsStatic() { return sLastSentAtMs; }
    public static long pumpedTotalStatic() { return sPumpedTotal; }

    private void publishCounters() {
        sQueued = link.queuedCount();
        sSent = link.sentCount();
        sRefused = link.refusedCount();
        sDropped = link.droppedCount();
        sPermanent = link.permanentRejectionCount();
        sQueuedTotal = link.queuedTotal();
        sParseFailures = link.parseFailures();
        sLastSentAtMs = link.lastSentAtMs();
    }

    public static State state() { return sState; }
    public static String detail() { return sDetail; }
    public static String linkHost() { return sLinkHost; }
    public static int linkPort() { return sLinkPort; }
    public static boolean consentBlocked() { return sConsentBlocked; }
    public static long pumpedTotal() { return sPumpedTotal; }
    public static long lastPumpAtMs() { return sLastPumpAtMs; }

    // ---- instance state -----------------------------------------------------------------
    private final ObservationLink link;
    private final WakeLockHolder wakeLock;
    private volatile boolean running = false;   // link currently wanted
    public static String lastStopReason() { return sLastStopReason; }

    /** Production construction: builds the real link and a real (partial, timeout-bounded) lock. */
    public NodeTransportHost(Context ctx, String deviceId, String displayName,
                             String host, int port) {
        this(new SessionObservationLink(ctx, deviceId, displayName, host, port), new AndroidWakeLock(ctx));
        noteEndpoint(host, port);
    }

    /**
     * Test construction: inject any {@link ObservationLink} and any {@link WakeLockHolder}. This is
     * what lets the whole lifecycle - start, refusal, pump, consent withdrawal, stop, failure - be
     * exercised on a plain JVM with no Android, no socket, no certificate and no phone.
     */
    public NodeTransportHost(ObservationLink link, WakeLockHolder wakeLock) {
        this.link = link;
        this.wakeLock = wakeLock == null ? new WakeLockHolder() {
            @Override public void acquire() { }
            @Override public void release() { }
            @Override public boolean isHeld() { return false; }
            @Override public long acquireCount() { return 0; }
            @Override public long releaseCount() { return 0; }
        } : wakeLock;
    }

    // ------------------------------------------------------------------ lifecycle

    /**
     * Begin the link. Acquires the wake lock only AFTER the link is confirmed startable, so a
     * request that cannot proceed never takes the lock.
     */
    public synchronized String start() {
        if (running) {
            sDetail = "already running";
            sState = State.CONNECTED;
            return sDetail;
        }
        if (sConsentBlocked) {
            // Consent was withdrawn and has not been explicitly restored. Refuse, and say why.
            sState = State.STOPPED;
            sDetail = "refused: consent was withdrawn and not restored";
            releaseWakeLock("start refused (consent blocked)");
            return sDetail;
        }
        if (!link.credentialsPresent()) {
            // Fail closed, exactly as NodePanel did. No credentials, no link.
            sState = State.BLOCKED_NO_CREDENTIALS;
            sDetail = "BLOCKED: no device credentials in app storage (provisioning required)";
            releaseWakeLock("start refused (no credentials)");
            Log.w(TAG, sDetail);
            return sDetail;
        }

        acquireWakeLock();
        running = true;
        sState = State.CONNECTING;
        sDetail = "connecting";
        try {
            link.start();
        } catch (RuntimeException e) {
            // A failed start must not leave the lock held or the flag set.
            running = false;
            sState = State.STOPPED;
            sDetail = "start failed: " + e.getClass().getSimpleName();
            releaseWakeLock("start threw");
            Log.e(TAG, sDetail, e);
            return sDetail;
        }
        sDetail = "link started";
        Log.i(TAG, sDetail);
        return sDetail;
    }

    /** Every exit path funnels through here, so the lock cannot be leaked by an unusual one. */
    public synchronized void stop(String reason) {
        if (running) {
            try {
                link.stop();
            } catch (RuntimeException e) {
                Log.w(TAG, "link.stop() threw: " + e);
            }
        }
        running = false;
        sLastStopReason = reason == null ? "unknown" : reason;
        sState = State.STOPPED;
        sDetail = "stopped: " + sLastStopReason;
        releaseWakeLock("stop: " + sLastStopReason);
        Log.i(TAG, sDetail);
    }

    /** Drive one batch of the native drain. Called by the SERVICE's scheduler, never a UI tick. */
    public synchronized void pump(String ndjson) {
        if (!running) {
            return;
        }
        try {
            link.pump(ndjson);
        } catch (RuntimeException e) {
            Log.w(TAG, "link.pump() threw: " + e);
            // A throwing link is a failing link: tear it down rather than spin on it.
            stop("link threw during pump");
            return;
        }
        sPumpedTotal++;
        sLastPumpAtMs = System.currentTimeMillis();
        publishCounters();
        if (link.isConnected()) {
            sState = State.CONNECTED;
        } else if (link.isSuspended()) {
            sState = State.SUSPENDED;
            sDetail = "suspended by hub: " + link.suspensionReason();
        } else {
            sState = State.CONNECTING;
        }
    }

    /**
     * Consent for at least one sensor was withdrawn. Stop sending and release everything.
     *
     * The flag is what makes this safe: the host stays blocked until {@link #clearConsentBlock()}
     * is called explicitly by an operator action, so a service restart or a stray reconnect cannot
     * silently resume transmission after a withdrawal.
     */
    public synchronized void onConsentWithdrawn() {
        sConsentBlocked = true;
        try {
            link.onAuthorizationWithdrawn();
        } catch (RuntimeException e) {
            Log.w(TAG, "link.onAuthorizationWithdrawn() threw: " + e);
        }
        stop("consent withdrawn");
        sConsentBlocked = true;   // stop() does not touch this; restate for clarity
        sDetail = "stopped: consent withdrawn (blocked until explicitly restored)";
    }

    /** Explicit operator restore after a withdrawal. The ONLY way out of the blocked state. */
    public synchronized void clearConsentBlock() {
        sConsentBlocked = false;
        try {
            link.resumeAfterAuthorization();
        } catch (RuntimeException e) {
            Log.w(TAG, "link.resumeAfterAuthorization() threw: " + e);
        }
        sDetail = "consent block cleared";
    }

    public synchronized void setDeviceId(String deviceId) {
        link.setDeviceId(deviceId);
    }

    // ------------------------------------------------------------------ observation accessors

    public boolean isRunning() { return running; }
    public boolean isConnected() { return link.isConnected(); }
    public boolean isSuspended() { return link.isSuspended(); }
    public String suspensionReason() { return link.suspensionReason(); }
    public long queuedCount() { return link.queuedCount(); }
    public long sentCount() { return link.sentCount(); }
    public long refusedCount() { return link.refusedCount(); }
    public long droppedCount() { return link.droppedCount(); }
    public long permanentRejectionCount() { return link.permanentRejectionCount(); }
    public long queuedTotal() { return link.queuedTotal(); }
    public long parseFailures() { return link.parseFailures(); }
    public long lastSentAtMs() { return link.lastSentAtMs(); }

    /** Compact, sanitized line for the acceptance log. Counts only - never a payload. */
    public String counters() {
        return "transport queued=" + link.queuedCount()
                + " sent=" + link.sentCount()
                + " refused=" + link.refusedCount()
                + " dropped=" + link.droppedCount()
                + " permanent=" + link.permanentRejectionCount()
                + " pumped=" + sPumpedTotal;
    }

    public String statusLine() {
        return "state=" + sState + " host=" + sLinkHost + ":" + sLinkPort
                + " consent_blocked=" + sConsentBlocked
                + " wake_lock=" + wakeLock.isHeld()
                + " detail=" + sDetail;
    }

    // ------------------------------------------------------------------ wake lock

    /** Record where we are dialling, for the status panel. Not identity - display only. */
    public void noteEndpoint(String host, int port) {
        sLinkHost = host == null ? "" : host;
        sLinkPort = port;
    }

    private void acquireWakeLock() {
        wakeLock.acquire();
    }

    private void releaseWakeLock(String why) {
        wakeLock.release();
    }

    /** Test/observability hook: is the wake lock held right now? */
    public boolean isHeldForTest() { return wakeLock.isHeld(); }

    /** How many times the lock was actually taken. Mirrors the holder. */
    public long wakeLockAcquireCount() { return wakeLock.acquireCount(); }

    /** How many times it was actually given back. Mirrors the holder. */
    public long wakeLockReleaseCount() { return wakeLock.releaseCount(); }
}
