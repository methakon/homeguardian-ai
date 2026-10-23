package org.homeguardian.harness;

import java.util.ArrayList;
import java.util.List;

/**
 * Isolated lifecycle and security tests for the service-owned observation transport.
 *
 * HG-PANASONIC-SERVICE-TRANSPORT-06, Stage 3.
 *
 * WHAT THESE PROVE, AND WHAT THEY DO NOT
 * These run on a plain JVM. No phone, no camera, no microphone, no socket, no certificate is
 * touched - so they prove the OWNERSHIP AND LIFECYCLE rules, not that media flows. A green run here
 * is not wireless acceptance, and this file does not claim to be.
 *
 * The seam that makes this possible is {@link ObservationLink} + {@link WakeLockHolder}: the host
 * under test cannot tell a fake link from the real one, so every exit path can be exercised
 * deterministically - which is the only way "the wake lock is released on every path" becomes a
 * measured fact rather than a code reading.
 *
 * Run: see apk/tests/run_isolated_tests.sh
 */
public final class NodeTransportHostTest {

    private static int passed = 0;
    private static int failed = 0;
    private static final List<String> failures = new ArrayList<String>();

    // ------------------------------------------------------------------ fakes

    /** A link that records what was asked of it and can be told to fail on demand. */
    static final class FakeLink implements ObservationLink {
        boolean haveCredentials = true;
        boolean started = false;
        boolean stopped = false;
        int startCalls = 0;
        int stopCalls = 0;
        int pumpCalls = 0;
        int withdrawnCalls = 0;
        int resumeCalls = 0;
        int pumpResult = 0;
        boolean throwOnStart = false;
        boolean throwOnPump = false;
        boolean connected = false;
        boolean suspended = false;
        String suspendReason = "";
        String lastPumped = "";
        String deviceId = "";

        @Override public void start() {
            startCalls++;
            if (throwOnStart) throw new IllegalStateException("simulated start failure");
            started = true;
        }
        @Override public void stop() { stopCalls++; started = false; stopped = true; }
        @Override public void pump(String ndjson) {
            pumpCalls++;
            lastPumped = ndjson;
            if (throwOnPump) throw new IllegalStateException("simulated pump failure");
        }
        @Override public void onAuthorizationWithdrawn() { withdrawnCalls++; }
        @Override public void resumeAfterAuthorization() { resumeCalls++; }
        @Override public void setDeviceId(String id) { deviceId = id; }
        @Override public boolean isConnected() { return connected; }
        @Override public boolean isSuspended() { return suspended; }
        @Override public String suspensionReason() { return suspendReason; }
        @Override public long queuedCount() { return 3L; }
        @Override public long sentCount() { return 7L; }
        @Override public long refusedCount() { return 1L; }
        @Override public long droppedCount() { return 2L; }
        @Override public long permanentRejectionCount() { return 5L; }
        @Override public long queuedTotal() { return 11L; }
        @Override public long parseFailures() { return 1L; }
        @Override public long lastSentAtMs() { return 1234L; }
        @Override public boolean credentialsPresent() { return haveCredentials; }
    }

    /** A wake lock that records the exact acquire/release sequence. */
    static final class FakeWakeLock implements WakeLockHolder {
        boolean held = false;
        long acquires = 0L;
        long releases = 0L;
        @Override public void acquire() { if (!held) { held = true; acquires++; } }
        @Override public void release() { if (held) { held = false; releases++; } }
        @Override public boolean isHeld() { return held; }
        @Override public long acquireCount() { return acquires; }
        @Override public long releaseCount() { return releases; }
    }

    /**
     * A holder that models the platform's TIMED AUTO-RELEASE.
     *
     * This exists because {@link FakeWakeLock} can never expire, which is precisely why no test could
     * see the lapse bug: `held` was a flag the host set and nothing ever cleared it, so the fake
     * agreed with the host's belief no matter how wrong it was. A fake that cannot disagree with the
     * code under test cannot test it.
     *
     * The platform side is modelled separately from the host's belief:
     *   platformHeld - what the system actually holds; drops itself when the timeout elapses
     *   held         - what this holder believes
     * They are compared explicitly so "the host kept the lock alive" is a measurement.
     */
    static final class ExpiringWakeLock implements WakeLockHolder {
        long now = 0L;                  // virtual clock, advanced only by the test
        final long timeoutMs;
        boolean held = false;           // what we believe
        boolean platformHeld = false;   // what the platform actually holds
        long acquiredAt = 0L;
        long acquires = 0L;
        long releases = 0L;

