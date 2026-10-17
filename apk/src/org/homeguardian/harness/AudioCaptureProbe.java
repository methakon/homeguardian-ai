package org.homeguardian.harness;

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaRecorder;
import android.os.SystemClock;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.Locale;

/**
 * HG-CAM-08C Stage 4: REAL local microphone acquisition.
 *
 * WHY THIS EXISTS
 * The harness already had a microphone path, but it drove the native SIMULATOR devices and the
 * accepted evidence for "microphone works" was only that RECORD_AUDIO had been granted. HG-CAM-08
 * explicitly forbids that:  "Do not report microphone support based merely on granted
 * RECORD_AUDIO permission." This class reads genuine PCM samples from the device microphone and
 * counts them.
 *
 * WHAT IT DOES NOT DO
 * It never writes a file, never transmits, and never stores a recording. Samples are read into a
 * bounded scratch buffer, counted, and dropped. The evidence is counters, format and timestamps -
 * not household audio. Talkback (speaker output) is a separate milestone and is NOT implemented.
 *
 * Android 9 restricts the microphone to the foreground, exactly as it does the camera, so this is
 * driven from CctvService like the camera is.
 */
public class AudioCaptureProbe {

    private static final String TAG = "HGAudio";

    private static final int SAMPLE_RATE = 48000;
    private static final int CHANNEL_MASK = AudioFormat.CHANNEL_IN_MONO;
    private static final int ENCODING = AudioFormat.ENCODING_PCM_16BIT;

    private final Context ctx;
    private final Result result = new Result();

    private AudioRecord record;
    private Thread readThread;
    private volatile boolean running = false;

    // ---- HG-CAM-08D Stage 3: optional voice-encoder stage -------------------
    /**
     * When set, the captured PCM is additionally fed through a real Android audio ENCODER so the
     * report can say what encoded frames the device actually produces, rather than what codecs it
     * merely advertises. This matters because raw PCM is not transmittable and because this device
     * has NO Opus encoder (decoder only), which is the codec WebRTC prefers.
     */
    private MediaCodec audioEnc;
    private volatile boolean encodeAudio = false;
    private volatile long encInFrames = 0;
    private volatile long encOutFrames = 0;
    private volatile long encOutBytes = 0;
    private volatile String encFormat = "";
    private volatile String encError = "";

    public String encodedAudioSummary() {
        if (!encodeAudio) return "voice-encode: not requested";
        StringBuilder sb = new StringBuilder();
        sb.append("voice-encode mime=").append(encFormat.length() > 0 ? encFormat : "n/a");
        sb.append(" pcmInFrames=").append(encInFrames);
        sb.append(" encodedFrames=").append(encOutFrames);
        sb.append(" encodedBytes=").append(encOutBytes);
        if (encError.length() > 0) sb.append(" ERROR=").append(encError);
        return sb.toString();
    }

    /** Start capture AND pipe it through the AAC encoder (the one encoder this device has). */
    public synchronized String startEncoded() {
        encodeAudio = true;
        encInFrames = 0; encOutFrames = 0; encOutBytes = 0;
        encFormat = ""; encError = "";
        String r = start();
        if (!result.started) { encodeAudio = false; }
        return r + " | voice-encode requested (audio/mp4a-latm)";
    }

    public AudioCaptureProbe(Context context) { this.ctx = context; }

    public static class Result {
        public volatile boolean started = false;
        public volatile int sampleRate = 0;
        public volatile int channelCount = 0;
        public volatile String encoding = "";
        public volatile int bufferFrames = 0;      // frames per read call
        public volatile long readCalls = 0;
        public volatile long totalSamples = 0;     // per-channel samples actually delivered
        public volatile long totalBytes = 0;
        public volatile long firstAtMs = -1, lastAtMs = -1;
        public volatile long elapsedMs = 0;
        public volatile boolean silent = true;     // were all samples zero?
        public volatile int nonZeroSamples = 0;
        public volatile String lastError = "";

        public double measuredSampleRate() {
            if (firstAtMs < 0 || lastAtMs <= firstAtMs || totalSamples == 0) return 0.0;
            return totalSamples * 1000.0 / (lastAtMs - firstAtMs);
        }

