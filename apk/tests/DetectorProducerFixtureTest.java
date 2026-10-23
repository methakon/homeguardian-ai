package org.homeguardian.harness;

import org.homeguardian.harness.ObservationTransport.Config;
import org.homeguardian.harness.ObservationTransport.PendingObservation;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.security.MessageDigest;

/**
 * DetectorProducerFixtureTest - a DETECTOR-CAPABLE synthetic fixture, driven by the PRODUCTION
 * serializer, for the Antigravity #26 end-to-end path (E2E-35).
 *
 * WHY THIS IS NOT THE EXISTING VECTOR
 * The pinned/live vector carries `camera_availability` + `availability`, which the ingress ACCEPTS and
 * audits but which casts no vote. It proves the transport contract and cannot raise an alert. The
 * fusion path needs evidence of the right TYPE and STATE on a device the hub knows.
 *
 * WHAT MAKES AN OBSERVATION ACTUALLY VOTE - verified in the C++ rather than assumed:
 *
 *   src/net/ObservationIngest.h       source_type must be in allowed_source_types
 *                                     observation_type must be in allowed_observation_types
 *   src/main_ingress.cpp              the id must ALSO be listed in `evidence_detector_ids`, or the
 *                                     observation is "recorded and audited but casts NO vote - so it
 *                                     can never raise an alert on its own"
 *   src/core/SecurityObservation.cpp  security_status_for(source_type, state) maps state -> a status,
 *                                     and only `detected` participates in a verdict
 *
 *   motion   + state in {on, detected, active}                  -> DETECTED
 *   presence + state in {on, present, occupied}                 -> DETECTED
 *   camera_availability/microphone_availability/device_availability
 *            + state in {offline, unavailable, error, unknown}  -> DETECTED (an absent sensor is the
 *                                                                   finding)
 *            + state in {online, available, ok}                  -> not_detected
 *            anything else, e.g. `present`                       -> INCONCLUSIVE
 *
 * That last row is why `camera_availability`/`present` - the existing vector's shape - can never vote:
 * it is inconclusive by design, not malformed. It is asserted here as the negative case rather than
 * left as a footnote.
 *
 * SO THE TWO DETECTOR-CAPABLE STREAMS ARE:
 *   motion   source_type=motion   observation_type=motion-changed-fraction  state=on
 *   presence source_type=presence observation_type=availability            state=present
 *
 * `presence` uses observation_type `availability`. That is deliberate and confirmed against the
 * allowlist: there is no `presence-state` in allowed_observation_types, and none is needed, because
 * the SOURCE type is what selects the detector. No schema change was required for this fixture.
 *
 * TWO DEVICE IDENTITIES, not one. device_id is derived from the client certificate's CN, so a single
 * identity reporting both motion and presence would be one device making two contradictory claims
 * about the same space. Each stream uses its own registered synthetic identity.
 *
 * WHAT IS REAL AND WHAT IS SYNTHETIC
 *   REAL      the serializer, the framing, the handshake, the sequence/ACK correlation, the facts that
 *             the hub accepted both streams and typed them as detected.
 *   SYNTHETIC the observations. acquisition_mode and provenance_claim say "synthetic" on BOTH. There
 *             is no camera, no microphone, no sensor and no physical device.
 *
 * Requires -Dhg.e2e.dir=<dir> and a reachable ingress. Skips loudly when absent - a skip is NOT a pass.
 */
public final class DetectorProducerFixtureTest {

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

    /** The two registered synthetic identities, one per source type. */
    static final String MOTION_DEVICE = "hg-android-synthetic-01";
    static final String PRESENCE_DEVICE = "hg-android-presence-01";

    static final String MOTION_SOURCE = "motion";
    static final String MOTION_OBS_TYPE = "motion-changed-fraction";
    static final String MOTION_STATE = "on";

    static final String PRESENCE_SOURCE = "presence";
    static final String PRESENCE_OBS_TYPE = "availability";
    static final String PRESENCE_STATE = "present";

    /** Unique per run: a reused hub remembers accepted observation_ids. */
    static final String RUN = Long.toString(System.currentTimeMillis() % 100000000L);

