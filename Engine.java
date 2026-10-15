package jp.mok.vutest;

import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.MediaRecorder;

// 入力機器から音を受け取って、VU(針の式)とピークを計算する
public class Engine {

    // VU: 0.3秒で99%に届き、少し行き過ぎて戻る (ブラウザ版と同じ式)
    private static final double ZETA = 0.81, RISE = 0.3;
    private static final double WN;
    static {
        double z = ZETA, wd = Math.sqrt(1 - z * z), x = 0;
        for (; x < 50; x += 0.0005) {
            double y = 1 - Math.exp(-z * x) * (Math.cos(wd * x) + z / wd * Math.sin(wd * x));
            if (y >= 0.99) break;
        }
        WN = x / RISE;
    }

    public volatile double vuL, vuR, pkL, pkR;   // vu: 絶対値の平均(針の位置) / pk: ピーク(0〜1)
    public volatile double latencyMs = -1;
    public volatile int overruns = 0;
    public volatile double gain = 1;              // 入力レベル調整 (倍率)

    private AudioRecord rec;
    private Thread thread;
    private volatile boolean running;
    private boolean isFloat;
    private int ch, sr, bufFrames, source;

    public int channels() { return ch; }
    public int sampleRate() { return sr; }
    public String formatName() { return isFloat ? "float" : "16bit"; }
    public double bufferMs() { return bufFrames * 1000.0 / sr; }
    public String sourceName() {
        switch (source) {
            case MediaRecorder.AudioSource.UNPROCESSED: return "加工なし";
            case MediaRecorder.AudioSource.VOICE_RECOGNITION: return "音声認識用";
            case MediaRecorder.AudioSource.MIC: return "通常マイク";
            default: return "src" + source;
        }
    }
    public AudioDeviceInfo routedDevice() { return rec == null ? null : rec.getRoutedDevice(); }

    // うまくいけば null、だめなら理由を返す
    public String start(AudioDeviceInfo dev, int wantSr) {
        int[] sources = {MediaRecorder.AudioSource.UNPROCESSED, MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC};
        int[] encs = {AudioFormat.ENCODING_PCM_FLOAT, AudioFormat.ENCODING_PCM_16BIT};
        String last = "?";
        // ステレオを優先して、だめなら条件を下げていく
        for (int chMask : new int[]{AudioFormat.CHANNEL_IN_STEREO, AudioFormat.CHANNEL_IN_MONO}) {
            for (int src : sources) {
                for (int enc : encs) {
                    try {
                        int min = AudioRecord.getMinBufferSize(wantSr, chMask, enc);
                        if (min <= 0) { last = "minBuffer " + min; continue; }
                        AudioRecord r = new AudioRecord.Builder()
                                .setAudioSource(src)
                                .setAudioFormat(new AudioFormat.Builder()
                                        .setSampleRate(wantSr).setChannelMask(chMask).setEncoding(enc).build())
                                .setBufferSizeInBytes(min * 2)
                                .build();
                        if (r.getState() != AudioRecord.STATE_INITIALIZED) { r.release(); last = "init"; continue; }
                        if (dev != null) r.setPreferredDevice(dev);
                        r.startRecording();
                        if (r.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) { r.release(); last = "start"; continue; }
                        rec = r;
                        source = src;
                        isFloat = enc == AudioFormat.ENCODING_PCM_FLOAT;
                        ch = r.getChannelCount();
                        sr = r.getSampleRate();
                        bufFrames = r.getBufferSizeInFrames();
                        running = true;
                        thread = new Thread(this::loop, "audio");
                        thread.setPriority(Thread.MAX_PRIORITY);
                        thread.start();
                        return null;
                    } catch (Exception e) {
                        last = e.getClass().getSimpleName() + " " + e.getMessage();
                    }
                }
            }
        }
        return last;
    }

    public void stop() {
        running = false;
        try { if (thread != null) thread.join(500); } catch (InterruptedException ignored) {}
        if (rec != null) { try { rec.stop(); } catch (Exception ignored) {} rec.release(); rec = null; }
    }

    private void loop() {
        final int chunk = Math.max(64, sr / 400);          // 1回に読む量 (約2.5ms)
        final float[] fb = new float[chunk * ch];
        final short[] sb = new short[chunk * ch];
        final int blk = Math.max(1, Math.round(sr / 1000f));   // 1ms ごとに針を進める
        final double h = (double) blk / sr, k1 = WN * WN, k2 = 2 * ZETA * WN;
        final double fall = Math.pow(10, -20.0 / 20 * h / 1.5);  // ピークは1.5秒で20dB下がる
        double yL = 0, vL = 0, yR = 0, vR = 0, sL = 0, sR = 0, mL = 0, mR = 0, pL = 0, pR = 0;
        int n = 0, reads = 0;
        long total = 0;
        AudioTimestamp ts = new AudioTimestamp();

        while (running) {
            int got = isFloat ? rec.read(fb, 0, fb.length, AudioRecord.READ_BLOCKING)
                              : rec.read(sb, 0, sb.length);
            if (got <= 0) { if (got < 0) overruns++; continue; }
            int frames = got / ch;
            final double g = gain;
            for (int i = 0; i < frames; i++) {
                double l, r;
                if (isFloat) { l = fb[i * ch]; r = ch > 1 ? fb[i * ch + 1] : l; }
                else { l = sb[i * ch] / 32768.0; r = ch > 1 ? sb[i * ch + 1] / 32768.0 : l; }
                l *= g; r *= g;
                double al = Math.abs(l), ar = Math.abs(r);
                sL += al; sR += ar;
                if (al > mL) mL = al;
                if (ar > mR) mR = ar;
                if (++n >= blk) {
                    vL += (k1 * (sL / n - yL) - k2 * vL) * h; yL += vL * h;
                    vR += (k1 * (sR / n - yR) - k2 * vR) * h; yR += vR * h;
                    pL = Math.max(mL, pL * fall); pR = Math.max(mR, pR * fall);
                    sL = sR = mL = mR = 0; n = 0;
                }
            }
            vuL = Math.max(0, yL); vuR = Math.max(0, yR); pkL = pL; pkR = pR;
            total += frames;

            // 入力の遅れの推定: 最後に受け取った音が、何ミリ秒前に入ってきたものか
            if (++reads % 20 == 0 && rec.getTimestamp(ts, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS) {
                long capNs = ts.nanoTime + (long) ((total - ts.framePosition) * 1e9 / sr);
                latencyMs = (System.nanoTime() - capNs) / 1e6;
            }
        }
    }
}
