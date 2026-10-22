package org.homeguardian.edgeservice;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import org.json.JSONObject;

/**
 * Operator UI for the HomeGuardian edge service.
 *
 * Shows what the service is actually doing (phase, port, probe results, restart
 * budget, native return codes) and offers the three controls the milestone asks
 * for: Start, Stop, Restart — plus a manual probe, a probe-log export for evidence
 * and token rotation.
 *
 * Everything is honest-by-construction: the screen renders values read back out of
 * the running native engine, so it cannot show "running" for a server that is not
 * listening. No bearer token is ever displayed.
 */
public class MainActivity extends Activity implements EdgeState.Listener {

    private static final String TAG = "HGEdge";
    private static final int LOG_TAIL_LINES = 25;

    private TextView statusView;
    private TextView logView;
    private Button startBtn;
    private Button stopBtn;
    private Button restartBtn;
    private EditText endpointField;
    private Button bridgeStartBtn;
    private Button bridgeStopBtn;
    private TextView bridgeView;
    private AlertDialog enrollmentDialog;
    private final Handler ui = new Handler();
    private final Runnable enrollmentTimeout = new Runnable() {
        public void run() { dismissEnrollment(); }
    };
    // Last rendered content. A 1 Hz ticker that calls setText() unconditionally emits an
    // accessibility content-changed event every second, which (a) wakes the compositor for
    // nothing on a battery-sensitive edge device and (b) makes the window never report
    // idle, so `uiautomator dump` fails with "could not get idle state" and the UI cannot
    // be driven or inspected. Rendering only on change fixes both.
    private String lastStatusText = null;
    private String lastLogText = null;
    private Boolean lastRunning = null;
    private String lastBridgeText = null;
    private Boolean lastBridgeRunning = null;

    private final Runnable ticker = new Runnable() {
        public void run() {
            render();
            ui.postDelayed(this, 1000L);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(buildUi());
        EdgeState.addListener(this);
        EdgeState.nativeLoaded = NativeEngine.isLoaded();
        EdgeState.nativeVersion = NativeEngine.version();
    }

    @Override
    protected void onResume() {
        super.onResume();
        ui.post(ticker);
    }

    @Override
    protected void onPause() {
        dismissEnrollment();
        super.onPause();
        ui.removeCallbacks(ticker);
    }

    @Override
    protected void onDestroy() {
        EdgeState.removeListener(this);
        super.onDestroy();
    }

    @Override
    public void onEdgeStateChanged() {
        ui.post(new Runnable() {
            public void run() { render(); }
        });
    }

    // ------------------------------------------------------------ UI setup --

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);

        TextView title = new TextView(this);
        title.setText("HomeGuardian Edge Service\nLocal MCP server on loopback only");
        title.setTextSize(16f);
        root.addView(title);

        statusView = new TextView(this);
        statusView.setTextSize(12f);
        statusView.setPadding(0, pad, 0, pad);
        root.addView(statusView);

        LinearLayout row1 = new LinearLayout(this);
        startBtn = button("Start", new View.OnClickListener() {
            public void onClick(View v) { send(EdgeService.ACTION_START); }
        });
        stopBtn = button("Stop", new View.OnClickListener() {
            public void onClick(View v) { send(EdgeService.ACTION_STOP); }
        });
        restartBtn = button("Restart", new View.OnClickListener() {
            public void onClick(View v) { send(EdgeService.ACTION_RESTART); }
        });
        row1.addView(startBtn);
        row1.addView(stopBtn);
        row1.addView(restartBtn);
        root.addView(row1);

        LinearLayout row2 = new LinearLayout(this);
        row2.addView(button("Probe now", new View.OnClickListener() {
            public void onClick(View v) { send(EdgeService.ACTION_PROBE_NOW); }
        }));
        row2.addView(button("Clear probe log", new View.OnClickListener() {
            public void onClick(View v) { onClearLog(); }
        }));
        root.addView(row2);

        LinearLayout row3 = new LinearLayout(this);
        row3.addView(button("Reset restart budget", new View.OnClickListener() {
            public void onClick(View v) { onResetBudget(); }
        }));
        row3.addView(button("Rotate token", new View.OnClickListener() {
            public void onClick(View v) { onRotate(); }
        }));
        root.addView(row3);

        // ---- HG-NET-02B outbound control bridge -------------------------------------------
        // Off by default and unusable until an endpoint is entered: an unconfigured install
        // dials nowhere, which is the property that makes shipping this safe.
        TextView bridgeTitle = new TextView(this);
        bridgeTitle.setText("\nOutbound control bridge (private coordinator)\n"
                + "Off until configured. Refuses public destinations and any non-loopback path "
                + "without a VPN tunnel up, so credentials never leave unencrypted.");
        bridgeTitle.setTextSize(13f);
        bridgeTitle.setPadding(0, pad, 0, 0);
        root.addView(bridgeTitle);

