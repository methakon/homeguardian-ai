package org.homeguardian.harness;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;

/**
 * Foreground service that will own the CCTV node's capture lifetime.
 *
 * WHY THIS CLASS EXISTS
 * The harness is deliberately activity-scoped: its manifest states that leaving the activity
 * stops and releases every device, and that no foreground service is declared because no
 * background capture is implemented. That is the right design for an acceptance harness and
 * exactly wrong for a camera node, because Android 9 blocks camera/microphone access for
 * background apps and names the foreground service as the documented fix. Without this, the
 * camera stops the moment the screen goes off, which is the entire product.
 *
 * WHAT THIS STEP DOES AND DOES NOT DO
 * This step stands up the service, its notification and its lifecycle ONLY. It deliberately
 * does NOT open the camera or the microphone. Capture wiring is a separate change that needs
 * the owner's media-test approval, so that the highest-risk mechanic - surviving screen-off -
 * can be measured without activating any sensor.
 *
 * DEEP SLEEP: A FOREGROUND SERVICE IS NOT A WAKE LOCK
 * Measured on the sibling Mi A3 node: a foreground service keeps an app out of the
 * background *restriction*, but it does not stop the SoC suspending. Handler.postDelayed is
 * uptime-based, so it stops advancing while suspended. That is why this service reports BOTH
 * uptime and elapsed realtime: if they diverge, the device slept and the loop was frozen. For
 * continuous capture the node will additionally need a partial wake lock, which is a battery
 * decision for the owner and is NOT taken here.
 *
 * Manual Start/Stop only: this service is started by an explicit operator action and stopped
 * by an explicit operator action (or the notification's Stop button). It does not start
 * itself at boot.
 */
public class CctvService extends Service {

    public static final String TAG = "HGCctv";

    /** Explicit operator actions. There is deliberately no implicit start path. */
    public static final String ACTION_START = "org.homeguardian.harness.CCTV_START";
    public static final String ACTION_STOP = "org.homeguardian.harness.CCTV_STOP";
    /**
     * HG-CAM-08C: arm/disarm capture while leaving the service running. The activity's camera
     * buttons use these. ACTION_START/ACTION_STOP additionally own the service lifecycle, and
     * ACTION_START arms capture too ("promote to foreground, THEN start capture").
     */
    public static final String ACTION_ARM = "org.homeguardian.harness.CCTV_ARM";
    public static final String ACTION_DISARM = "org.homeguardian.harness.CCTV_DISARM";
    /**
     * Stage 4: the microphone is SEPARATELY controllable from video, as HG-CAM-08 requires.
     * Arming audio never implies arming video, and vice versa.
     */
    public static final String ACTION_MIC_ARM = "org.homeguardian.harness.CCTV_MIC_ARM";
    public static final String ACTION_MIC_DISARM = "org.homeguardian.harness.CCTV_MIC_DISARM";

    /**
     * THE capture session. Owned by the service, never by the activity, so that activity
     * onPause()/onStop() cannot terminate screen-off capture. Exactly one instance exists.
     * Unbounded (runMs <= 0): it runs until an explicit disarm/stop.
     */
    private Camera2EncoderProbe capture;

    private static volatile String captureSummary = "capture: not armed";

    /** Stage 4: the microphone, separately owned and separately armed from video. */
    private AudioCaptureProbe mic;
    private static volatile String micSummary = "mic: not armed";
    public static volatile boolean micArmed = false;

    /** Read by the activity panel and by scripted tests. Never gates anything. */
    public static String captureStatus() { return captureSummary; }
    public static String micStatus() { return micSummary; }

    /** True when the service currently owns a live capture session. */
    public static volatile boolean captureArmed = false;

    private static final String CHANNEL_ID = "hg_cctv_node";
    private static final int NOTIF_ID = 4201;
    private static final long TICK_MS = 5000L;

    private Handler ticker;
    private long startedUptimeMs = 0L;
    private int ticks = 0;
    private volatile boolean running = false;

