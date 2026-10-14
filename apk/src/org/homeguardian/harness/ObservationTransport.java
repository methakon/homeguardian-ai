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
import java.util.List;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
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

    private final Config config;
    private final ArrayDeque<PendingObservation> queue = new ArrayDeque<>();
    private final Object lock = new Object();
    private volatile boolean running = false;
    private Thread worker;
    private long sequenceCounter = 0;
    private long dropped = 0;
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

    /** Queue one observation. Bounded: the oldest entry is dropped when full. */
    public void enqueue(PendingObservation observation) {
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
        worker = new Thread(new Runnable() {
            @Override
            public void run() {
                loop();
            }
        }, "hg-observation-transport");
        worker.start();
    }

    public void stop() {
        running = false;
        synchronized (lock) {
            lock.notifyAll();
        }
        if (worker != null) {
            try {
                worker.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            worker = null;
        }
    }

    // ---- worker ------------------------------------------------------------

    private void loop() {
        while (running) {
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
            if (transmit(next)) {
                synchronized (lock) {
                    queue.pollFirst();
                }
                sent++;
            } else {
                online = false;
                // Bounded backoff: a phone that cannot reach the hub retries
                // slowly instead of spinning, and the queue cap bounds memory.
                sleepQuietly(3000);
            }
        }
    }

    private boolean transmit(PendingObservation observation) {
        SSLSocket socket = null;
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
            if (parsed.optBoolean("accepted", false)) {
                return true;
            }
            // The hub refused it for a reason we should not paper over.
            refused++;
            Log.w(TAG, "hub refused an observation: " + parsed.optString("reason"));
            // A refused sequence is terminal for that message: do not retry it.
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
        socket.connect(new InetSocketAddress(config.host, config.port), config.connectTimeoutMs);
        socket.setSoTimeout(config.readTimeoutMs);
        socket.setEnabledProtocols(new String[] {"TLSv1.2", "TLSv1.3"});
        socket.startHandshake();
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
        return "welcome".equals(parsed.optString("kind"));
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
    private char[] readSecret(String path) throws Exception {
        final File file = new File(path);
        if (!file.isFile()) {
            throw new IllegalStateException("secret file missing");
        }
        final ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        try (InputStream in = new FileInputStream(file)) {
            final byte[] chunk = new byte[256];
            int n;
            while ((n = in.read(chunk)) > 0) {
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
        try {
            socket.close();
        } catch (Exception ignored) {
            // closing a dead socket is not interesting
        }
    }

    /** Non-sensitive status line for the node UI (never a credential). */
    public String statusLine() {
        return "transport: " + (online ? "online" : "offline")
                + " queued=" + queuedCount() + " sent=" + sentCount()
                + " dropped=" + droppedCount() + " refused=" + refusedCount();
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
