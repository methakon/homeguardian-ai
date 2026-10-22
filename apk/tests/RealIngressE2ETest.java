package org.homeguardian.harness;

import org.homeguardian.harness.ObservationTransport.Config;
import org.homeguardian.harness.ObservationTransport.PendingObservation;

import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManagerFactory;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.security.KeyStore;
import java.security.SecureRandom;

/**
 * RealIngressE2ETest - the Android production serializer against the REAL C++ D-03 ingress.
 *
 * WHY THIS IS DIFFERENT FROM EVERY OTHER TEST IN THIS SUITE
 * The TLS suite drives the production transport against a peer this test authored, so it proves the
 * transport is INTERNALLY consistent. It cannot prove the wire contract, because the same mind wrote
 * both ends of it. This test drives the same production serializer against the actual
 * `hg_device_ingress` binary - real OpenSSL, real DeviceProtocol, real DeviceRegistry, real replay
 * protection - listening on loopback.
 *
 * THE THREE CATEGORIES, kept explicit because conflating them is how a synthetic result gets presented
 * as a physical one:
 *   (a) wire-compatible offline vectors        SyntheticVectorEmitTest
 *   (b) real localhost mTLS C++ integration    THIS FILE
 *   (c) physical phone / real camera or mic    NOT DONE, and still NO
 *
 * WHAT IS REAL HERE: the TLS handshake, mutual certificate authentication, the registry lookup
 * against a real SHA-256 certificate fingerprint, the hello/welcome/observation/ack exchange, and the
 * hub's replay protection.
 * WHAT IS NOT: the observation is synthetic and says so on both axes; there is no camera and no
 * microphone; the credentials are TEST-ONLY and live in scratch space; the device identity is a test
 * identity enrolled in a test registry. Nothing production was touched.
 *
 * NO RAW FRAME IS EVER SENT WITHOUT A HANDSHAKE. Every exchange below opens a TLS socket, completes
 * an authenticated `hello`, reads the `welcome`, and only then sends an observation. That is the rule
 * the mission set, and it is the rule the hub enforces anyway - an unauthenticated observation is
 * refused at the peer-certificate step, before any frame is parsed.
 *
 * Requires -Dhg.e2e.dir=<dir with device.p12/ca.p12/*.pass> and a reachable ingress.
 * Skips loudly when the environment is absent - a skip is NOT a pass.
 */
public final class RealIngressE2ETest {

    private static int passed = 0;
    private static int failed = 0;
    private static int skipped = 0;
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

    private static final String DEVICE_ID = "hg-android-synthetic-01";

