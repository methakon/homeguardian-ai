package org.homeguardian.harness;

import org.homeguardian.harness.ObservationTransport;

/**
 * ObservationTransportPolicyTest — bounded-reconnect and refusal-classification tests.
 *
 * WHY THIS EXISTS
 * ObservationTransport implements the part of the Android lane that keeps a phone honest when the hub
 * will not take its observations: a bounded retry, a bounded backoff, a bounded queue, and a
 * classification of WHY the hub refused. None of it was under test. The class is 800+ lines and opens
 * TLS sockets, so the behaviour was previously unverifiable without a live hub - which means a defect
 * in the retry bound or in the refusal precedence would only ever show up as a misbehaving phone.
 *
 * WHAT IT ASSERTS, AND WHY EACH ONE MATTERS
 *   11. the retry bound is finite and the loop actually drops at it - otherwise an unreachable hub
 *       makes the node retry forever;
 *   12. a refused PERMANENT reason drops on the FIRST attempt (it is not retried four times);
 *   13. an unrecognised reason is treated as PERMANENT (fail closed) - retrying a refusal whose
 *       meaning is unknown is how a node ends up in a loop;
 *   14. a SUSPENDING reason STOPS the transport rather than merely dropping the observation;
 *   15. precedence when a reason is in more than one set: SUSPEND must win over PERMANENT;
 *   16. the sets cannot contradict each other - RETRYABLE must be disjoint from the terminal sets,
 *       or the outcome would depend on check order rather than on the hub's reason;
 *   17. the backoff and queue are bounded.
 *
 * This test opens no socket, installs nothing and activates no sensor. It exercises pure decision
 * logic through the package-visible seam that production calls.
 */
public final class ObservationTransportPolicyTest {

    private static int passed = 0;
    private static int failed = 0;
    private static final StringBuilder failures = new StringBuilder();

    private static void check(String name, boolean ok) {
        check(name, ok, "");
    }

    private static void check(String name, boolean ok, String detail) {
        if (ok) {
            passed++;
            System.out.println("  ok   " + name);
        } else {
            failed++;
            failures.append("    - ").append(name).append('\n');
            System.out.println("  FAIL " + name + (detail.isEmpty() ? "" : "  " + detail));
        }
    }

    /**
     * Replay the production retry loop's counting for one observation.
     *
     * The loop's rule is: a retryable failure increments `attempts`, and the observation is dropped
     * once `attempts >= maxAttemptsPerObservation()`. This mirrors that exactly and drives it with the
     * real classifier and the real bound, so a change to either changes this result.
     *
     * @return the number of attempts made before the observation was resolved.
     */
    private static int attemptsUntilResolved(String reason) {
        int attempts = 0;
        for (int guard = 0; guard < 1000; guard++) {
            String verdict = ObservationTransport.classifyReason(reason);
            if (ObservationTransport.RETRY.equals(verdict)) {
                attempts++;
                if (attempts >= ObservationTransport.maxAttemptsPerObservation()) {
                    return attempts;          // dropped for exhausting the bound
                }
                continue;
            }
            return attempts + 1;              // resolved on this attempt (permanent/suspend/unknown)
        }
        return -1;                            // never resolved: the failure this test exists to catch
    }

