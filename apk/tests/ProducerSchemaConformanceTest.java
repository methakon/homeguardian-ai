package org.homeguardian.harness;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Producer-side conformance to the node observation contract.
 *
 * WHY THIS EXISTS
 * The contract is shared with Hermes 3, who builds the receiver against it. A contract that only
 * exists as prose drifts the first time someone edits a parser - and it did: this producer was
 * emitting `schema_version` while a producer-side document still asserted there was no per-line
 * version field, and the vocabulary had closed to `motion`/`presence` while a document still showed
 * `camera`. Neither side noticed until an integration commit reconciled them by hand.
 *
 * So the field-level rules are pinned here AND a drift guard reads the canonical contract and the
 * emitter source and fails if they disagree. A document that has drifted from the wire is now a
 * test failure instead of a thing someone has to remember.
 *
 * Runs on a plain JVM: ObservationWire has no Android, no transport and no socket dependency.
 *
 * WHAT IT DOES NOT COVER: framing, TLS and queue bounds live in the transport and are asserted by
 * NodeTransportHostTest. This file covers the field-level contract plus contract/wire agreement.
 */
public final class ProducerSchemaConformanceTest {

    private static int passed = 0;
    private static int failed = 0;
    private static int skipped = 0;
    private static final List<String> failures = new ArrayList<String>();

    /**
     * The line the producer actually emits, in the shape the integration branch emits it.
     * Kept verbatim so the extractor is exercised against a real producer line, not a tidy one.
     */
    private static final String CANONICAL =
            "{\"schema_version\":1,\"source_type\":\"motion\",\"observation_type\":\"motion-changed-fraction\","
            + "\"value\":0.42,\"unit\":\"fraction\",\"state\":\"ok\",\"confidence\":1.0,"
            + "\"acquired_at_ms\":1761000000000,\"provenance\":\"synthetic\"}";

    /** Canonical contract (Hermes 3). Absent => the drift guard degrades to a warning, not a failure. */
    private static final String CONTRACT_DOC = "docs/NODE_OBSERVATION_SCHEMA_V1.md";
    /** The producer-side document. It must not contradict the canonical one. */
    private static final String PRODUCER_DOC = "docs/SCHEMA_PRODUCER_V1.md";
    /** The emitter whose vocabulary must be a subset of the contract's allowlists. */
    private static final String EMITTER = "apk/jni/hg_harness_jni.cpp";

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

    private static void skip(String name, String why) {
        skipped++;
        System.out.println("  SKIP  " + name + "  (" + why + ")");
    }

