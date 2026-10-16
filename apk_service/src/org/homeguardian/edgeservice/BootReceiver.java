package org.homeguardian.edgeservice;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * Starts the edge service when the device finishes booting.
 *
 * This app is NOT direct-boot aware: its state (the credential file, the SQLite
 * audit store) lives in credential-encrypted storage, which is unreadable until the
 * user unlocks the device the first time after a reboot. So the correct, honest
 * behaviour is the one Android gives us — BOOT_COMPLETED is delivered AFTER the
 * first unlock, and the server comes back then. ACTION_LOCKED_BOOT_COMPLETED is
 * deliberately NOT handled, because starting the server before unlock would mean
 * either storing the credential unencrypted or failing closed on every boot.
 */
public class BootReceiver extends BroadcastReceiver {

    private static final String TAG = "HGEdge";

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = (intent == null) ? "(null)" : intent.getAction();
        Log.i(TAG, "BootReceiver action=" + action);
        McpProbe.appendLog(context, "BOOT_RECEIVED action=" + action);

        Intent svc = new Intent(context, EdgeService.class);
        svc.setAction(EdgeService.ACTION_START);
        try {
            context.startForegroundService(svc);
        } catch (Throwable t) {
            // Recorded, not swallowed: a boot failure must be visible in the probe log.
            Log.e(TAG, "could not start service from boot: " + t);
            McpProbe.appendLog(context, "BOOT_START_FAILED " + t);
        }
    }
}