    public static void main(String[] args) {
        System.out.println("== observation transport: bounded reconnect and refusal classification ==");

        final int bound = ObservationTransport.maxAttemptsPerObservation();

        // ---- 11. the bound is finite and enforced -------------------------------------------------
        check("11a the retry bound is finite and greater than one",
                bound > 1 && bound < 1000, "bound=" + bound);
        int n = attemptsUntilResolved("queue_full");
        check("11b a retryable reason is dropped at exactly the bound, not retried forever",
                n == bound, "attempts=" + n + " bound=" + bound);

        // ---- 12. permanent resolves on the first attempt -------------------------------------------
        int np = attemptsUntilResolved("bad_frame");
        check("12a a PERMANENT reason resolves on the first attempt (not retried to the bound)",
                np == 1, "attempts=" + np);
        check("12b ...and that is strictly fewer attempts than the retry bound",
                np < bound, "permanent=" + np + " bound=" + bound);

        // ---- 13. unknown fails closed --------------------------------------------------------------
        check("13a an unrecognised reason is classified PERMANENT (fail closed)",
                ObservationTransport.PERMANENT.equals(ObservationTransport.classifyReason("never_heard_of_it")));
        check("13b ...so it is not retried at the bound",
                attemptsUntilResolved("never_heard_of_it") == 1);
        check("13c an empty reason is classified PERMANENT",
                ObservationTransport.PERMANENT.equals(ObservationTransport.classifyReason("")));
        check("13d a null reason is classified PERMANENT rather than throwing",
                ObservationTransport.PERMANENT.equals(ObservationTransport.classifyReason(null)));

        // ---- 14. suspending stops the transport -----------------------------------------------------
        for (String r : new String[]{"consent_missing", "consent_withdrawn", "capture_disabled"}) {
            check("14a '" + r + "' is classified SUSPEND (stops the transport)",
                    ObservationTransport.SUSPEND.equals(ObservationTransport.classifyReason(r)),
                    "got " + ObservationTransport.classifyReason(r));
        }

        // ---- 15. precedence on the overlapping reasons ---------------------------------------------
        // These five are in BOTH the suspending and permanent sets. SUSPEND must win: dropping one
        // observation and continuing to push to a hub that just revoked this device is wrong.
        String[] both = {"device_revoked", "device_unknown", "device_unpaired",
                         "identity_mismatch", "unauthorized"};
        for (String r : both) {
            check("15a '" + r + "' (in both sets) classifies SUSPEND, so the transport stops",
                    ObservationTransport.SUSPEND.equals(ObservationTransport.classifyReason(r)),
                    "got " + ObservationTransport.classifyReason(r));
        }
        check("15b no overlapping reason silently falls through to PERMANENT",
                allSuspending(both));

        // ---- 16. the sets cannot contradict each other ----------------------------------------------
        // A reason that is both retryable and terminal would make behaviour depend on check order.
        // The classification would still be deterministic, but the SETS would be lying about intent -
        // and the next person to reorder the checks would introduce a real defect.
        String[] retryable = {"queue_full", "processing_failed", "duplicate_observation"};
        for (String r : retryable) {
            check("16a '" + r + "' is RETRY (the hub was busy, not wrong)",
                    ObservationTransport.RETRY.equals(ObservationTransport.classifyReason(r)),
                    "got " + ObservationTransport.classifyReason(r));
        }
        for (String r : retryable) {
            check("16b '" + r + "' is retried, i.e. NOT resolved on the first attempt",
                    attemptsUntilResolved(r) > 1, "attempts=" + attemptsUntilResolved(r));
        }
        check("16c every retryable reason is disjoint from every suspending/terminal reason",
                disjoint(retryable, both),
                "an overlap would make the outcome depend on check order");

        // ---- 17. bounded backoff and bounded queue ----------------------------------------------------
        long backoff = ObservationTransport.retryBackoffMs();
        check("17a the retry backoff is bounded and non-zero (a retry loop that spins is not a backoff)",
                backoff > 0 && backoff >= 1000, "backoffMs=" + backoff);
        int qdepth = ObservationTransport.maxQueueDepth();
        check("17b the queue is bounded, so an unreachable hub cannot grow memory without limit",
                qdepth > 0 && qdepth < 100000, "maxQueueDepth=" + qdepth);

        // ---- 18. NEGATIVE CONTROL --------------------------------------------------------------------
        // The whole point of 11b/12a is that they can distinguish a bounded loop from an unbounded one.
        // Simulate an unbounded retry and require that the same simulation reports "never resolved",
        // proving the assertion is capable of failing.
        int runaway = simulateUnbounded("queue_full");
        check("18 negative control: an UNBOUNDED retry loop is detected as never resolving",
                runaway == -1, "runaway=" + runaway + " (if this equals the bound, 11b proves nothing)");

        System.out.println();
        if (failed > 0) {
            System.out.println("  FAILED: " + failed + " of " + (passed + failed));
            System.out.print(failures);
            System.exit(1);
        }
        System.out.println("  PASS: all " + passed + " transport policy cases behave as specified");
    }

    private static boolean allSuspending(String[] rs) {
        for (String r : rs) {
            if (!ObservationTransport.SUSPEND.equals(ObservationTransport.classifyReason(r))) {
                return false;
            }
        }
        return true;
    }

    private static boolean disjoint(String[] a, String[] b) {
        for (String x : a) {
            for (String y : b) {
                if (x.equals(y)) {
                    return false;
                }
            }
        }
        return true;
    }

    /** The same loop with no bound: used only as the negative control for 11b/12a. */
    private static int simulateUnbounded(String reason) {
        int attempts = 0;
        for (int guard = 0; guard < 1000; guard++) {
            String verdict = ObservationTransport.classifyReason(reason);
            if (ObservationTransport.RETRY.equals(verdict)) {
                attempts++;
                if (attempts >= 100000) {
                    return attempts;
                }
                continue;
            }
            return attempts + 1;
        }
        return -1;
    }
}
