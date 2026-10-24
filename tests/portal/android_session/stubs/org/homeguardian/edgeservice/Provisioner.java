package org.homeguardian.edgeservice;
import android.content.Context;
/** Host-only credential source; never reads/writes real credentials. Target matches production. */
public final class Provisioner {
    public static final String LOOPBACK = "127.0.0.1";
    public static final int DEFAULT_PORT = 8790;
    public static String readToken(Context ignored) { return "host-test-bearer"; }
}