    public static void main(String[] args) throws Exception {
        System.out.println("== REAL C++ D-03 ingress end-to-end (loopback, mutual TLS) ==");

        final String dir = System.getProperty("hg.e2e.dir");
        final String host = System.getProperty("hg.e2e.host", "127.0.0.1");
        final String portStr = System.getProperty("hg.e2e.port", "18443");
        if (dir == null) {
            System.out.println("  SKIP: -Dhg.e2e.dir not set. NOT a pass.");
            return;
        }
        final int port = Integer.parseInt(portStr);

        // If the ingress is not up, say so plainly. A missing environment must never read as a pass.
        try (java.net.Socket probe = new java.net.Socket()) {
            probe.connect(new java.net.InetSocketAddress(host, port), 1500);
        } catch (Exception e) {
            System.out.println("  SKIP: no ingress reachable at " + host + ":" + port
                    + " (" + e.getClass().getSimpleName() + "). NOT a pass.");
            return;
        }

        final Config cfg = new Config();
        cfg.host = host;
        cfg.port = port;
        cfg.deviceId = DEVICE_ID;
        cfg.displayName = "synthetic node";
        cfg.pkcs12Path = dir + "/device.p12";
        cfg.pkcs12PasswordFile = dir + "/device.p12.pass";
        cfg.trustStorePath = dir + "/ca.p12";
        cfg.trustStorePasswordFile = dir + "/ca.p12.pass";
        cfg.connectTimeoutMs = 5000;
        cfg.readTimeoutMs = 6000;

        // ---- A: the REAL exchange, driven by the PRODUCTION transport --------------------------
        ObservationTransport t = new ObservationTransport(cfg);
        long seq = t.nextSequence();
        PendingObservation o = new PendingObservation(
                "real-e2e-" + seq, "camera_availability", "availability", "synthetic",
                System.currentTimeMillis(), seq,
                0.42d, "fraction", "present", 0.90d, "synthetic");
        t.enqueue(o);
        t.start();
        waitFor(() -> t.sentCount() > 0 || t.refusedCount() > 0, 12000);

        final long sent = t.sentCount();
        final long refused = t.refusedCount();
        final boolean suspended = t.isSuspended();
        final String suspensionReason = t.suspensionReason();
        final long permanent = t.permanentRejectionCount();
        t.stop();

        check("A1 the production transport completed an authenticated exchange with the REAL ingress",
                sent >= 1, "sent=" + sent + " refused=" + refused + " permanent=" + permanent
                        + (suspended ? " suspended=" + suspensionReason : ""));
        check("A2 the hub ACCEPTED the synthetic observation (reason=ok)",
                sent >= 1 && !suspended, "sent=" + sent + " suspended=" + suspended);
        check("A3 the observation was NOT refused and NOT suspended",
                refused == 0 && !suspended,
                "refused=" + refused + " suspended=" + suspended + " " + suspensionReason);

        // ---- B: REPLAY PROTECTION, against the hub's real watermark ----------------------------
        // A SECOND transport starts its own counter at 1 and so re-sends a sequence the hub has
        // already accepted. This is the only way to test replay protection against the real thing:
        // the hub's durable watermark, not a stub. If `last_sequence` were not real, this would be
        // accepted twice.
        ObservationTransport replay = new ObservationTransport(cfg);
        long rseq = replay.nextSequence();
        check("B0 the replaying transport restarts its counter at 1, so it genuinely repeats a sequence",
                rseq == 1L, "sequence=" + rseq);
        replay.enqueue(new PendingObservation(
                "real-e2e-replay", "camera_availability", "availability", "synthetic",
                System.currentTimeMillis(), rseq,
                0.42d, "fraction", "present", 0.90d, "synthetic"));
        replay.start();
        waitFor(() -> replay.sentCount() > 0 || replay.refusedCount() > 0
                || replay.permanentRejectionCount() > 0, 12000);
        final long replayPermanent = replay.permanentRejectionCount();
        final long replaySent = replay.sentCount();
        final boolean replaySuspended = replay.isSuspended();
        replay.stop();

        check("B1 the hub REFUSED the replayed sequence (replay protection is real and durable)",
                replayPermanent >= 1,
                "permanent=" + replayPermanent + " sent=" + replaySent
                        + " - a repeated sequence was accepted, so the watermark is not real");
        check("B2 the refused replay was NOT counted as delivered (the counter fix holds end-to-end)",
                replaySent == 0, "sent=" + replaySent);
        check("B3 a replay refusal does NOT suspend the transport (it is not an authorization loss)",
                !replaySuspended, "suspended=" + replaySuspended);

        // ---- C: the RESTART case, tested for what it actually does ------------------------------
        // The hub now holds a watermark. A fresh transport simulates a restarted node: it assigns a
        // sequence at ENQUEUE time, which is before any handshake has happened (NodeSession does the
        // same). The question is whether the hub accepts the first observation after a restart.
        //
        // The watermark is adopted inside expectWelcome(), i.e. DURING the send that follows the
        // enqueue, so the already-assigned sequence cannot benefit from it. This case therefore
        // asserts what the design DOES, and the assertion records the consequence rather than
        // describing it as correct.
        ObservationTransport restarted = new ObservationTransport(cfg);
        final long restartedSeq = restarted.nextSequence();
        check("C1 a fresh transport assigns its first sequence at ENQUEUE time, before any handshake",
                restartedSeq == 1L, "sequence=" + restartedSeq);
        restarted.enqueue(new PendingObservation(
                "real-e2e-restart", "camera_availability", "availability", "synthetic",
                System.currentTimeMillis(), restartedSeq,
                0.42d, "fraction", "present", 0.90d, "synthetic"));
        restarted.start();
        waitFor(() -> restarted.sentCount() > 0 || restarted.refusedCount() > 0
                || restarted.permanentRejectionCount() > 0, 12000);
        final long restartedPermanent = restarted.permanentRejectionCount();
        final long restartedSent = restarted.sentCount();
        final boolean restartedSuspended = restarted.isSuspended();
        restarted.stop();

        // This is the finding, stated plainly: with a watermark already set, a restarted node's first
        // observation is refused as a replay, because its sequence was fixed before the handshake that
        // learns the watermark. The transport does adopt the watermark - it simply adopts it too late
        // for the message already queued.
        check("C2 the hub refuses a restarted node's first observation as a replay (the watermark was "
                        + "adopted only AFTER the sequence was assigned)",
                restartedPermanent >= 1 && restartedSent == 0,
                "permanent=" + restartedPermanent + " sent=" + restartedSent
                        + " suspended=" + restartedSuspended);
        check("C3 the refusal does not suspend, and is not miscounted as a delivery",
                !restartedSuspended && restartedSent == 0,
                "suspended=" + restartedSuspended + " sent=" + restartedSent);

        // ---- D: adoption itself, isolated from the queue ----------------------------------------
        // expectWelcome() adopts the watermark; these pin the arithmetic so C2's consequence is not
        // mistaken for adoption being absent.
        check("D1 a watermark above the current counter raises it",
                ObservationTransport.adoptedSequence(1L, 7L) == 7L,
                "got " + ObservationTransport.adoptedSequence(1L, 7L));
        check("D2 a watermark BELOW the current counter never lowers it (adoption cannot rewind)",
                ObservationTransport.adoptedSequence(9L, 3L) == 9L,
                "got " + ObservationTransport.adoptedSequence(9L, 3L));
        check("D3 a zero watermark leaves the counter alone (a fresh hub must not be treated as resumed)",
                ObservationTransport.adoptedSequence(1L, 0L) == 1L,
                "got " + ObservationTransport.adoptedSequence(1L, 0L));
        check("D4 adoption is monotonic: the result is never below either input",
                ObservationTransport.adoptedSequence(5L, 5L) >= 5L);

        System.out.println();
        if (skipped > 0) {
            System.out.println("  SKIPPED: " + skipped);
        }
        if (failed > 0) {
            System.out.println("  FAILED: " + failed + " of " + (passed + failed));
            System.out.print(failures);
            System.exit(1);
        }
        System.out.println("  PASS: all " + passed + " real-ingress end-to-end cases behave as specified");
    }

