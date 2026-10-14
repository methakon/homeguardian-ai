package org.homeguardian.harness;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.hardware.Camera;
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
            }
            refreshCounters();
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
    private native String nativeStartCamera(int cameraId);
    private native String nativeStopCamera();
    private native String nativeStartMic(int sampleRate, int channels);
    private native String nativeStopMic();
    private native boolean nativeMicStopRequested();
    private native String nativeStats();
    private native String nativeDrainObservations();
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

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);

        camera1 = new Camera1Bridge();
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
        String r = nativeStartCamera(cameraId);
        if (r.startsWith("camera: capturing")) {
            r = camera1.startCamera(cameraId, size[0], size[1], fps);
        }
        setStatus(r);
        refreshCounters();
    }

    private void stopCamera() {
        if (camera1 != null) {
            camera1.stopCamera();
        }
        setStatus(nativeStopCamera());
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

    @Override
    protected void onResume() {
        super.onResume();
        syncPermissions();
        ticker.post(tick);
    }

    @Override
    protected void onPause() {
        super.onPause();
        ticker.removeCallbacks(tick);
        // No background capture: leaving the foreground stops and releases
        // everything. The operator must press Start again on return.
        stopEverything();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopEverything();
    }
}
