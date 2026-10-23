package org.homeguardian.harness;

import android.content.Context;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;
import android.util.Size;
import android.view.Surface;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * HG-CAM-08B probe: Camera2 -> MediaCodec input surface -> encoded H.264 buffers.
 *
 * WHY THIS EXISTS
 * The Camera1 path delivered ZERO frames on the ELUGA I7. The vendor HAL said:
 *
 *   MtkCam/streambuf: ...App:YV12:0|HW_TEXTURE:0... released w/o using -> ERROR buffer
 *   MtkCam/pipeline:  [getBufferLockedImp] [requestNo:0 frameNo:0 streamId:0] mNonReleasedNum==0
 *
 * "released w/o using" together with zero non-released buffers is the signature of a buffer
 * queue nobody consumes. The H-03 design attached a bare `new SurfaceTexture(0)` and never
 * called updateTexImage() (it only wanted the preview callback), so the queue filled and the
 * producer stalled. Camera1 cannot target anything else - setPreviewTexture takes a
 * SurfaceTexture, not a Surface - so the fix is not a different texture, it is a different API.
 *
 * A MediaCodec input surface is consumed by the codec itself, so no app-side drain is needed and
 * the stall cannot occur - and the output is exactly what a CCTV node wants.
 *
 * This class ONLY measures: encoded buffers, timestamps, rate, sizes. It writes nothing to disk
 * and sends nothing anywhere. Buffers are released undecoded (`releaseOutputBuffer(idx, false)`),
 * so no picture is rendered or retained.
 *
 * Manual Start/Stop only, bounded run, no auto-start.
 */
public class Camera2EncoderProbe {

    private static final String TAG = "HGCam2";

    /** Bounded: long enough to measure a rate, short enough to stay attended. */
    private static final int RUN_MS = 12000;
    private static final int PREFERRED_W = 640;
    private static final int PREFERRED_H = 480;
    private static final int MAX_W = 1280;
    private static final int MAX_H = 720;
    private static final int BITRATE = 2000000;
    private static final int FRAME_RATE = 15;

    private final Context ctx;
    /** <= 0 means UNBOUNDED: the foreground service owns the lifetime (HG-CAM-08C). */
    private final int runMs;
    private final ProbeResult result = new ProbeResult();

    private HandlerThread thread;
    private Handler handler;
    private CameraDevice device;
    private CameraCaptureSession session;
    private MediaCodec encoder;
    private Surface inputSurface;
    private volatile boolean running = false;
    private Thread drainThread;

    /** Bounded probe run - the HG-CAM-08B acceptance behaviour. */
    public Camera2EncoderProbe(Context context) { this(context, RUN_MS); }

    /**
     * runMs > 0  : bounded run that auto-releases on completion (probe / acceptance test).
     * runMs <= 0 : UNBOUNDED, released only by an explicit stop(). The foreground service uses
     *              this, because a CCTV node captures until the operator stops it - and because
     *              auto-releasing on a timer would defeat the screen-off assertion.
     */
    public Camera2EncoderProbe(Context context, int runMs) {
        this.ctx = context;
        this.runMs = runMs;
    }

    public boolean isUnbounded() { return runMs <= 0; }

    public static class ProbeResult {
        public volatile boolean started = false;
        public volatile String cameraId = "";
        public volatile int width = 0, height = 0;
        public volatile int encodedBuffers = 0;
        public volatile long firstPtsUs = -1, lastPtsUs = -1;
        public volatile long totalBytes = 0;
        public volatile int keyFrames = 0;
        public volatile String outputFormat = "";
        public volatile String lastError = "";
        public volatile long elapsedMs = 0;

        public double measuredFps() {
            if (firstPtsUs < 0 || lastPtsUs <= firstPtsUs) return 0.0;
            return (encodedBuffers - 1) * 1_000_000.0 / (lastPtsUs - firstPtsUs);
        }

        public String summary() {
            StringBuilder sb = new StringBuilder();
            sb.append("cam2probe ").append(started ? "STARTED" : "NOT-STARTED");
            sb.append(" camera=").append(cameraId);
            sb.append(" size=").append(width).append('x').append(height);
            sb.append(" encodedBuffers=").append(encodedBuffers);
            sb.append(" keyFrames=").append(keyFrames);
            sb.append(" fps=").append(String.format(Locale.US, "%.2f", measuredFps()));
            sb.append(" bytes=").append(totalBytes);
            sb.append(" elapsedMs=").append(elapsedMs);
            sb.append(" firstPtsUs=").append(firstPtsUs).append(" lastPtsUs=").append(lastPtsUs);
            if (outputFormat.length() > 0) sb.append(" fmt=").append(outputFormat);
            if (lastError.length() > 0) sb.append(" ERROR=").append(lastError);
            return sb.toString();
        }
    }

