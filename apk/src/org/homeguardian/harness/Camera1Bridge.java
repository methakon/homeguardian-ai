package org.homeguardian.harness;

import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.util.Log;

import java.io.IOException;
import java.util.List;

/**
 * Camera1 (legacy android.hardware.Camera) acquisition path for HomeGuardian AI
 * (milestone D-02).
 *
 * This path exists because the documented target device (MT6580 / Android 8.1 /
 * API 27) exposes cameras through the Camera1 compatibility path, on which the
 * NDK camera2 API enumerates zero devices. Selection is explicit and documented;
 * a camera2-native device uses NdkCameraDevice instead. The same application
 * therefore runs on both older and newer phones.
 *
 * SAFETY CONTRACT (enforced here and in native code):
 *  - Nothing opens until the operator calls startCamera() explicitly, the OS has
 *    granted CAMERA, and consent is recorded. The Activity stops capture when it
 *    leaves the foreground.
 *  - Every preview frame is handed to native onPreviewFrame(), which re-checks
 *    consent/permission/capture state for THAT frame and accounts it (bounded
 *    size, dimension and timestamp validation, saturating counters). On refusal
 *    native returns false and this bridge stops preview and releases the camera.
 *  - Frames are delivered in memory only: no decoding, no inference, no file, no
 *    upload. The camera-owned byte[] must not be retained past the callback.
 *
 * The camera is opened against a dummy SurfaceTexture because only the preview
 * callback stream is needed: nothing is rendered, recorded or persisted.
 */
public class Camera1Bridge {

    private static final String TAG = "HGCam1";

    // Preview size candidates, smallest first: bounded memory on a 1 GB device.
    private static final int[][] SIZE_CANDIDATES = {
            {320, 240}, {640, 480}, {854, 480}
    };

    /** Native delivery gate + accountant (hg_harness_jni.cpp). */
    private native boolean nativeOnPreviewFrame(byte[] data, int width, int height, int format,
                                                long timestampMs);

    /**
     * Asks the NATIVE feature-bounds policy which supported preview size to negotiate. Returns "WxH",
     * or "" when no supported size is processable - in which case this bridge refuses to start rather
     * than hand the pipeline frames it will skip (the defect this replaces: a 720x576 preview produced
     * 518 authorized frames and ZERO processed ones, because 720x576 exceeds the processing bound).
     */
    private native String nativeChoosePreviewSize(int[] widths, int[] heights, int targetWidth,
                                                  int targetHeight);

    private Camera camera;
    private volatile boolean capturing = false;
    private SurfaceTexture dummyTexture;

    private int activeWidth = 0;
    private int activeHeight = 0;
    private int activeFps = 0;
    private int activeCameraId = -1;
    private long framesSeen = 0;          // local count for the UI (authoritative count is native)
    private long lastFrameTimestampMs = 0;

    /** Camera indices and their facing, for the UI selector. */
    public static int cameraCount() {
        try {
            return Camera.getNumberOfCameras();
        } catch (RuntimeException e) {
            return 0;
        }
    }

    public static boolean hasFacing(int facing) {
        int n = cameraCount();
        Camera.CameraInfo info = new Camera.CameraInfo();
        for (int i = 0; i < n; i++) {
            try {
                Camera.getCameraInfo(i, info);
            } catch (RuntimeException e) {
                continue;
            }
            if (info.facing == facing) {
                return true;
            }
        }
        return false;
    }

    /** Index of the first camera with the requested facing, or -1. */
    public static int findCameraId(int facing) {
        int n = cameraCount();
        Camera.CameraInfo info = new Camera.CameraInfo();
        for (int i = 0; i < n; i++) {
            try {
                Camera.getCameraInfo(i, info);
            } catch (RuntimeException e) {
                continue;
            }
            if (info.facing == facing) {
                return i;
            }
        }
        return -1;
    }

    /**
     * Explicit operator entry point. Opens the selected camera and starts
     * preview. targetWidth/targetHeight/targetFps are requests: the closest
     * supported values are used. Returns a human-readable status string.
     */
    public synchronized String startCamera(int cameraId, int targetWidth, int targetHeight,
                                           int targetFps) {
        if (capturing) {
            return "camera1: already capturing";
        }
        if (cameraId < 0 || cameraId >= cameraCount()) {
            return "camera1: no camera with the requested facing (id=" + cameraId + ")";
        }
        try {
            camera = Camera.open(cameraId);
            if (camera == null) {
                return "camera1: Camera.open returned null";
            }
        } catch (RuntimeException e) {
            camera = null;
            Log.w(TAG, "Camera.open failed: " + e.getMessage());
            return "camera1: open failed (" + e.getMessage() + ")";
        }

        try {
            Camera.Parameters params = camera.getParameters();
            List<Camera.Size> supported = params.getSupportedPreviewSizes();
            Camera.Size best = choosePreviewSize(supported, targetWidth, targetHeight);
            if (best == null) {
                // No supported size is processable. Release and say exactly why - never negotiate a
                // geometry the feature pipeline would skip silently.
                StringBuilder offered = new StringBuilder();
                if (supported != null) {
                    for (Camera.Size s : supported) {
                        if (offered.length() > 0) offered.append(',');
                        offered.append(s.width).append('x').append(s.height);
                    }
                }
                releaseQuietly();
                return "camera1: no supported preview size inside the processing bounds "
                        + "(needs >= 16 and <= 640x480 pixels); platform offers: " + offered;
            }
            params.setPreviewSize(best.width, best.height);
            params.setPreviewFormat(ImageFormat.NV21);
            applyFrameRate(params, targetFps);
            camera.setParameters(params);

            // Dummy texture: a valid surface for setPreviewTexture keeps frames
            // flowing to the callback without rendering anything on screen.
            dummyTexture = new SurfaceTexture(0);
            camera.setPreviewTexture(dummyTexture);

            activeWidth = best.width;
            activeHeight = best.height;
            activeCameraId = cameraId;
            activeFps = resolveFps(params, targetFps);
            framesSeen = 0;

            final int w = best.width;
            final int h = best.height;
            final int fmt = params.getPreviewFormat();

            camera.setPreviewCallback(new Camera.PreviewCallback() {
                @Override
                public void onPreviewFrame(byte[] data, Camera cam) {
                    framesSeen++;
                    lastFrameTimestampMs = System.currentTimeMillis();
                    // Wall-clock arrival time in ms, same base as the native
                    // accountant's clock, so plausibility checks are meaningful.
                    boolean keep = nativeOnPreviewFrame(data, w, h, fmt, lastFrameTimestampMs);
                    if (!keep) {
                        Log.i(TAG, "delivery gate denied a frame; stopping capture");
                        stopPreviewAndRelease();
                    }
                }
            });

            camera.startPreview();
            capturing = true;
            return "camera1: capturing " + activeWidth + "x" + activeHeight + " @ "
                    + activeFps + "fps (id=" + cameraId + ", delivery-gated)";
        } catch (IOException e) {
            Log.w(TAG, "setPreviewTexture failed: " + e.getMessage());
            releaseQuietly();
            return "camera1: preview setup failed (" + e.getMessage() + ")";
        } catch (RuntimeException e) {
            Log.w(TAG, "preview start failed: " + e.getMessage());
            releaseQuietly();
            return "camera1: preview failed (" + e.getMessage() + ")";
        }
    }

