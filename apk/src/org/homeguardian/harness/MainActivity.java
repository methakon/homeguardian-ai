package org.homeguardian.harness;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Minimal acceptance-test harness for HomeGuardian AI.
 *
 * Safety properties (by design):
 *  - Capture is DISABLED by default. No camera or microphone is opened on
 *    launch, and nothing starts automatically.
 *  - The native self-test runs on launch and reports its result on screen.
 *  - Camera and microphone capture are started ONLY by an explicit operator
 *    button press, and ONLY after the corresponding runtime permission has
 *    been granted through the standard Android flow (no adb pm grant).
 *  - No media is written to disk or uploaded. Frames/samples are held in
 *    memory only for the duration of an operator-initiated test.
 *
 * Note: this class deliberately uses anonymous inner classes instead of Java 8
 * lambdas so the dex output contains no invokedynamic, which the Android 8.1
 * (API 27) runtime's dex verifier on this device rejects.
 */
public class MainActivity extends Activity {

    private static final String TAG = "HGHarness";
    private static final int REQ_CAMERA = 1001;
    private static final int REQ_MIC = 1002;

    private TextView status;
    private static boolean nativeLoaded = false;

    static {
        try {
            System.loadLibrary("hg_harness");
            nativeLoaded = true;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "native lib load failed", e);
        }
    }

    // Implemented in native code (hg_harness_jni.cpp). Each returns a short
    // status string. Capture entry points are no-ops unless the operator has
    // explicitly requested capture AND the gate authorizes it.
    private native String nativeSelfTest();
    private native String nativeStartCamera();   // operator-initiated only
    private native String nativeStopCamera();
    private native String nativeWithdrawConsent();
    private native String nativeStartMic();       // operator-initiated only
    private native String nativeStopMic();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        status = new TextView(this);
        status.setText(nativeLoaded
                ? "Native loaded. Running non-capture self-test...\n" + nativeSelfTest()
                : "Native library failed to load.");
        root.addView(status);

        root.addView(makeButton("Run non-capture self-test", new View.OnClickListener() {
            @Override public void onClick(View v) { status.setText(nativeSelfTest()); }
        }));

        root.addView(makeButton("Start CAMERA test (needs permission)", new View.OnClickListener() {
            @Override public void onClick(View v) { requestCameraThenStart(); }
        }));

        root.addView(makeButton("Stop camera", new View.OnClickListener() {
            @Override public void onClick(View v) { status.setText(nativeStopCamera()); }
        }));

        root.addView(makeButton("Withdraw consent", new View.OnClickListener() {
            @Override public void onClick(View v) { status.setText(nativeWithdrawConsent()); }
        }));

        root.addView(makeButton("Start MIC test (needs permission)", new View.OnClickListener() {
            @Override public void onClick(View v) { requestMicThenStart(); }
        }));

        root.addView(makeButton("Stop mic", new View.OnClickListener() {
            @Override public void onClick(View v) { status.setText(nativeStopMic()); }
        }));

        setContentView(root);
    }

    private Button makeButton(String label, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(label);
        b.setOnClickListener(onClick);
        return b;
    }

    private void requestCameraThenStart() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            // Standard runtime permission flow. We do NOT bypass with adb.
            requestPermissions(new String[]{Manifest.permission.CAMERA}, REQ_CAMERA);
        } else {
            status.setText(nativeStartCamera());
        }
    }

    private void requestMicThenStart() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
        } else {
            status.setText(nativeStartMic());
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        boolean granted = grantResults.length > 0 &&
                grantResults[0] == PackageManager.PERMISSION_GRANTED;
        if (requestCode == REQ_CAMERA) {
            // Only start capture if the operator granted the permission. Never
            // auto-start on a denial.
            status.setText(granted ? nativeStartCamera()
                    : "CAMERA permission denied; capture not started.");
        } else if (requestCode == REQ_MIC) {
            status.setText(granted ? nativeStartMic()
                    : "MIC permission denied; capture not started.");
        }
    }
}