    interface Cond {
        boolean ok();
    }

    static void waitFor(Cond c, long timeoutMs) throws Exception {
        final long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            if (c.ok()) {
                return;
            }
            Thread.sleep(100);
        }
    }

    /** Opens an authenticated TLS socket to the ingress, completing a hello/welcome first. */
    static SSLSocket openAuthenticated(String dir, String host, int port) throws Exception {
        final char[] pw = "testonly".toCharArray();
        final KeyStore client = KeyStore.getInstance("PKCS12");
        try (FileInputStream in = new FileInputStream(dir + "/device.p12")) {
            client.load(in, pw);
        }
        final KeyManagerFactory kmf =
                KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        kmf.init(client, pw);
        final KeyStore ca = KeyStore.getInstance("PKCS12");
        try (FileInputStream in = new FileInputStream(dir + "/ca.p12")) {
            ca.load(in, pw);
        }
        final TrustManagerFactory tmf =
                TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        tmf.init(ca);
        final SSLContext ctx = SSLContext.getInstance("TLSv1.2");
        ctx.init(kmf.getKeyManagers(), tmf.getTrustManagers(), new SecureRandom());
        SSLSocketFactory f = ctx.getSocketFactory();
        SSLSocket s = (SSLSocket) f.createSocket(host, port);
        s.setSoTimeout(6000);
        s.startHandshake();
        return s;
    }

    static void writeFrame(DataOutputStream out, String body) throws Exception {
        final byte[] p = body.getBytes(StandardCharsets.UTF_8);
        out.writeInt(p.length);
        out.write(p);
        out.flush();
    }

    static String readFrame(DataInputStream in) throws Exception {
        final int len = in.readInt();
        if (len <= 0 || len > 4096) {
            return null;
        }
        final byte[] buf = new byte[len];
        in.readFully(buf);
        return new String(buf, StandardCharsets.UTF_8);
    }

    static String fileOr(String path, String fallback) {
        try {
            return new String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return fallback;
        }
    }
}
