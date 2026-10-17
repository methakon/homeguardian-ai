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

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * HG-CAM-08D Stage 3: in-memory H.264 encoder -> decoder compatibility check.
 *
 * WHY THIS EXISTS
 * "The encoder produced buffers" and "the encoded stream is decodable" are different claims. A
 * vendor encoder can emit well-formed-looking AVC buffers that no decoder accepts - wrong profile
 * level, missing SPS/PPS, or a NAL layout a decoder chokes on. Only a real decode proves the stream
 * is usable, and a downstream consumer (a dashboard, or the eventual WebRTC path) can decode
 * nothing else.
 *
 * WHAT IT DOES
 * Runs the SAME Camera2 -> MediaCodec pipeline, but instead of dropping each encoded buffer it
 * feeds it straight into a decoder configured from the encoder's own output format (CSD-0/CSD-1 =
 * SPS/PPS). It then counts DECODED frames and reports the decoder's negotiated output format.
 *
 * PRIVACY
 * The decoder is configured with a NULL surface, so decoded pictures are never rendered, never
 * written and never transmitted. Nothing leaves the process; the evidence is frame counts,
 * timestamps and format strings. Private video is never stored.
 *
 * Bounded run, manual start only.
 */
public class H264RoundTripCheck {

    private static final String TAG = "HGDecode";

    private static final int RUN_MS = 10000;
    private static final int W = 640;
    private static final int H = 480;
    private static final int BITRATE = 2000000;
    private static final int FRAME_RATE = 15;

    private final Context ctx;
    private final Result result = new Result();

    private HandlerThread thread;
    private Handler handler;
    private CameraDevice device;
    private CameraCaptureSession session;
    private MediaCodec encoder;
    private MediaCodec decoder;
    private Surface inputSurface;
    private volatile boolean running = false;
    private Thread pump;
    private boolean decoderConfigured = false;

    public H264RoundTripCheck(Context context) { this.ctx = context; }

    public static class Result {
        public volatile boolean started = false;
        public volatile int encodedBuffers = 0;
        public volatile long encodedBytes = 0;
        public volatile int encodedKeyFrames = 0;
        public volatile int decodedFrames = 0;
        public volatile int decodedKeyFrames = 0;
        public volatile long decFirstPtsUs = -1, decLastPtsUs = -1;
        public volatile String encoderCfg = "";
        public volatile String decoderOut = "";
        public volatile boolean sawSpsPps = false;
        public volatile String lastError = "";
        public volatile long elapsedMs = 0;

        public String summary() {
            StringBuilder sb = new StringBuilder();
            sb.append("h264roundtrip ").append(started ? "STARTED" : "NOT-STARTED");
            sb.append(" encoded=").append(encodedBuffers).append("B/").append(encodedBytes).append("bytes");
            sb.append(" key=").append(encodedKeyFrames);
            sb.append(" DECODED=").append(decodedFrames);
            sb.append(" decKey=").append(decodedKeyFrames);
            sb.append(" csd=").append(sawSpsPps ? "SPS/PPS-present" : "SPS/PPS-MISSING");
            sb.append(" enc=").append(encoderCfg);
            sb.append(" decOut=").append(decoderOut);
            sb.append(" elapsedMs=").append(elapsedMs);
            if (lastError.length() > 0) sb.append(" ERROR=").append(lastError);
            return sb.toString();
        }

        public boolean compatible() {
            return decodedFrames > 0 && lastError.length() == 0;
        }
    }

    public Result result() { return result; }

    public synchronized String start() {
        if (running) return "decodecheck: already running";
        result.lastError = "";
        result.encodedBuffers = 0; result.encodedBytes = 0; result.encodedKeyFrames = 0;
        result.decodedFrames = 0; result.decodedKeyFrames = 0;
        result.decFirstPtsUs = -1; result.decLastPtsUs = -1;
        result.decoderOut = "none"; result.sawSpsPps = false;
        result.started = false; decoderConfigured = false;
        thread = new HandlerThread("hg-decode");
        thread.start();
        handler = new Handler(thread.getLooper());
        running = true;
        openCamera();
        return "decodecheck: starting (see " + TAG + " log)";
    }