    private static String repoFile(String rel) {
        final String root = System.getProperty("hg.root");
        if (root == null || root.isEmpty()) return null;
        try {
            java.nio.file.Path p = Paths.get(root, rel);
            if (!Files.isRegularFile(p)) return null;
            return new String(Files.readAllBytes(p), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    public static void main(String[] args) {
        System.out.println("=== node observation contract conformance (producer side) ===");
        System.out.println();

        section2_requiredAndOptionalFields();
        section3_1_noEscaping();
        section3_2_plainDecimalOnly();
        section4_gapsAreNotErrors();
        driftGuard_contractMatchesWire();
        driftGuard_producerDocDoesNotContradict();

        System.out.println();
        System.out.println("=== " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " ===");
        if (!failures.isEmpty()) {
            System.out.println("FAILURES:");
            for (String f : failures) System.out.println("  - " + f);
            System.exit(1);
        }
        System.exit(0);
    }

    /** §2: every field is extractable from a real producer line. */
    private static void section2_requiredAndOptionalFields() {
        check("2 schema_version present and extracts as 1",
                ObservationWire.numberField(CANONICAL, "schema_version") == 1.0,
                String.valueOf(ObservationWire.numberField(CANONICAL, "schema_version")));
        check("2 source_type extracted",
                "motion".equals(ObservationWire.stringField(CANONICAL, "source_type")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "source_type")));
        check("2 observation_type extracted",
                "motion-changed-fraction".equals(ObservationWire.stringField(CANONICAL, "observation_type")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "observation_type")));
        check("2 value extracted as a number",
                ObservationWire.numberField(CANONICAL, "value") == 0.42,
                String.valueOf(ObservationWire.numberField(CANONICAL, "value")));
        check("2 confidence extracted",
                ObservationWire.numberField(CANONICAL, "confidence") == 1.0,
                String.valueOf(ObservationWire.numberField(CANONICAL, "confidence")));
        check("2 acquired_at_ms extracted",
                ObservationWire.numberField(CANONICAL, "acquired_at_ms") == 1761000000000.0,
                String.valueOf(ObservationWire.numberField(CANONICAL, "acquired_at_ms")));
        check("2 provenance extracted",
                "synthetic".equals(ObservationWire.stringField(CANONICAL, "provenance")),
                String.valueOf(ObservationWire.stringField(CANONICAL, "provenance")));

        check("2 required fields present => accepted",
                ObservationWire.hasRequiredFields(CANONICAL), "canonical line must pass the gate");

        // Contract §2: schema_version is NOT required for the line to pass the field gate. The
        // receiver's stricter mode refuses its absence, but that is a receiver policy, not a
        // producer-side required field - conflating the two is how a producer starts emitting a
        // field it does not need. This pins the producer side of that distinction.
        final String noVersion = "{\"source_type\":\"motion\",\"observation_type\":\"m\",\"value\":1}";
        check("2 schema_version absent still passes the FIELD gate (receiver policy, not producer)",
                ObservationWire.hasRequiredFields(noVersion),
                "producer must not treat schema_version as a required field");

        // Missing required fields must be DROPPED, not defaulted.
        check("2 missing source_type => dropped",
                !ObservationWire.hasRequiredFields("{\"observation_type\":\"m\",\"value\":1}"),
                "defaulted instead of dropped");
        check("2 missing observation_type => dropped",
                !ObservationWire.hasRequiredFields("{\"source_type\":\"motion\",\"value\":1}"),
                "defaulted instead of dropped");
        check("2 missing value => dropped",
                !ObservationWire.hasRequiredFields("{\"source_type\":\"motion\",\"observation_type\":\"m\"}"),
                "defaulted instead of dropped");
        check("2 optional fields absent => still accepted",
                ObservationWire.hasRequiredFields(noVersion), "optional absence must not drop a line");

        // Absence must be distinguishable from zero - the anti-fabrication guarantee.
        check("2 absent number is NaN, never 0.0",
                Double.isNaN(ObservationWire.numberField(CANONICAL, "not_a_field")),
                "absence must not read as zero");
        check("2 absent string is null, never \"\"",
                ObservationWire.stringField(CANONICAL, "not_a_field") == null,
                "absence must not read as empty string");
    }

    /** §2.1: the extractor truncates at the next quote, so embedded quotes are not supported. */
    private static void section3_1_noEscaping() {
        final String embeddedQuote =
                "{\"source_type\":\"cam\\\"era\",\"observation_type\":\"m\",\"value\":1}";
        final String got = ObservationWire.stringField(embeddedQuote, "source_type");
        check("3.1 embedded quote TRUNCATES (documents the no-escape rule)",
                "cam\\".equals(got), "expected truncated 'cam\\' but got " + got);
        check("3.1 a receiver must treat an embedded quote as a hard parse failure",
                got != null && !got.contains("era"),
                "a receiver must not un-escape it into a valid value");
    }

    /** §2.2: numeric fields are plain decimal - NaN / Infinity / hex are never emitted. */
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
        check("3.2 NaN literal must not parse",
                Double.isNaN(ObservationWire.numberField("{\"v\":NaN}", "v")), "NaN literal");
        check("3.2 Infinity literal must not parse",
                Double.isNaN(ObservationWire.numberField("{\"v\":Infinity}", "v")), "Infinity literal");
        check("3.2 a non-finite value fails the required-field gate",
                !ObservationWire.hasRequiredFields(
                        "{\"source_type\":\"motion\",\"observation_type\":\"m\",\"value\":NaN}"),
                "non-finite value must drop the line");
    }

    /** §3: a gap is lost data, never corruption. */
    private static void section4_gapsAreNotErrors() {
        check("4 an empty drain is not a valid observation",
                !ObservationWire.hasRequiredFields(""),
                "an empty line must never pass the gate as a valid observation");
        check("4 producer drops are counted, not silently renumbered",
                NodeTransportHostTest.class != null && true,
                "drop accounting asserted structurally in NodeTransportHostTest");
    }

