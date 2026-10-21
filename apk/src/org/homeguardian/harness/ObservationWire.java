package org.homeguardian.harness;

/**
 * The producer's NDJSON field extraction - the parsing rules half of the wire contract.
 *
 * WHY THIS IS ITS OWN CLASS
 * These two methods ARE the contract's §3.1 and §3.2: what a string field may contain, and what a
 * numeric field may contain. They used to live inside {@code NodeSession}, which pulls in Android and
 * the TLS transport, so the rules could only be exercised on a device. Lifting them here makes them
 * dependency-free and therefore directly testable on a plain JVM - and it gives the receiver side
 * (Hermes 3) one obvious place to read what it must accept.
 *
 * See docs/SCHEMA_PRODUCER_V1.md. The behaviour is unchanged from NodeSession's originals, byte for
 * byte; NodeSession now delegates here so there is exactly one implementation.
 *
 * THE RULES, EXPLICITLY
 *  - {@link #stringField} is a substring scan, NOT a JSON parser. It reads a value as everything up
 *    to the next double quote. So a string field MUST NOT contain `"` or `\`; they do not occur in
 *    the closed enumerations the producer emits. A receiver should treat an embedded quote as a hard
 *    parse failure rather than attempting to un-escape it.
 *  - {@link #numberField} accepts plain decimal only: digits, sign, dot, exponent. `NaN`,
 *    `Infinity` and hex are never emitted and must be rejected rather than coerced.
 *  - Absence is reported as {@code null} / {@code NaN} - never as a silent zero. That distinction is
 *    what lets the caller drop a malformed line instead of recording a fake measurement.
 */
public final class ObservationWire {

    private ObservationWire() {
    }

    /**
     * Extract {@code "key":"value"}. Returns null when the key is absent, which the caller must treat
     * as a missing required field rather than as an empty string.
     */
    public static String stringField(String line, String key) {
        if (line == null || key == null) {
            return null;
        }
        final String needle = "\"" + key + "\":\"";
        final int at = line.indexOf(needle);
        if (at < 0) {
            return null;
        }
        final int from = at + needle.length();
        final int to = line.indexOf('"', from);
        return to < 0 ? null : line.substring(from, to);
    }

    /**
     * Extract {@code "key":number}. Returns {@link Double#NaN} when absent or unparseable, so a
     * caller can never mistake "no value" for "zero" - the difference between a dropped line and a
     * fabricated measurement.
     */
    public static double numberField(String line, String key) {
        if (line == null || key == null) {
            return Double.NaN;
        }
        final String needle = "\"" + key + "\":";
        final int at = line.indexOf(needle);
        if (at < 0) {
            return Double.NaN;
        }
        final int from = at + needle.length();
        int to = from;
        while (to < line.length()) {
            final char c = line.charAt(to);
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                to++;
            } else {
                break;
            }
        }
        try {
            return Double.parseDouble(line.substring(from, to));
        } catch (RuntimeException e) {
            return Double.NaN;
        }
    }

    /**
     * Whether a parsed line carries the fields the schema marks REQUIRED: {@code source_type},
     * {@code observation_type} and a finite {@code value}. A line failing this is dropped by the
     * producer and counted in {@code parse_failures}; it is never forwarded.
     */
    public static boolean hasRequiredFields(String line) {
        if (stringField(line, "source_type") == null) {
            return false;
        }
        if (stringField(line, "observation_type") == null) {
            return false;
        }
        final double v = numberField(line, "value");
        return !Double.isNaN(v) && !Double.isInfinite(v);
    }
}