        public String summary() {
            StringBuilder sb = new StringBuilder();
            sb.append("mic ").append(started ? "STARTED" : "NOT-STARTED");
            sb.append(" rate=").append(sampleRate).append("Hz ch=").append(channelCount);
            sb.append(" fmt=").append(encoding);
            sb.append(" bufferFrames=").append(bufferFrames);
            sb.append(" readCalls=").append(readCalls);
            sb.append(" samples=").append(totalSamples);
            sb.append(" measuredRate=").append(String.format(Locale.US, "%.0f", measuredSampleRate()));
            sb.append(" nonZero=").append(nonZeroSamples).append(silent ? " SILENT" : " AUDIO");
            sb.append(" elapsedMs=").append(elapsedMs);
            if (lastError.length() > 0) sb.append(" ERROR=").append(lastError);
            return sb.toString();
        }
    }

    public Result result() { return result; }

    public synchronized String start() {
        if (running) return "mic: already running";
        int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL_MASK, ENCODING);
        if (minBuf <= 0) {
            result.lastError = "getMinBufferSize returned " + minBuf;
            Log.e(TAG, "PROBE FAILURE: " + result.lastError);
            return "mic: unsupported configuration (" + minBuf + ")";
        }
        // Two min-buffers per read: enough headroom that a slow reader does not overrun.
        int bufBytes = minBuf * 2;
        try {
            record = new AudioRecord(MediaRecorder.AudioSource.MIC, SAMPLE_RATE,
                    CHANNEL_MASK, ENCODING, bufBytes);
        } catch (Exception e) {
            result.lastError = "AudioRecord ctor: " + e;
            Log.e(TAG, "PROBE FAILURE: " + result.lastError);
            return "mic: " + result.lastError;
        }
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            result.lastError = "AudioRecord not initialized (state=" + record.getState() + ")";
            Log.e(TAG, "PROBE FAILURE: " + result.lastError);
            record.release(); record = null;
            return "mic: " + result.lastError;
        }

        result.sampleRate = record.getSampleRate();
        result.channelCount = record.getChannelCount();
        result.encoding = (record.getAudioFormat() == ENCODING) ? "PCM_16BIT" : ("fmt" + record.getAudioFormat());
        result.bufferFrames = bufBytes / 2 / result.channelCount;
        result.readCalls = 0;
        result.totalSamples = 0;
        result.totalBytes = 0;
        result.firstAtMs = -1;
        result.lastAtMs = -1;
        result.nonZeroSamples = 0;
        result.silent = true;
        result.lastError = "";

        try {
            record.startRecording();
        } catch (Exception e) {
            result.lastError = "startRecording: " + e;
            Log.e(TAG, "PROBE FAILURE: " + result.lastError);
            record.release(); record = null;
            return "mic: " + result.lastError;
        }
        if (record.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
            result.lastError = "not RECORDING (state=" + record.getRecordingState() + ")";
            Log.e(TAG, "PROBE FAILURE: " + result.lastError);
            record.release(); record = null;
            return "mic: " + result.lastError;
        }

        result.started = true;
        running = true;
        Log.i(TAG, "mic STARTED source=MIC " + result.sampleRate + "Hz ch=" + result.channelCount
                + " " + result.encoding + " bufferFrames=" + result.bufferFrames);
        startReader();
        return "mic: started (" + result.summary() + ")";
    }

    private void startReader() {
        readThread = new Thread(new Runnable() {
            @Override public void run() {
                short[] buf = new short[result.bufferFrames];
                long t0 = SystemClock.elapsedRealtime();
                while (running) {
                    int n;
                    try {
                        n = record.read(buf, 0, buf.length);
                    } catch (Exception e) {
                        if (running) { result.lastError = "read: " + e; Log.e(TAG, "PROBE FAILURE: " + result.lastError); }
                        break;
                    }
                    if (n < 0) {
                        result.lastError = "read returned error " + n;
                        Log.e(TAG, "PROBE FAILURE: " + result.lastError);
                        break;
                    }
                    if (n > 0) {
                        long now = SystemClock.elapsedRealtime();
                        if (result.firstAtMs < 0) result.firstAtMs = now;
                        result.lastAtMs = now;
                        result.readCalls++;
                        result.totalSamples += n;
                        result.totalBytes += (long) n * 2;
                        int nz = 0;
                        for (int i = 0; i < n; i++) if (buf[i] != 0) { nz++; }
                        if (nz > 0) { result.nonZeroSamples += nz; result.silent = false; }
                        if (encodeAudio) feedEncoder(buf, n);
                        if (result.readCalls <= 2 || result.readCalls % 200 == 0) {
                            Log.i(TAG, "mic#" + result.readCalls + " frames=" + n
                                    + " totalSamples=" + result.totalSamples + " nonZero=" + nz);
                        }
                    }
                }
                result.elapsedMs = SystemClock.elapsedRealtime() - t0;
                Log.i(TAG, "MIC READER FINISHED: " + result.summary());
            }
        }, "hg-audio-read");
        readThread.start();
    }

    /** Stop and release the microphone. Safe when nothing is running. */
    public synchronized void stop() {
        if (!running && record == null) return;
        running = false;
        try { if (record != null) { record.stop(); } } catch (Exception e) { Log.w(TAG, "mic stop: " + e); }
        if (readThread != null) { try { readThread.join(2000); } catch (InterruptedException ignored) {} readThread = null; }
        try { if (record != null) { record.release(); } } catch (Exception e) { Log.w(TAG, "mic release: " + e); }
        record = null;
        // Release the voice encoder too, or a muted-but-held codec survives the stop.
        try { if (audioEnc != null) { audioEnc.stop(); audioEnc.release(); } } catch (Exception e) { Log.w(TAG, "aac release: " + e); }
        audioEnc = null;
        result.started = false;
        Log.i(TAG, "mic stopped and released: " + result.summary());
        if (encodeAudio) Log.i(TAG, "mic stopped and released: " + encodedAudioSummary());
    }

    public synchronized boolean isRunning() { return running; }

    /**
     * HG-CAM-08D Stage 3: pipe one PCM block through a REAL audio encoder and count what comes out.
     * Created lazily on first use. AAC-LC is the only audio encoder this device actually has
     * (Opus exists as a DECODER only), so this is the available path - the report says so rather
     * than implying WebRTC-grade Opus is present.
     */
    private final MediaCodec.BufferInfo encInfo = new MediaCodec.BufferInfo();

    private void feedEncoder(short[] pcm, int frames) {
        try {
            if (audioEnc == null) {
                MediaFormat f = MediaFormat.createAudioFormat("audio/mp4a-latm",
                        result.sampleRate, Math.max(1, result.channelCount));
                f.setInteger(MediaFormat.KEY_BIT_RATE, 64000);
                f.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
                f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384);
                audioEnc = MediaCodec.createEncoderByType("audio/mp4a-latm");
                audioEnc.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                audioEnc.start();
                encFormat = "audio/mp4a-latm(AAC-LC) 64kbps";
                Log.i(TAG, "AAC encoder started: " + encFormat);
            }
            int idx = audioEnc.dequeueInputBuffer(0);
            if (idx >= 0) {
                ByteBuffer in = audioEnc.getInputBuffer(idx);
                if (in != null && frames * 2 <= in.capacity()) {
                    in.clear();
                    for (int i = 0; i < frames; i++) in.putShort(pcm[i]);
                    long ptsUs = encInFrames * 1000000L / Math.max(1, result.sampleRate);
                    audioEnc.queueInputBuffer(idx, 0, frames * 2, ptsUs, 0);
                    encInFrames += frames;
                } else if (in != null) {
                    audioEnc.queueInputBuffer(idx, 0, 0, 0, 0);
                }
            }
            int oidx;
            while ((oidx = audioEnc.dequeueOutputBuffer(encInfo, 0)) >= 0) {
                if (encInfo.size > 0) {
                    encOutFrames++;
                    encOutBytes += encInfo.size;
                    if (encOutFrames <= 3 || encOutFrames % 50 == 0) {
                        Log.i(TAG, "aac#" + encOutFrames + " bytes=" + encInfo.size
                                + " ptsUs=" + encInfo.presentationTimeUs);
                    }
                }
                audioEnc.releaseOutputBuffer(oidx, false);
            }
        } catch (Exception e) {
            encError = "feedEncoder: " + e;
            Log.e(TAG, "AAC ENCODE FAILURE: " + encError);
        }
    }
}