    /**
     * Reflects the last stop reason for the status panel. "never_started", "operator_stop",
     * "system_killed" (START_STICKY restarted us with a null intent), "destroyed".
     */
    private static volatile String lastStopReason = "never_started";

    public static String lastStopReason() { return lastStopReason; }

    /** True once the service is in the foreground. Read by the panel; never gates capture. */
    public static volatile boolean isRunning = false;

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            ticks++;
            refreshCaptureSummary();
            refreshMicSummary();
            // HG-CAM-08C: the SERVICE logs its own media counters. The activity's heartbeat
            // stops the moment the screen goes off (onPause), so a service-side trace on an
            // uptime-based timer is the only way to see counters advance while screen-off.
            Log.i(TAG, "tick #" + ticks + " " + captureSummary);
            Log.i(TAG, "tick #" + ticks + " " + micSummary);
            updateNotification();
            // NOTE: uptime-based, so this stops firing while the device is suspended. The
            // notification text makes that visible rather than hiding it.
            ticker.postDelayed(this, TICK_MS);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        ticker = new Handler();
        createChannel();
        Log.i(TAG, "service created");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = (intent == null) ? null : intent.getAction();

        if (ACTION_STOP.equals(action)) {
            Log.i(TAG, "stop requested by operator");
            lastStopReason = "operator_stop";
            disarmCapture("operator stop");
            disarmMic("operator stop");
            stopSelf();
            return START_NOT_STICKY;
        }

        // A null intent means the system restarted us after a kill (START_STICKY). That is
        // recorded honestly rather than silently treated as a normal operator start.
        if (action == null) {
            Log.i(TAG, "restarted by the system (null intent)");
            lastStopReason = "system_killed";
        }

        // GO FOREGROUND FIRST, INDEPENDENTLY OF WHAT WAS ASKED FOR.
        // Android 9 refuses BOTH the camera and the microphone to background callers, so the
        // foreground promotion must precede any sensor access - and it must happen for the
        // microphone actions too, not only the camera ones. Previously a mic action returned
        // before reaching this block, so a cold mic arm would have been refused by the OS.
        if (!running) {
            running = true;
            isRunning = true;
            startedUptimeMs = SystemClock.uptimeMillis();
            startForeground(NOTIF_ID, buildNotification());
            ticker.removeCallbacks(tick);
            ticker.postDelayed(tick, TICK_MS);
            Log.i(TAG, "foreground started (no sensor touched yet)");
        }

        // NOW dispatch. Each sensor is armed ONLY by its own explicit action, so video and audio
        // stay SEPARATELY controllable: arming the microphone must never open the camera.
        // ACTION_START retains its meaning for the camera path ("promote to foreground, then
        // start capture"); ACTION_ARM arms video only; ACTION_MIC_ARM arms audio only.
        if (ACTION_ARM.equals(action) || ACTION_START.equals(action)) {
            armCapture();
        } else if (ACTION_DISARM.equals(action)) {
            disarmCapture("operator disarm");
        } else if (ACTION_MIC_ARM.equals(action)) {
            armMic();
        } else if (ACTION_MIC_DISARM.equals(action)) {
            disarmMic("operator disarm");
        }

        updateNotification();

