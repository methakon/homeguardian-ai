package org.json;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * JVM test stub for android's org.json.JSONObject.
 *
 * WHY THIS EXISTS
 * The transport parses hub frames with JSONObject. On a device that is the platform class; on a plain
 * JVM there is none, and android.jar's version throws for every call. The handshake tests could not
 * run off-device until this existed - which is exactly why `expectWelcome` had no coverage at all.
 *
 * WHAT IT IS AND IS NOT
 * It is a small, correct parser for the FLAT objects these frames are: string and number values at
 * the top level. It is deliberately not a general JSON implementation and does not parse nested
 * objects or arrays; any nested value is kept as a raw string. That is sufficient for hello, welcome,
 * acknowledgement and error frames, whose fields are all scalars, and it is NOT sufficient for an
 * observation frame's `features` - so no test asserts on a parsed `features` object.
 *
 * This divergence is a real limitation and is stated rather than hidden: a test that passed here while
 * disagreeing with the device's real parser would be worse than no test. Every value the handshake
 * tests read is a scalar, so the subset is exact for what is under test.
 *
 * TEST-ONLY. It lives under apk/tests/, and apk/build_apk.sh compiles apk/src, so nothing in the
 * shipped application can reach it.
 */
public class JSONObject {

    private final Map<String, Object> values = new LinkedHashMap<String, Object>();

    public JSONObject() {
    }

    public JSONObject(String source) throws JSONException {
        parse(source == null ? "" : source.trim());
    }

    /** Minimal flat-object parser: {"key": value, ...} with string or numeric values. */
    private void parse(String s) throws JSONException {
        if (s.isEmpty()) {
            throw new JSONException("empty");
        }
        if (s.charAt(0) != '{' || s.charAt(s.length() - 1) != '}') {
            throw new JSONException("not an object");
        }
        String body = s.substring(1, s.length() - 1).trim();
        int i = 0;
        while (i < body.length()) {
            while (i < body.length() && (body.charAt(i) == ',' || Character.isWhitespace(body.charAt(i)))) {
                i++;
            }
            if (i >= body.length()) {
                break;
            }
            if (body.charAt(i) != '"') {
                throw new JSONException("expected a key at " + i);
            }
            int keyEnd = body.indexOf('"', i + 1);
            if (keyEnd < 0) {
                throw new JSONException("unterminated key");
            }
            String key = body.substring(i + 1, keyEnd);
            i = keyEnd + 1;
            while (i < body.length() && (Character.isWhitespace(body.charAt(i)) || body.charAt(i) == ':')) {
                i++;
            }
            if (i >= body.length()) {
                throw new JSONException("missing value for " + key);
            }
            Object value;
            char c = body.charAt(i);
            if (c == '"') {
                int end = i + 1;
                StringBuilder sb = new StringBuilder();
                while (end < body.length()) {
                    char d = body.charAt(end);
                    if (d == '\\' && end + 1 < body.length()) {
                        sb.append(body.charAt(end + 1));
                        end += 2;
                        continue;
                    }
                    if (d == '"') {
                        break;
                    }
                    sb.append(d);
                    end++;
                }
                if (end >= body.length()) {
                    throw new JSONException("unterminated string for " + key);
                }
                value = sb.toString();
                i = end + 1;
            } else {
                int end = i;
                int depth = 0;
                while (end < body.length()) {
                    char d = body.charAt(end);
                    if (d == '[' || d == '{') { depth++; }
                    else if (d == ']' || d == '}') { if (depth == 0) { break; } depth--; }
                    else if (d == ',' && depth == 0) { break; }
                    end++;
                }
                value = body.substring(i, end).trim();
                i = end;
            }
            values.put(key, value);
        }
    }

    public boolean has(String key) {
        return values.containsKey(key);
    }

    public Object opt(String key) {
        return values.get(key);
    }

    public String optString(String key) {
        return optString(key, "");
    }

    public String optString(String key, String fallback) {
        Object v = values.get(key);
        return v == null ? fallback : String.valueOf(v);
    }

    public int optInt(String key, int fallback) {
        try {
            return Integer.parseInt(String.valueOf(values.get(key)).trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    public long optLong(String key, long fallback) {
        try {
            return Long.parseLong(String.valueOf(values.get(key)).trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    public double optDouble(String key, double fallback) {
        try {
            return Double.parseDouble(String.valueOf(values.get(key)).trim());
        } catch (Exception e) {
            return fallback;
        }
    }

    public boolean optBoolean(String key, boolean fallback) {
        Object v = values.get(key);
        if (v == null) {
            return fallback;
        }
        String t = String.valueOf(v).trim();
        if ("true".equalsIgnoreCase(t)) { return true; }
        if ("false".equalsIgnoreCase(t)) { return false; }
        return fallback;
    }

    public JSONObject put(String key, Object value) {
        values.put(key, value);
        return this;
    }

    public JSONObject put(String key, double value) {
        values.put(key, String.valueOf(value));
        return this;
    }

    public JSONObject put(String key, int value) {
        values.put(key, String.valueOf(value));
        return this;
    }

    public JSONObject put(String key, long value) {
        values.put(key, String.valueOf(value));
        return this;
    }

    public JSONObject put(String key, boolean value) {
        values.put(key, String.valueOf(value));
        return this;
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        for (Map.Entry<String, Object> e : values.entrySet()) {
            if (!first) { sb.append(','); }
            first = false;
            sb.append('"').append(e.getKey()).append("\":");
            Object v = e.getValue();
            if (v instanceof Number || "true".equals(v) || "false".equals(v)) {
                sb.append(v);
            } else {
                sb.append('"').append(String.valueOf(v).replace("\"", "\\\"")).append('"');
            }
        }
        return sb.append('}').toString();
    }
}