    public synchronized void stop() {
        if (!running && encoder == null && decoder == null) return;
        running = false;
        if (handler != null) {
            handler.post(new Runnable() { @Override public void run() { release(); } });
        } else {
            release();
        }
        if (pump != null) { try { pump.join(3000); } catch (InterruptedException ignored) {} pump = null; }
        if (thread != null) { thread.quitSafely(); thread = null; }
    }

    /** Runs on the handler thread. */
    private void release() {
        Log.i(TAG, "releasing; " + result.summary());
        try { if (session != null) session.close(); } catch (Exception e) { Log.w(TAG, "session: " + e); }
        session = null;
        try { if (device != null) device.close(); } catch (Exception e) { Log.w(TAG, "device: " + e); }
        device = null;
        try { if (encoder != null) { encoder.stop(); encoder.release(); } } catch (Exception e) { Log.w(TAG, "encoder: " + e); }
        encoder = null;
        try { if (decoder != null) { decoder.stop(); decoder.release(); } } catch (Exception e) { Log.w(TAG, "decoder: " + e); }
        decoder = null;
        try { if (inputSurface != null) inputSurface.release(); } catch (Exception e) { Log.w(TAG, "surface: " + e); }
        inputSurface = null;
        Log.i(TAG, "stopped and released: " + result.summary());
    }

