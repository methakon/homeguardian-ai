package org.json;

import java.util.ArrayList;
import java.util.List;

/**
 * JVM test stub for android's org.json.JSONArray.
 *
 * WHY THIS EXISTS
 * The transport's hello frame carries a bounded `capabilities` array, so the real-socket handshake
 * test cannot run off-device without this class. Its absence is what closed the connection during the
 * first TLS run - the hello threw and the peer saw the socket drop - which is exactly the kind of
 * failure that only a real socket reveals.
 *
 * SCOPE, STATED PLAINLY
 * A list of scalars, which is all `capabilities` is. It is NOT a general JSON array: nested objects
 * and arrays are not parsed. Any test relying on nested structure would be testing this stub rather
 * than the device's parser, so none does.
 *
 * The stubs together are a DESKTOP approximation of Android's org.json. Evidence from a test that
 * uses them is desktop evidence. It is not on-device evidence, and the two are reported separately.
 *
 * TEST-ONLY, under apk/tests/, never packaged into the APK.
 */
public class JSONArray {

    private final List<Object> values = new ArrayList<Object>();

    public JSONArray() {
    }

    public JSONArray(String source) throws JSONException {
        String s = source == null ? "" : source.trim();
        if (s.length() < 2 || s.charAt(0) != '[' || s.charAt(s.length() - 1) != ']') {
            throw new JSONException("not an array");
        }
        String body = s.substring(1, s.length() - 1).trim();
        if (body.isEmpty()) {
            return;
        }
        for (String part : body.split(",")) {
            String t = part.trim();
            if (t.startsWith("\"") && t.endsWith("\"") && t.length() >= 2) {
                values.add(t.substring(1, t.length() - 1));
            } else {
                values.add(t);
            }
        }
    }

    public int length() {
        return values.size();
    }

    public Object get(int index) {
        return values.get(index);
    }

    public String getString(int index) {
        return String.valueOf(values.get(index));
    }

    public JSONArray put(Object value) {
        values.add(value);
        return this;
    }

    public JSONArray put(double value) {
        values.add(value);
        return this;
    }

    public JSONArray put(int value) {
        values.add(value);
        return this;
    }

    public JSONArray put(long value) {
        values.add(value);
        return this;
    }

    public JSONArray put(boolean value) {
        values.add(value);
        return this;
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < values.size(); i++) {
            if (i > 0) {
                sb.append(',');
            }
            Object v = values.get(i);
            if (v instanceof Number || "true".equals(v) || "false".equals(v)) {
                sb.append(v);
            } else {
                sb.append('"').append(String.valueOf(v).replace("\"", "\\\"")).append('"');
            }
        }
        return sb.append(']').toString();
    }
}
