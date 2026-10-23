package org.homeguardian.harness;

import android.content.Context;
import android.os.Build;
import android.provider.Settings;

/**
 * Collects the node identity (milestone D-02).
 *
 * Everything gathered here is non-personal and non-sensitive: a device-scoped
 * identifier (Android ID, read without any permission), the operator-visible
 * display name, the API level, the primary ABI and the device model. Here it is
 * hashed into an opaque node id by the native layer; D-03 uses that id for hub
 * registration and authorization.
 *
 * Deliberately NOT collected: accounts, contacts, telephony identifiers (IMEI),
 * location, or any content from the microphone/camera.
 */
public final class DeviceIdentity {

    private DeviceIdentity() {
    }

    /** Device-scoped seed, hashed natively into an opaque node id. */
    public static String deviceSeed(Context ctx) {
        try {
            String id = Settings.Secure.getString(ctx.getContentResolver(),
                    Settings.Secure.ANDROID_ID);
            return id == null ? "" : id;
        } catch (RuntimeException e) {
            return "";
        }
    }

    public static String defaultDisplayName() {
        try {
            String model = Build.MODEL == null ? "android" : Build.MODEL;
            return "Node " + model;
        } catch (RuntimeException e) {
            return "Node";
        }
    }

    public static int apiLevel() {
        return Build.VERSION.SDK_INT;
    }

    public static String primaryAbi() {
        try {
            String[] abis = Build.SUPPORTED_ABIS;
            if (abis != null && abis.length > 0 && abis[0] != null) {
                return abis[0];
            }
        } catch (RuntimeException e) {
            // fall through to the legacy single-ABI field
        }
        return Build.CPU_ABI == null ? "" : Build.CPU_ABI;
    }

    public static String model() {
        return Build.MODEL == null ? "" : Build.MODEL;
    }
}
