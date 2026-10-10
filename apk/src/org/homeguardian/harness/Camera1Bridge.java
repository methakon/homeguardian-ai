package org.homeguardian.harness;

import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.util.Log;
import android.view.SurfaceHolder;

import java.io.IOException;
import java.util.List;

/**
 * Camera1 (legacy android.hardware.Camera) bridge for HomeGuardian AI.
 *
 * This exists ONLY because the live diagnosis (friction log incident 10)
 * established that the connected MT6580 / Android 8.1 device exposes cameras
 * through the Camera1 compatibility path (Camera HAL v1.0 + Camera1 shim), on
 * which the NDK camera2 API enumerates zero devices. For camera2-native
 * devices, NdkCameraDevice is used instead; selection is explicit, never a
 * silent fallback.
 *
 * SAFETY CONTRACT (enforced here and in native code):
 *  - Capture is DISABLED by default. Nothing opens until the operator calls
 *    startCamera() explicitly AND the OS has granted CAMERA.
 *  - Every preview frame is handed to native onPreviewFrame() which MUST call
 *    the delivery gate (authorize_delivery) before processing. On consent
 *    withdrawal, permission revocation, or device error, native signals back
 *    and this bridge stops preview and releases the camera.
 *  - No frame is written to disk or uploaded. Frames are delivered to native
 *    in memory only; the byte[] buffer is reused by the camera and must not be
 *    retained past the callback.
 *
 * The camera is opened with a dummy SurfaceTexture because we only need the
 * preview callback data stream, not on-screen preview. No SurfaceView is used,
 * so nothing is rendered or persisted.
 */
public class Camera1Bridge {

    private static final String TAG = "HGCam1";
    private static final int PREVIEW_WIDTH = 320;
    private static final int PREVIEW_HEIGHT = 240;

    // Native delivery gate + counters (implemented in hg_harness_jni.cpp).
    // Returns true if the frame may be processed; false means native has
    // already decided capture must stop (consent withdrawn / revoked / error),
    // and we must stop preview.
    private native boolean nativeOnPreviewFrame(byte[] data, int width, int height, int format);

    private Camera camera;
    private boolean capturing = false;
    private SurfaceTexture dummyTexture;

    /** Explicit operator entry point. Opens and starts preview. Returns a status string. */
    public synchronized String startCamera() {
        if (capturing) {
            return "camera1: already capturing";
        }
        try {
            int cameraId = findBackCameraId();
            camera = Camera.open(cameraId);
            if (camera == null) {
                return "camera1: Camera.open returned null";
            }
        } catch (RuntimeException e) {
            // Camera.open throws if the camera is in use or disabled.
            camera = null;
            Log.w(TAG, "Camera.open failed: " + e.getMessage());
            return "camera1: open failed (" + e.getMessage() + ")";
        } catch (Exception e) {
            camera = null;
            return "camera1: open error (" + e.getMessage() + ")";
        }

        try {
            Camera.Parameters params = camera.getParameters();
            Camera.Size best = choosePreviewSize(params.getSupportedPreviewSizes());
            params.setPreviewSize(best.width, best.height);
            params.setPreviewFormat(ImageFormat.NV21);
            camera.setParameters(params);

            // Dummy texture: we need a valid surface for setPreviewTexture but
            // never display it. This keeps the preview data flowing to the
            // callback without rendering or persisting anything.
            dummyTexture = new SurfaceTexture(0);
            camera.setPreviewTexture(dummyTexture);

            final int w = best.width;
            final int h = best.height;
            final int fmt = params.getPreviewFormat();

            camera.setPreviewCallback(new Camera.PreviewCallback() {
                @Override
                public void onPreviewFrame(byte[] data, Camera cam) {
                    // Delivery boundary: native re-checks consent/permission/
                    // device state for THIS frame before it is processed.
                    boolean keep = nativeOnPreviewFrame(data, w, h, fmt);
                    if (!keep) {
                        // Native decided capture must stop now. Stop preview and
                        // release the camera from this (callback) thread.
                        Log.i(TAG, "delivery gate denied a frame; stopping capture");
                        stopPreviewAndRelease();
                    }
                }
            });

            camera.startPreview();
            capturing = true;
            return "camera1: capturing " + w + "x" + h + " (delivery-gated)";
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

    /** Stop preview and release the camera. Idempotent; safe from any thread. */
    public synchronized void stopCamera() {
        stopPreviewAndRelease();
    }

    private void stopPreviewAndRelease() {
        Camera c = camera;
        camera = null;
        capturing = false;
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

    private int findBackCameraId() {
        int numberOfCameras = Camera.getNumberOfCameras();
        Camera.CameraInfo info = new Camera.CameraInfo();
        for (int i = 0; i < numberOfCameras; i++) {
            Camera.getCameraInfo(i, info);
            if (info.facing == Camera.CameraInfo.CAMERA_FACING_BACK) {
                return i;
            }
        }
        return 0; // fall back to camera 0 if no back camera reported
    }

    private Camera.Size choosePreviewSize(List<Camera.Size> sizes) {
        if (sizes == null || sizes.isEmpty()) {
            // No supported sizes reported; request a small default.
            Camera.Size fallback = camera.new Size(PREVIEW_WIDTH, PREVIEW_HEIGHT);
            return fallback;
        }
        // Pick the supported size closest to the small target to bound memory.
        Camera.Size best = sizes.get(0);
        long bestDiff = diff(best);
        for (Camera.Size s : sizes) {
            long d = diff(s);
            if (d < bestDiff) {
                bestDiff = d;
                best = s;
            }
        }
        return best;
    }

    private long diff(Camera.Size s) {
        long dw = s.width - PREVIEW_WIDTH;
        long dh = s.height - PREVIEW_HEIGHT;
        return dw * dw + dh * dh;
    }
}
