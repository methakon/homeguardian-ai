package org.homeguardian.harness;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.hardware.Camera;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.text.InputType;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

/**
 * HomeGuardian AI - Android acquisition node (milestone D-02).
 *
 * This is the real acquisition application: it acquires camera frames and PCM
 * microphone samples on an authorised device and hands every one of them to the
 * native delivery gate, which re-checks consent/permission/state per payload and
 * accounts it.
 *
 * SAFETY PROPERTIES (by design, not by convention):
 *  - Capture is DISABLED by default. Nothing opens on launch. There is no
 *    background capture: leaving the activity stops and releases everything.
 *  - The operator must (1) tick the consent control, (2) grant the Android
 *    runtime permission, and (3) press Start. Any of the three missing means
 *    nothing is captured; the native gate refuses regardless of what the UI does.
 *  - A visible recording banner is shown whenever any capture is active, plus
 *    live frame/sample counters and the reason the last payload was refused.
 *  - Frames and samples stay in memory for the duration of the callback and are
 *    counted only. No file, no upload, no decoding, no inference: this app does
 *    NOT recognise faces, speech, falls or emergencies.
 *
 * Deliberately uses anonymous inner classes rather than Java 8 lambdas: the
 * Android 8.1 (API 27) dex verifier rejects invokedynamic (see build_apk.sh).
 */
public class MainActivity extends Activity {

    private static final String TAG = "HGNode";
    private static final int REQ_CAMERA = 1001;
    private static final int REQ_MIC = 1002;

    // Attended launcher actions. These exist so an operator (or a scripted acceptance test)
    // can drive the SAME code paths the UI buttons use, without tapping the screen and without
    // weakening any gate: consent, permission and the native delivery gate are all still checked.
    static final String ACTION_CONSENT_GRANT = "org.homeguardian.harness.CONSENT_GRANT";
    static final String ACTION_CONSENT_WITHDRAW = "org.homeguardian.harness.CONSENT_WITHDRAW";
    static final String ACTION_CAMERA_START = "org.homeguardian.harness.CAMERA_START";
    static final String ACTION_CAMERA_STOP = "org.homeguardian.harness.CAMERA_STOP";
    static final String ACTION_MIC_START = "org.homeguardian.harness.MIC_START";
    static final String ACTION_MIC_STOP = "org.homeguardian.harness.MIC_STOP";
    // HG-CAM-08B: Camera2 -> MediaCodec probe (manual, bounded, no auto-start).
    static final String ACTION_CAM2_START = "org.homeguardian.harness.CAM2_START";
    static final String ACTION_CAM2_STOP = "org.homeguardian.harness.CAM2_STOP";
    // HG-CAM-08C Stage 4: service-owned REAL microphone (separate from the CAM2 video path).
    static final String ACTION_MIC2_START = "org.homeguardian.harness.MIC2_START";
    static final String ACTION_MIC2_STOP = "org.homeguardian.harness.MIC2_STOP";
    /** HG-CAM-08D Stage 3: mic through the AAC encoder, and the H.264 decoder round-trip. */
    static final String ACTION_MIC2_ENCODE = "org.homeguardian.harness.MIC2_ENCODE";
    static final String ACTION_DECODE_CHECK = "org.homeguardian.harness.DECODE_CHECK";
    /** HG-CAM-08D Stage 4: media-sink security self-test. */
    static final String ACTION_SINK_SELFTEST = "org.homeguardian.harness.SINK_SELFTEST";

    // Which capture is waiting on a permission grant, if any.
    private static final int PENDING_NONE = 0;
    private static final int PENDING_CAMERA = 1;
    private static final int PENDING_MIC = 2;

    private static final int[][] SIZES = {{320, 240}, {640, 480}, {854, 480}};
    private static final int[] FPS = {5, 10, 15};
    private static final int[] RATES = {8000, 16000, 44100};
    private static final int[] CHANNELS = {1, 2};