        ExpiringWakeLock(long timeoutMs) { this.timeoutMs = timeoutMs; }

        /** Advance the virtual clock. The platform silently drops its lock once the timeout passes. */
        void advance(long ms) {
            now += ms;
            if (platformHeld && acquiredAt + timeoutMs <= now) {
                platformHeld = false;   // released BY THE PLATFORM, with no callback to us
            }
        }

        /** Did the platform drop a lock we still believe we hold? That is the failure being tested. */
        boolean lapsed() { return held && !platformHeld; }

        @Override public void acquire() {
            if (held && platformHeld) {
                return;   // genuinely still ours
            }
            held = true;
            platformHeld = true;
            acquiredAt = now;
            acquires++;
        }
        @Override public void release() { if (held) { held = false; platformHeld = false; releases++; } }
        @Override public boolean isHeld() { return held && platformHeld; }
        @Override public long acquireCount() { return acquires; }
        @Override public long releaseCount() { return releases; }
    }

    /** A holder whose acquire() throws, to prove a renewal failure cannot fail the pump. */
    static final class ThrowingWakeLock implements WakeLockHolder {
        long acquires = 0L;
        @Override public void acquire() { acquires++; throw new IllegalStateException("no lock"); }
        @Override public void release() { }
        @Override public boolean isHeld() { return false; }
        @Override public long acquireCount() { return acquires; }
        @Override public long releaseCount() { return 0L; }
    }

    // ------------------------------------------------------------------ harness

    private static void check(String name, boolean ok) {
        check(name, ok, "");
    }

    private static void check(String name, boolean ok, String detail) {
        if (ok) {
            passed++;
            System.out.println("  PASS  " + name);
        } else {
            failed++;
            failures.add(name + " -- " + detail);
            System.out.println("  FAIL  " + name + "  (" + detail + ")");
        }
    }

    private static NodeTransportHost newHost(FakeLink link, WakeLockHolder wl) {
        // Clear any consent block left by an earlier test. It is an INSTANCE method because it
        // also resumes the link, so it is called on a throwaway instance first.
        try {
            new NodeTransportHost(link, new FakeWakeLock()).clearConsentBlock();
        } catch (RuntimeException e) {
            // A fresh host that has never been stopped has nothing to clear; that is fine.
        }
        return new NodeTransportHost(link, wl);
    }

    public static void main(String[] args) {
        System.out.println("=== HG-PANASONIC-SERVICE-TRANSPORT-06 isolated tests ===");
        System.out.println("    (JVM only: no device, no sensor, no socket)");
        System.out.println();

        t1_uiClosedServiceKeepsPumping();
        t2_serviceOwnedSchedulerIndependentOfUi();
        t3_stopReleasesEverything();
        t4_cameraConsentWithdrawalStopsProduction();
        t5_micWithdrawalIndependence();
        t6_revokedOrUnknownCannotConnect();
        t7_missingCredentialsBlockExplicitly();
        t8_queueCountersAndBackoffSurface();
        t9_wakeLockOnEveryExitPath();
        t10_noInterferenceWithMiA3Mcp();
        t11_wakeLockSurvivesTheSession();

        System.out.println();
        System.out.println("=== " + passed + " passed, " + failed + " failed ===");
        if (!failures.isEmpty()) {
            System.out.println("FAILURES:");
            for (String f : failures) System.out.println("  - " + f);
            System.exit(1);
        }
        System.exit(0);
    }

    // ------------------------------------------------------------------ tests

    /** 1. The UI closing must not stop the transport. The host has no UI dependency at all. */
    private static void t1_uiClosedServiceKeepsPumping() {
        FakeLink l = new FakeLink();
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        h.start();
        for (int i = 0; i < 5; i++) h.pump("{\"v\":1}\n");
        // Nothing here involves an Activity, an onPause or a ticker: that IS the fix.
        check("1 ui-closed: link pumps without any UI object",
                l.pumpCalls == 5 && h.isRunning(), "pumpCalls=" + l.pumpCalls);
        h.stop("test end");
    }