    /** Stop preview and release the camera. Idempotent and safe from any thread. */
    public synchronized void stopCamera() {
        stopPreviewAndRelease();
    }

    private void stopPreviewAndRelease() {
        Camera c = camera;
        camera = null;
        capturing = false;
        activeCameraId = -1;
        if (c != null) {
            try {
                c.setPreviewCallback(null);
                c.stopPreview();
            } catch (RuntimeException e) {
                Log.w(TAG, "stopPreview threw (ignored): " + e.getMessage());
            }
            try {
                c.release();
            } catch (Exception e) {
                Log.w(TAG, "release threw (ignored): " + e.getMessage());
            }
        }
        if (dummyTexture != null) {
            dummyTexture.release();
            dummyTexture = null;
        }
    }

    private void releaseQuietly() {
        stopPreviewAndRelease();
    }

    public synchronized boolean isCapturing() {
        return capturing;
    }

    public synchronized int activeWidth() {
        return activeWidth;
    }

    public synchronized int activeHeight() {
        return activeHeight;
    }

    public synchronized int activeFps() {
        return activeFps;
    }

    public synchronized int activeCameraId() {
        return activeCameraId;
    }

    public synchronized long framesSeen() {
        return framesSeen;
    }

    private void applyFrameRate(Camera.Parameters params, int targetFps) {
        try {
            List<int[]> ranges = params.getSupportedPreviewFpsRange();
            if (ranges != null && !ranges.isEmpty()) {
                int[] best = null;
                for (int[] r : ranges) {
                    if (best == null) {
                        best = r;
                        continue;
                    }
                    // Prefer a fixed range (min == max): an honest, steady rate.
                    boolean rFixed = (r[0] == r[1]);
                    boolean bestFixed = (best[0] == best[1]);
                    if (rFixed && !bestFixed) {
                        best = r;
                    }
                }
                if (best != null) {
                    params.setPreviewFpsRange(best[0], best[1]);
                    return;
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "setPreviewFpsRange unsupported: " + e.getMessage());
        }
        try {
            params.setPreviewFrameRate(targetFps);
        } catch (RuntimeException e) {
            Log.w(TAG, "setPreviewFrameRate unsupported: " + e.getMessage());
        }
    }

    private int resolveFps(Camera.Parameters params, int requested) {
        try {
            List<Integer> rates = params.getSupportedPreviewFrameRates();
            if (rates != null && rates.contains(Integer.valueOf(requested))) {
                return requested;
            }
            if (rates != null && !rates.isEmpty()) {
                int closest = rates.get(0);
                int bestDiff = Math.abs(closest - requested);
                for (Integer r : rates) {
                    int d = Math.abs(r.intValue() - requested);
                    if (d < bestDiff) {
                        bestDiff = d;
                        closest = r.intValue();
                    }
                }
                return closest;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "frame rate query failed: " + e.getMessage());
        }
        return requested;
    }

    /**
     * Delegates the choice to the native feature-bounds policy (one definition of the bounds), and
     * returns null when nothing it offered is processable. The target is a PREFERENCE, not a demand:
     * the closest supported size inside the bounds wins.
     */
    private Camera.Size choosePreviewSize(List<Camera.Size> sizes, int targetWidth, int targetHeight) {
        if (sizes == null || sizes.isEmpty()) {
            return null;
        }
        int[] widths = new int[sizes.size()];
        int[] heights = new int[sizes.size()];
        for (int i = 0; i < sizes.size(); i++) {
            widths[i] = sizes.get(i).width;
            heights[i] = sizes.get(i).height;
        }
        String chosen = nativeChoosePreviewSize(widths, heights, targetWidth, targetHeight);
        if (chosen == null || chosen.isEmpty()) {
            return null;
        }
        int sep = chosen.indexOf('x');
        if (sep <= 0) {
            return null;
        }
        try {
            int w = Integer.parseInt(chosen.substring(0, sep));
            int h = Integer.parseInt(chosen.substring(sep + 1));
            return camera.new Size(w, h);
        } catch (NumberFormatException e) {
            Log.w(TAG, "unparseable preview size from the native policy: " + chosen);
            return null;
        }
    }
}