    private void openCamera() {
        try {
            CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
            if (cm == null) { fail("no CameraManager"); return; }
            String[] ids = cm.getCameraIdList();
            if (ids.length == 0) { fail("no camera2 device"); return; }
            String chosen = ids[0];
            for (String id : ids) {
                Integer f = cm.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                if (f != null && f == CameraCharacteristics.LENS_FACING_BACK) { chosen = id; break; }
            }
            final String cid = chosen;
            Log.i(TAG, "camera id=" + cid);
            cm.openCamera(cid, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice d) { device = d; Log.i(TAG, "camera OPENED"); setup(); }
                @Override public void onDisconnected(CameraDevice d) { d.close(); device = null; }
                @Override public void onError(CameraDevice d, int code) { fail("camera error " + code); d.close(); device = null; }
            }, handler);
        } catch (Exception e) {
            fail("openCamera: " + e);
        }
    }

    private void setup() {
        try {
            MediaFormat fmt = MediaFormat.createVideoFormat("video/avc", W, H);
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, BITRATE);
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, FRAME_RATE);
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            encoder = MediaCodec.createEncoderByType("video/avc");
            encoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            inputSurface = encoder.createInputSurface();
            encoder.start();
            Log.i(TAG, "encoder started " + W + "x" + H);

            List<Surface> targets = new ArrayList<Surface>();
            targets.add(inputSurface);
            device.createCaptureSession(targets, new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession s) {
                    session = s;
                    try {
                        CaptureRequest.Builder b = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                        b.addTarget(inputSurface);
                        b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                        session.setRepeatingRequest(b.build(), null, handler);
                        result.started = true;
                        Log.i(TAG, "session CONFIGURED; repeating request set");
                    } catch (Exception e) { fail("repeating request: " + e); }
                }
                @Override public void onConfigureFailed(CameraCaptureSession s) { fail("session configure failed"); }
            }, handler);

            startPump();
        } catch (Exception e) {
            fail("setup: " + e);
        }
    }

    /**
     * Single thread: drain the encoder, hand each buffer straight to the decoder, then drain the
     * decoder. Keeping it on one thread makes the encoder->decoder handoff trivially ordered.
     */
    private void startPump() {
        pump = new Thread(new Runnable() {
            @Override public void run() {
                MediaCodec.BufferInfo ei = new MediaCodec.BufferInfo();
                MediaCodec.BufferInfo di = new MediaCodec.BufferInfo();
                long t0 = SystemClock.elapsedRealtime();
                while (running && (SystemClock.elapsedRealtime() - t0) < RUN_MS) {
                    try {
                        int eidx = encoder.dequeueOutputBuffer(ei, 2000);
                        if (eidx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                            MediaFormat of = encoder.getOutputFormat();
                            result.encoderCfg = of.getString(MediaFormat.KEY_MIME)
                                    + " " + of.getInteger(MediaFormat.KEY_WIDTH)
                                    + "x" + of.getInteger(MediaFormat.KEY_HEIGHT)
                                    + (of.containsKey(MediaFormat.KEY_PROFILE) ? " profile=" + of.getInteger(MediaFormat.KEY_PROFILE) : "");
                            configureDecoder(of);
                        } else if (eidx >= 0) {
                            if (ei.size > 0) {
                                result.encodedBuffers++;
                                result.encodedBytes += ei.size;
                                if ((ei.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) result.encodedKeyFrames++;
                                feedDecoder(eidx, ei);
                            } else {
                                encoder.releaseOutputBuffer(eidx, false);
                            }
                        }
                        int didx = decoder == null ? MediaCodec.INFO_TRY_AGAIN_LATER : decoder.dequeueOutputBuffer(di, 0);
                        while (didx >= 0) {
                            if (di.size > 0) {
                                result.decodedFrames++;
                                if ((di.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) result.decodedKeyFrames++;
                                if (result.decFirstPtsUs < 0) result.decFirstPtsUs = di.presentationTimeUs;
                                result.decLastPtsUs = di.presentationTimeUs;
                            }
                            decoder.releaseOutputBuffer(didx, false); // false = never render
                            didx = decoder.dequeueOutputBuffer(di, 0);
                        }
                    } catch (Exception e) {
                        if (running) fail("roundtrip pump: " + e);
                        break;
                    }
                }
                result.elapsedMs = SystemClock.elapsedRealtime() - t0;
                Log.i(TAG, "PUMP FINISHED: " + result.summary());
                Log.i(TAG, "COMPATIBLE=" + result.compatible());
            }
        }, "hg-decode-pump");
        pump.start();
    }

    /** Configure the decoder from the ENCODER's own output format - CSD-0/CSD-1 are SPS/PPS. */
    private void configureDecoder(MediaFormat encOut) {
        try {
            ByteBuffer csd0 = encOut.getByteBuffer("csd-0");
            ByteBuffer csd1 = encOut.getByteBuffer("csd-1");
            result.sawSpsPps = (csd0 != null && csd1 != null);
            MediaFormat dec = MediaFormat.createVideoFormat("video/avc", W, H);
            if (csd0 != null) dec.setByteBuffer("csd-0", csd0);
            if (csd1 != null) dec.setByteBuffer("csd-1", csd1);
            decoder = MediaCodec.createDecoderByType("video/avc");
            // null surface: decode in memory, never render, never persist.
            decoder.configure(dec, null, null, 0);
            decoder.start();
            decoderConfigured = true;
            Log.i(TAG, "decoder configured from encoder CSD (SPS/PPS present=" + result.sawSpsPps + ")");
        } catch (Exception e) {
            fail("decoder configure: " + e);
        }
    }

    private void feedDecoder(int encIdx, MediaCodec.BufferInfo ei) {
        if (!decoderConfigured || decoder == null) {
            encoder.releaseOutputBuffer(encIdx, false);
            return;
        }
        try {
            int didx = decoder.dequeueInputBuffer(2000);
            if (didx >= 0) {
                ByteBuffer src = encoder.getOutputBuffer(encIdx);
                ByteBuffer dst = decoder.getInputBuffer(didx);
                if (src != null && dst != null) {
                    src.position(ei.offset);
                    src.limit(ei.offset + ei.size);
                    dst.clear();
                    dst.put(src);
                    decoder.queueInputBuffer(didx, 0, ei.size, ei.presentationTimeUs, ei.flags);
                } else {
                    decoder.queueInputBuffer(didx, 0, 0, ei.presentationTimeUs, 0);
                }
            }
        } catch (Exception e) {
            fail("feed decoder: " + e);
        } finally {
            try { encoder.releaseOutputBuffer(encIdx, false); } catch (Exception ignored) { }
        }
    }

    private void fail(String why) {
        result.lastError = why;
        Log.e(TAG, "CHECK FAILURE: " + why);
    }
}
