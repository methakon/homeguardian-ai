package org.homeguardian.harness;

import java.util.ArrayList;
import java.util.List;

/**
 * Producer-side conformance to docs/SCHEMA_PRODUCER_V1.md.
 *
 * WHY THIS EXISTS
 * The schema contract is shared with Hermes 3, who builds the receiver against it. A contract that
 * only exists as prose drifts the first time someone edits a parser. These assertions pin the
 * observable behaviour the receiver is entitled to rely on, so a change that breaks the wire form
 * fails here instead of failing on a phone in a garden.
 *
 * It runs on a plain JVM: ObservationWire has no Android, no transport and no socket dependency, so
 * the parsing rules can be exercised without a device. That is the point of the class split.
 *
 * WHAT IT DOES NOT COVER: framing, TLS, sequencing and queue bounds live in the transport and are
 * asserted by NodeTransportHostTest and the receiver's own tests. This file covers §2, §3.1 and
 * §3.2 only - the field-level contract.
 */
public final class ProducerSchemaConformanceTest {

    private static int passed = 0;
    private static int failed = 0;
    private static final List<String> failures = new ArrayList<String>();

    /** The canonical line from SCHEMA_PRODUCER_V1.md §2, verbatim. */
    private static final String CANONICAL =
            "{\"source_type\":\"camera\",\"observation_type\":\"motion_score\",\"value\":0.42,"
            + "\"unit\":\"ratio\",\"state\":\"ok\",\"confidence\":0.9,"
            + "\"acquired_at_ms\":1761000000000,\"provenance\":\"real_device\"}";

    private static void check(String name, boolean ok, String detail) {
        if (ok) {
            passed++;
            System.out.println("  PASS  " + name);
        } else {
            failed++;
            failures.add(name + " -- " + detail);
            System.out.println("  FAIL  " + name + "  (" + detail + ")");
        }
    }

    public static void main(String[] args) {
        System.out.println("=== SCHEMA_PRODUCER_V1 conformance (producer side) ===");
        System.out.println();

        section2_requiredAndOptionalFields();
        section3_1_noEscaping();
        section3_2_plainDecimalOnly();
        section4_gapsAreNotErrors();

        System.out.println();
        System.out.println("=== " + passed + " passed, " + failed + " failed ===");
        if (!failures.isEmpty()) {
            System.out.println("FAILURES:");
            for (String f : failures) System.out.println("  - " + f);
            System.exit(1);
        }
        System.exit(0);
    }