    public ProbeResult result() { return result; }

    // ---------------------------------------------------------------- control
    public synchronized String start() {
        if (running) return "cam2probe: already running";
        result.lastError = "";
        result.encodedBuffers = 0;
        result.keyFrames = 0;
        result.totalBytes = 0;
        result.firstPtsUs = -1;
        result.lastPtsUs = -1;
        result.started = false;

        thread = new HandlerThread("hg-cam2");
        thread.start();
        handler = new Handler(thread.getLooper());
        running = true;
        openCamera();
        return "cam2probe: starting (see " + TAG + " log)";
    }

    public synchronized void stop() {
        if (!running && encoder == null && device == null) return;
        running = false;
        // Close on the handler thread so the camera callbacks stay ordered.
        if (handler != null) {
            handler.post(new Runnable() { @Override public void run() { releaseResources(); } });
        } else {
            releaseResources();
        }
        if (drainThread != null) { try { drainThread.join(3000); } catch (InterruptedException ignored) {} drainThread = null; }
        if (thread != null) { thread.quitSafely(); thread = null; }
    }

    /** Must run on the handler thread. Cameras closed here are never left open. */
    private void releaseResources() {
        Log.i(TAG, "releasing; " + result.summary());
        try { if (session != null) { session.close(); } } catch (Exception e) { Log.w(TAG, "session close: " + e); }
        session = null;
        try { if (device != null) { device.close(); } } catch (Exception e) { Log.w(TAG, "device close: " + e); }
        device = null;
        try { if (encoder != null) { encoder.stop(); encoder.release(); } } catch (Exception e) { Log.w(TAG, "encoder release: " + e); }
        encoder = null;
        try { if (inputSurface != null) { inputSurface.release(); } } catch (Exception e) { Log.w(TAG, "surface release: " + e); }
        inputSurface = null;
        Log.i(TAG, "stopped and released: " + result.summary());
    }

    public synchronized boolean isRunning() { return running; }

    // ---------------------------------------------------------------- camera
    private void openCamera() {
        try {
            CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
            if (cm == null) { fail("no CameraManager"); return; }
            String[] ids = cm.getCameraIdList();
            Log.i(TAG, "camera2 ids=" + Arrays.toString(ids));
            if (ids.length == 0) { fail("camera2 enumerates zero devices"); return; }

            String chosen = null;
            for (String id : ids) {
                Integer facing = cm.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) { chosen = id; break; }
            }
            if (chosen == null) chosen = ids[0];
            result.cameraId = chosen;

            CameraCharacteristics cc = cm.getCameraCharacteristics(chosen);
            Integer level = cc.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL);
            Log.i(TAG, "camera " + chosen + " hardwareLevel=" + level
                    + " (0=LIMITED,1=FULL,2=LEGACY,3=LEVEL_3)");

            Size[] encSizes = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    .getOutputSizes(MediaCodec.class);
            Log.i(TAG, "encoder-compatible sizes (" + (encSizes == null ? 0 : encSizes.length) + "): "
                    + Arrays.toString(encSizes));