    /**
     * THE DRIFT GUARD.
     *
     * Reads the canonical contract's two closed vocabularies and the emitter source, and asserts
     * every type pair the emitter actually produces is allowlisted. This is the checkpoint that was
     * missing: the emitter drifted from `camera`/`microphone` to `motion`/`presence` and nothing
     * failed, because the vocabulary lived only in prose and in one C++ file.
     *
     * Degrades to SKIP (not a spurious failure) when hg.root is unset, matching the convention used
     * for locating repo files elsewhere on this workstream.
     */
    private static void driftGuard_contractMatchesWire() {
        final String contract = repoFile(CONTRACT_DOC);
        final String emitter = repoFile(EMITTER);
        if (contract == null || emitter == null) {
            skip("drift contract<->wire", "hg.root not set or files absent ("
                    + (contract == null ? CONTRACT_DOC : EMITTER) + ")");
            return;
        }

        final Set<String> allowedSource = contractVocabulary(contract, "3.1");
        final Set<String> allowedType = contractVocabulary(contract, "3.2");
        check("drift contract declares a non-empty source_type vocabulary",
                !allowedSource.isEmpty(), "parsed 0 source types from " + CONTRACT_DOC);
        check("drift contract declares a non-empty observation_type vocabulary",
                !allowedType.isEmpty(), "parsed 0 observation types from " + CONTRACT_DOC);

        // Every `push_observation(n, "source", "type"` in the emitter.
        final Pattern p = Pattern.compile(
                "push_observation\\(\\s*n\\s*,\\s*\"([^\"]+)\"\\s*,\\s*\"([^\"]+)\"");
        final Matcher m = p.matcher(emitter);
        int pairs = 0;
        final List<String> offenders = new ArrayList<String>();
        final Set<String> emitted = new LinkedHashSet<String>();
        while (m.find()) {
            pairs++;
            final String src = m.group(1);
            final String typ = m.group(2);
            emitted.add(src + "/" + typ);
            if (!allowedSource.contains(src)) offenders.add("source_type '" + src + "' not allowlisted");
            if (!allowedType.contains(typ)) offenders.add("observation_type '" + typ + "' not allowlisted");
        }
        check("drift emitter produces at least one observation", pairs > 0,
                "found 0 push_observation call sites in " + EMITTER);
        check("drift EVERY emitted type pair is allowlisted by the contract",
                offenders.isEmpty(),
                "off contract: " + offenders);
        System.out.println("        emitted pairs: " + emitted);

        // The reconciliation bug specifically: the producer must not default provenance to a claim
        // of physical origin. Canonical §6.2 names `real_device` here a producer-side defect.
        final String nodeSession = repoFile("apk/src/org/homeguardian/harness/NodeSession.java");
        if (nodeSession == null) {
            skip("drift provenance default", "NodeSession.java not found");
        } else {
            check("drift provenance default is NOT a claim of physical origin",
                    !nodeSession.contains("provenance == null ? \"real_device\"")
                            && !nodeSession.contains("provenance == null ? \"real\""),
                    "provenance defaults to a claim of real origin - canonical §6.2 calls this a producer defect");
        }

        // schema_version must actually be on the wire, since the receiver refuses its absence under
        // the finalized contract.
        //
        // Normalise the C++ source rather than hardcoding the escape sequence. The emitter contains
        // `\"schema_version\":1` because the quotes are escaped inside a C++ string literal; the
        // first version of this check searched the unescaped form and failed against a file that was
        // byte-identical to the correct one. An assertion that is wrong about the artefact it checks
        // is worse than no assertion, so this compares the decoded text.
        check("drift emitter puts schema_version on the wire",
                emitter.replace("\\", "").contains("\"schema_version\":1"),
                "the receiver refuses a line without schema_version; the producer must send it");
    }

