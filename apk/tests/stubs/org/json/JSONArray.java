package org.json;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

/**
 * JVM test stub for android's org.json.JSONArray.
 *
 * WHY THIS EXISTS
 * On a device org.json is a platform class; on a plain JVM there is none, and android.jar's version
 * throws for every call. The offline harness needs BOTH directions: the transport serializes its
 * frames with these classes and parses the hub's replies with them.
 *
 * IT MUST SERIALIZE CORRECTLY, and an earlier version of this stub did not. It kept nested values as
 * raw strings on the way out, so a hello built by the transport came out as
 *
 *     "capabilities":"[\"camera\",\"microphone\"]"
 *
 * - a JSON STRING where the receiver requires an ARRAY. The real D-03 ingress refused that hello,
 * which is how the defect was found. This matters more than a weak assertion would: a stub that
 * mis-serializes makes the test exercise a wire format that does not exist, and it fails in the
 * direction of looking fine, because every assertion is written against the same broken output.
 *
 * So: small, but correct for the JSON this protocol uses - strings, numbers, booleans, null, nested
 * objects and nested arrays.
 *
 * TEST-ONLY: lives under apk/tests/; apk/build_apk.sh compiles apk/src, so nothing shipped can reach it.
 */
public class JSONArray {

    private final List<Object> values = new ArrayList<Object>();

    public JSONArray() {
    }

    public JSONArray(String source) throws JSONException {
        final Object parsed = Json.parse(source);
        if (!(parsed instanceof JSONArray)) {
            throw new JSONException("not an array");
        }
        values.addAll(((JSONArray) parsed).values);
    }

    public int length() {
        return values.size();
    }

    public Object get(int index) {
        return values.get(index);
    }

    public Object opt(int index) {
        return index >= 0 && index < values.size() ? values.get(index) : null;
    }

    public String getString(int index) {
        return String.valueOf(values.get(index));
    }

    public String optString(int index, String fallback) {
        final Object v = opt(index);
        return v == null ? fallback : String.valueOf(v);
    }

    public JSONObject optJSONObject(int index) {
        final Object v = opt(index);
        return v instanceof JSONObject ? (JSONObject) v : null;
    }

    public JSONArray put(Object value) {
        values.add(value);
        return this;
    }

    public JSONArray put(double value) {
        values.add(Double.valueOf(value));
        return this;
    }

    public JSONArray put(int value) {
        values.add(Integer.valueOf(value));
        return this;
    }

    public JSONArray put(long value) {
        values.add(Long.valueOf(value));
        return this;
    }

    public JSONArray put(boolean value) {
        values.add(Boolean.valueOf(value));
        return this;
    }

    public List<Object> asList() {
        return values;
    }

    @Override
    public String toString() {
        return Json.write(values);
    }

    // ---------------------------------------------------------------------------------------------
    // One scanner and one writer, shared with JSONObject so the two cannot drift apart.
    // ---------------------------------------------------------------------------------------------
    static final class Json {

        private final String s;
        private int i = 0;

        private Json(String source) {
            this.s = source == null ? "" : source.trim();
        }

        static Object parse(String source) throws JSONException {
            final Json j = new Json(source);
            final Object v = j.parseValue();
            j.skipWs();
            if (j.i < j.s.length()) {
                throw new JSONException("trailing data at " + j.i);
            }
            return v;
        }

        Object parseValue() throws JSONException {
            skipWs();
            if (i >= s.length()) {
                throw new JSONException("unexpected end of input");
            }
            final char c = s.charAt(i);
            if (c == '{') {
                return parseObject();
            }
            if (c == '[') {
                return parseArray();
            }
            if (c == '"') {
                return parseString();
            }
            if (s.startsWith("true", i)) {
                i += 4;
                return Boolean.TRUE;
            }
            if (s.startsWith("false", i)) {
                i += 5;
                return Boolean.FALSE;
            }
            if (s.startsWith("null", i)) {
                i += 4;
                return null;
            }
            return parseNumber();
        }