        endpointField = new EditText(this);
        endpointField.setHint("ws://<vpn-private-address>:<port>/");
        endpointField.setText(getSharedPreferences("hg_bridge", MODE_PRIVATE)
                .getString("endpoint", ""));
        root.addView(endpointField);

        LinearLayout row4 = new LinearLayout(this);
        bridgeStartBtn = button("Start bridge", new View.OnClickListener() {
            public void onClick(View v) { onBridgeStart(); }
        });
        bridgeStopBtn = button("Stop bridge", new View.OnClickListener() {
            public void onClick(View v) { onBridgeStop(); }
        });
        row4.addView(bridgeStartBtn);
        row4.addView(bridgeStopBtn);
        root.addView(row4);
        root.addView(button("Bridge enrollment", new View.OnClickListener() {
            public void onClick(View v) { onBridgeEnrollment(); }
        }));

        bridgeView = new TextView(this);
        bridgeView.setTextSize(11f);
        root.addView(bridgeView);

        logView = new TextView(this);
        logView.setTextSize(10f);
        logView.setPadding(0, pad, 0, 0);
        root.addView(logView);
        return scroll;
    }

    private Button button(String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setOnClickListener(l);
        return b;
    }

    private void send(String action) {
        Intent i = new Intent(this, EdgeService.class);
        i.setAction(action);
        if (EdgeService.ACTION_START.equals(action)) {
            startForegroundService(i);
        } else {
            startService(i);
        }
    }

    // ------------------------------------------------------------- actions --

    private void onClearLog() {
        McpProbe.clearLog(this);
        render();
    }

    private void onResetBudget() {
        getSharedPreferences("hg_edge_recovery", MODE_PRIVATE)
                .edit().putInt("restarts", 0).putLong("windowStart", 0L).apply();
        EdgeState.restartsUsed = 0;
        render();
    }

    private void onRotate() {
        try {
            Provisioner.rotateToken(this);
            // The running engine holds the OLD token in memory: a rotation only takes
            // effect after a restart, which is stated rather than assumed.
            EdgeState.lastError = "token rotated - restart the service to apply it";
        } catch (Throwable t) {
            EdgeState.lastError = "rotate failed: " + t;
        }
        render();
    }

    // ------------------------------------------------------- bridge (HG-NET-02B) --

    /**
     * Validates the endpoint BEFORE asking the service to dial, so an unacceptable destination is
     * rejected in the UI with its reason rather than half-starting. The service re-checks via
     * OutboundMcpBridge.start(), because a UI check alone would be advisory.
     */
    private void onBridgeStart() {
        String url = endpointField.getText().toString().trim();
        getSharedPreferences("hg_bridge", MODE_PRIVATE).edit().putString("endpoint", url).apply();
        RouteSecurityPolicy.wireAndroidPlatform(this);
        String refusal = RouteSecurityPolicy.refusalReason(url);
        if (refusal != null) {
            EdgeState.bridgeRunning = false;
            EdgeState.bridgeStatus = "bridge: " + refusal;
            EdgeState.notifyChanged();
            render();
            return;
        }
        EdgeState.bridgeEndpoint = url;
        EdgeState.bridgeStatus = "bridge: starting...";
        Intent i = new Intent(this, EdgeService.class);
        i.setAction(EdgeService.ACTION_BRIDGE_START);
        i.putExtra(EdgeService.EXTRA_BRIDGE_URL, url);
        startService(i);
        render();
    }

    private void onBridgeStop() {
        Intent i = new Intent(this, EdgeService.class);
        i.setAction(EdgeService.ACTION_BRIDGE_STOP);
        startService(i);
        render();
    }

    /** Explicit, temporary enrollment display. Never reads the local MCP bearer. */
    private void onBridgeEnrollment() {
        dismissEnrollment();
        try {
            JSONObject payload = new JSONObject();
            payload.put("device_id", OutboundMcpBridge.deviceId(this));
            payload.put("auth_token", OutboundMcpBridge.authToken(this));
            enrollmentDialog = new AlertDialog.Builder(this)
                    .setTitle("Bridge enrollment (30 seconds)")
                    .setMessage(payload.toString())
                    .setPositiveButton("Dismiss", null)
                    .setCancelable(true)
                    .create();
            enrollmentDialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
                public void onDismiss(DialogInterface dialog) {
                    ((AlertDialog) dialog).setMessage("");
                    if (enrollmentDialog == dialog) {
                        ui.removeCallbacks(enrollmentTimeout);
                        enrollmentDialog = null;
                    }
                }
            });
            enrollmentDialog.getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
            enrollmentDialog.show();
            ui.postDelayed(enrollmentTimeout, 30000L);
        } catch (Exception ignored) {
            dismissEnrollment();
            EdgeState.bridgeStatus = "bridge: enrollment unavailable";
            render();
        }
    }

    private void dismissEnrollment() {
        ui.removeCallbacks(enrollmentTimeout);
        if (enrollmentDialog != null) {
            enrollmentDialog.setMessage("");
            enrollmentDialog.dismiss();
            enrollmentDialog = null;
        }
    }

    // -------------------------------------------------------------- render --

    private void render() {
        boolean running = NativeEngine.isLoaded() && NativeEngine.isRunning();
        EdgeState.nativeRunning = running;
        if (running) EdgeState.port = NativeEngine.port();

        StringBuilder sb = new StringBuilder();
        sb.append("native library : ").append(NativeEngine.isLoaded() ? "loaded" : "NOT LOADED").append('\n');
        if (!NativeEngine.isLoaded()) {
            sb.append("load error     : ").append(NativeEngine.loadError()).append('\n');
        }
        sb.append("native version : ").append(NativeEngine.version()).append('\n');
        sb.append("phase          : ").append(EdgeState.phase).append('\n');
        sb.append("native running : ").append(running).append('\n');
        sb.append("bound port     : ").append(EdgeState.port)
          .append("  (loopback 127.0.0.1 only)\n");
        sb.append("creds file mode: ").append(Provisioner.credsMode(this)).append('\n');
        sb.append("probe          : ").append(EdgeState.lastProbeOk ? "OK" : "FAIL")
          .append("  ok=").append(EdgeState.probeOkCount)
          .append(" fail=").append(EdgeState.probeFailCount).append('\n');
        sb.append("probe latency  : ").append(EdgeState.lastProbeLatencyMs).append(" ms\n");
        sb.append("probe detail   : ").append(EdgeState.lastProbeDetail).append('\n');
        sb.append("restart budget : ").append(EdgeState.restartsUsed)
          .append("/").append(EdgeState.maxRestarts).append('\n');
        if (EdgeState.lastError.length() > 0) {
            sb.append("last error     : ").append(EdgeState.lastError).append('\n');
        }
        sb.append("native stats   : ").append(NativeEngine.statsJson()).append('\n');
        String status = sb.toString();
        if (!status.equals(lastStatusText)) {
            statusView.setText(status);
            lastStatusText = status;
        }

        if (lastRunning == null || lastRunning.booleanValue() != running) {
            startBtn.setEnabled(!running);
            stopBtn.setEnabled(running);
            restartBtn.setEnabled(true);
            lastRunning = Boolean.valueOf(running);
        }

        // ---- bridge render ----------------------------------------------------------------
        // vpnTunnelUp() is read live rather than cached, so the screen tells the operator the
        // truth about whether a tunnel exists right now - which is the precondition the transport
        // policy gates on, and the field most likely to change under them.
        boolean bridgeUp = EdgeState.bridgeRunning;
        StringBuilder bb = new StringBuilder();
        bb.append("bridge         : ").append(bridgeUp ? "RUNNING" : "stopped").append('\n');
        bb.append("endpoint       : ")
          .append(EdgeState.bridgeEndpoint.length() == 0 ? "(none configured)" : EdgeState.bridgeEndpoint)
          .append('\n');
        bb.append("vpn tunnel     : ").append(TransportPolicy.vpnTunnelUp() ? "UP" : "DOWN").append('\n');
        bb.append("bridge status  : ").append(EdgeState.bridgeStatus).append('\n');
        String btext = bb.toString();
        if (!btext.equals(lastBridgeText)) {
            bridgeView.setText(btext);
            lastBridgeText = btext;
        }
        if (lastBridgeRunning == null || lastBridgeRunning.booleanValue() != bridgeUp) {
            bridgeStartBtn.setEnabled(!bridgeUp);
            bridgeStopBtn.setEnabled(bridgeUp);
            lastBridgeRunning = Boolean.valueOf(bridgeUp);
        }

        String logs = "probe log (tail):\n" + tailProbeLog();
        if (!logs.equals(lastLogText)) {
            logView.setText(logs);
            lastLogText = logs;
        }
    }

    private String tailProbeLog() {
        File f = new File(getFilesDir(), "probe.log");
        if (!f.isFile()) return "(no probe log yet)";
        java.util.ArrayDeque<String> tail = new java.util.ArrayDeque<String>();
        BufferedReader r = null;
        try {
            r = new BufferedReader(new FileReader(f));
            String line;
            while ((line = r.readLine()) != null) {
                tail.addLast(line);
                while (tail.size() > LOG_TAIL_LINES) tail.removeFirst();
            }
        } catch (Throwable t) {
            return "(probe log unreadable: " + t + ")";
        } finally {
            if (r != null) try { r.close(); } catch (Throwable ignored) {}
        }
        StringBuilder sb = new StringBuilder();
        for (String l : tail) sb.append(l).append('\n');
        return sb.length() == 0 ? "(empty)" : sb.toString();
    }
}
