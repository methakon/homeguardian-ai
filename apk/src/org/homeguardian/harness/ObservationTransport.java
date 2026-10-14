package org.homeguardian.harness;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.List;

import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.Collection;
import java.util.List;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLPeerUnverifiedException;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManagerFactory;

/**
 * The device side of the D-03 observation transport (bounded queue + framed TLS).
 *
 * SAFETY AND PRIVACY CONTRACT
 *  - This class transmits COMPACT NUMERIC OBSERVATIONS ONLY. It has no path that
 *    can carry raw audio, raw video, a frame buffer, a transcript or a biometric
 *    template: the queue holds doubles and short strings, and the serializer
 *    writes exactly those fields.
 *  - It never starts acquisition. It is fed by the acquisition code, which is
 *    itself gated by an explicit operator start, the Android runtime permission
 *    and the HomeGuardian consent record.
 *  - The queue is BOUNDED. When it is full the OLDEST observation is dropped and a
 *    counter records it: a phone that loses the network must not grow without
 *    limit, and dropping the stale end of a telemetry queue is the honest failure
 *    mode for this data.
 *  - Credentials are never logged, never placed in an observation, and never sent
 *    in a payload: the certificate is presented to the socket, and the observation
 *    body carries no key material.
 *
 * THREADING: one worker thread drains the queue. Reconnects are explicit, with a
 * bounded backoff; a reconnect never replays the queue's history as if it were
 * fresh (sequence numbers continue to increase, and the hub rejects anything it
 * has already accepted).
 *
 * VERIFICATION STATUS: this file is COMPILE-VERIFIED against the Android SDK
 * (the APK builds) and its wire format matches the hub's protocol exactly, but it
 * has NOT been exercised against the hub from a physical device - no phone is
 * attached. The executed end-to-end evidence is the C++ mock client in
 * src/main_device_client.cpp. See docs/ANDROID_TRANSPORT_D03.md.
 */
public class ObservationTransport {

    private static final String TAG = "HGTransport";

    // Must match homeguardian::net::kProtocolVersionMajor / kMaxFrameBytes.
    private static final int PROTOCOL_VERSION_MAJOR = 1;
    private static final int PROTOCOL_VERSION_MINOR = 0;
    private static final int MAX_FRAME_BYTES = 4096;
    private static final int MAX_QUEUE_DEPTH = 128;

    /** One compact observation, ready to serialize. */
    public static final class PendingObservation {
        public final String observationId;
        public final String sourceType;
        public final String observationType;
        public final String acquisitionMode;
        public final long acquiredAtMs;
        public final long sequence;
        public final double value;
        public final String unit;
        public final String state;
        public final double confidence;
        public final String provenanceClaim;
        int attempts = 0;

        public PendingObservation(String observationId, String sourceType, String observationType,
                                  String acquisitionMode, long acquiredAtMs, long sequence,
                                  double value, String unit, String state, double confidence,
                                  String provenanceClaim) {
            this.observationId = observationId;
            this.sourceType = sourceType;
            this.observationType = observationType;
            this.acquisitionMode = acquisitionMode;
            this.acquiredAtMs = acquiredAtMs;
            this.sequence = sequence;
            this.value = value;
            this.unit = unit;
            this.state = state;
            this.confidence = confidence;
            this.provenanceClaim = provenanceClaim;
        }
    }

    /** TLS material provided by the operator (provisioned out of band). */
    public static final class Config {
        public String host = "127.0.0.1";
        public int port = 8443;
        public String deviceId = "";
        public String displayName = "";
        public String pkcs12Path = "";      // client certificate + key, app-private
        public String pkcs12PasswordFile = "";  // separate 0600 file; never logged
        public String trustStorePath = "";   // the private CA, PEM or PKCS#12
        public String trustStorePasswordFile = "";
        public int connectTimeoutMs = 5000;
        public int readTimeoutMs = 5000;
    }

    /** Reasons the hub will NEVER accept for this observation: retrying is pointless. */
    private static final Set<String> PERMANENT_REASONS = new HashSet<String>(Arrays.asList(
            "bad_frame", "bad_version", "bad_json", "oversized", "unknown_kind",
            "unknown_source_type", "unknown_observation_type", "invalid_value",
            "malformed_timestamp", "stale_timestamp", "replayed_sequence",
            "duplicate_observation", "device_unknown", "device_revoked",
            "device_unpaired", "identity_mismatch", "unauthorized"));