        // START_STICKY: the system recreates the service after a low-memory kill, which is the
        // recoverable-after-disconnection behaviour this node needs. It does NOT survive a
        // reboot; the operator starts it manually, per the manual Start/Stop requirement.
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        isRunning = false;
        // Release the camera before the service goes away, whatever the reason. A leaked
        // capture session would survive as a phantom camera client on device 0.
        disarmCapture("service destroyed");
        disarmMic("service destroyed");
        if (ticker != null) ticker.removeCallbacks(tick);
        if (lastStopReason.equals("never_started") || lastStopReason.equals("system_killed")) {
            lastStopReason = "destroyed";
        }
        stopForeground(true);
        Log.i(TAG, "service destroyed; reason=" + lastStopReason);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null; // started service only; no binding surface
    }

    // ------------------------------------------------------------------ capture ownership

    /**
     * Arm the single capture session. Idempotent by design: "enforce one active capture session"
     * is a requirement, so a second arm must never create a second camera client.
     */
    private synchronized void armCapture() {
        if (capture != null && capture.isRunning()) {
            Log.i(TAG, "arm ignored - a capture session is already active");
            refreshCaptureSummary();
            return;
        }
        if (capture == null) capture = new Camera2EncoderProbe(this, 0); // 0 = UNBOUNDED
        captureArmed = true;
        Log.i(TAG, "arming capture: " + capture.start());
        refreshCaptureSummary();
    }

    /** Stop and release the capture session. Safe to call when nothing is armed. */
    private synchronized void disarmCapture(String why) {
        if (capture == null) { captureArmed = false; return; }
        Log.i(TAG, "disarming capture (" + why + ")");
        capture.stop();                       // releases camera, session, codec and surface
        captureArmed = false;
        refreshCaptureSummary();
    }

    /** Publish the capture counters. These are the only honest measure of screen-off success. */
    private void refreshCaptureSummary() {
        if (capture == null) { captureSummary = "capture: not armed"; return; }
        captureSummary = capture.result().summary();
    }

    // ------------------------------------------------------------------ microphone ownership

    private synchronized void armMic() {
        if (mic != null && mic.isRunning()) {
            Log.i(TAG, "mic arm ignored - already running");
            refreshMicSummary();
            return;
        }
        if (mic == null) mic = new AudioCaptureProbe(this);
        micArmed = true;
        Log.i(TAG, "arming mic: " + mic.start());
        refreshMicSummary();
    }

    private synchronized void disarmMic(String why) {
        if (mic == null) { micArmed = false; return; }
        Log.i(TAG, "disarming mic (" + why + ")");
        mic.stop();
        micArmed = false;
        refreshMicSummary();
    }

    private void refreshMicSummary() {
        if (mic == null) { micSummary = "mic: not armed"; return; }
        micSummary = mic.result().summary();
    }

    // ------------------------------------------------------------------ notification

    private void createChannel() {
        // minSdk is 27, so a channel is mandatory; without it the notification is dropped
        // silently and startForeground can fail.
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID, "HomeGuardian CCTV", NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("Dedicated CCTV node service");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    private Notification buildNotification() {
        PendingIntent open = PendingIntent.getActivity(
                this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stop = PendingIntent.getService(
                this, 1, new Intent(this, CctvService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_UPDATE_CURRENT);

        long uptimeS = (SystemClock.uptimeMillis() - startedUptimeMs) / 1000L;
        long elapsedS = (SystemClock.elapsedRealtime() - (SystemClock.uptimeMillis() - startedUptimeMs)) / 1000L;
        long sleptS = elapsedS - uptimeS;

        // The notification carries the LIVE media counters, so screen-off progress is visible
        // from the device itself and not only from a logcat pull after the fact.
        String media;
        if (captureArmed && capture != null) {
            java.util.Locale L = java.util.Locale.US;
            media = "capturing " + capture.result().width + "x" + capture.result().height
                    + " | " + capture.result().encodedBuffers + " bufs"
                    + " @ " + String.format(L, "%.1f", capture.result().measuredFps()) + "fps";
            if (capture.result().lastError.length() > 0) {
                media = "capture ERROR: " + capture.result().lastError;
            }
        } else {
            media = "standing by - media not armed";
        }

        String text = media + " | " + (micArmed && mic != null
                    ? (mic.result().totalSamples + " samples" + (mic.result().silent ? " SILENT" : ""))
                    : "mic off")
                + " | up " + uptimeS + "s"
                + (sleptS > 5 ? " | suspended " + sleptS + "s" : "");

        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        return b.setContentTitle("HomeGuardian CCTV node")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.presence_video_online)
                .setOngoing(true)
                .setContentIntent(open)
                .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", stop)
                .build();
    }

    private void updateNotification() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(NOTIF_ID, buildNotification());
    }
}
