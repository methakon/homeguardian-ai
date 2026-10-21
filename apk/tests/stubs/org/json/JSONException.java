package org.json;

/**
 * JVM test stub for android's org.json.JSONException.
 *
 * TEST-ONLY, under apk/tests/, never packaged into the APK. A real class rather than a stub that
 * throws "Stub!", because the handshake tests must be able to observe an unparseable frame being
 * REFUSED (H3), and a stub that threw unconditionally would make that path unreachable.
 */
public class JSONException extends Exception {

    public JSONException(String message) {
        super(message);
    }

    public JSONException(String message, Throwable cause) {
        super(message, cause);
    }
}