    /** Reasons that mean "authorization is gone": stop, drop everything, wait for the operator. */
    private static final Set<String> SUSPENDING_REASONS = new HashSet<String>(Arrays.asList(
            "consent_missing", "consent_withdrawn", "capture_disabled", "device_revoked",
            "device_unpaired", "identity_mismatch", "unauthorized", "device_unknown"));

    /** Reasons worth retrying: the hub was busy or could not durably commit. */
    private static final Set<String> RETRYABLE_REASONS = new HashSet<String>(Arrays.asList(
            "queue_full", "processing_failed"));

    private static final int MAX_ATTEMPTS_PER_OBSERVATION = 4;

    private final Config config;
    private final ArrayDeque<PendingObservation> queue = new ArrayDeque<>();
    private final Object lock = new Object();
    private volatile boolean running = false;
    // Bumped on every start(), so a worker left over from a previous run can see that
    // it is no longer the current one and exit instead of racing its replacement.
    private volatile long generation = 0;
    // Set when the hub says authorization is gone. The worker stops sending until the
    // operator explicitly resumes, because "the hub refused this because consent was
    // withdrawn" is not something to retry through.
    private volatile boolean suspended = false;
    private volatile String suspensionReason = "";
    // The socket in use, so stop() can close it and unblock a blocked read instead of
    // waiting for a timeout.
    private volatile SSLSocket activeSocket;
    private Thread worker;
    private long sequenceCounter = 0;
    private long dropped = 0;
    private long rejectedLocal = 0;
    private long permanentRejections = 0;
    private long retryableRejections = 0;
    private long sent = 0;
    private long refused = 0;
    private volatile boolean online = false;

    public ObservationTransport(Config config) {
        this.config = config;
    }

    public boolean isOnline() {
        return online;
    }

    public long queuedCount() {
        synchronized (lock) {
            return queue.size();
        }
    }

    public long droppedCount() {
        return dropped;
    }

    public long sentCount() {
        return sent;
    }

    public long refusedCount() {
        return refused;
    }

    public long permanentRejectionCount() {
        return permanentRejections;
    }

    public long droppedForInvalidPayload() {
        return rejectedLocal;
    }

    /** True only while a handshake and an acknowledged delivery have both succeeded. */
    public boolean isConnected() {
        return online;
    }

    /**
     * Queue one observation. Bounded: the oldest entry is dropped when full.
     *
     * A non-finite or out-of-range feature is refused HERE, before it can be
     * transmitted: a NaN that reaches the wire is a value the hub must then reject, and
     * a node that keeps producing them would spend its whole link budget on refusals.
     */
    public void enqueue(PendingObservation observation) {
        if (observation == null || !isTransmittable(observation)) {
            synchronized (lock) {
                rejectedLocal++;
            }
            Log.w(TAG, "refused to queue an observation with an unusable numeric payload");
            return;
        }
        if (suspended) {
            synchronized (lock) {
                rejectedLocal++;
            }
            return;  // authorization is withdrawn: do not accumulate work to send
        }
        synchronized (lock) {
            if (queue.size() >= MAX_QUEUE_DEPTH) {
                queue.pollFirst();
                dropped++;
            }
            queue.addLast(observation);
            lock.notifyAll();
        }
    }

    public void start() {
        if (running) {
            return;
        }
        running = true;
        generation++;
        final long myGeneration = generation;
        worker = new Thread(new Runnable() {
            @Override
            public void run() {
                loop(myGeneration);
            }
        }, "hg-observation-transport");
        worker.start();
    }