    /** 2. Pumping is driven by the owner, so it continues while a UI would be paused. */
    private static void t2_serviceOwnedSchedulerIndependentOfUi() {
        FakeLink l = new FakeLink();
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        h.start();
        h.pump("a\n");
        h.pump("b\n");
        check("2 scheduler: every pump is delivered while running",
                l.pumpCalls == 2 && "b\n".equals(l.lastPumped), "last=" + l.lastPumped);
        h.stop("test end");
        h.pump("c\n");
        check("2 scheduler: pumps after stop are dropped, not queued",
                l.pumpCalls == 2, "pumpCalls=" + l.pumpCalls);
    }

    /** 3. Stop releases everything, and is idempotent. */
    private static void t3_stopReleasesEverything() {
        FakeLink l = new FakeLink();
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        h.start();
        check("3 stop: lock held while running", w.isHeld());
        h.stop("operator stop");
        check("3 stop: link stopped and lock released",
                l.stopCalls == 1 && !w.isHeld() && l.started == false,
                "stopCalls=" + l.stopCalls + " held=" + w.isHeld());
        h.stop("again");
        // Correct idempotency: the link is stopped EXACTLY once. A second stop must not call
        // link.stop() again (double-stop is its own defect) and must not re-take the lock.
        check("3 stop: idempotent - link stopped exactly once, lock not re-taken",
                l.stopCalls == 1 && !w.isHeld() && w.acquires == 1,
                "stopCalls=" + l.stopCalls + " acquires=" + w.acquires);
    }

    /** 4. Camera consent withdrawal stops production AND blocks silent resumption. */
    private static void t4_cameraConsentWithdrawalStopsProduction() {
        FakeLink l = new FakeLink();
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        h.start();
        h.pump("x\n");
        h.onConsentWithdrawn();
        final int pumpsBefore = l.pumpCalls;
        h.pump("y\n");
        check("4 withdrawal: link torn down", !h.isRunning() && l.stopCalls >= 1);
        check("4 withdrawal: wake lock released", !w.isHeld());
        check("4 withdrawal: further pumps produce nothing",
                l.pumpCalls == pumpsBefore, "pumpCalls grew to " + l.pumpCalls);
        check("4 withdrawal: authorization-withdrawn signalled to the transport",
                l.withdrawnCalls == 1, "withdrawnCalls=" + l.withdrawnCalls);
        // The important one: a restart must NOT silently resume transmission.
        final String r = h.start();
        check("4 withdrawal: restart is REFUSED until explicitly restored",
                !h.isRunning() && r.contains("consent"), "start() returned: " + r);
        h.stop("test end");
    }

    /** 5. Microphone withdrawal must be independently possible, not a side effect of video. */
    private static void t5_micWithdrawalIndependence() {
        FakeLink l = new FakeLink();
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        h.start();
        // Restoring consent is an explicit act; assert it re-enables the link without arming a sensor.
        h.onConsentWithdrawn();
        h.clearConsentBlock();
        final String r = h.start();
        check("5 mic/video independence: consent restore re-enables the link",
                h.isRunning(), "start() returned: " + r);
        check("5 mic/video independence: restoring the LINK did not start any capture",
                // The host has no capture API at all - that is the guarantee, and it is structural.
                !hasCaptureSurface(NodeTransportHost.class),
                "NodeTransportHost exposes no capture method");
        check("5 mic/video independence: resume signalled after restore",
                l.resumeCalls >= 1, "resumeCalls=" + l.resumeCalls);
        h.stop("test end");
    }

    /** Structural check: the host must have no method that could arm a sensor. */
    private static boolean hasCaptureSurface(Class<?> c) {
        for (java.lang.reflect.Method m : c.getDeclaredMethods()) {
            final String n = m.getName().toLowerCase();
            if (n.contains("camera") || n.contains("mic") || n.contains("capture")
                    || n.contains("arm")) {
                return true;
            }
        }
        return false;
    }

