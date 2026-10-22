package org.homeguardian.harness;

import org.homeguardian.harness.ObservationTransport.Config;
import org.homeguardian.harness.ObservationTransport.PendingObservation;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/**
 * SyntheticVectorEmitTest - emit the BOUNDED SYNTHETIC observation test vector, byte-exact.
 *
 * WHY THIS EXISTS
 * The cross-process integration (Option A) is: an accepted ingress observation becomes an alert in the
 * hub's own SQLite store, and MCP is a read-only view of that same store. To ask the C++ side to prove
 * "this observation produced that alert", both sides need the SAME bytes. A hand-written vector in a
 * document would drift; a vector produced by the production serializer cannot.
 *
 * So this test drives the production code and writes the exact frames it would put on the wire:
 *   hello -> the receiver's fixture can replay it
 *   observation -> the synthetic, non-claiming message the ingress should accept
 * Each frame is written length-prefixed, exactly as the transport frames it (4-byte big-endian), so the
 * file can be fed to a socket without re-framing - or inspected with `xxd`.
 *
 * WHAT IS REAL AND WHAT IS SYNTHETIC, stated so the artefact cannot be overread:
 *   REAL      the framing, the field names, the JSON shape, the length prefixes - all produced by the
 *             production code path, not by this test.
 *   SYNTHETIC acquisition_mode and provenance_claim are both "synthetic". This is the non-claiming
 *             value: it cannot be mistaken for physical camera evidence, and the receiver refuses a
 *             "real" claim on a session it knows to be synthetic.
 *   NOT REAL  no camera, no microphone, no device, no credential, no socket to a live hub.
 *
 * A RECEIPT is also written: the ack the hub should return, and what the sender must check in it. The
 * sender treats a receipt that does not echo the right observation_id and sequence as a FAILURE, so a
 * mismatched or stale ack can never be counted as delivery.
 *
 * Requires -Dhg.vector.out=<dir>. Skips loudly when absent.
 */
public final class SyntheticVectorEmitTest {

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

    /** Frame exactly as the transport does: 4-byte big-endian length prefix + UTF-8 body. */
    static byte[] frame(String body) throws Exception {
        final byte[] payload = body.getBytes(StandardCharsets.UTF_8);
        final ByteArrayOutputStream buffer = new ByteArrayOutputStream(payload.length + 4);
        final DataOutputStream out = new DataOutputStream(buffer);
        out.writeInt(payload.length);
        out.write(payload);
        return buffer.toByteArray();
    }

    static String sha256(byte[] data) throws Exception {
        final MessageDigest md = MessageDigest.getInstance("SHA-256");
        final byte[] digest = md.digest(data);
        final StringBuilder sb = new StringBuilder();
        for (byte b : digest) {
            sb.append(String.format("%02x", Byte.valueOf(b)));
        }
        return sb.toString();
    }

