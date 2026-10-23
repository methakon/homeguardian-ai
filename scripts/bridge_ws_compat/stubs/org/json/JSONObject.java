package org.json;

import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Lightweight, robust JSONObject stub for desktop testing of OutboundMcpBridge and Provisioner.
 */
public class JSONObject {
    private final Map<String, Object> map = new LinkedHashMap<>();

    public JSONObject() {}

    public JSONObject(String source) throws Exception {
        if (source == null) throw new NullPointerException("source is null");
        parse(source.trim());
    }

    public JSONObject put(String key, Object value) {
        map.put(key, value);
        return this;
    }

    public JSONObject put(String key, int value) {
        map.put(key, Integer.valueOf(value));
        return this;
    }

    public JSONObject put(String key, long value) {
        map.put(key, Long.valueOf(value));
        return this;
    }

    public JSONObject put(String key, boolean value) {
        map.put(key, Boolean.valueOf(value));
        return this;
    }

    public boolean has(String key) {
        return map.containsKey(key);
    }

    public JSONObject optJSONObject(String key) {
        Object val = map.get(key);
        if (val instanceof JSONObject) return (JSONObject) val;
        return null;
    }

    public int optInt(String key, int fallback) {
        Object val = map.get(key);
        if (val instanceof Number) return ((Number) val).intValue();
        if (val instanceof String) {
            try { return Integer.parseInt((String) val); } catch (Exception ignored) {}
        }
        return fallback;
    }

    public int optInt(String key) {
        return optInt(key, 0);
    }

    public long optLong(String key, long fallback) {
        Object val = map.get(key);
        if (val instanceof Number) return ((Number) val).longValue();
        if (val instanceof String) {
            try { return Long.parseLong((String) val); } catch (Exception ignored) {}
        }
        return fallback;
    }

    public long optLong(String key) {
        return optLong(key, 0L);
    }

    public String optString(String key, String fallback) {
        Object val = map.get(key);
        if (val == null) return fallback;
        return val.toString();
    }

    public String optString(String key) {
        return optString(key, "");
    }

    public Object get(String key) throws Exception {
        if (!map.containsKey(key)) throw new Exception("Key not found: " + key);
        return map.get(key);
    }

    public Iterator<String> keys() {
        return map.keySet().iterator();
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        for (Map.Entry<String, Object> e : map.entrySet()) {
            if (!first) sb.append(",");
            first = false;
            sb.append(escapeString(e.getKey())).append(":");
            sb.append(formatValue(e.getValue()));
        }
        sb.append("}");
        return sb.toString();
    }

    private static String formatValue(Object v) {
        if (v == null) return "null";
        if (v instanceof String) {
            String s = (String) v;
            if ((s.startsWith("[") && s.endsWith("]")) || (s.startsWith("{") && s.endsWith("}"))) {
                return s;
            }
            return escapeString(s);
        }
        if (v instanceof Number || v instanceof Boolean) return v.toString();
        if (v instanceof JSONObject) return v.toString();
        return escapeString(v.toString());
    }

    private static String escapeString(String s) {
        StringBuilder sb = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"') sb.append("\\\"");
            else if (c == '\\') sb.append("\\\\");
            else if (c == '\b') sb.append("\\b");
            else if (c == '\f') sb.append("\\f");
            else if (c == '\n') sb.append("\\n");
            else if (c == '\r') sb.append("\\r");
            else if (c == '\t') sb.append("\\t");
            else if (c < 32) sb.append(String.format("\\u%04x", (int) c));
            else sb.append(c);
        }
        sb.append("\"");
        return sb.toString();
    }

    private void parse(String s) throws Exception {
        if (!s.startsWith("{") || !s.endsWith("}")) {
            throw new Exception("Not a valid JSON object: " + s);
        }
        int i = 1;
        int len = s.length() - 1;
        while (i < len) {
            i = skipWhitespace(s, i, len);
            if (i >= len) break;
            if (s.charAt(i) == ',') {
                i++;
                continue;
            }
            if (s.charAt(i) != '"') {
                throw new Exception("Expected string key at " + i);
            }
            int[] res = parseString(s, i);
            String key = s.substring(i + 1, res[0]);
            i = skipWhitespace(s, res[1], len);
            if (i >= len || s.charAt(i) != ':') {
                throw new Exception("Expected ':' after key at " + i);
            }
            i = skipWhitespace(s, i + 1, len);
            Object val;
            char c = s.charAt(i);
            if (c == '"') {
                res = parseString(s, i);
                val = unescapeString(s.substring(i + 1, res[0]));
                i = res[1];
            } else if (c == '{') {
                int objEnd = findMatchingBrace(s, i, '{', '}');
                val = new JSONObject(s.substring(i, objEnd + 1));
                i = objEnd + 1;
            } else if (c == '[') {
                int arrEnd = findMatchingBrace(s, i, '[', ']');
                val = s.substring(i, arrEnd + 1);
                i = arrEnd + 1;
            } else {
                int end = i;
                while (end < len && s.charAt(end) != ',' && s.charAt(end) != '}' && !Character.isWhitespace(s.charAt(end))) {
                    end++;
                }
                String token = s.substring(i, end).trim();
                if ("true".equals(token)) val = Boolean.TRUE;
                else if ("false".equals(token)) val = Boolean.FALSE;
                else if ("null".equals(token)) val = null;
                else {
                    try {
                        val = Long.parseLong(token);
                    } catch (Exception e1) {
                        try {
                            val = Double.parseDouble(token);
                        } catch (Exception e2) {
                            val = token;
                        }
                    }
                }
                i = end;
            }
            map.put(key, val);
        }
    }

    private static int skipWhitespace(String s, int i, int len) {
        while (i < len && Character.isWhitespace(s.charAt(i))) i++;
        return i;
    }

    private static int[] parseString(String s, int start) throws Exception {
        int i = start + 1;
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '"') {
                return new int[]{i, i + 1};
            }
            i++;
        }
        throw new Exception("Unterminated string starting at " + start);
    }

    private static int findMatchingBrace(String s, int start, char open, char close) throws Exception {
        int depth = 0;
        boolean inStr = false;
        for (int i = start; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\' && inStr) {
                i++;
                continue;
            }
            if (c == '"') {
                inStr = !inStr;
                continue;
            }
            if (!inStr) {
                if (c == open) depth++;
                else if (c == close) {
                    depth--;
                    if (depth == 0) return i;
                }
            }
        }
        throw new Exception("Mismatched " + open + close + " starting at " + start);
    }

    private static String unescapeString(String s) {
        if (s.indexOf('\\') == -1) return s;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length()) {
                char next = s.charAt(++i);
                if (next == 'n') sb.append('\n');
                else if (next == 'r') sb.append('\r');
                else if (next == 't') sb.append('\t');
                else if (next == 'b') sb.append('\b');
                else if (next == 'f') sb.append('\f');
                else if (next == '"') sb.append('"');
                else if (next == '\\') sb.append('\\');
                else if (next == 'u' && i + 4 < s.length()) {
                    String hex = s.substring(i + 1, i + 5);
                    sb.append((char) Integer.parseInt(hex, 16));
                    i += 4;
                } else sb.append(next);
            } else {
                sb.append(c);
            }
        }
        return sb.toString();
    }
}