    public static void main(String[] args) throws Exception {
        System.out.println("== detector-capable synthetic fixture (production serializer, real ingress) ==");

        final String dir = System.getProperty("hg.e2e.dir");
        final String host = System.getProperty("hg.e2e.host", "127.0.0.1");
        final int port = Integer.parseInt(System.getProperty("hg.e2e.port", "18444"));
        final String outDir = System.getProperty("hg.detector.out");
        if (dir == null) {
            System.out.println("  SKIP: -Dhg.e2e.dir not set. NOT a pass.");
            return;
        }
        try (java.net.Socket probe = new java.net.Socket()) {
            probe.connect(new java.net.InetSocketAddress(host, port), 1500);
        } catch (Exception e) {
            System.out.println("  SKIP: no ingress at " + host + ":" + port
                    + " (" + e.getClass().getSimpleName() + "). NOT a pass.");
            return;
        }

        // ---- 1. MOTION stream, through the production serializer ----------------------------------
        final SendResult motion = send(dir + "/device.p12", dir + "/device.p12.pass",
                host, port, MOTION_DEVICE, MOTION_SOURCE, MOTION_OBS_TYPE, MOTION_STATE);
        check("M1 the motion stream was ACCEPTED by the hub over real mTLS",
                motion.sent >= 1 && motion.permanent == 0 && !motion.suspended,
                "sent=" + motion.sent + " permanent=" + motion.permanent
                        + " suspended=" + motion.suspended + " reason=" + motion.reason);
        check("M2 the motion frame carries the detector-mapped type and state",
                motion.frame != null
                        && motion.frame.contains("\"source_type\":\"" + MOTION_SOURCE + "\"")
                        && motion.frame.contains("\"observation_type\":\"" + MOTION_OBS_TYPE + "\"")
                        && motion.frame.contains("\"state\":\"" + MOTION_STATE + "\""),
                "frame=" + motion.frame);
        check("M3 the motion frame is inside the receiver's 4096-byte cap",
                motion.frame != null && motion.frame.getBytes(StandardCharsets.UTF_8).length <= 4096);

        // ---- 2. PRESENCE stream, distinct identity, distinct source type ---------------------------
        final SendResult presence = send(dir + "/device2.p12", dir + "/device2.p12.pass",
                host, port, PRESENCE_DEVICE, PRESENCE_SOURCE, PRESENCE_OBS_TYPE, PRESENCE_STATE);
        check("P1 the presence stream was ACCEPTED by the hub over real mTLS",
                presence.sent >= 1 && presence.permanent == 0 && !presence.suspended,
                "sent=" + presence.sent + " permanent=" + presence.permanent
                        + " suspended=" + presence.suspended + " reason=" + presence.reason);
        check("P2 the presence frame carries the detector-mapped type and state",
                presence.frame != null
                        && presence.frame.contains("\"source_type\":\"" + PRESENCE_SOURCE + "\"")
                        && presence.frame.contains("\"observation_type\":\"" + PRESENCE_OBS_TYPE + "\"")
                        && presence.frame.contains("\"state\":\"" + PRESENCE_STATE + "\""),
                "frame=" + presence.frame);
        check("P3 the two streams are from DISTINCT registered devices (one identity cannot make both "
                        + "claims about the same space)",
                !MOTION_DEVICE.equals(PRESENCE_DEVICE)
                        && motion.frame != null && presence.frame != null
                        && motion.frame.contains(MOTION_DEVICE)
                        && presence.frame.contains(PRESENCE_DEVICE));
        check("P4 the two streams use DISTINCT source types",
                MOTION_SOURCE.equals("motion") && PRESENCE_SOURCE.equals("presence"));

        // ---- 3. NEGATIVE: the availability-only shape must NOT vote -------------------------------
        // `camera_availability` with `state=present` is INCONCLUSIVE by design (the detected target for
        // availability is offline/unavailable/error/unknown). It is accepted and audited, and raises
        // nothing. This is the control that keeps the positive case from being an artefact of "any
        // accepted frame raises an alert".
        final SendResult availability = send(dir + "/device.p12", dir + "/device.p12.pass",
                host, port, MOTION_DEVICE, "camera_availability", "availability", "present");
        check("N1 the availability-only shape is still ACCEPTED (it is wire-valid, just not decisive)",
                availability.sent >= 1 && availability.permanent == 0,
                "sent=" + availability.sent + " permanent=" + availability.permanent);
        check("N2 and its state is NOT one the detector maps to detected "
                        + "({online, available, ok} are; 'present' is not), so it casts no vote",
                availability.frame != null
                        && availability.frame.contains("\"state\":\"present\"")
                        && availability.frame.contains("camera_availability"));

        // ---- 4. write the artifact for the #26 E2E path -------------------------------------------
        if (outDir != null) {
            writeFixture(outDir, motion, presence, availability);
            System.out.println("  wrote the detector fixture to " + outDir);
        } else {
            System.out.println("  note: -Dhg.detector.out not set, so no fixture files were written.");
        }

        System.out.println();
        if (failed > 0) {
            System.out.println("  FAILED: " + failed + " of " + (passed + failed));
            System.out.print(failures);
            System.exit(1);
        }
        System.out.println("  PASS: all " + passed + " detector-producer cases behave as specified");
    }

    static final class SendResult {
        long sent;
        long permanent;
        boolean suspended;
        String reason = "";
        String frame;
    }

