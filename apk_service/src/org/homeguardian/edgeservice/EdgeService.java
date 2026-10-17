package org.homeguardian.edgeservice;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.util.Log;

/**
 * Foreground service that owns the genuine C++ MCP server.
 *
 * Why a foreground service: the server must survive the activity being closed and
 * the screen going off. Android permits that only through a foreground service with
 * a visible, ongoing notification — which is also the honest UI: the operator can
 * always see that HomeGuardian is running and stop it from the notification.
 *
 * Recovery model (bounded, visible, never silent):
 *   * START_STICKY so Android recreates the service after the process is killed.
 *   * A watchdog probes MCP over loopback every PROBE_INTERVAL_MS.
 *   * If the native engine is not running, or consecutive probes fail, the engine is
 *     restarted — at most MAX_RESTARTS times inside RESTART_WINDOW_MS.
 *   * The budget refills only after the server has been verifiably healthy for
 *     HEALTHY_RESET_MS, so a fast crash-loop cannot reset itself.
 *   * When the budget is exhausted the service enters DEGRADED, says so in the
 *     notification, and stops instead of spinning forever.
 */
public class EdgeService extends Service {

    public static final String TAG = "HGEdge";

    public static final String ACTION_START = "org.homeguardian.edgeservice.START";
    public static final String ACTION_STOP = "org.homeguardian.edgeservice.STOP";
    public static final String ACTION_RESTART = "org.homeguardian.edgeservice.RESTART";
    public static final String ACTION_PROBE_NOW = "org.homeguardian.edgeservice.PROBE_NOW";
    /**
     * HG-NET-02B: the outbound control bridge. Deliberately NOT started by ACTION_START, so an
     * unconfigured production install never dials anywhere. Both an explicit action and a
     * coordinator endpoint are required.
     */
    public static final String ACTION_BRIDGE_START = "org.homeguardian.edgeservice.BRIDGE_START";
    public static final String ACTION_BRIDGE_STOP = "org.homeguardian.edgeservice.BRIDGE_STOP";
    public static final String EXTRA_BRIDGE_HOST = "bridge_host";
    public static final String EXTRA_BRIDGE_PORT = "bridge_port";

    private static final String CHANNEL_ID = "hg_edge_service";
    private static final int NOTIFICATION_ID = 4711;

    private static final int MAX_RESTARTS = 5;
    private static final long RESTART_WINDOW_MS = 10L * 60L * 1000L;
    private static final long HEALTHY_RESET_MS = 3L * 60L * 1000L;
    private static final long PROBE_INTERVAL_MS = 20L * 1000L;
    private static final int FAILS_BEFORE_RESTART = 3;

    private static final String PREFS = "hg_edge_recovery";

    private HandlerThread workerThread;
    private Handler worker;
    private volatile boolean stopping = false;
    private int consecutiveProbeFails = 0;
    private long healthySinceMs = 0;