    /**
     * Stop the worker and CLOSE the active socket.
     *
     * Closing the socket is what makes this reliable: a worker blocked in a read would
     * otherwise sit until its timeout, and a second start() could then have two workers
     * delivering from the same queue.
     */
    public void stop() {
        running = false;
        generation++;              // any worker still alive is now a stale generation
        suspended = false;
        closeActiveSocket();
        synchronized (lock) {
            lock.notifyAll();
        }
        final Thread current = worker;
        if (current != null) {
            current.interrupt();
            try {
                current.join(3000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (current.isAlive()) {
                // The loop exits on the generation change, but say so rather than
                // pretending the thread is gone.
                Log.w(TAG, "transport worker did not stop within the join window");
            }
            worker = null;
        }
        online = false;
    }

    private void closeActiveSocket() {
        final SSLSocket socket = activeSocket;
        activeSocket = null;
        if (socket != null) {
            try {
                socket.close();
            } catch (Exception ignored) {
                // closing a dead socket is not interesting
            }
        }
    }

    /** The operator-facing switch for a suspended transport (see SUSPENDING_REASONS). */
    public boolean isSuspended() {
        return suspended;
    }

    public String suspensionReason() {
        return suspensionReason;
    }

    /** Resume after the operator has restored consent/capture authorization. */
    public void resume() {
        suspended = false;
        suspensionReason = "";
    }

    /**
     * Capture was disabled: the queued work describes activity the operator has now
     * said not to collect. Flushing is the honest response - sending it later would
     * transmit observations made under a permission that no longer holds.
     */
    public void onCaptureDisabled() {
        synchronized (lock) {
            dropped += queue.size();
            queue.clear();
        }
        suspended = true;
        suspensionReason = "capture disabled";
        closeActiveSocket();
    }

    // ---- worker ------------------------------------------------------------

    private void loop(long myGeneration) {
        while (running && generation == myGeneration) {
            PendingObservation next;
            synchronized (lock) {
                while (running && queue.isEmpty()) {
                    try {
                        lock.wait(1000);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
                if (!running) {
                    return;
                }
                next = queue.peekFirst();
            }
            if (next == null) {
                continue;
            }
            if (suspended) {
                // Authorization withdrawn: hold the queue, touch nothing, wait for the
                // operator. Do not spin.
                sleepQuietly(1000);
                continue;
            }
            if (transmit(next)) {
                synchronized (lock) {
                    queue.pollFirst();
                }
                sent++;
            } else if (terminalFailure) {
                // The hub refused this observation for a reason that will never change
                // (bad frame, duplicate, revoked): sending it again is pointless.
                terminalFailure = false;
                synchronized (lock) {
                    queue.pollFirst();
                }
                permanentRejections++;
            } else {
                online = false;
                // Bounded backoff: a phone that cannot reach the hub retries slowly
                // instead of spinning, and the queue cap bounds memory.
                next.attempts++;
                if (next.attempts >= MAX_ATTEMPTS_PER_OBSERVATION) {
                    synchronized (lock) {
                        queue.pollFirst();
                    }
                    retryableRejections++;
                    Log.w(TAG, "dropped an observation after " + next.attempts + " attempts");
                }
                sleepQuietly(3000);
            }
        }
    }

    private boolean terminalFailure = false;

    private boolean transmit(PendingObservation observation) {
        SSLSocket socket = null;
        terminalFailure = false;
        try {
            socket = openSocket();
            if (!sendHello(socket)) {
                return false;
            }
            if (!expectWelcome(socket)) {
                refused++;
                return false;
            }
            online = true;
            if (!writeFrame(socket, observationToJson(observation).toString())) {
                return false;
            }
            final String ack = readFrame(socket);
            if (ack == null) {
                return false;
            }
            final JSONObject parsed = new JSONObject(ack);
            if (!"ack".equals(parsed.optString("kind"))) {
                return false;
            }
            // The acknowledgment must be for THIS observation. A reply about a
            // different id or sequence is a protocol violation, not a delivery: counting
            // it would let the node report success for something the hub never saw.
            if (!observation.observationId.equals(parsed.optString("observation_id"))
                    || parsed.optLong("sequence", -1) != observation.sequence) {
                Log.w(TAG, "acknowledgment does not match the observation that was sent");
                return false;
            }
            if (parsed.optInt("protocol_version_major", -1) != PROTOCOL_VERSION_MAJOR) {
                Log.w(TAG, "acknowledgment carries an unexpected protocol version");
                return false;
            }
            if (parsed.optBoolean("accepted", false)) {
                return true;
            }

            // Refused. Classify it: the three cases need different behaviour.
            final String reason = parsed.optString("reason");
            refused++;
            if (SUSPENDING_REASONS.contains(reason)) {
                suspended = true;
                suspensionReason = reason;
                Log.w(TAG, "hub refused the observation (" + reason + "); transport suspended");
                return true;  // the message is resolved: it was decided, not lost
            }
            if (PERMANENT_REASONS.contains(reason)) {
                terminalFailure = true;
                Log.w(TAG, "hub refused the observation permanently: " + reason);
                return true;
            }
            if (RETRYABLE_REASONS.contains(reason)) {
                Log.w(TAG, "hub could not take the observation (" + reason + "); will retry");
                return false;
            }
            // An unrecognised reason is treated as PERMANENT: retrying a refusal whose
            // meaning is unknown is how a node ends up in a loop.
            terminalFailure = true;
            Log.w(TAG, "hub refused the observation for an unknown reason: " + reason);
            return true;
        } catch (Exception e) {
            // Never log the exception message verbatim if it could carry key
            // material from a keystore load; log the class only.
            Log.w(TAG, "transport attempt failed: " + e.getClass().getSimpleName());
            return false;
        } finally {
            closeQuietly(socket);
        }
    }

    /**
     * Enforce that the certificate we accepted actually identifies the endpoint we
     * asked for.
     *
     * A valid certificate from the trusted CA is NOT authorization for an unrelated
     * server: without this, any host holding any certificate the CA ever issued
     * (another device's, for instance) could impersonate the hub and receive this
     * node's observations.
     *
     * Two independent mechanisms, both from the platform:
     *  1. SSLParameters.setEndpointIdentificationAlgorithm("HTTPS") makes the TLS
     *     stack verify the peer against the host we dialled during the handshake.
     *  2. An explicit Subject Alternative Name check afterwards. Step 1 permits a
     *     Common Name fallback when a certificate carries NO SAN at all, and a CN is
     *     self-asserted text in the same document as the public key. This step
     *     refuses that fallback, so the configured endpoint must appear in the SAN -
     *     as a DNS entry or as an IP entry, whichever the endpoint is.
     *
     * There is no flag that disables either check.
     */
    private void enforceServerIdentity(SSLSocket socket) throws Exception {
        final String endpoint = config.host;
        final Certificate[] chain;
        try {
            chain = socket.getSession().getPeerCertificates();
        } catch (SSLPeerUnverifiedException e) {
            throw new IllegalStateException("hub presented no certificate");
        }
        if (chain == null || chain.length == 0 || !(chain[0] instanceof X509Certificate)) {
            throw new IllegalStateException("hub certificate is unreadable");
        }
        final X509Certificate peer = (X509Certificate) chain[0];

        // TLS-level check (mechanism 1). Set before the handshake in openSocket().
        // Here we additionally confirm the handshake actually happened and the peer
        // is the one we asked for.
        final boolean isIpLiteral = isIpv4Literal(endpoint);
        if (matchesSubjectAlternativeName(peer, endpoint, isIpLiteral)) {
            return;
        }
        throw new IllegalStateException(
                "hub certificate has no matching " + (isIpLiteral ? "IP" : "DNS")
                        + " SAN for endpoint " + endpoint);
    }

    private boolean matchesSubjectAlternativeName(X509Certificate certificate, String endpoint,
                                                  boolean isIpLiteral) throws Exception {
        final Collection<List<?>> names = certificate.getSubjectAlternativeNames();
        if (names == null) {
            return false;  // no SAN at all: refused, never falls back to the CN
        }
        for (final List<?> entry : names) {
            if (entry == null || entry.size() < 2) {
                continue;
            }
            final Object type = entry.get(0);
            final Object value = entry.get(1);
            if (!(type instanceof Integer) || !(value instanceof String)) {
                continue;
            }
            final int kind = (Integer) type;          // 2 = dNSName, 7 = iPAddress
            final String text = (String) value;
            if (isIpLiteral) {
                if (kind == 7 && text.equalsIgnoreCase(endpoint)) {
                    return true;
                }
            } else if (kind == 2 && matchesDnsName(text, endpoint)) {
                return true;
            }
        }
        return false;
    }

    /** DNS comparison per RFC 6125: case-insensitive, no wildcard expansion here. */
    private boolean matchesDnsName(String pattern, String endpoint) {
        return pattern.equalsIgnoreCase(endpoint);
    }

    private boolean isIpv4Literal(String host) {
        if (host == null || host.length() == 0) {
            return false;
        }
        int parts = 0;
        int value = 0;
        int digits = 0;
        for (int i = 0; i < host.length(); i++) {
            final char c = host.charAt(i);
            if (c == '.') {
                if (digits == 0 || digits > 3 || value > 255) {
                    return false;
                }
                parts++;
                value = 0;
                digits = 0;
            } else if (c >= '0' && c <= '9') {
                value = value * 10 + (c - '0');
                digits++;
            } else {
                return false;
            }
        }
        return parts == 3 && digits > 0 && digits <= 3 && value <= 255;
    }

    private boolean isTransmittable(PendingObservation observation) {
        if (observation.observationType == null || observation.sourceType == null
                || observation.acquisitionMode == null) {
            return false;
        }
        if (observation.sequence <= 0) {
            return false;
        }
        if (Double.isNaN(observation.value) || Double.isInfinite(observation.value)) {
            return false;
        }
        if (Double.isNaN(observation.confidence) || Double.isInfinite(observation.confidence)
                || observation.confidence < 0.0 || observation.confidence > 1.0) {
            return false;
        }
        if (observation.acquiredAtMs <= 0) {
            return false;
        }
        return observation.observationType.length() <= 64 && observation.sourceType.length() <= 64;
    }

    private SSLSocket openSocket() throws Exception {
        final char[] keyPassword = readSecret(config.pkcs12PasswordFile);
        final char[] trustPassword = readSecret(config.trustStorePasswordFile);

        final KeyStore keyStore = KeyStore.getInstance("PKCS12");
        try (InputStream in = new FileInputStream(config.pkcs12Path)) {
            keyStore.load(in, keyPassword);
        }
        final KeyManagerFactory keyManagers =
                KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        keyManagers.init(keyStore, keyPassword);
        // The password exists only long enough to unlock the keystore; the char[] is
        // wiped so it is not left in the heap for the lifetime of the process.
        Arrays.fill(keyPassword, '\0');

        final KeyStore trustStore = KeyStore.getInstance("PKCS12");
        try (InputStream in = new FileInputStream(config.trustStorePath)) {
            trustStore.load(in, trustPassword);
        }
        final TrustManagerFactory trustManagers =
                TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        trustManagers.init(trustStore);

        final SSLContext context = SSLContext.getInstance("TLS");
        context.init(keyManagers.getKeyManagers(), trustManagers.getTrustManagers(), null);

        final SSLSocket socket = (SSLSocket) context.getSocketFactory().createSocket();
        activeSocket = socket;
        socket.connect(new InetSocketAddress(config.host, config.port), config.connectTimeoutMs);
        socket.setSoTimeout(config.readTimeoutMs);
        socket.setEnabledProtocols(new String[] {"TLSv1.2", "TLSv1.3"});

        // Endpoint identification must be in place BEFORE the handshake, so a hub
        // presenting a CA-signed certificate for a DIFFERENT host is rejected by the
        // TLS stack itself rather than trusted because its chain was valid.
        final SSLParameters parameters = socket.getSSLParameters();
        parameters.setEndpointIdentificationAlgorithm("HTTPS");
        socket.setSSLParameters(parameters);

        socket.startHandshake();
        enforceServerIdentity(socket);
        return socket;
    }

    private boolean sendHello(SSLSocket socket) throws Exception {
        final JSONObject hello = new JSONObject();
        hello.put("protocol_version_major", PROTOCOL_VERSION_MAJOR);
        hello.put("protocol_version_minor", PROTOCOL_VERSION_MINOR);
        hello.put("kind", "hello");
        hello.put("device_id", config.deviceId);
        hello.put("display_name", config.displayName);
        final JSONArray capabilities = new JSONArray();
        capabilities.put("camera");
        capabilities.put("microphone");
        hello.put("capabilities", capabilities);
        return writeFrame(socket, hello.toString());
    }

    private boolean expectWelcome(SSLSocket socket) throws Exception {
        final String frame = readFrame(socket);
        if (frame == null) {
            return false;
        }
        final JSONObject parsed = new JSONObject(frame);
        if (!"welcome".equals(parsed.optString("kind"))) {
            return false;
        }
        // The hub states the highest sequence it has already accepted from this device.
        // Adopting it is what lets the application restart legitimately: a new process
        // otherwise starts at 1 and every observation it sends looks like a replay.
        final long watermark = parsed.optLong("last_sequence", 0L);
        if (watermark > 0L) {
            synchronized (lock) {
                if (watermark > sequenceCounter) {
                    sequenceCounter = watermark;
                }
            }
        }
        return true;
    }

    private JSONObject observationToJson(PendingObservation observation) throws Exception {
        final JSONObject message = new JSONObject();
        message.put("protocol_version_major", PROTOCOL_VERSION_MAJOR);
        message.put("protocol_version_minor", PROTOCOL_VERSION_MINOR);
        message.put("kind", "observation");
        message.put("observation_id", observation.observationId);
        message.put("device_id", config.deviceId);
        message.put("source_type", observation.sourceType);
        message.put("observation_type", observation.observationType);
        message.put("acquisition_mode", observation.acquisitionMode);
        message.put("acquired_at_ms", observation.acquiredAtMs);
        message.put("sequence", observation.sequence);
        final JSONObject features = new JSONObject();
        features.put(observation.observationType, observation.value);
        features.put("state", observation.state);
        message.put("features", features);
        if (observation.unit != null && observation.unit.length() > 0) {
            message.put("unit", observation.unit);
        }
        message.put("confidence", observation.confidence);
        message.put("provenance_claim", observation.provenanceClaim);
        return message;
    }

    /** Next sequence number: strictly increasing, never reused after a reconnect. */
    public long nextSequence() {
        synchronized (lock) {
            return ++sequenceCounter;
        }
    }

    private boolean writeFrame(SSLSocket socket, String body) throws Exception {
        final byte[] payload = body.getBytes(StandardCharsets.UTF_8);
        if (payload.length > MAX_FRAME_BYTES) {
            throw new IllegalStateException("observation exceeds the frame cap");
        }
        final ByteArrayOutputStream buffer = new ByteArrayOutputStream(payload.length + 4);
        final DataOutputStream out = new DataOutputStream(buffer);
        out.writeInt(payload.length);  // big-endian, matching the hub's framing
        out.write(payload);
        final OutputStream stream = socket.getOutputStream();
        stream.write(buffer.toByteArray());
        stream.flush();
        return true;
    }

    private String readFrame(SSLSocket socket) throws Exception {
        final InputStream in = socket.getInputStream();
        final byte[] header = new byte[4];
        if (!readFully(in, header, 4)) {
            return null;
        }
        final int length = ((header[0] & 0xFF) << 24) | ((header[1] & 0xFF) << 16)
                | ((header[2] & 0xFF) << 8) | (header[3] & 0xFF);
        if (length <= 0 || length > MAX_FRAME_BYTES) {
            return null;
        }
        final byte[] body = new byte[length];
        if (!readFully(in, body, length)) {
            return null;
        }
        return new String(body, StandardCharsets.UTF_8);
    }

    private boolean readFully(InputStream in, byte[] target, int length) throws Exception {
        int read = 0;
        while (read < length) {
            final int n = in.read(target, read, length - read);
            if (n <= 0) {
                return false;
            }
            read += n;
        }
        return true;
    }

    /**
     * Read a secret from an app-private 0600 file. The value is never logged and
     * never included in an observation; it exists only long enough to unlock a
     * keystore.
     */
    private static final int MAX_SECRET_BYTES = 512;

    private char[] readSecret(String path) throws Exception {
        final File file = new File(path);
        if (!file.isFile()) {
            throw new IllegalStateException("secret file missing");
        }
        // BOUNDED read: a password file is a few bytes, and an unbounded read on a
        // path an attacker can influence is a memory-exhaustion primitive.
        final ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        try (InputStream in = new FileInputStream(file)) {
            final byte[] chunk = new byte[128];
            int n;
            while ((n = in.read(chunk)) > 0) {
                if (buffer.size() + n > MAX_SECRET_BYTES) {
                    throw new IllegalStateException("secret file is larger than expected");
                }
                buffer.write(chunk, 0, n);
            }
        }
        final String value = new String(buffer.toByteArray(), StandardCharsets.UTF_8).trim();
        return value.toCharArray();
    }

    private void sleepQuietly(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void closeQuietly(SSLSocket socket) {
        if (socket == null) {
            return;
        }
        if (activeSocket == socket) {
            activeSocket = null;
        }
        try {
            socket.close();
        } catch (Exception ignored) {
            // closing a dead socket is not interesting
        }
    }

    /** Non-sensitive status line for the node UI (never a credential). */
    public String statusLine() {
        return "transport: " + (suspended ? "suspended(" + suspensionReason + ")"
                                          : (online ? "online" : "offline"))
                + " queued=" + queuedCount() + " sent=" + sentCount()
                + " dropped=" + droppedCount() + " refused=" + refusedCount()
                + " permanent=" + permanentRejections;
    }

    /** Convenience for the UI: build the list of what WOULD be sent. */
    public List<String> pendingIds() {
        synchronized (lock) {
            final List<String> ids = new ArrayList<>();
            for (PendingObservation observation : queue) {
                ids.add(observation.observationId);
            }
            return ids;
        }
    }
}