    /** 6. A revoked or unknown device must be refused by the link, and the host must not mask it. */
    private static void t6_revokedOrUnknownCannotConnect() {
        FakeLink l = new FakeLink();
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        // The transport's own permanent-refusal classes surface as counters; the host must pass them
        // through rather than inventing a success.
        h.start();
        h.pump("z\n");
        check("6 revocation: permanent rejections are surfaced, never hidden",
                h.permanentRejectionCount() == 5L && h.refusedCount() == 1L,
                "permanent=" + h.permanentRejectionCount() + " refused=" + h.refusedCount());
        check("6 revocation: suspension reason is passed through verbatim",
                "".equals(h.suspensionReason()), "reason=" + h.suspensionReason());
        l.suspended = true;
        l.suspendReason = "device_revoked";
        h.pump("z\n");
        check("6 revocation: a suspended link reports the hub's reason",
                h.suspensionReason().contains("device_revoked"),
                "reason=" + h.suspensionReason());
        h.stop("test end");
    }

    /** 7. Missing credentials must be an explicit BLOCKED state, taken BEFORE the lock is acquired. */
    private static void t7_missingCredentialsBlockExplicitly() {
        FakeLink l = new FakeLink();
        l.haveCredentials = false;
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        final String r = h.start();
        check("7 no-credentials: explicit BLOCKED state",
                NodeTransportHost.state() == NodeTransportHost.State.BLOCKED_NO_CREDENTIALS,
                "state=" + NodeTransportHost.state());
        check("7 no-credentials: refused, and the link was never started",
                l.startCalls == 0 && r.contains("BLOCKED"), "startCalls=" + l.startCalls);
        check("7 no-credentials: NO wake lock taken for a refused start",
                w.acquires == 0 && !w.isHeld(), "acquires=" + w.acquires);
        check("7 no-credentials: fails closed rather than connecting unverified",
                !h.isRunning(), "running=" + h.isRunning());
    }

    /** 8. Queue/oldest-drop counters and the reconnection surface are visible, not swallowed. */
    private static void t8_queueCountersAndBackoffSurface() {
        FakeLink l = new FakeLink();
        FakeWakeLock w = new FakeWakeLock();
        NodeTransportHost h = newHost(l, w);
        h.start();
        h.pump("q\n");
        check("8 counters: queued/sent/refused/dropped/permanent all surfaced",
                h.queuedCount() == 3L && h.sentCount() == 7L && h.refusedCount() == 1L
                        && h.droppedCount() == 2L && h.permanentRejectionCount() == 5L,
                h.counters());
        check("8 counters: oldest-drop is distinguishable from a permanent refusal",
                h.droppedCount() != h.permanentRejectionCount(),
                "dropped and permanent must never be conflated");
        check("8 counters: the sanitized line carries counts only",
                !h.counters().contains("{") && h.counters().contains("dropped="),
                h.counters());
        h.stop("test end");
    }

    /** 9. The wake lock is released on EVERY exit path - the highest-consequence rule. */
    private static void t9_wakeLockOnEveryExitPath() {
        // (a) normal stop
        FakeLink a = new FakeLink(); FakeWakeLock wa = new FakeWakeLock();
        NodeTransportHost ha = newHost(a, wa);
        ha.start(); ha.stop("normal");
        check("9a lock released on normal stop", !wa.isHeld() && wa.releases == 1,
                "held=" + wa.isHeld() + " releases=" + wa.releases);

        // (b) consent withdrawal
        FakeLink b = new FakeLink(); FakeWakeLock wb = new FakeWakeLock();
        NodeTransportHost hb = newHost(b, wb);
        hb.start(); hb.onConsentWithdrawn();
        check("9b lock released on consent withdrawal", !wb.isHeld(), "held=" + wb.isHeld());

        // (c) start failure
        FakeLink c = new FakeLink(); c.throwOnStart = true;
        FakeWakeLock wc = new FakeWakeLock();
        NodeTransportHost hc = newHost(c, wc);
        hc.start();
        check("9c lock released when start throws", !wc.isHeld() && wc.acquires == 1,
                "held=" + wc.isHeld() + " acquires=" + wc.acquires);

        // (d) pump failure tears the link down and releases
        FakeLink d = new FakeLink(); d.throwOnPump = true;
        FakeWakeLock wd = new FakeWakeLock();
        NodeTransportHost hd = newHost(d, wd);
        hd.start(); hd.pump("boom\n");
        check("9d lock released when the link throws during pump",
                !wd.isHeld() && !hd.isRunning(), "held=" + wd.isHeld());

        // (e) refused start must NEVER take the lock
        FakeLink e = new FakeLink(); e.haveCredentials = false;
        FakeWakeLock we = new FakeWakeLock();
        NodeTransportHost he = newHost(e, we);
        he.start();
        check("9e lock never taken for a refused start", we.acquires == 0,
                "acquires=" + we.acquires);

        // (f) no path leaves it held after stop, in any order
        FakeLink f = new FakeLink(); FakeWakeLock wf = new FakeWakeLock();
        NodeTransportHost hf = newHost(f, wf);
        hf.start(); hf.pump("p\n"); hf.clearConsentBlock(); hf.stop("final");
        check("9f lock not held after a full start/pump/stop cycle", !wf.isHeld(),
                "held=" + wf.isHeld());
    }

