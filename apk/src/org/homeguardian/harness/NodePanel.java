package org.homeguardian.harness;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * H-03: the operator-visible hub link and node status panel.
 *
 * HG-PANASONIC-SERVICE-TRANSPORT-06 - WHAT CHANGED AND WHY
 * This panel used to OWN the transport: it built the ObservationTransport, held the NodeSession and
 * fed it from {@code sample(...)} on the Activity's tick. {@code MainActivity.onPause()} removes that
 * ticker, so the transport went idle the moment the screen went off - while capture kept running.
 * The node produced media and stopped reporting, which is the opposite of what a headless sensor
 * node is for.
 *
 * The panel now COMMANDS and OBSERVES:
 *   - "Connect" sends an Intent to {@code CctvService}; the service builds and owns the link.
 *   - "Disconnect" sends an Intent; the service tears it down, wake lock included.
 *   - The status text is READ from {@code NodeTransportHost}'s published state. The panel holds NO
 *     reference to the transport, so nothing this object does - or fails to do when it is paused -
 *     can stop the node reporting. That is deliberate, and it is the bug this change removes.
 *
 * It still shows ONLY what it can learn, and nothing is inferred. No observation, payload or
 * credential is ever displayed: only counts, states, and the OS's own battery/thermal/Wi-Fi report.
 */
public final class NodePanel {

    /** Implemented by the Activity: the panel cannot call a native method itself, so it asks. */
    public interface Host {
        String setObservationIntervalMs(int ms);
    }

    private Host host;

    private final Context ctx;
    private final LinearLayout root;
    private final EditText hostEdit;
    private final EditText portEdit;
    private final TextView stateView;
    private String deviceId = "";
    private String displayName = "";

    private long lastFrameCount = -1;
    private long lastFrameSampleMs = 0;
    private double lastFps = 0.0;