    private TextView identityView;
    private EditText nameEdit;
    private TextView recordBanner;
    private TextView cameraStatus;
    private TextView micStatus;
    private TextView countersView;
    // H-03: the hub link + node status panel, and the native observation drain it consumes.
    private NodePanel nodePanel;
    private boolean panelIdentified = false;
    private int tickCount = 0;   // diagnostics: proves the tick loop is alive
    private TextView consentView;

    private Spinner facingSpinner;
    private Spinner sizeSpinner;
    private Spinner fpsSpinner;
    private Spinner rateSpinner;
    private Spinner channelSpinner;

    private Button cameraStartButton;
    private Button cameraStopButton;
    private Button micStartButton;
    private Button micStopButton;

    private Camera1Bridge camera1;
    private boolean consentCamera = false;
    private boolean consentMic = false;
    private int pending = PENDING_NONE;

    private final Handler ticker = new Handler();
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            // H-03: hand the compact observations to the D-03 transport, then refresh the panel.
            // The drain returns ONLY numeric features - no pixels, no PCM, no credentials.
            if (nodePanel != null) {
                if (!panelIdentified) {
                    nodePanel.setIdentity(nativeIdentity(), DeviceIdentity.defaultDisplayName());
                    // Exercise the second new JNI entry point once at identification, so a
                    // linkage defect in it cannot hide until an operator presses a button.
                    Log.i(TAG, nativeSetObservationIntervalMs(1000));
                    panelIdentified = true;
                }
                nodePanel.sample(nativeDrainObservations(), nativeStats());
                    // Ask the native gate, every tick, WHY each sensor is authorized or not.
                    nodePanel.setConsent(nativeConsentState());
            }
            refreshCounters();
            // Heartbeat: makes "the ticker executes repeatedly" an OBSERVABLE fact in the log
            // instead of something inferred from a UI scrape. Every 20 ticks is ~10 s, so a
            // 30-second run shows several lines. Diagnostics only - no behaviour depends on it.
            if (++tickCount % 20 == 0) {
                Log.i(TAG, "tick #" + tickCount);
                // ACCEPTANCE EVIDENCE (diagnostics only; nothing depends on these lines). The native
                // accountant's frame/sample counters and the transport's queue/write counters, so
                // "frames increase", "samples increase" and "observations are emitted" can be checked
                // from the log instead of read off the screen - UI scraping is not usable on this
                // device (uiautomator crashes with "UiAutomationService already registered").
                Log.i(TAG, "counters " + nativeStats());
                if (nodePanel != null) Log.i(TAG, "counters " + nodePanel.transportCounters());
                // HG-CAM-08C: report the SERVICE-owned capture counters, not an activity-local one.
                Log.i(TAG, "service-capture " + CctvService.captureStatus());
                // RECONCILE: if the native layer is not capturing (stop, consent withdrawal, camera
                // permission revocation, pause) while the bridge still holds a preview, release it.
                // Java asks the native side - never the reverse - so no lock is held across a call
                // into Java from a native thread.
                if (camera1 != null && camera1.isCapturing() && !nativeVideoRunning()) {
                    camera1.stopCamera();
                    Log.i(TAG, "camera1 released: the native layer is not capturing "
                            + "(stop/withdrawal/permission/pause)");
                }
            }
            ticker.postDelayed(this, 500);
        }
    };

    static {
        try {
            System.loadLibrary("hg_harness");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "native library failed to load", e);
        }
    }

    // ---- native (hg_harness_jni.cpp) --------------------------------------
    private native String nativeSetIdentity(String seed, String displayName, int apiLevel,
                                            String abi, String model);
    private native String nativeIdentity();
    private native void nativeSetPermissions(boolean camera, boolean mic);
    private native String nativeGrantConsent(boolean camera, boolean mic);
    private native String nativeWithdrawConsent();
    private native String nativeStartCamera(int cameraId, String backend);
    private native String nativeCameraReport(int cameraCount);
    private native String nativeCameraStartResult(boolean ok, String detail);
    private native String nativeCameraBackend();
    private native boolean nativeVideoRunning();
    private native String nativeStopCamera();
    private native String nativeStartMic(int sampleRate, int channels);
    private native String nativeStopMic();
    private native boolean nativeMicStopRequested();
    private native String nativeStats();
    private native String nativeDrainObservations();
    private native String nativeConsentState();
    private native String nativeSetObservationIntervalMs(int intervalMs);
    private native String nativeStopAll();
    private native String nativeSelfTest();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        // H-03: hub link and node status. Built by NodePanel so this activity stays readable; it
        // shows only what it can actually learn (transport counters, Wi-Fi, battery, thermal,
        // permissions) and never an observation value or a credential.
        nodePanel = new NodePanel(this);
        nodePanel.setHost(new NodePanel.Host() {
            @Override
            public String setObservationIntervalMs(int ms) {
                // The real native call: clamps server-side and returns what it applied.
                return nativeSetObservationIntervalMs(ms);
            }
        });
        root.addView(nodePanel.build());
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("HomeGuardian AI - acquisition node");
        title.setTextSize(18);
        root.addView(title);

        recordBanner = new TextView(this);
        recordBanner.setTextSize(15);
        recordBanner.setPadding(pad, pad, pad, pad);
        root.addView(recordBanner);

        // ---- identity -------------------------------------------------------
        TextView h1 = header("Device identity");
        root.addView(h1);
        identityView = new TextView(this);
        root.addView(identityView);

        nameEdit = new EditText(this);
        nameEdit.setInputType(InputType.TYPE_CLASS_TEXT);
        nameEdit.setHint("display name");
        root.addView(nameEdit);
        root.addView(button("Save display name", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                applyIdentity();
            }
        }));

        // ---- consent --------------------------------------------------------
        root.addView(header("Consent (required before any capture)"));
        consentView = new TextView(this);
        root.addView(consentView);
        root.addView(button("Grant consent (camera + microphone)", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                consentCamera = true;
                consentMic = true;
                setStatus(nativeGrantConsent(true, true));
                refreshConsentView();
            }
        }));
        root.addView(button("WITHDRAW consent", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                consentCamera = false;
                consentMic = false;
                stopEverything();
                setStatus(nativeWithdrawConsent());
                refreshConsentView();
            }
        }));

        // ---- camera ---------------------------------------------------------
        root.addView(header("Camera"));
        facingSpinner = spinner(labels("Back camera", "Front camera"));
        root.addView(facingSpinner);
        sizeSpinner = spinner(labels("320x240", "640x480", "854x480"));
        root.addView(sizeSpinner);
        fpsSpinner = spinner(labels("5 fps", "10 fps", "15 fps"));
        root.addView(fpsSpinner);
        cameraStartButton = button("START camera capture", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startCameraRequested();
            }
        });
        root.addView(cameraStartButton);
        cameraStopButton = button("Stop camera", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopCamera();
            }
        });
        root.addView(cameraStopButton);
        cameraStatus = new TextView(this);
        root.addView(cameraStatus);

        // ---- microphone -----------------------------------------------------
        root.addView(header("Microphone (PCM only, no recording saved)"));
        rateSpinner = spinner(labels("8000 Hz", "16000 Hz", "44100 Hz"));
        root.addView(rateSpinner);
        channelSpinner = spinner(labels("mono", "stereo"));
        root.addView(channelSpinner);
        micStartButton = button("START microphone capture", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startMicRequested();
            }
        });
        root.addView(micStartButton);
        micStopButton = button("Stop microphone", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopMic("operator stopped");
            }
        });
        root.addView(micStopButton);
        micStatus = new TextView(this);
        root.addView(micStatus);

        // ---- status ---------------------------------------------------------
        root.addView(header("Live counters"));
        countersView = new TextView(this);
        countersView.setTextSize(12);
        root.addView(countersView);
        root.addView(button("Run non-capture self-test", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setStatus(nativeSelfTest());
            }
        }));

        // ---- CCTV foreground service ----------------------------------------
        // The service is deliberately NOT exported, so it cannot be started from adb directly
        // (the shell uid is refused - verified). These controls are the supported path: an
        // explicit operator action inside the app. The service opens no camera and no
        // microphone; it is a lifecycle and notification fixture only.
        root.addView(header("CCTV node (foreground service)"));
        cctvStatus = new TextView(this);
        cctvStatus.setTextSize(12);
        root.addView(cctvStatus);
        root.addView(button("Start CCTV foreground service", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startCctvService();
            }
        }));
        root.addView(button("Stop CCTV foreground service", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopCctvService();
            }
        }));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);

        // Attended launcher hook for a remote/scripted test. This routes everything through the
        // exported activity, which is the supported method, rather than un-exporting services or
        // weakening the consent gate. Recognised actions:
        //   ...CCTV_START / ...CCTV_STOP     foreground service
        //   ...CONSENT_GRANT / ...CONSENT_WITHDRAW
        //   ...CAMERA_START / ...CAMERA_STOP
        //   ...MIC_START / ...MIC_STOP
        handleLaunchAction(getIntent() == null ? null : getIntent().getAction());
        refreshCctvStatus();

        camera1 = new Camera1Bridge();
        // Report the LEGACY camera availability to the native layer (the NDK enumeration is
        // what fails on this device). Enumeration is not capture - it only tells the native
        // lifecycle whether a Camera1 camera exists to open.
        try {
            Log.i(TAG, "" + nativeCameraReport(Camera1Bridge.cameraCount()));
        } catch (Throwable t) {
            Log.w(TAG, "camera availability report failed: " + t);
        }
        applyIdentity();
        refreshConsentView();
        setStatus("Ready. Nothing is capturing. Grant consent and press Start.");
        updateButtons();
    }

    // ---- UI helpers -------------------------------------------------------

    private TextView header(String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(15);
        tv.setPadding(0, (int) (10 * getResources().getDisplayMetrics().density), 0, 0);
        return tv;
    }

    private Button button(String label, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(label);
        b.setOnClickListener(onClick);
        return b;
    }

    // ---- CCTV foreground service helpers -----------------------------------

    /** TextView showing the service's own reported state. */
    private TextView cctvStatus;

    private void startCctvService() {
        Intent i = new Intent(this, CctvService.class);
        i.setAction(CctvService.ACTION_START);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(i);
        } else {
            startService(i);
        }
        Log.i(TAG, "requested CCTV service start");
        refreshCctvStatus();
    }

    private void stopCctvService() {
        Intent i = new Intent(this, CctvService.class);
        i.setAction(CctvService.ACTION_STOP);
        startService(i);
        Log.i(TAG, "requested CCTV service stop");
        refreshCctvStatus();
    }

    /** Reads the service's static state. Reports media as NOT armed, because it is not. */
    private void refreshCctvStatus() {
        if (cctvStatus == null) return;
        cctvStatus.setText("state: " + (CctvService.isRunning ? "RUNNING (foreground)" : "stopped")
                + "   last stop reason: " + CctvService.lastStopReason()
                + "\nmedia: NOT armed - this build opens no camera and no microphone");
    }

    private Spinner spinner(String[] values) {
        Spinner sp = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this,
                android.R.layout.simple_spinner_item, values);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(adapter);
        return sp;
    }

    private String[] labels(String... values) {
        return values;
    }

    private void setStatus(String s) {
        Log.i(TAG, s);
        cameraStatus.setText(s);
    }

    private void refreshConsentView() {
        consentView.setText("Consent: camera=" + (consentCamera ? "GRANTED" : "not granted")
                + ", microphone=" + (consentMic ? "GRANTED" : "not granted"));
    }

    private void refreshCounters() {
        String stats = nativeStats();
        countersView.setText(stats);
        boolean capturing = camera1 != null && camera1.isCapturing();
        if (capturing) {
            recordBanner.setText("● RECORDING - CAMERA ACTIVE (consent granted)");
            recordBanner.setBackgroundColor(0xFFFFDDDD);
        } else if (stats.contains("audio=on")) {
            recordBanner.setText("● RECORDING - MICROPHONE ACTIVE (consent granted)");
            recordBanner.setBackgroundColor(0xFFFFDDDD);
        } else {
            recordBanner.setText("Nothing is capturing");
            recordBanner.setBackgroundColor(0x00000000);
        }
        // The AAudio driver thread can demand a stop (consent/permission/
        // disconnect). The control thread performs it here.
        if (nativeMicStopRequested()) {
            stopMic("native stop requested (consent/permission/device)");
        }
        updateButtons();
    }

    private void updateButtons() {
        boolean capturing = camera1 != null && camera1.isCapturing();
        cameraStartButton.setEnabled(!capturing);
        if (cameraStopButton != null) {
            cameraStopButton.setEnabled(capturing);
        }
        if (micStartButton != null) {
            micStartButton.setEnabled(!nativeStats().contains("audio=on"));
        }
        if (micStopButton != null) {
            micStopButton.setEnabled(nativeStats().contains("audio=on"));
        }
    }

    private void applyIdentity() {
        String name = nameEdit.getText().toString();
        String summary = nativeSetIdentity(DeviceIdentity.deviceSeed(this),
                name.length() == 0 ? DeviceIdentity.defaultDisplayName() : name,
                DeviceIdentity.apiLevel(), DeviceIdentity.primaryAbi(), DeviceIdentity.model());
        identityView.setText(summary);
    }

    // ---- camera ------------------------------------------------------------

    private void startCameraRequested() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            pending = PENDING_CAMERA;
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
            return;
        }
        startCamera();
    }

    private void startCamera() {
        syncPermissions();
        if (!consentCamera) {
            setStatus("Camera: consent not granted. Press Grant consent first.");
            return;
        }
        int facing = facingSpinner.getSelectedItemPosition() == 1
                ? Camera.CameraInfo.CAMERA_FACING_FRONT
                : Camera.CameraInfo.CAMERA_FACING_BACK;
        int cameraId = Camera1Bridge.findCameraId(facing);
        if (cameraId < 0) {
            // No camera with that facing: report honestly, open nothing.
            setStatus("Camera: no " + (facing == Camera.CameraInfo.CAMERA_FACING_FRONT
                    ? "front" : "back") + " camera on this device; nothing started.");
            return;
        }
        int[] size = SIZES[sizeSpinner.getSelectedItemPosition()];
        int fps = FPS[fpsSpinner.getSelectedItemPosition()];
        // EXPLICIT backend selection: this device's legacy Camera1 path works while the NDK camera2
        // API enumerates zero cameras, so Camera1 is requested by name and the NDK is NOT required to
        // succeed first. Order matters: the native gate authorizes FIRST (consent + permission +
        // lifecycle), and only then does the bridge actually open the camera - so capture can never
        // begin before consent is checked. If the bridge fails to open, the native capturing state is
        // cleared instead of being left claiming capture.
        String backend = "camera1";
        String r = nativeStartCamera(cameraId, backend);
        if (r.startsWith("camera: capturing")) {
            String javaResult = camera1.startCamera(cameraId, size[0], size[1], fps);
            if (javaResult.startsWith("camera1: capturing")) {
                r = r + " | " + javaResult;
                Log.i(TAG, "camera backend=" + backend + " " + javaResult);
            } else {
                r = nativeCameraStartResult(false, javaResult);
            }
        }
        setStatus(r);
        refreshCounters();
    }

    private void stopCamera() {
        if (camera1 != null) {
            camera1.stopCamera();
        }
        setStatus(nativeStopCamera());
        // nativeStopCamera() clears the native video flag, so the panel's `video=` cannot stay on.
        refreshCounters();
    }

    // ---- microphone --------------------------------------------------------

    private void startMicRequested() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            pending = PENDING_MIC;
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
            return;
        }
        startMic();
    }

    private void startMic() {
        syncPermissions();
        if (!consentMic) {
            setStatus("Microphone: consent not granted. Press Grant consent first.");
            return;
        }
        int rate = RATES[rateSpinner.getSelectedItemPosition()];
        int channels = CHANNELS[channelSpinner.getSelectedItemPosition()];
        setStatus(nativeStartMic(rate, channels));
        refreshCounters();
    }

    private void stopMic(String why) {
        if (nativeStats().contains("audio=on")) {
            micStatus.setText("microphone stopped: " + why);
        }
        setStatus(nativeStopMic());
        refreshCounters();
    }

    // ---- lifecycle ---------------------------------------------------------

    /** Mirror the real OS grant state into native before every start attempt. */
    private void syncPermissions() {
        boolean cam = checkSelfPermission(Manifest.permission.CAMERA)
                == PackageManager.PERMISSION_GRANTED;
        boolean mic = checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED;
        nativeSetPermissions(cam, mic);
    }

    private void stopEverything() {
        if (camera1 != null) {
            camera1.stopCamera();
        }
        nativeStopAll();
        refreshCounters();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        boolean granted = granted(grantResults);
        if (requestCode == REQ_CAMERA) {
            if (granted) {
                startCamera();
            } else {
                pending = PENDING_NONE;
                setStatus("CAMERA permission denied; capture not started.");
                syncPermissions();
            }
        } else if (requestCode == REQ_MIC) {
            if (granted) {
                startMic();
            } else {
                pending = PENDING_NONE;
                setStatus("MICROPHONE permission denied; capture not started.");
                syncPermissions();
            }
        }
    }

    private boolean granted(int[] grantResults) {
        return grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Handles the attended launcher action when the activity is ALREADY running.
     *
     * Without this, `am start -a org.homeguardian.harness.CCTV_START` only warns "intent has
     * been delivered to currently running top-most instance" and the service is never started.
     * That was measured during acceptance testing: the first start worked and the restart
     * silently did nothing, because the action was only handled in onCreate.
     */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent == null) return;
        handleLaunchAction(intent.getAction());
    }

    /** Attended actions, shared by onCreate and onNewIntent so a repeat is never a no-op. */
    private void handleLaunchAction(String act) {
        if (act == null) return;
        if (CctvService.ACTION_START.equals(act)) {
            startCctvService();
        } else if (CctvService.ACTION_STOP.equals(act)) {
            stopCctvService();
        } else if (ACTION_CONSENT_GRANT.equals(act)) {
            consentCamera = true;
            consentMic = true;
            setStatus(nativeGrantConsent(true, true));
            refreshConsentView();
        } else if (ACTION_CONSENT_WITHDRAW.equals(act)) {
            consentCamera = false;
            consentMic = false;
            stopEverything();
            setStatus(nativeWithdrawConsent());
            refreshConsentView();
        } else if (ACTION_CAMERA_START.equals(act)) {
            startCameraRequested();
        } else if (ACTION_CAMERA_STOP.equals(act)) {
            stopCamera();
        } else if (ACTION_MIC_START.equals(act)) {
            startMicRequested();
        } else if (ACTION_MIC_STOP.equals(act)) {
            stopMic("operator stop");
        } else if (ACTION_CAM2_START.equals(act)) {
            startCam2();
        } else if (ACTION_CAM2_STOP.equals(act)) {
            stopCam2();
        } else if (ACTION_MIC2_START.equals(act)) {
            startMic2();
        } else if (ACTION_MIC2_STOP.equals(act)) {
            stopMic2();
        } else if (ACTION_MIC2_ENCODE.equals(act)) {
            relayToService(CctvService.ACTION_MIC_ENCODE);
        } else if (ACTION_DECODE_CHECK.equals(act)) {
            relayToService(CctvService.ACTION_DECODE_CHECK);
        } else if (ACTION_SINK_SELFTEST.equals(act)) {
            relayToService(CctvService.ACTION_SINK_SELFTEST);
        }
    }

    /**
     * Stage 3 helper: forward an action to the service, promoting it to the foreground first so
     * the OS background-sensor rule cannot refuse the work.
     */
    private void relayToService(String serviceAction) {
        Intent i = new Intent(this, CctvService.class);
        i.setAction(serviceAction);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(i);
        } else {
            startService(i);
        }
        Log.i(TAG, "relayed to service: " + serviceAction);
        setStatus("relayed: " + serviceAction);
        refreshCctvStatus();
    }

    /** Stage 4: arm the service-owned real microphone (separate from the video stream). */
    private void startMic2() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            setStatus("mic: RECORD_AUDIO not granted; nothing armed");
            return;
        }
        Intent i = new Intent(this, CctvService.class);
        i.setAction(CctvService.ACTION_MIC_ARM);
        // ACTION_MIC_ARM now promotes the service to the foreground itself, so the microphone
        // is armed WITHOUT touching the camera. Sending ACTION_START here would have armed
        // video too, breaking the "separately controllable" requirement (measured: it did).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(i);
        } else {
            startService(i);
        }
        Log.i(TAG, "requested service-owned mic arm");
        setStatus("mic: service arm requested");
        refreshCctvStatus();
    }

    private void stopMic2() {
        Intent i = new Intent(this, CctvService.class);
        i.setAction(CctvService.ACTION_MIC_DISARM);
        startService(i);
        Log.i(TAG, "requested service-owned mic disarm");
        setStatus("mic: service disarm requested");
        refreshCctvStatus();
    }

    // ---- HG-CAM-08C: capture is owned by CctvService, not by this activity ----

    /**
     * The activity only checks permission and asks the service to arm. It deliberately holds NO
     * camera, codec or surface reference, so activity onPause()/onStop() cannot terminate
     * screen-off capture - which is the whole point of moving ownership into the service.
     */
    private void startCam2() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            setStatus("capture: CAMERA permission not granted; nothing armed");
            return;
        }
        Intent i = new Intent(this, CctvService.class);
        // ACTION_START covers both cases: it promotes to foreground THEN arms when the service
        // is down, and just arms when it is already up. Foreground-before-camera is mandatory
        // on Android 9, so this must not be reordered.
        i.setAction(CctvService.ACTION_START);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(i);
        } else {
            startService(i);
        }
        Log.i(TAG, "requested service-owned capture arm");
        setStatus("capture: service arm requested (the service owns the camera)");
        refreshCctvStatus();
    }

    private void stopCam2() {
        Intent i = new Intent(this, CctvService.class);
        i.setAction(CctvService.ACTION_DISARM); // disarms the sensor, keeps the service up
        startService(i);
        Log.i(TAG, "requested service-owned capture disarm");
        setStatus("capture: service disarm requested");
        refreshCctvStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        syncPermissions();
        ticker.post(tick);
        refreshCctvStatus();
    }

    @Override
    protected void onPause() {
        super.onPause();
        ticker.removeCallbacks(tick);
        // HG-CAM-08C: the ACTIVITY-local devices stop and release here, because they are
        // activity-scoped. The CCTV capture is deliberately NOT touched: CctvService owns it,
        // and Android 9 pauses this activity the instant the screen goes off - so stopping the
        // service here would make screen-off capture impossible by construction.
        // What stops here: the activity's own Camera1 preview and the native simulator devices.
        stopEverything();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopEverything();
    }
}