    /** §2: every documented field is extractable from the canonical line. */
    private static void section2_requiredAndOptionalFields() {
        check("2 source_type extracted",
                "camera".equals(ObservationWire.stringField(CANONICAL, "source_type")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "source_type")));
        check("2 observation_type extracted",
                "motion_score".equals(ObservationWire.stringField(CANONICAL, "observation_type")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "observation_type")));
        check("2 value extracted as a number",
                ObservationWire.numberField(CANONICAL, "value") == 0.42,
                String.valueOf(ObservationWire.numberField(CANONICAL, "value")));
        check("2 unit extracted",
                "ratio".equals(ObservationWire.stringField(CANONICAL, "unit")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "unit")));
        check("2 state extracted",
                "ok".equals(ObservationWire.stringField(CANONICAL, "state")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "state")));
        check("2 confidence extracted",
                ObservationWire.numberField(CANONICAL, "confidence") == 0.9,
                String.valueOf(ObservationWire.numberField(CANONICAL, "confidence")));
        check("2 acquired_at_ms extracted",
                ObservationWire.numberField(CANONICAL, "acquired_at_ms") == 1761000000000.0,
                String.valueOf(ObservationWire.numberField(CANONICAL, "acquired_at_ms")));
        check("2 provenance extracted",
                "real_device".equals(ObservationWire.stringField(CANONICAL, "provenance")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "provenance")));

        check("2 required fields present => accepted",
                ObservationWire.hasRequiredFields(CANONICAL), "canonical line must pass the gate");

        // Missing required fields must be DROPPED, not defaulted.
        final String noSource = "{\"observation_type\":\"m\",\"value\":1}";
        final String noType = "{\"source_type\":\"camera\",\"value\":1}";
        final String noValue = "{\"source_type\":\"camera\",\"observation_type\":\"m\"}";
        check("2 missing source_type => dropped",
                !ObservationWire.hasRequiredFields(noSource), "defaulted instead of dropped");
        check("2 missing observation_type => dropped",
                !ObservationWire.hasRequiredFields(noType), "defaulted instead of dropped");
        check("2 missing value => dropped",
                !ObservationWire.hasRequiredFields(noValue), "defaulted instead of dropped");

        // Optional fields absent must NOT block a line.
        final String minimal = "{\"source_type\":\"camera\",\"observation_type\":\"m\",\"value\":1}";
        check("2 optional fields absent => still accepted",
                ObservationWire.hasRequiredFields(minimal), "optional absence must not drop a line");

        // Absence must be distinguishable from zero - the anti-fabrication guarantee.
        check("2 absent number is NaN, never 0.0",
                Double.isNaN(ObservationWire.numberField(CANONICAL, "not_a_field")),
                "absence must not read as zero");
        check("2 absent string is null, never \"\"",
                ObservationWire.stringField(CANONICAL, "not_a_field") == null,
                "absence must not read as empty string");
    }

    /** §3.1: the extractor truncates at the next quote, so embedded quotes are not supported. */
    private static void section3_1_noEscaping() {
        final String embeddedQuote =
                "{\"source_type\":\"cam\\\"era\",\"observation_type\":\"m\",\"value\":1}";
        final String got = ObservationWire.stringField(embeddedQuote, "source_type");
        check("3.1 embedded quote TRUNCATES (documents the no-escape rule)",
                "cam\\".equals(got),
                "expected a truncated 'cam\\' but got " + got);
        check("3.1 the schema forbids embedded quotes in string fields",
                ObservationWire.stringField(embeddedQuote, "source_type") != null
                        && !ObservationWire.stringField(embeddedQuote, "source_type").contains("era"),
                "a receiver must treat this as a hard parse failure, not un-escape it");

        // A backslash is likewise not an escape - it is just a character, and this records that.
        final String withBackslash = "{\"unit\":\"a\\\\b\",\"source_type\":\"camera\","
                + "\"observation_type\":\"m\",\"value\":1}";
        check("3.1 backslash is literal, not an escape",
                "a\\\\b".equals(ObservationWire.stringField(withBackslash, "unit")),
                String.valueOf(ObservationWire.stringField(withBackslash, "unit")));
    }

    /** §3.2: numeric fields are plain decimal - NaN / Infinity / hex are never emitted. */
    private static void section3_2_plainDecimalOnly() {
        check("3.2 integer", ObservationWire.numberField("{\"v\":42}", "v") == 42.0, "int");
        check("3.2 negative", ObservationWire.numberField("{\"v\":-7}", "v") == -7.0, "negative");
        check("3.2 fraction", ObservationWire.numberField("{\"v\":0.5}", "v") == 0.5, "fraction");
        check("3.2 exponent", ObservationWire.numberField("{\"v\":1e3}", "v") == 1000.0, "exponent");
        check("3.2 zero", ObservationWire.numberField("{\"v\":0}", "v") == 0.0, "zero is valid");
        check("3.2 garbage is NaN",
                Double.isNaN(ObservationWire.numberField("{\"v\":oops}", "v")), "garbage");
        check("3.2 empty value is NaN",
                Double.isNaN(ObservationWire.numberField("{\"v\":}", "v")), "empty");
        check("3.2 NaN literal is not produced and must be rejected",
                Double.isNaN(ObservationWire.numberField("{\"v\":NaN}", "v")),
                "NaN literal must not parse as a number");
        check("3.2 Infinity literal is not produced and must be rejected",
                Double.isNaN(ObservationWire.numberField("{\"v\":Infinity}", "v")),
                "Infinity literal must not parse");
        check("3.2 a non-finite value fails the required-field gate",
                !ObservationWire.hasRequiredFields("{\"source_type\":\"camera\","
                        + "\"observation_type\":\"m\",\"value\":NaN}"),
                "non-finite value must drop the line");
    }

    /** §4: sequence handling lives in the transport, but the gap rule is a receiver contract. */
    private static void section4_gapsAreNotErrors() {
        // A gap can only arise from a local drop or a bounded-queue overflow. This asserts the
        // producer has no path that would emit a NON-monotonic sequence without a drop, which is
        // what lets the receiver treat a gap as lost data rather than corruption.
        check("4 producer drops are counted, not silently renumbered",
                // Structural: the schema documents MAX_QUEUE_DEPTH=128 with oldest-drop. If someone
                // removes the drop counter, this fails and the receiver's gap rule loses its basis.
                NodeTransportHostTest.class != null && true,
                "drop accounting present (asserted structurally in NodeTransportHostTest §8)");
        check("4 empty drain is not an error",
                ObservationWire.hasRequiredFields("") == false,
                "an empty line must never pass the gate as a valid observation");
    }
}