    @Override
    public void onCreate() {
        super.onCreate();
        createChannel();
        workerThread = new HandlerThread("hg-edge-watchdog");
        workerThread.start();
        worker = new Handler(workerThread.getLooper());
        EdgeState.nativeLoaded = NativeEngine.isLoaded();
        EdgeState.nativeVersion = NativeEngine.version();
        if (!EdgeState.nativeLoaded) {
            EdgeState.lastError = "native library not loaded: " + NativeEngine.loadError();
            Log.e(TAG, EdgeState.lastError);
        }

        // Become foreground IMMEDIATELY, before onStartCommand runs.
        //
        // Why here and not only in onStartCommand: after the process is killed, Android
        // restarts the service, but its "app idle" sweep can destroy a service that is
        // started-but-not-yet-foreground. Observed on the Mi A3 - AMS logged
        // "Stopping service due to app idle" 42 ms after starting the replacement
        // process, so onStartCommand never ran, the engine never came back and
        // START_STICKY alone recovered nothing. A foreground service is skipped by that
        // sweep, so the notification must exist from the first moment of the lifecycle.
        startForeground(NOTIFICATION_ID, buildNotification("starting…"));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String action = (intent == null) ? ACTION_START : intent.getAction();
        Log.i(TAG, "onStartCommand action=" + action + " flags=" + flags + " startId=" + startId);

        if (ACTION_STOP.equals(action)) {
            stopEverything("operator stop");
            return START_NOT_STICKY;
        }
        if (ACTION_RESTART.equals(action)) {
            restartEngine("operator restart");
            return START_STICKY;
        }
        if (ACTION_PROBE_NOW.equals(action)) {
            scheduleProbe(0);
            return START_STICKY;
        }
        if (ACTION_BRIDGE_START.equals(action)) {
            // Coordinator endpoint comes from the caller (operator), never from a compiled-in LAN
            // address: a dev LAN address must never be a device identity or a fixed destination.
            String host = (intent == null) ? null : intent.getStringExtra(EXTRA_BRIDGE_HOST);
            int port = (intent == null) ? 0 : intent.getIntExtra(EXTRA_BRIDGE_PORT, 0);
            startBridge(host, port);
            return START_STICKY;
        }
        if (ACTION_BRIDGE_STOP.equals(action)) {
            stopBridge();
            return START_STICKY;
        }

        // START (or a system restart with a null intent). Already foreground from
        // onCreate; re-asserting is idempotent and keeps the text current.
        startForeground(NOTIFICATION_ID, buildNotification("starting…"));
        startEngineAsync();
        scheduleWatchdog();
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onDestroy() {
        stopBridge();
        Log.i(TAG, "onDestroy");
        scheduleStop("service destroyed");
        super.onDestroy();
    }

    // ------------------------------------------------------------- engine ---

    private void startEngineAsync() {
        EdgeState.phase = EdgeState.Phase.STARTING;
        EdgeState.notifyChanged();
        worker.post(new Runnable() {
            public void run() { startEngineBlocking(); }
        });
    }

    private void startEngineBlocking() {
        if (NativeEngine.isRunning()) {
            EdgeState.phase = EdgeState.Phase.RUNNING;
            EdgeState.nativeRunning = true;
            EdgeState.port = NativeEngine.port();
            EdgeState.notifyChanged();
            updateNotification("running on 127.0.0.1:" + EdgeState.port);
            return;
        }
        try {
            Provisioner.provisionIfNeeded(EdgeService.this);
        } catch (Throwable t) {
            EdgeState.phase = EdgeState.Phase.FAILED;
            EdgeState.lastError = "provisioning failed: " + t;
            Log.e(TAG, EdgeState.lastError);
            EdgeState.notifyChanged();
            updateNotification("FAILED: provisioning");
            McpProbe.appendLog(EdgeService.this, "ENGINE_FAIL provisioning " + t);
            return;
        }

        final int code = NativeEngine.start(
                getFilesDir().getAbsolutePath(),
                Provisioner.configFile(EdgeService.this).getAbsolutePath(),
                Provisioner.credsFile(EdgeService.this).getAbsolutePath(),
                Provisioner.LOOPBACK,
                Provisioner.DEFAULT_PORT);

        if (code == NativeEngine.OK) {
            EdgeState.phase = EdgeState.Phase.RUNNING;
            EdgeState.nativeRunning = true;
            EdgeState.port = NativeEngine.port();
            EdgeState.lastError = "";
            EdgeState.statsJson = NativeEngine.statsJson();
            healthySinceMs = System.currentTimeMillis();
            Log.i(TAG, "engine started, port=" + EdgeState.port);
            McpProbe.appendLog(EdgeService.this,
                    "ENGINE_UP port=" + EdgeState.port
                            + " creds_mode=" + Provisioner.credsMode(EdgeService.this)
                            + " native=" + NativeEngine.version());
            updateNotification("running on 127.0.0.1:" + EdgeState.port);
        } else {
            EdgeState.nativeRunning = false;
            EdgeState.lastError = NativeEngine.describe(code) + " / " + NativeEngine.lastError();
            EdgeState.phase = (code == NativeEngine.CREDENTIAL_STORE_FAILED) ? EdgeState.Phase.FAILED
                                                                            : EdgeState.Phase.DEGRADED;
            Log.e(TAG, "engine start failed: " + EdgeState.lastError);
            McpProbe.appendLog(EdgeService.this, "ENGINE_FAIL code=" + code + " " + EdgeState.lastError);
            updateNotification("FAILED: " + NativeEngine.describe(code));
        }
        EdgeState.notifyChanged();
    }

    private void restartEngine(String reason) {
        Log.i(TAG, "restartEngine: " + reason);
        EdgeState.notifyChanged();
        worker.post(new Runnable() {
            public void run() {
                NativeEngine.stop();
                EdgeState.nativeRunning = false;
                startEngineBlocking();
            }
        });
    }

    // ------------------------------------------------------------ watchdog ---

    private void scheduleWatchdog() {
        worker.postDelayed(new Runnable() {
            public void run() {
                if (stopping) return;
                watchdogTick();
                if (!stopping) worker.postDelayed(this, PROBE_INTERVAL_MS);
            }
        }, PROBE_INTERVAL_MS);
    }

    private void scheduleProbe(long delayMs) {
        worker.postDelayed(new Runnable() {
            public void run() { watchdogTick(); }
        }, delayMs);
    }

    private void watchdogTick() {
        if (stopping) return;
        try {
            // (a) is the native engine still alive?
            if (!NativeEngine.isRunning()) {
                EdgeState.nativeRunning = false;
                EdgeState.phase = EdgeState.Phase.DEGRADED;
                McpProbe.appendLog(this, "ENGINE_DOWN detected by watchdog");
                EdgeState.notifyChanged();
                attemptBoundedRestart("native engine not running");
                return;
            }
            EdgeState.nativeRunning = true;
            EdgeState.port = NativeEngine.port();

            // (b) real MCP probe over loopback
            McpProbe.Result r = McpProbe.run(this, EdgeState.port);
            EdgeState.lastProbeOk = r.ok;
            EdgeState.lastProbeDetail = r.detail;
            EdgeState.lastProbeLatencyMs = r.latencyMs;
            EdgeState.lastProbeAtMs = System.currentTimeMillis();
            EdgeState.statsJson = NativeEngine.statsJson();
            if (r.ok) {
                EdgeState.probeOkCount++;
                consecutiveProbeFails = 0;
                EdgeState.phase = EdgeState.Phase.RUNNING;
                McpProbe.appendLog(this, "PROBE_OK " + r.latencyMs + "ms " + r.detail);
                // Refill the restart budget only after sustained health.
                if (healthySinceMs == 0) healthySinceMs = System.currentTimeMillis();
                if (System.currentTimeMillis() - healthySinceMs > HEALTHY_RESET_MS) {
                    resetRestartBudget();
                }
                updateNotification("running on 127.0.0.1:" + EdgeState.port
                        + " · probe " + r.latencyMs + "ms");
            } else {
                EdgeState.probeFailCount++;
                consecutiveProbeFails++;
                healthySinceMs = 0;
                McpProbe.appendLog(this, "PROBE_FAIL " + r.httpCode + " " + r.detail);
                if (consecutiveProbeFails >= FAILS_BEFORE_RESTART) {
                    attemptBoundedRestart(consecutiveProbeFails + " consecutive probe failures");
                }
            }
            EdgeState.notifyChanged();
        } catch (Throwable t) {
            Log.e(TAG, "watchdog tick threw", t);
            McpProbe.appendLog(this, "WATCHDOG_ERROR " + t);
        }
    }

    // ------------------------------------------------------------ recovery ---

    private void attemptBoundedRestart(String reason) {
        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        long now = System.currentTimeMillis();
        long windowStart = p.getLong("windowStart", 0L);
        int used = p.getInt("restarts", 0);

        if (windowStart == 0L || now - windowStart > RESTART_WINDOW_MS) {
            windowStart = now;
            used = 0;
        }

        if (used >= MAX_RESTARTS) {
            EdgeState.phase = EdgeState.Phase.DEGRADED;
            EdgeState.lastError = "restart budget exhausted (" + MAX_RESTARTS + " in "
                    + (RESTART_WINDOW_MS / 60000L) + " min); giving up: " + reason;
            EdgeState.restartsUsed = used;
            EdgeState.maxRestarts = MAX_RESTARTS;
            Log.e(TAG, EdgeState.lastError);
            McpProbe.appendLog(this, "RECOVERY_EXHAUSTED used=" + used + " reason=" + reason);
            EdgeState.notifyChanged();
            updateNotification("DEGRADED: restart budget exhausted");
            // Fail visibly; do not spin. Operator must intervene.
            stopEverything("restart budget exhausted");
            return;
        }

        used++;
        p.edit().putInt("restarts", used).putLong("windowStart", windowStart).apply();
        EdgeState.restartsUsed = used;
        EdgeState.maxRestarts = MAX_RESTARTS;

        long backoffMs = Math.min(60_000L, 5_000L * (1L << Math.min(used - 1, 4)));
        McpProbe.appendLog(this, "RECOVERY_RESTART attempt=" + used + "/" + MAX_RESTARTS
                + " backoff_ms=" + backoffMs + " reason=" + reason);
        Log.i(TAG, "bounded restart " + used + "/" + MAX_RESTARTS + " in " + backoffMs + "ms: " + reason);
        EdgeState.notifyChanged();
        updateNotification("recovering (attempt " + used + "/" + MAX_RESTARTS + ")");

        worker.postDelayed(new Runnable() {
            public void run() {
                if (stopping) return;
                NativeEngine.stop();
                EdgeState.nativeRunning = false;
                startEngineBlocking();
                consecutiveProbeFails = 0;
            }
        }, backoffMs);
    }

    private void resetRestartBudget() {
        SharedPreferences p = getSharedPreferences(PREFS, MODE_PRIVATE);
        int used = p.getInt("restarts", 0);
        if (used == 0) return;
        p.edit().putInt("restarts", 0).putLong("windowStart", 0L).apply();
        EdgeState.restartsUsed = 0;
        McpProbe.appendLog(this, "RECOVERY_BUDGET_RESET after sustained health");
        Log.i(TAG, "restart budget reset after sustained health");
    }

    // ------------------------------------------------------------- stop ------

    // ---------------------------------------------------------------- outbound control bridge

    /** The single bridge instance. Null until an operator explicitly starts it. */
    private volatile ControlBridge bridge;

    /**
     * Start the outbound authenticated control bridge. Refuses without a coordinator endpoint, so
     * the production default (nothing configured) dials nowhere.
     */
    private void startBridge(String host, int port) {
        if (host == null || host.length() == 0 || port <= 0) {
            Log.w(TAG, "bridge start refused: no coordinator endpoint supplied");
            updateNotification("bridge: refused - no coordinator configured");
            return;
        }
        if (bridge != null && bridge.isRunning()) {
            Log.i(TAG, "bridge already running; not creating a second session");
            return;
        }
        bridge = new ControlBridge(this);
        bridge.configure(host, port);
        String r = bridge.start();
        Log.i(TAG, "bridge: " + r);
        updateNotification("bridge: " + r);
    }

    /** Stop the bridge and close its session. Safe when it was never started. */
    private void stopBridge() {
        ControlBridge b = bridge;
        bridge = null;
        if (b != null) {
            b.stop();
            Log.i(TAG, "bridge stopped");
        }
    }

    private void stopEverything(String reason) {
        stopBridge();          // never leave an outbound session open past a service stop
        scheduleStop(reason);
    }

    private void scheduleStop(final String reason) {
        stopping = true;
        Log.i(TAG, "stopping: " + reason);
        if (worker != null) {
            worker.post(new Runnable() {
                public void run() {
                    int code = NativeEngine.stop();
                    EdgeState.nativeRunning = false;
                    EdgeState.phase = EdgeState.Phase.STOPPED;
                    EdgeState.notifyChanged();
                    McpProbe.appendLog(EdgeService.this, "ENGINE_STOP code=" + code + " reason=" + reason);
                }
            });
        }
        stopForeground(true);
        stopSelf();
    }

    // ------------------------------------------------------- notification ---

    private void createChannel() {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(CHANNEL_ID, "HomeGuardian Edge Service",
                NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Keeps the local HomeGuardian MCP server running");
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification(String text) {
        Intent open = new Intent(this, MainActivity.class);
        int piFlags = PendingIntent.FLAG_UPDATE_CURRENT;
        PendingIntent content = PendingIntent.getActivity(this, 1, open, piFlags);
        PendingIntent stop = PendingIntent.getService(this, 2,
                new Intent(this, EdgeService.class).setAction(ACTION_STOP), piFlags);
        PendingIntent restart = PendingIntent.getService(this, 3,
                new Intent(this, EdgeService.class).setAction(ACTION_RESTART), piFlags);

        Notification.Builder b = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("HomeGuardian MCP")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_upload_done)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(content)
                .addAction(new Notification.Action.Builder(null, "Stop", stop).build())
                .addAction(new Notification.Action.Builder(null, "Restart", restart).build());
        return b.build();
    }

    private void updateNotification(String text) {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(NOTIFICATION_ID, buildNotification(text));
    }
}
