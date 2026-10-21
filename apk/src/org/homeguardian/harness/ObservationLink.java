package org.homeguardian.harness;

/**
 * The seam between the observation transport and whatever owns its lifecycle.
 *
 * WHY THIS INTERFACE EXISTS
 * The transport used to be created, started and pumped by {@code NodePanel}, which is built by
 * the Activity and therefore stops being ticked the moment the screen goes off (onPause removes
 * the Activity's ticker). Capture survived screen-off; the transport did not. Moving ownership
 * into the foreground service is the fix, and this interface is what makes that fix testable:
 * a test can substitute a fake link and exercise the whole service/host lifecycle - start, pump,
 * consent withdrawal, stop, wake-lock release - without a socket, a certificate or a phone.
 *
 * It is deliberately narrow. It exposes exactly what the owner (the host/service) and the status
 * panel need, and nothing that would let a caller reach a payload, a credential or a raw frame.
 * Every method here is implemented today by {@code NodeSession} + {@code ObservationTransport}
 * via {@link SessionObservationLink}; none of them changes that class's security behaviour.
 */
public interface ObservationLink {

    /** Begin connecting and draining. Must be safe to call on an already-started link. */
    void start();

    /**
     * Stop the link and release everything it holds. Must be idempotent and must be safe to call
     * from any exit path, including failure paths.
     */
    void stop();

    /**
     * Hand the link one batch of the native drain (NDJSON, numeric scalars only). Called by the
     * OWNER's scheduler, not by a UI tick.
     */
    void pump(String ndjson);

    /**
     * Authorization was withdrawn (consent, capture disabled, device revoked). The link must stop
     * accumulating and stop sending. It must NOT silently keep a queue to replay later as if it
     * were fresh.
     */
    void onAuthorizationWithdrawn();

    /** The operator restored authorization. Sending may resume. */
    void resumeAfterAuthorization();

    void setDeviceId(String deviceId);

    boolean isConnected();

    boolean isSuspended();

    String suspensionReason();

    // Counters. Counts only - never a payload, an id or a credential.
    long queuedCount();

    long sentCount();

    long refusedCount();

    long droppedCount();

    long permanentRejectionCount();

    long queuedTotal();

    long parseFailures();

    long lastSentAtMs();

    /** True when the link holds credentials and can be started at all. */
    boolean credentialsPresent();
}
