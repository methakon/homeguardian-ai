package org.json;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * JVM test stub for android's org.json.JSONObject.
 *
 * WHY THIS EXISTS
 * On a device org.json is a platform class; on a plain JVM there is none, and android.jar's version
 * throws for every call. Without this the handshake tests could not run off-device at all - which is
 * why `expectWelcome` had no coverage.
 *
 * IT MUST SERIALIZE CORRECTLY, and an earlier version did not. It stored every value as a String and
 * stringified nested objects, so a hello built by the transport serialized as
 *
 *     "protocol_version_major":"1"                      a number, quoted
 *     "capabilities":"[\"camera\",\"microphone\"]"      an array, as a string
 *
 * The real D-03 ingress refuses that hello, which is how the defect was found: the offline suite had
 * been exercising a wire format that does not exist, and every assertion was written against the same
 * broken output, so nothing disagreed.
 *
 * Numbers, booleans, null, nested objects and nested arrays are now emitted as real JSON, and nested
 * values are parsed into real JSONObject/JSONArray so a test can assert on them.
 *
 * TEST-ONLY: lives under apk/tests/; apk/build_apk.sh compiles apk/src, so nothing shipped can reach it.
 */
public class JSONObject {

    private final Map<String, Object> values = new LinkedHashMap<String, Object>();

    public JSONObject() {
    }

    public JSONObject(String source) throws JSONException {
        final Object parsed = JSONArray.Json.parse(source);
        if (!(parsed instanceof JSONObject)) {
            throw new JSONException("not an object");
        }
        values.putAll(((JSONObject) parsed).values);
    }

    public boolean has(String key) {
        return values.containsKey(key);
    }

    public boolean isEmpty() {
        return values.isEmpty();
    }

    public Object opt(String key) {
        return values.get(key);
    }

    public String optString(String key) {
        return optString(key, "");
    }

    public String optString(String key, String fallback) {
        final Object v = values.get(key);
        return v == null ? fallback : String.valueOf(v);
    }

    public int optInt(String key, int fallback) {
        final Object v = values.get(key);
        if (v instanceof Number) {
            return ((Number) v).intValue();
        }
        try {
            return Integer.parseInt(String.valueOf(v).trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    public long optLong(String key, long fallback) {
        final Object v = values.get(key);
        if (v instanceof Number) {
            return ((Number) v).longValue();
        }
        try {
            return Long.parseLong(String.valueOf(v).trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    public double optDouble(String key, double fallback) {
        final Object v = values.get(key);
        if (v instanceof Number) {
            return ((Number) v).doubleValue();
        }
        try {
            return Double.parseDouble(String.valueOf(v).trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    public boolean optBoolean(String key, boolean fallback) {
        final Object v = values.get(key);
        if (v instanceof Boolean) {
            return ((Boolean) v).booleanValue();
        }
        if (v == null) {
            return fallback;
        }
        final String t = String.valueOf(v).trim();
        if ("true".equalsIgnoreCase(t)) {
            return true;
        }
        if ("false".equalsIgnoreCase(t)) {
            return false;
        }
        return fallback;
    }

    public JSONObject optJSONObject(String key) {
        final Object v = values.get(key);
        return v instanceof JSONObject ? (JSONObject) v : null;
    }

    public JSONArray optJSONArray(String key) {
        final Object v = values.get(key);
        return v instanceof JSONArray ? (JSONArray) v : null;
    }

    public JSONObject put(String key, Object value) {
        values.put(key, value);
        return this;
    }

    /** Used by the parser: stores an already-parsed value without re-wrapping it. */
    void putRaw(String key, Object value) {
        values.put(key, value);
    }

    public JSONObject put(String key, double value) {
        values.put(key, Double.valueOf(value));
        return this;
    }

    public JSONObject put(String key, int value) {
        values.put(key, Integer.valueOf(value));
        return this;
    }

    public JSONObject put(String key, long value) {
        values.put(key, Long.valueOf(value));
        return this;
    }

    public JSONObject put(String key, boolean value) {
        values.put(key, Boolean.valueOf(value));
        return this;
    }

    @Override
    public String toString() {
        final StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        for (Map.Entry<String, Object> e : values.entrySet()) {
            if (!first) {
                sb.append(',');
            }
            first = false;
            sb.append(JSONArray.Json.write(e.getKey()));
            sb.append(':');
            sb.append(JSONArray.Json.write(e.getValue()));
        }
        return sb.append('}').toString();
    }
}
