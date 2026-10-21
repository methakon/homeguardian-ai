package org.homeguardian.harness;

import android.content.Context;

/**
 * The real {@link ObservationLink}: {@code NodeSession} + {@code ObservationTransport} behind the
 * narrow interface the service-owned host depends on.
 *
 * This class adds NO security behaviour of its own. It forwards, and nothing else:
 *  - it does not widen what may be sent (the transport still carries compact numeric observations
 *    only - see ObservationTransport's own contract);
 *  - it does not touch credentials (NodeSession.buildConfig reads them from app-private storage);
 *  - it does not weaken identity (the transport still verifies the hub certificate against the
 *    endpoint SAN, with no CN fallback);
 *  - it does not create a listening socket. The node dials out; nothing here listens.
 *
 * Its whole reason to exist is that ownership of the lifecycle moved from the Activity to the
 * foreground service, and that move must be testable without a socket.
 */
public final class SessionObservationLink implements ObservationLink {

    private final Context ctx;
    private final String host;
    private final int port;
    private final String displayName;

    private ObservationTransport transport;
    private NodeSession session;
    private String deviceId;
    private volatile boolean started = false;

    public SessionObservationLink(Context ctx, String deviceId, String displayName,
                                  String host, int port) {
        this.ctx = ctx;
        this.deviceId = deviceId == null ? "" : deviceId;
        this.displayName = displayName == null ? "" : displayName;
        this.host = host == null ? "" : host;
        this.port = port;
    }

    @Override
    public boolean credentialsPresent() {
        return NodeSession.credentialsPresent(ctx);
    }

    @Override
    public synchronized void setDeviceId(String deviceId) {
        this.deviceId = deviceId == null ? "" : deviceId;
        if (session != null) {
            session.setDeviceId(this.deviceId);
        }
    }

    /**
     * Refuses to start without credentials rather than connecting unverified. This is the same
     * fail-closed rule NodePanel applied before the move; it is preserved here so that moving the
     * owner cannot loosen it.
     */
    @Override
    public synchronized void start() {
        if (!credentialsPresent()) {
            started = false;
            return;
        }
        if (started) {
            return;
        }
        if (host.length() == 0 || port <= 0) {
            started = false;
            return;
        }
        transport = new ObservationTransport(
                NodeSession.buildConfig(ctx, deviceId, displayName, host, port));
        session = new NodeSession(transport);
        session.setDeviceId(deviceId);
        transport.start();
        started = true;
    }

    @Override
    public synchronized void stop() {
        started = false;
        if (transport != null) {
            transport.stop();
        }
        session = null;
    }

    @Override
    public synchronized void pump(String ndjson) {
        if (!started || session == null) {
            return;
        }
        session.pump(ndjson);
    }

    @Override
    public synchronized void onAuthorizationWithdrawn() {
        if (transport != null) {
            // The transport's own withdrawal path: hold the queue, do not accumulate work that
            // will never be accepted. Naming it here keeps the call site honest about intent.
            transport.onCaptureDisabled();
        }
    }

    @Override
    public synchronized void resumeAfterAuthorization() {
        if (transport != null) {
            transport.resume();
        }
    }

    @Override
    public boolean isConnected() {
        return transport != null && transport.isConnected();
    }

    @Override
    public boolean isSuspended() {
        return transport != null && transport.isSuspended();
    }

    @Override
    public String suspensionReason() {
        return transport == null ? "" : transport.suspensionReason();
    }

    @Override
    public long queuedCount() {
        return transport == null ? 0L : transport.queuedCount();
    }

    @Override
    public long sentCount() {
        return transport == null ? 0L : transport.sentCount();
    }

    @Override
    public long refusedCount() {
        return transport == null ? 0L : transport.refusedCount();
    }

    @Override
    public long droppedCount() {
        return transport == null ? 0L : transport.droppedCount();
    }

    @Override
    public long permanentRejectionCount() {
        return transport == null ? 0L : transport.permanentRejectionCount();
    }

    @Override
    public long queuedTotal() {
        return session == null ? 0L : session.queuedTotal();
    }

    @Override
    public long parseFailures() {
        return session == null ? 0L : session.parseFailures();
    }

    @Override
    public long lastSentAtMs() {
        return session == null ? 0L : session.lastSentAtMs();
    }
}