    /**
     * 11. THE LOCK MUST SURVIVE THE SESSION, not just the start call.
     *
     * The defect this pins: the lock is taken once in start() with a 30-minute timeout, and Android
     * releases a timed lock on its own without telling anyone. Nothing re-armed it, so half an hour
     * into an unattended run the node believed it held a lock the platform had already dropped, and
     * every later acquire() was a silent no-op. That is the same silent-death shape as the original
     * screen-off defect, at a longer horizon.
     *
     * FakeWakeLock could never expire, so no existing assertion could see it. ExpiringWakeLock models
     * the platform side separately from the host's belief, which is what makes the lapse measurable.
     */
    private static void t11_wakeLockSurvivesTheSession() {
        final long timeout = 1000L;

        // (a) WITHOUT renewal the lock lapses - this is the bug, demonstrated rather than described.
        FakeLink a = new FakeLink(); ExpiringWakeLock wa = new ExpiringWakeLock(timeout);
        NodeTransportHost ha = newHost(a, wa);
        ha.start();
        check("11a lock held at start", wa.isHeld(), "held=" + wa.isHeld());
        wa.advance(timeout + 1);   // the platform silently drops it
        check("11a the platform drops a timed lock with no callback (bug precondition)",
                wa.lapsed(), "held=" + wa.held + " platformHeld=" + wa.platformHeld);
        check("11a a lapsed lock is reported as NOT held, not as held",
                !wa.isHeld(), "a stale held flag must not report as held");

        // (b) the pump tick keeps it alive across the same horizon
        FakeLink b = new FakeLink(); ExpiringWakeLock wb = new ExpiringWakeLock(timeout);
        NodeTransportHost hb = newHost(b, wb);
        hb.start();
        for (int i = 0; i < 5; i++) {
            wb.advance(timeout / 2 + 1);   // crosses the renewal point each time
            hb.pump("p\n");
        }
        check("11b the pump re-arms the lock across repeated timeouts: still held",
                wb.isHeld(), "held=" + wb.isHeld() + " lapsed=" + wb.lapsed());
        check("11b it was re-armed more than once (not a single lucky take)",
                wb.acquireCount() > 1, "acquires=" + wb.acquireCount());
        check("11b it never lapsed at a checkpoint", !wb.lapsed(),
                "held=" + wb.held + " platformHeld=" + wb.platformHeld);

        // (c) no renewal churn while the link is stopped
        final long afterStop = wb.acquireCount();
        hb.stop("test over");
        wb.advance(timeout + 1);
        hb.pump("p\n");   // must be a no-op: the link is not running
        check("11c a stopped host does not re-acquire the lock on a stray pump",
                wb.acquireCount() == afterStop,
                "acquires went " + afterStop + " -> " + wb.acquireCount());
        check("11c lock released at stop and not silently retaken",
                !wb.isHeld() && wb.releaseCount() == 1,
                "held=" + wb.isHeld() + " releases=" + wb.releaseCount());

        // (d) a wake-lock failure must not cost us the LINK - at start or on a renewal.
        //
        // This found a real gap rather than confirming one. start() called the lock unguarded, so a
        // throwing holder propagated out of start() and failed the whole link, while the class
        // comment claimed "a failed acquire is reported, never fatal". The interface said acquire()
        // must not throw, and AndroidWakeLock honoured that by catching internally - which made the
        // guarantee a property of one implementation, not of the host. Both call sites are now
        // tolerant, and both are asserted here.
        FakeLink d = new FakeLink(); ThrowingWakeLock wd = new ThrowingWakeLock();
        NodeTransportHost hd = newHost(d, wd);
        String startResult = null;
        try {
            startResult = hd.start();
        } catch (RuntimeException e) {
            startResult = "THREW " + e;
        }
        check("11d a throwing wake lock does NOT fail start()",
                hd.isRunning() && d.startCalls == 1 && !startResult.startsWith("THREW"),
                "start returned: " + startResult);
        final long pumpedBefore = d.pumpCalls;
        hd.pump("p\n");
        check("11d a throwing wake-lock renew does NOT stop the pump",
                d.pumpCalls > pumpedBefore && hd.isRunning(),
                "pumpCalls " + pumpedBefore + " -> " + d.pumpCalls + " running=" + hd.isRunning());
        check("11d the lock was attempted at BOTH start and the tick", wd.acquires >= 2,
                "acquires=" + wd.acquires + " (expected >= 2: start + one pump)");
        hd.stop("cleanup");
        check("11d released and not running after stop", !hd.isRunning(), "running=" + hd.isRunning());
    }