    public NodePanel(Context ctx) {
        this.ctx = ctx;
        this.root = new LinearLayout(ctx);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(0, 16, 0, 16);

        final TextView header = new TextView(ctx);
        header.setText("Hub link (Wi-Fi, mutual TLS) - service-owned");
        header.setPadding(0, 12, 0, 4);
        root.addView(header);

        hostEdit = new EditText(ctx);
        hostEdit.setHint("hub host or LAN IP");
        hostEdit.setSingleLine(true);
        root.addView(hostEdit);

        portEdit = new EditText(ctx);
        portEdit.setHint("port (8443)");
        portEdit.setSingleLine(true);
        portEdit.setText("8443");
        root.addView(portEdit);

        // These buttons no longer start a transport. They ASK THE SERVICE to, which is the point.
        root.addView(button("Connect to hub (via service)", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                connect();
            }
        }));
        root.addView(button("Disconnect (via service)", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                disconnect();
            }
        }));

        final EditText intervalEdit = new EditText(ctx);
        intervalEdit.setHint("observation interval ms (100-60000)");
        intervalEdit.setSingleLine(true);
        intervalEdit.setText("1000");
        root.addView(intervalEdit);
        root.addView(button("Apply observation interval", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (host == null) {
                    stateView.setText("interval control unavailable (no host attached)");
                    return;
                }
                int ms;
                try {
                    ms = Integer.parseInt(intervalEdit.getText().toString().trim());
                } catch (RuntimeException e) {
                    stateView.setText("interval must be a number of milliseconds");
                    return;
                }
                // The native side clamps to 100..60000 and reports what it actually set.
                stateView.setText(host.setObservationIntervalMs(ms));
            }
        }));

        root.addView(button("Import credentials", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                // Explicit operator action: copies the four files from the app's own external
                // files dir into private storage, then deletes the source copies.
                final String result = NodeSession.importCredentials(ctx);
                stateView.setText(result);
                refresh("import requested");
            }
        }));

        final TextView importHint = new TextView(ctx);
        importHint.setText("provisioning: drop the four files in "
                + "Android/data/org.homeguardian.harness/files/" + NodeSession.IMPORT_DIR);
        root.addView(importHint);

        stateView = new TextView(ctx);
        stateView.setText("not connected");
        root.addView(stateView);
    }

    public View build() {
        return root;
    }

    public void setHost(Host host) {
        this.host = host;
    }

    public void setIdentity(String deviceId, String displayName) {
        this.deviceId = deviceId == null ? "" : deviceId;
        this.displayName = displayName == null ? "" : displayName;
        refresh("identity set");
    }

    private Button button(String label, View.OnClickListener listener) {
        final Button b = new Button(ctx);
        b.setText(label);
        b.setOnClickListener(listener);
        return b;
    }

    /**
     * Ask the SERVICE to start the link. The endpoint travels as Intent extras and the panel keeps
     * no reference to the resulting transport - so a paused panel cannot stop the node reporting.
     */
    private void connect() {
        final String hubHost = hostEdit.getText().toString().trim();
        final int port;
        try {
            port = Integer.parseInt(portEdit.getText().toString().trim());
        } catch (RuntimeException e) {
            stateView.setText("port must be a number");
            return;
        }
        if (hubHost.length() == 0) {
            stateView.setText("enter the hub host or LAN IP");
            return;
        }
        if (!NodeSession.credentialsPresent(ctx)) {
            // Refuse rather than connect unverified: no certificate, no link. The service repeats
            // this check; failing here just gives the operator a faster, clearer answer.
            stateView.setText("no device credentials in app storage; provisioning is required first"
                    + " (see the H-03 acceptance doc). Nothing was connected.");
            return;
        }
        final Intent i = new Intent(ctx, CctvService.class);
        i.setAction(CctvService.ACTION_LINK_START);
        i.putExtra(CctvService.EXTRA_LINK_HOST, hubHost);
        i.putExtra(CctvService.EXTRA_LINK_PORT, port);
        i.putExtra(CctvService.EXTRA_DEVICE_ID, deviceId);
        i.putExtra(CctvService.EXTRA_DISPLAY_NAME, displayName);
        ctx.startService(i);
        stateView.setText("asked the service to connect to " + hubHost + ":" + port + " ...");
    }

    /** Ask the SERVICE to stop the link. Releasing the wake lock is part of that. */
    private void disconnect() {
        final Intent i = new Intent(ctx, CctvService.class);
        i.setAction(CctvService.ACTION_LINK_STOP);
        ctx.startService(i);
        stateView.setText("asked the service to disconnect");
    }

    // Latest live consent state from the native gate ("consent: camera=... | microphone=...").
    private String consentState = "not polled yet";

    /** Shows the native gate's own reason for each sensor. Sanitized by the native side. */
    public void setConsent(String state) {
        if (state == null || state.isEmpty() || state.equals(consentState)) return;
        this.consentState = state;
        refresh("consent state");
    }

    /**
     * Repaint. Called by the Activity's own (optional) observer tick. Nothing here feeds the link -
     * the link is pumped by the service, so a missing repaint only makes the DISPLAY stale, never
     * the node silent.
     */
    public void refreshView(String nativeStats) {
        refresh(nativeStats);
    }

    /**
     * Compact transport counters for the acceptance log. Machine-checkable and sanitized: counts only
     * - never a payload, a device id, a credential or any media.
     *
     * Read from the host's published counters because the SERVICE owns the transport instance now.
     */
    public String transportCounters() {
        return "transport state=" + NodeTransportHost.state()
                + " queued=" + NodeTransportHost.queuedCountStatic()
                + " sent=" + NodeTransportHost.sentCountStatic()
                + " refused=" + NodeTransportHost.refusedCountStatic()
                + " dropped=" + NodeTransportHost.droppedCountStatic()
                + " permanent=" + NodeTransportHost.permanentRejectionCountStatic()
                + " pumped=" + NodeTransportHost.pumpedTotalStatic()
                + " consent_blocked=" + NodeTransportHost.consentBlocked()
                + " link=" + CctvService.linkStatus();
    }

    private void refresh(String nativeStats) {
        final StringBuilder sb = new StringBuilder();
        sb.append("hub link (service-owned): ").append(NodeTransportHost.state());
        sb.append('\n').append("  ").append(NodeTransportHost.detail());
        sb.append('\n').append("  last stop: ").append(NodeTransportHost.lastStopReason());
        sb.append('\n').append("  consent block: ").append(NodeTransportHost.consentBlocked());
        if (NodeTransportHost.linkHost().length() > 0) {
            sb.append('\n').append("  endpoint: ").append(NodeTransportHost.linkHost())
                    .append(':').append(NodeTransportHost.linkPort());
        }
        sb.append('\n').append("wi-fi: ").append(wifiState());
        sb.append('\n').append(consentState);
        sb.append('\n').append("credentials in app storage: ")
                .append(NodeSession.credentialsPresent(ctx) ? "present" : "MISSING");
        final String cam = permState("android.permission.CAMERA");
        final String mic = permState("android.permission.RECORD_AUDIO");
        sb.append('\n').append("permissions: camera=").append(cam)
                .append(" microphone=").append(mic);

        sb.append('\n').append("transport: queued=").append(NodeTransportHost.queuedCountStatic())
                .append(" sent=").append(NodeTransportHost.sentCountStatic())
                .append(" refused=").append(NodeTransportHost.refusedCountStatic())
                .append(" dropped=").append(NodeTransportHost.droppedCountStatic())
                .append(" permanent=").append(NodeTransportHost.permanentRejectionCountStatic());
        sb.append('\n').append("observations queued by this app: ")
                .append(NodeTransportHost.queuedTotalStatic())
                .append(" parse_failures=").append(NodeTransportHost.parseFailuresStatic());
        sb.append('\n').append("frames written to the hub: ")
                .append(NodeTransportHost.sentCountStatic());
        if (NodeTransportHost.lastSentAtMsStatic() > 0) {
            sb.append(" (last write ").append(ago(NodeTransportHost.lastSentAtMsStatic())).append(")");
        }
        // HONEST GAP: the transport parses and validates the hub's acknowledgment but keeps no
        // counter for it, so "last acknowledged observation" cannot be reported yet. Saying
        // "acknowledged" here would be claiming more than the code knows.
        sb.append('\n').append("hub acknowledgment counter: not exposed by the transport yet")
                .append(" (gap, see the H-03 acceptance doc)");
        sb.append('\n').append("service pump ticks: ").append(NodeTransportHost.pumpedTotalStatic());
        sb.append('\n').append("camera fps (measured): ").append(fps());
        sb.append('\n').append("native: ").append(nativeStats == null ? "(unavailable)" : nativeStats);
        sb.append('\n').append("battery: ").append(batteryState());
        sb.append('\n').append("thermal: ").append(thermalState());
        stateView.setText(sb.toString());
    }

    private String permState(String permission) {
        try {
            final boolean granted = ctx.checkSelfPermission(permission)
                    == android.content.pm.PackageManager.PERMISSION_GRANTED;
            return granted ? "granted" : "NOT granted";
        } catch (RuntimeException e) {
            return "unknown";
        }
    }

    private String wifiState() {
        try {
            final ConnectivityManager cm =
                    (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
            final NetworkInfo info = cm == null ? null : cm.getActiveNetworkInfo();
            if (info == null) {
                return "no active network";
            }
            return (info.isConnected() ? "connected via " : "not connected via ")
                    + (info.getTypeName() == null ? "unknown" : info.getTypeName());
        } catch (SecurityException e) {
            // This is what "wi-fi: unknown" actually was: getActiveNetworkInfo() needs
            // ACCESS_NETWORK_STATE, which the manifest did not declare. Name the cause instead of
            // hiding it behind "unknown".
            return "unknown (ACCESS_NETWORK_STATE not granted)";
        } catch (RuntimeException e) {
            return "unknown (" + e.getClass().getSimpleName() + ")";
        }
    }

    /** FPS from the native frame counter, measured over wall time - never estimated. */
    private String fps() {
        final long now = System.currentTimeMillis();
        final long frames = extractCounter(lastStatsCache, "frames_processed");
        if (frames < 0) {
            return "unavailable";
        }
        if (lastFrameCount < 0) {
            lastFrameCount = frames;
            lastFrameSampleMs = now;
            return "measuring";
        }
        final long elapsed = now - lastFrameSampleMs;
        if (elapsed >= 1000) {
            lastFps = (frames - lastFrameCount) * 1000.0 / elapsed;
            lastFrameCount = frames;
            lastFrameSampleMs = now;
        }
        return String.format(java.util.Locale.US, "%.1f", lastFps);
    }

    private String lastStatsCache = "";

    /** Pushed in by the Activity each observer tick, so fps can be derived without owning anything. */
    public void setNativeStats(String nativeStats) {
        this.lastStatsCache = nativeStats == null ? "" : nativeStats;
    }

    static long extractCounter(String stats, String key) {
        if (stats == null) {
            return -1;
        }
        final int at = stats.indexOf(key + "=");
        if (at < 0) {
            return -1;
        }
        final int from = at + key.length() + 1;
        int to = from;
        while (to < stats.length() && Character.isDigit(stats.charAt(to))) {
            to++;
        }
        try {
            return Long.parseLong(stats.substring(from, to));
        } catch (RuntimeException e) {
            return -1;
        }
    }

    private String batteryState() {
        try {
            final Intent battery = ctx.registerReceiver(null,
                    new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (battery == null) {
                return "unavailable";
            }
            final int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            final int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
            final int status = battery.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            final String charging = status == BatteryManager.BATTERY_STATUS_CHARGING ? "charging"
                    : (status == BatteryManager.BATTERY_STATUS_FULL ? "full" : "discharging");
            if (level < 0 || scale <= 0) {
                return charging;
            }
            return ((int) (level * 100.0 / scale)) + "% " + charging;
        } catch (RuntimeException e) {
            return "unavailable";
        }
    }

    private String thermalState() {
        if (Build.VERSION.SDK_INT < 29) {
            // Honest: the API does not exist on this Android version, so no thermal claim is made.
            return "not exposed on API " + Build.VERSION.SDK_INT;
        }
        // Reached BY REFLECTION on purpose: this project compiles against android-27, so the
        // method is not in the compile-time platform jar. Calling it directly would not build,
        // and compiling against a newer platform would change the target this node ships for.
        try {
            final PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
            if (pm == null) {
                return "unavailable";
            }
            final Object status = PowerManager.class.getMethod("getCurrentThermalStatus").invoke(pm);
            return "status " + status;
        } catch (Throwable e) {
            return "unavailable";
        }
    }

    private static String ago(long atMs) {
        final long seconds = (System.currentTimeMillis() - atMs) / 1000;
        return seconds <= 1 ? "just now" : (seconds + " s ago");
    }
}