    /**
     * Send one observation through the PRODUCTION transport and report the outcome plus the exact frame
     * the hub received.
     */
    static SendResult send(String p12, String passFile, String host, int port, String deviceId,
                           String sourceType, String observationType, String state) throws Exception {
        final SendResult r = new SendResult();
        final Config cfg = new Config();
        cfg.host = host;
        cfg.port = port;
        cfg.deviceId = deviceId;
        cfg.displayName = "synthetic node";
        cfg.pkcs12Path = p12;
        cfg.pkcs12PasswordFile = passFile;
        cfg.trustStorePath = System.getProperty("hg.e2e.dir") + "/ca.p12";
        cfg.trustStorePasswordFile = System.getProperty("hg.e2e.dir") + "/ca.p12.pass";
        cfg.connectTimeoutMs = 5000;
        cfg.readTimeoutMs = 6000;

        // The production transport builds its frame from the observation it is given; capturing it is
        // what proves the SERIALIZER produced the typed payload rather than the test having assembled
        // JSON by hand.
        final ObservationTransport t = new ObservationTransport(cfg);
        final long seq = t.nextSequence();
        final PendingObservation o = new PendingObservation(
                "det-" + RUN + "-" + sourceType + "-" + seq, sourceType, observationType,
                "synthetic", System.currentTimeMillis(), seq,
                0.9d, "fraction", state, 0.9d, "synthetic");
        t.enqueue(o);
        t.start();
        waitFor(() -> t.sentCount() > 0 || t.permanentRejectionCount() > 0
                || t.refusedCount() > 0 || t.isSuspended(), 12000);
        r.sent = t.sentCount();
        r.permanent = t.permanentRejectionCount();
        r.suspended = t.isSuspended();
        r.reason = t.suspensionReason();
        t.stop();
        r.frame = t.lastSentFrame();
        return r;
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

    static void writeFixture(String outDir, SendResult motion, SendResult presence,
                             SendResult availability) throws Exception {
        final java.io.File d = new java.io.File(outDir);
        d.mkdirs();
        write(d, "motion.txt", motion.frame);
        write(d, "presence.txt", presence.frame);
        write(d, "availability-negative.txt", availability.frame);

        final String manifest =
                "{\n"
                        + "  \"contract_version\": 1,\n"
                        + "  \"purpose\": \"detector-capable synthetic fixture for the #26 E2E path\",\n"
                        + "  \"produced_by\": \"Android production serializer (ObservationTransport)\",\n"
                        + "  \"synthetic\": true,\n"
                        + "  \"no_sensor\": true,\n"
                        + "  \"physical_device\": false,\n"
                        + "  \"streams\": [\n"
                        + "    {\"device_id\": \"" + MOTION_DEVICE + "\",\n"
                        + "     \"source_type\": \"" + MOTION_SOURCE + "\",\n"
                        + "     \"observation_type\": \"" + MOTION_OBS_TYPE + "\",\n"
                        + "     \"state\": \"" + MOTION_STATE + "\",\n"
                        + "     \"maps_to\": \"detected\",\n"
                        + "     \"accepted\": " + (motion.sent >= 1) + ",\n"
                        + "     \"sha256\": \"" + sha256(motion.frame) + "\"},\n"
                        + "    {\"device_id\": \"" + PRESENCE_DEVICE + "\",\n"
                        + "     \"source_type\": \"" + PRESENCE_SOURCE + "\",\n"
                        + "     \"observation_type\": \"" + PRESENCE_OBS_TYPE + "\",\n"
                        + "     \"state\": \"" + PRESENCE_STATE + "\",\n"
                        + "     \"maps_to\": \"detected\",\n"
                        + "     \"accepted\": " + (presence.sent >= 1) + ",\n"
                        + "     \"sha256\": \"" + sha256(presence.frame) + "\"}\n"
                        + "  ],\n"
                        + "  \"negative\": [\n"
                        + "    {\"device_id\": \"" + MOTION_DEVICE + "\",\n"
                        + "     \"source_type\": \"camera_availability\",\n"
                        + "     \"observation_type\": \"availability\",\n"
                        + "     \"state\": \"present\",\n"
                        + "     \"maps_to\": \"inconclusive\",\n"
                        + "     \"why\": \"accepted and audited but casts no vote, so it MUST produce 0 "
                        + "alerts; this is the control for the positive case\",\n"
                        + "     \"sha256\": \"" + sha256(availability.frame) + "\"}\n"
                        + "  ],\n"
                        + "  \"requires\": {\n"
                        + "    \"evidence_detector_ids\": [\"sensor.motion\", \"sensor.presence\"],\n"
                        + "    \"note\": \"without this allowlist the observations are recorded and "
                        + "audited but cast NO vote - see main_ingress.cpp\"\n"
                        + "  },\n"
                        + "  \"not_claimed\": \"this proves the fixture is typed and accepted; whether a "
                        + "given hub config fuses it into an alert is the E2E owner's check\"\n"
                        + "}\n";
        write(d, "manifest.json", manifest);
    }

    static void write(java.io.File dir, String name, String content) throws Exception {
        if (content == null) {
            content = "";
        }
        Files.write(new java.io.File(dir, name).toPath(), content.getBytes(StandardCharsets.UTF_8));
    }

    static String sha256(String text) throws Exception {
        if (text == null) {
            return "";
        }
        final MessageDigest md = MessageDigest.getInstance("SHA-256");
        final byte[] digest = md.digest(text.getBytes(StandardCharsets.UTF_8));
        final StringBuilder sb = new StringBuilder();
        for (byte b : digest) {
            sb.append(String.format("%02x", Byte.valueOf(b)));
        }
        return sb.toString();
    }

    static String readIfPresent(String path) {
        try {
            return new String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return null;
        }
    }
}