            cm.openCamera(chosen, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) {
                    device = d;
                    Log.i(TAG, "camera2 device OPENED id=" + result.cameraId);
                    configureSession();
                }
                @Override public void onDisconnected(CameraDevice d) {
                    Log.w(TAG, "camera2 device DISCONNECTED");
                    d.close(); device = null;
                }
                @Override public void onError(CameraDevice d, int code) {
                    fail("camera2 device error code=" + code);
                    d.close(); device = null;
                }
            }, handler);
        } catch (Exception e) {
            fail("openCamera threw: " + e);
        }
    }

    private void configureSession() {
        try {
            // Choose the smallest encoder-compatible size at or above the preferred one, capped
            // at 1280x720: bounded encode work on a 4x Cortex-A53.
            Size chosen = new Size(PREFERRED_W, PREFERRED_H);
            try {
                CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
                CameraCharacteristics cc = cm.getCameraCharacteristics(result.cameraId);
                Size[] sizes = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                        .getOutputSizes(MediaCodec.class);
                Size best = null;
                if (sizes != null) {
                    for (Size s : sizes) {
                        if (s.getWidth() > MAX_W || s.getHeight() > MAX_H) continue;
                        if (s.getWidth() < PREFERRED_W || s.getHeight() < PREFERRED_H) continue;
                        if (best == null || s.getWidth() * s.getHeight() < best.getWidth() * best.getHeight()) {
                            best = s;
                        }
                    }
                }
                if (best != null) chosen = best;
            } catch (Exception e) {
                Log.w(TAG, "size selection fell back to default: " + e);
            }
            result.width = chosen.getWidth();
            result.height = chosen.getHeight();

            MediaFormat fmt = MediaFormat.createVideoFormat("video/avc", result.width, result.height);
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, BITRATE);
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, FRAME_RATE);
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);

            encoder = MediaCodec.createEncoderByType("video/avc");
            encoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            inputSurface = encoder.createInputSurface();
            encoder.start();
            Log.i(TAG, "encoder started " + result.width + "x" + result.height
                    + " (MediaCodec consumes the queue - no app-side drain, unlike the Camera1 trap)");

            List<Surface> targets = new ArrayList<Surface>();
            targets.add(inputSurface);
            device.createCaptureSession(targets, new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession s) {
                    session = s;
                    Log.i(TAG, "capture session CONFIGURED with the encoder input surface");
                    startRepeating();
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) {
                    fail("capture session configure FAILED");
                }
            }, handler);

            startDrain();
        } catch (Exception e) {
            fail("configureSession threw: " + e);
        }
    }

    private void startRepeating() {
        try {
            CaptureRequest.Builder b = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            b.addTarget(inputSurface);
            b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
            session.setRepeatingRequest(b.build(), null, handler);
            result.started = true;
            Log.i(TAG, "repeating request set (TEMPLATE_RECORD -> encoder surface)");
        } catch (Exception e) {
            fail("setRepeatingRequest threw: " + e);
        }
    }

    // ---------------------------------------------------------------- encode
    private void startDrain() {
        drainThread = new Thread(new Runnable() {
            @Override public void run() {
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                long t0 = SystemClock.elapsedRealtime();
                while (running && (runMs <= 0 || (SystemClock.elapsedRealtime() - t0) < runMs)) {
                    try {
                        int idx = encoder.dequeueOutputBuffer(info, 2000);
                        if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                            MediaFormat of = encoder.getOutputFormat();
                            result.outputFormat = of.getString(MediaFormat.KEY_MIME)
                                    + " w=" + of.getInteger(MediaFormat.KEY_WIDTH)
                                    + " h=" + of.getInteger(MediaFormat.KEY_HEIGHT);
                            Log.i(TAG, "encoder OUTPUT FORMAT CHANGED: " + result.outputFormat);
                        } else if (idx >= 0) {
                            if (info.size > 0) {
                                result.encodedBuffers++;
                                result.totalBytes += info.size;
                                boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                                if (key) result.keyFrames++;
                                if (result.firstPtsUs < 0) result.firstPtsUs = info.presentationTimeUs;
                                result.lastPtsUs = info.presentationTimeUs;
                                if (result.encodedBuffers <= 3 || result.encodedBuffers % 25 == 0) {
                                    Log.i(TAG, "encoded#" + result.encodedBuffers + " size=" + info.size
                                            + " ptsUs=" + info.presentationTimeUs + " key=" + key);
                                }
                            }
                            encoder.releaseOutputBuffer(idx, false); // false = never render
                        }
                    } catch (Exception e) {
                        if (running) fail("encoder drain: " + e);
                        break;
                    }
                }
                result.elapsedMs = SystemClock.elapsedRealtime() - t0;
                Log.i(TAG, "DRAIN FINISHED: " + result.summary());
                // The bounded run is over. Clear `running` and release the camera, otherwise
                // isRunning() stays true forever and the NEXT start() is silently refused as
                // "already running" - the same silent-refusal class as the singleTop defect.
                // An UNBOUNDED run (the service) is released only by an explicit stop().
                if (runMs > 0 && running) {
                    running = false;
                    Log.i(TAG, "bounded run complete - auto-releasing the camera (attend a new start to measure again)");
                    if (handler != null) {
                        handler.post(new Runnable() { @Override public void run() { releaseResources(); } });
                    }
                    if (thread != null) { thread.quitSafely(); thread = null; } // quitSafely runs the post above
                }
            }
        }, "hg-cam2-drain");
        drainThread.start();
    }

    private void fail(String why) {
        result.lastError = why;
        Log.e(TAG, "PROBE FAILURE: " + why);
    }
}
