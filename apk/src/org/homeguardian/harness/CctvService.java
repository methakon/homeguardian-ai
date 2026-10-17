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
            stopSelf();
            return START_NOT_STICKY;
        }

        // A null intent means the system restarted us after a kill (START_STICKY). That is
        // recorded honestly rather than silently treated as a normal operator start.
        if (action == null) {
            Log.i(TAG, "restarted by the system (null intent)");
            lastStopReason = "system_killed";
        }

        if (!running) {
            running = true;
            isRunning = true;
            startedUptimeMs = SystemClock.uptimeMillis();
            startForeground(NOTIF_ID, buildNotification());
            ticker.removeCallbacks(tick);
            ticker.postDelayed(tick, TICK_MS);
            Log.i(TAG, "foreground started; media NOT armed");
        } else {
            updateNotification();
        }

        // START_STICKY: the system recreates the service after a low-memory kill, which is the
        // recoverable-after-disconnection behaviour this node needs. It does NOT survive a
        // reboot; the operator starts it manually, per the manual Start/Stop requirement.
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        isRunning = false;
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

        String text = "standing by - media not armed | up " + uptimeS + "s"
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