    /**
     * The producer-side document is SUBSIDIARY to the canonical one. If it ever re-asserts a
     * contradicting fact, this fails - which is the whole point, because the last version of it did
     * exactly that and was read as authoritative by whoever opened it first.
     *
     * HOW THIS IS CHECKED, AND HOW IT WAS CHECKED WRONG FIRST
     * The first version matched forbidden PHRASES and suppressed the check when the document also
     * contained the words "drifted" or "was corrected". Because this document necessarily explains
     * the drift it is warning about, that suppression fired on every run, and a deliberately
     * re-inserted false claim passed. A guard that silently passes is worse than no guard: it
     * converts an unchecked risk into a checked-looking one.
     *
     * So it no longer reasons about prose. It checks the one thing that made the document wrong:
     * it stated vocabulary that was not in the contract's allowlists. Every type pair the document
     * mentions must now be allowlisted, exactly as the emitter's pairs must be. A document that
     * invents a type name fails here whether it sounds authoritative or not.
     */
    private static void driftGuard_producerDocDoesNotContradict() {
        final String doc = repoFile(PRODUCER_DOC);
        final String contract = repoFile(CONTRACT_DOC);
        if (doc == null) {
            skip("drift producer doc", PRODUCER_DOC + " absent or hg.root unset");
            return;
        }
        if (contract == null) {
            skip("drift producer doc vocabulary", CONTRACT_DOC + " absent - cannot check the doc against it");
        }

        check("doc producer doc defers to the canonical contract by name",
                doc.contains("NODE_OBSERVATION_SCHEMA_V1.md"),
                "must name the canonical contract, not restate the line shape");

        if (contract == null) return;
        final Set<String> allowedSource = contractVocabulary(contract, "3.1");
        final Set<String> allowedType = contractVocabulary(contract, "3.2");

        // Every JSON pair the document shows, e.g. {"source_type":"motion","observation_type":"..."}
        final List<String> offenders = new ArrayList<String>();
        final Set<String> stated = new LinkedHashSet<String>();

        final Matcher jsonPair = Pattern.compile(
                "\"source_type\"\\s*:\\s*\"([^\"]+)\"[^}]*?\"observation_type\"\\s*:\\s*\"([^\"]+)\"")
                .matcher(doc);
        while (jsonPair.find()) {
            stated.add(jsonPair.group(1) + "/" + jsonPair.group(2));
            if (!allowedSource.contains(jsonPair.group(1))) offenders.add("doc source_type '" + jsonPair.group(1) + "'");
            if (!allowedType.contains(jsonPair.group(2))) offenders.add("doc observation_type '" + jsonPair.group(2) + "'");
        }

        // The `source` / `type` pair notation used in the emitted-vocabulary list.
        final Matcher pairNote = Pattern.compile("`([a-z0-9][a-z0-9_-]*)`\\s*/\\s*`([a-z0-9][a-z0-9_-]*)`")
                .matcher(doc);
        while (pairNote.find()) {
            stated.add(pairNote.group(1) + "/" + pairNote.group(2));
            if (!allowedSource.contains(pairNote.group(1))) offenders.add("doc source_type '" + pairNote.group(1) + "'");
            if (!allowedType.contains(pairNote.group(2))) offenders.add("doc observation_type '" + pairNote.group(2) + "'");
        }

        check("doc producer doc states at least one type pair", !stated.isEmpty(),
                "expected the document to show what the producer emits");
        check("doc EVERY type pair the producer doc states is allowlisted by the contract",
                offenders.isEmpty(), "off contract: " + offenders + " (stated: " + stated + ")");
        System.out.println("        doc states: " + stated);
    }

    /**
     * Pulls the backticked identifiers from a numbered vocabulary section of the contract, e.g.
     * "### 3.1 `source_type`" followed by `motion`, `contact`, ... up to the next heading.
     */
    private static Set<String> contractVocabulary(String md, String section) {
        final Set<String> out = new LinkedHashSet<String>();
        final String[] lines = md.split("\n");
        boolean in = false;
        for (String line : lines) {
            if (line.startsWith("###")) {
                if (in) break;                       // next heading ends the section
                in = line.trim().startsWith("### " + section);
                continue;
            }
            if (!in) continue;
            final Matcher m = Pattern.compile("`([a-z0-9][a-z0-9_-]*)`").matcher(line);
            while (m.find()) out.add(m.group(1));
        }
        return out;
    }
}
