package org.homeguardian.edgeservice;

import java.util.concurrent.CopyOnWriteArrayList;

/**
 * In-process state shared between the service and the UI.
 *
 * Deliberately a plain static holder rather than a broadcast: everything lives in
 * one process, and this avoids adding a dependency or an exported component that
 * could be triggered from outside the app.
 */
public final class EdgeState {

    public interface Listener {
        void onEdgeStateChanged();
    }

    /** Service lifecycle as the operator sees it. */
    public enum Phase {
        STOPPED, STARTING, RUNNING, DEGRADED, FAILED
    }

    public static volatile Phase phase = Phase.STOPPED;
    public static volatile int port = 0;
    public static volatile boolean nativeRunning = false;
    public static volatile boolean lastProbeOk = false;
    public static volatile String lastProbeDetail = "(no probe yet)";
    public static volatile long lastProbeLatencyMs = 0;
    public static volatile long lastProbeAtMs = 0;
    public static volatile int probeOkCount = 0;
    public static volatile int probeFailCount = 0;
    public static volatile int restartsUsed = 0;
    public static volatile int maxRestarts = 5;
    public static volatile String lastError = "";
    public static volatile String nativeVersion = "";
    public static volatile String statsJson = "{}";
    public static volatile boolean nativeLoaded = false;

    // ---- HG-NET-02B outbound control bridge -----------------------------------------------
    // Mirrors what OutboundMcpBridge reports, so the activity can render the real bridge state
    // rather than an optimistic local guess. bridgeStatus carries the exact refusal reason when
    // the transport policy blocks a start, which is what makes a misconfiguration fixable.
    public static volatile boolean bridgeRunning = false;
    public static volatile String bridgeStatus = "bridge: idle (not started)";
    /** Bridge recovery attempts consumed by the supervisor; 0 when healthy. */
    public static volatile int bridgeAttempts = 0;
    public static volatile String bridgeEndpoint = "";

    private static final CopyOnWriteArrayList<Listener> LISTENERS = new CopyOnWriteArrayList<Listener>();

    private EdgeState() {}

    public static void addListener(Listener l) {
        if (l != null && !LISTENERS.contains(l)) LISTENERS.add(l);
    }

    public static void removeListener(Listener l) {
        LISTENERS.remove(l);
    }

    public static void notifyChanged() {
        for (Listener l : LISTENERS) {
            try {
                l.onEdgeStateChanged();
            } catch (Throwable ignored) {
                // A broken UI observer must not take the service down.
            }
        }
    }

    public static String summary() {
        StringBuilder sb = new StringBuilder();
        sb.append("phase=").append(phase)
          .append(" nativeRunning=").append(nativeRunning)
          .append(" port=").append(port)
          .append(" probeOk=").append(lastProbeOk)
          .append(" ok=").append(probeOkCount)
          .append(" fail=").append(probeFailCount)
          .append(" restarts=").append(restartsUsed).append("/").append(maxRestarts);
        return sb.toString();
    }
}