        private JSONObject parseObject() throws JSONException {
            final JSONObject out = new JSONObject();
            i++;  // '{'
            skipWs();
            if (i < s.length() && s.charAt(i) == '}') {
                i++;
                return out;
            }
            while (i < s.length()) {
                skipWs();
                if (i >= s.length() || s.charAt(i) != '"') {
                    throw new JSONException("expected a key at " + i);
                }
                final String key = parseString();
                skipWs();
                if (i >= s.length() || s.charAt(i) != ':') {
                    throw new JSONException("expected ':' after " + key);
                }
                i++;
                out.putRaw(key, parseValue());
                skipWs();
                if (i < s.length() && s.charAt(i) == ',') {
                    i++;
                    continue;
                }
                if (i < s.length() && s.charAt(i) == '}') {
                    i++;
                    return out;
                }
                throw new JSONException("expected ',' or '}' at " + i);
            }
            throw new JSONException("unterminated object");
        }

        private JSONArray parseArray() throws JSONException {
            final JSONArray out = new JSONArray();
            i++;  // '['
            skipWs();
            if (i < s.length() && s.charAt(i) == ']') {
                i++;
                return out;
            }
            while (i < s.length()) {
                out.values.add(parseValue());
                skipWs();
                if (i < s.length() && s.charAt(i) == ',') {
                    i++;
                    continue;
                }
                if (i < s.length() && s.charAt(i) == ']') {
                    i++;
                    return out;
                }
                throw new JSONException("expected ',' or ']' at " + i);
            }
            throw new JSONException("unterminated array");
        }

        private String parseString() throws JSONException {
            i++;  // opening quote
            final StringBuilder sb = new StringBuilder();
            while (i < s.length()) {
                final char c = s.charAt(i);
                if (c == '\\' && i + 1 < s.length()) {
                    final char esc = s.charAt(i + 1);
                    switch (esc) {
                        case 'n': sb.append('\n'); break;
                        case 't': sb.append('\t'); break;
                        case 'r': sb.append('\r'); break;
                        case 'b': sb.append('\b'); break;
                        case 'f': sb.append('\f'); break;
                        case 'u':
                            if (i + 5 < s.length()) {
                                sb.append((char) Integer.parseInt(s.substring(i + 2, i + 6), 16));
                                i += 4;
                            }
                            break;
                        default: sb.append(esc); break;
                    }
                    i += 2;
                    continue;
                }
                if (c == '"') {
                    i++;
                    return sb.toString();
                }
                sb.append(c);
                i++;
            }
            throw new JSONException("unterminated string");
        }

        private Object parseNumber() throws JSONException {
            final int start = i;
            while (i < s.length()) {
                final char c = s.charAt(i);
                if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.'
                        || c == 'e' || c == 'E') {
                    i++;
                } else {
                    break;
                }
            }
            final String text = s.substring(start, i);
            if (text.isEmpty()) {
                throw new JSONException("expected a value at " + start);
            }
            try {
                if (text.indexOf('.') < 0 && text.indexOf('e') < 0 && text.indexOf('E') < 0) {
                    return Long.valueOf(text);
                }
                return Double.valueOf(text);
            } catch (NumberFormatException e) {
                throw new JSONException("bad number: " + text);
            }
        }

        private void skipWs() {
            while (i < s.length() && Character.isWhitespace(s.charAt(i))) {
                i++;
            }
        }

        /** Serialize a value. Numbers and booleans stay bare; strings are quoted and escaped. */
        static String write(Object value) {
            if (value == null) {
                return "null";
            }
            if (value instanceof JSONObject || value instanceof JSONArray) {
                return value.toString();
            }
            if (value instanceof Boolean || value instanceof Number) {
                return String.valueOf(value);
            }
            if (value instanceof Collection) {
                return write((Collection<?>) value);
            }
            final StringBuilder sb = new StringBuilder();
            sb.append('"');
            final String text = String.valueOf(value);
            for (int k = 0; k < text.length(); k++) {
                final char c = text.charAt(k);
                switch (c) {
                    case '"': sb.append("\\\""); break;
                    case '\\': sb.append("\\\\"); break;
                    case '\n': sb.append("\\n"); break;
                    case '\r': sb.append("\\r"); break;
                    case '\t': sb.append("\\t"); break;
                    default:
                        if (c < 0x20) {
                            sb.append(String.format("\\u%04x", Integer.valueOf(c)));
                        } else {
                            sb.append(c);
                        }
                        break;
                }
            }
            return sb.append('"').toString();
        }

        static String write(Collection<?> items) {
            final StringBuilder sb = new StringBuilder("[");
            boolean first = true;
            for (Object o : items) {
                if (!first) {
                    sb.append(',');
                }
                first = false;
                sb.append(write(o));
            }
            return sb.append(']').toString();
        }
    }
}