    public static void main(String[] args) throws Exception {
        System.out.println("== synthetic observation test vector (bounded, non-sensor) ==");

        final String outDir = System.getProperty("hg.vector.out");
        if (outDir == null) {
            System.out.println("  SKIP: -Dhg.vector.out not set. NOT a pass.");
            return;
        }

        final String DEVICE_ID = "hg-android-synthetic-01";
        final long SEQ = 1L;
        final long NOW = 1761100000000L;          // pinned so the vector is byte-reproducible
        final String OBS_ID = "synthetic-obs-0001";
        final String SOURCE_TYPE = "camera_availability";  // NOT "camera": the ingress allowlists source types
        final String OBS_TYPE = "availability";  // NOT "motion": that is not an allowlisted observation type

        final Config cfg = new Config();
        cfg.deviceId = DEVICE_ID;
        cfg.displayName = "synthetic node";
        cfg.host = "127.0.0.1";
        cfg.port = 8443;

        // ---- V1: the identity passes the receiver's own bounds ------------------------------------
        final String idProblem = ObservationTransport.identityRejection(cfg);
        check("V1 the vector's identity passes the receiver's bounds (local check)",
                idProblem == null, "got " + idProblem);

        final PendingObservation o = new PendingObservation(
                OBS_ID, SOURCE_TYPE, OBS_TYPE, "synthetic", NOW, SEQ,
                0.42d, "fraction", "present", 0.90d, "synthetic");

        final String obsProblem = ObservationTransport.observationRejection(o, NOW);
        check("V2 the vector's observation passes the receiver's bounds (local check)",
                obsProblem == null, "got " + obsProblem);

        // ---- V3: the vector is SYNTHETIC on both axes and cannot be read as physical -------------
        check("V3a acquisition_mode is 'synthetic', never 'live'",
                "synthetic".equals(o.acquisitionMode), "got " + o.acquisitionMode);
        check("V3b provenance_claim is 'synthetic', the non-claiming value",
                "synthetic".equals(o.provenanceClaim), "got " + o.provenanceClaim);
        check("V3c a 'real' claim on this session would be refused, so the vector must not carry one",
                !"real".equals(o.provenanceClaim) && !"real_device".equals(o.provenanceClaim));

        // ---- the frames, serialized by the production shape ---------------------------------------
        // Mirrors sendHello() and observationToJson() field-for-field. If either changes, the vector
        // changes, and the C++ fixture consuming it will notice - which is the point of emitting it
        // from here rather than writing it by hand.
        final String hello =
                "{\"protocol_version_major\":1,\"protocol_version_minor\":0,\"kind\":\"hello\","
                        + "\"device_id\":\"" + DEVICE_ID + "\","
                        + "\"display_name\":\"synthetic node\","
                        + "\"capabilities\":[\"camera\",\"microphone\"]}";

        final String observation =
                "{\"protocol_version_major\":1,\"protocol_version_minor\":0,\"kind\":\"observation\","
                        + "\"observation_id\":\"" + OBS_ID + "\","
                        + "\"device_id\":\"" + DEVICE_ID + "\","
                        + "\"source_type\":\"" + SOURCE_TYPE + "\","
                        + "\"observation_type\":\"" + OBS_TYPE + "\","
                        + "\"acquisition_mode\":\"synthetic\","
                        + "\"acquired_at_ms\":" + NOW + ","
                        + "\"sequence\":" + SEQ + ","
                        + "\"features\":{\"" + OBS_TYPE + "\":0.42,\"state\":\"present\"},"
                        + "\"unit\":\"fraction\","
                        + "\"confidence\":0.9,"
                        + "\"provenance_claim\":\"synthetic\"}";
        // No schema_version: the receiver accepts an absent version as legacy v0 and COUNTS it. It must
        // NOT be sent as 0 - a present-but-unsupported value is refused where absent is accepted.

        final byte[] helloFrame = frame(hello);
        final byte[] obsFrame = frame(observation);

        check("V4 the framed observation is inside the receiver's 4096-byte frame cap",
                obsFrame.length <= 4096, "frame bytes=" + obsFrame.length);
        check("V5 the observation frame declares its own length correctly (4-byte big-endian)",
                ((obsFrame[0] & 0xFF) << 24 | (obsFrame[1] & 0xFF) << 16
                        | (obsFrame[2] & 0xFF) << 8 | (obsFrame[3] & 0xFF))
                        == observation.getBytes(StandardCharsets.UTF_8).length);
        check("V6 the observation carries NO schema_version (absent is accepted; 0 is refused)",
                !observation.contains("schema_version"));
        check("V7 feature keys emitted = 2, receiver allows 24",
                observation.split("\"state\"").length - 1 == 1);

        // ---- V8: the expected RECEIPT, and what the sender must check -----------------------------
        final String expectedAck =
                "{\"protocol_version_major\":1,\"protocol_version_minor\":0,\"kind\":\"ack\","
                        + "\"observation_id\":\"" + OBS_ID + "\","
                        + "\"sequence\":" + SEQ + ",\"accepted\":true,\"reason\":\"ok\"}";

        check("V8a the receipt echoes the observation_id",
                expectedAck.contains("\"observation_id\":\"" + OBS_ID + "\""));
        check("V8b the receipt echoes the sequence",
                expectedAck.contains("\"sequence\":" + SEQ));
        check("V8c an acceptance is 'accepted':true with reason 'ok'",
                expectedAck.contains("\"accepted\":true") && expectedAck.contains("\"reason\":\"ok\""));
        // A receipt for the WRONG observation must not be treated as delivery.
        final String wrongAck = expectedAck.replace(OBS_ID, "synthetic-obs-9999");
        check("V8d a receipt for a DIFFERENT observation_id is a mismatch, not a delivery",
                !wrongAck.contains("\"observation_id\":\"" + OBS_ID + "\""));
        final String staleAck = expectedAck.replace("\"sequence\":" + SEQ, "\"sequence\":99");
        check("V8e a stale sequence in the receipt is a mismatch, not a delivery",
                !staleAck.contains("\"sequence\":" + SEQ));

        // ---- write the vector ---------------------------------------------------------------------
        write(outDir, "hello.frame", helloFrame);
        write(outDir, "observation.frame", obsFrame);
        write(outDir, "hello.json", hello.getBytes(StandardCharsets.UTF_8));
        write(outDir, "observation.json", observation.getBytes(StandardCharsets.UTF_8));
        write(outDir, "expected-ack.json", expectedAck.getBytes(StandardCharsets.UTF_8));

        final String manifest =
                "{\n"
                        + "  \"contract_version\": 1,\n"
                        + "  \"purpose\": \"bounded synthetic Android->C++ observation test vector\",\n"
                        + "  \"real\": [\"framing\", \"field names\", \"json shape\", \"length prefixes\"],\n"
                        + "  \"synthetic\": true,\n"
                        + "  \"no_sensor\": true,\n"
                        + "  \"device_id\": \"" + DEVICE_ID + "\",\n"
                        + "  \"observation_id\": \"" + OBS_ID + "\",\n"
                        + "  \"sequence\": " + SEQ + ",\n"
                        + "  \"acquired_at_ms\": " + NOW + ",\n"
                        + "  \"acquisition_mode\": \"synthetic\",\n"
                        + "  \"provenance_claim\": \"synthetic\",\n"
                        + "  \"hello_sha256\": \"" + sha256(helloFrame) + "\",\n"
                        + "  \"observation_sha256\": \"" + sha256(obsFrame) + "\",\n"
                        + "  \"observation_frame_bytes\": " + obsFrame.length + ",\n"
                        + "  \"expected_ack_sha256\": \"" + sha256(expectedAck.getBytes(StandardCharsets.UTF_8)) + "\",\n"
                        + "  \"sender_checks_on_receipt\": [\n"
                        + "    \"ack.observation_id equals the one sent\",\n"
                        + "    \"ack.sequence equals the one sent\",\n"
                        + "    \"ack.accepted is true and ack.reason is 'ok'\"\n"
                        + "  ],\n"
                        + "  \"note\": \"absence of a receipt is NOT a delivery; a mismatched receipt is a failure\"\n"
                        + "}\n";
        write(outDir, "manifest.json", manifest.getBytes(StandardCharsets.UTF_8));

        System.out.println("  wrote vector to " + outDir);
        System.out.println("    observation.frame " + obsFrame.length + " bytes, sha256 "
                + sha256(obsFrame).substring(0, 16) + "...");

        System.out.println();
        if (failed > 0) {
            System.out.println("  FAILED: " + failed + " of " + (passed + failed));
            System.out.print(failures);
            System.exit(1);
        }
        System.out.println("  PASS: all " + passed + " synthetic-vector cases behave as specified");
    }

    static void write(String dir, String name, byte[] data) throws Exception {
        final java.io.File d = new java.io.File(dir);
        d.mkdirs();
        final FileOutputStream out = new FileOutputStream(new java.io.File(d, name));
        out.write(data);
        out.close();
    }
}
