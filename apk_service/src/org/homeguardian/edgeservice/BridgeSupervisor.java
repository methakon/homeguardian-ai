package org.homeguardian.edgeservice;

/**
 * Bounded retry policy for the outbound control bridge.
 *
 * WHY THIS EXISTS
 * ---------------
 * The bridge is refused while no VPN tunnel is up, and a phone restart brings the
 * service up long before (or without) a tunnel. The service used to attempt the
 * bridge exactly once, at engine start, so a refusal at that instant left the
 * bridge stopped for the rest of the process lifetime even after the tunnel
 * returned. Observed as a silent remote outage: authenticated reads answered
 * 503 while the local service looked healthy.
 *
 * The fix is a retry, but not a fast one: retrying tightly on a phone burns radio
 * and battery and floods the probe log. This class owns only the arithmetic and
 * the classification, with no Android dependency, so the policy is unit-testable
 * on a host JVM and the service keeps the threading.
 *
 * Rules:
 *   - Backoff grows and is capped, so a long outage costs one attempt per cap.
 *   - Retry only while the bridge is wanted, not already running, and the service
 *     is not stopping: an operator's Stop must not be undone by the supervisor.
 *   - A reason string is classified so troubleshooting can name the cause instead
 *     of reporting "bridge down".
 */
public final class BridgeSupervisor {

    /** Growth of the delay between attempts, in milliseconds. The last entry repeats. */
    static final long[] DELAYS_MS = {5_000L, 15_000L, 45_000L, 120_000L, 300_000L};

    private int attempts;

    public synchronized void reset() {
        attempts = 0;
    }

    public synchronized int attempts() {
        return attempts;
    }

    /** Delay before the next attempt. Each call consumes one attempt slot. */
    public synchronized long nextDelayMs() {
        long delay = DELAYS_MS[Math.min(attempts, DELAYS_MS.length - 1)];
        if (attempts < Integer.MAX_VALUE) attempts++;
        return delay;
    }

    /**
     * A tunnel name this app may ask WireGuard to raise. Bounded and restricted to a plain
     * profile name: the value is placed in an Intent extra, so anything exotic is refused here
     * rather than sanitised later.
     */
    private static final java.util.regex.Pattern TUNNEL_NAME =
            java.util.regex.Pattern.compile("[A-Za-z0-9 ._-]{1,64}");

    public static boolean validTunnelName(String name) {
        return name != null && TUNNEL_NAME.matcher(name).matches();
    }

    /** Whether the supervisor is allowed to try at all in this state. */
    public static boolean shouldAttempt(boolean wanted, boolean running, boolean stopping) {
        return wanted && !running && !stopping;
    }

    /**
     * Reduces a refusal/status string to a stable category. Categories are for
     * diagnostics only and never carry a URL, token or address.
     */
    public static String classify(String reason) {
        if (reason == null || reason.length() == 0) return "unknown";
        String r = reason.toLowerCase(java.util.Locale.US);
        if (r.contains("no vpn") || r.contains("vpn") && (r.contains("down") || r.contains("not active"))) {
            return "vpn_unavailable";
        }
        if (r.contains("401") || r.contains("unauthor") || r.contains("auth")) return "auth_refused";
        if (r.contains("timeout") || r.contains("timed out") || r.contains("connection reset")) return "timeout";
        // Checked before the generic refusal: "refused - no coordinator configured" is a
        // configuration gap, not a network refusal, and the two need different operator action.
        if (r.contains("no coordinator") || r.contains("not configured")) return "not_configured";
        if (r.contains("refused") || r.contains("reject")) return "refused";
        return "other";
    }
}