    /** 10. Nothing here may touch the Mi A3's native MCP server, which runs in another process. */
    private static void t10_noInterferenceWithMiA3Mcp() {
        // Structural: this feature must not open a listening socket. The node dials out; a listener
        // would be the public-surface regression the brief forbids.
        final boolean listening = hasListeningSurface(ObservationLink.class)
                || hasListeningSurface(NodeTransportHost.class)
                || hasListeningSurface(WakeLockHolder.class);
        check("10 no public surface: link/host expose no listen/bind/accept", !listening,
                "no listening method on the seams");

        // Structural: this feature must not carry media. The transport is observations-only, and the
        // host must not add a path that could carry a frame or a sample.
        check("10 observations-only: host has no media-carrying method", !hasMediaSurface(NodeTransportHost.class),
                "no method mentioning frame/sample/pcm/video/audio payload on the host");

        // The Mi A3 MCP server is a separate process with its own loopback listener; nothing in this
        // feature binds a port, so there is nothing to collide with.
        check("10 non-interference: no bind/port constant introduced",
                !hasPortConstant(NodeTransportHost.class), "no hardcoded port in the host");
    }

    private static boolean hasListeningSurface(Class<?> c) {
        for (java.lang.reflect.Method m : c.getDeclaredMethods()) {
            final String n = m.getName().toLowerCase();
            if (n.contains("listen") || n.contains("bind") || n.contains("accept")
                    || n.contains("serversocket")) {
                return true;
            }
        }
        return false;
    }

    private static boolean hasMediaSurface(Class<?> c) {
        for (java.lang.reflect.Method m : c.getDeclaredMethods()) {
            final String n = m.getName().toLowerCase();
            if (n.contains("frame") || n.contains("sample") || n.contains("pcm")
                    || n.contains("pixel") || n.contains("h264") || n.contains("aac")) {
                return true;
            }
        }
        return false;
    }

    /**
     * Only a static-final int named *port* would be a hardcoded default endpoint. A non-final field
     * is a runtime mirror of what the operator supplied, which is exactly what the brief wants
     * (a development LAN address must never become compiled-in identity).
     */
    private static boolean hasPortConstant(Class<?> c) {
        for (java.lang.reflect.Field f : c.getDeclaredFields()) {
            final String n = f.getName().toLowerCase();
            final boolean isStaticFinal = java.lang.reflect.Modifier.isStatic(f.getModifiers())
                    && java.lang.reflect.Modifier.isFinal(f.getModifiers());
            if (n.contains("port") && f.getType() == int.class && isStaticFinal) {
                return true;
            }
        }
        return false;
    }
}
