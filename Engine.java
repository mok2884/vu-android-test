package jp.mok.vutest;

import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.MediaRecorder;
import android.util.Base64;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

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
    public volatile double gainL = 1, gainR = 1;  // L / R それぞれのレベル (倍率)。VUの計算に使う
    public volatile boolean mono = false;         // true = 1ch目を L と R の両方に使う (HTML版の LL)
    // 多chのとき、L と R に使う ch (0 から数える)。と、全chのピーク (0〜1、ゆっくり下がる)
    public volatile int selA = 0, selB = 1;
    public final float[] chPk = new float[64];

    // HTML側に渡す音 (加工前の L, R を交互に並べたもの)。約2.7秒ぶん持てる
    private static final int RING = 1 << 18, MASK = RING - 1;
    private final float[] ring = new float[RING];
    private volatile long wpos = 0;
    private long rpos = 0;
    private boolean primed = false;

    // HTML側が frames 個(1chあたり)まで受け取る。たまりすぎていたら古いぶんを捨てて、遅れが増えないようにする
    public synchronized String pull(int frames) {
        long w = wpos;
        long avail = (w - rpos) / 2;
        // 最初は少しためてから渡し始める (足りなくなりにくくするため)
        if (!primed) { if (avail < frames * 2L) return ""; primed = true; }
        if (avail > frames * 4L) { rpos = w - frames * 2L * 2; avail = frames * 2L; }
        int n = (int) Math.min(frames, avail);
        if (n <= 0) return "";
        ByteBuffer bb = ByteBuffer.allocate(n * 8).order(ByteOrder.LITTLE_ENDIAN);
        for (int i = 0; i < n * 2; i++) bb.putFloat(ring[(int) ((rpos + i) & MASK)]);
        rpos += n * 2L;
        return Base64.encodeToString(bb.array(), Base64.NO_WRAP);
    }
    public volatile double lookMs = 0;            // 先読み (ミリ秒)。0なら今の針の位置をそのまま出す

    // 先読み用: 針の位置・速さと、直近20msの入力の平均
    private volatile double sYL, sVL, sUL, sYR, sVR, sUR;

    // 「入力がこのまま続いたら、ms ミリ秒後に針はどこにいるか」を針の式で計算する
    private static double ahead(double y, double v, double u, double ms) {
        final double h = 0.001, k1 = WN * WN, k2 = 2 * ZETA * WN;
        int steps = (int) Math.round(ms);
        for (int i = 0; i < steps; i++) { v += (k1 * (u - y) - k2 * v) * h; y += v * h; }
        return Math.max(0, y);
    }
    public double needleL() { double m = lookMs; return m <= 0 ? vuL : ahead(sYL, sVL, sUL, m); }
    public double needleR() { double m = lookMs; return m <= 0 ? vuR : ahead(sYR, sVR, sUR, m); }

    private AudioRecord rec;
    private Thread thread;
    private volatile boolean running;
    private boolean isFloat;
    private int ch, sr, bufFrames, source;
    private boolean nat;          // true = 低遅延モード (AAudio)
    private int natBurst, natShare, natPerf, natDev;

    public boolean isNative() { return nat; }
    public int nativeDeviceId() { return natDev; }
    public String nativeInfo() {
        return (natShare == 0 ? "専有" : "共有") + " / " + (natPerf == 12 ? "低遅延" : "通常(" + natPerf + ")") +
               " / 1回 " + String.format("%.1f", natBurst * 1000.0 / sr) + "ms";
    }

    // 低遅延モードで開始。うまくいけば null
    public String startNative(AudioDeviceInfo dev, int wantSr) {
        if (!NativeAudio.OK) return "この機種では使えません (Android 8 以上が必要)";
        int id = dev == null ? 0 : dev.getId();
        int r = -1;
        // 専有→共有、ステレオ→モノラルの順に試す
        outer:
        for (boolean ex : new boolean[]{true, false}) {
            for (int c : new int[]{2, 1}) {
                r = NativeAudio.open(id, wantSr, c, MediaRecorder.AudioSource.UNPROCESSED, ex);
                if (r == 0) break outer;
                r = NativeAudio.open(id, wantSr, c, MediaRecorder.AudioSource.VOICE_RECOGNITION, ex);
                if (r == 0) break outer;
            }
        }
        if (r != 0) return "AAudio エラー " + r;
        finishNative();
        return null;
    }

    // 多chで開く (DM3 など)。まず低遅延モード、だめなら標準。うまくいけば null
    public String startMulti(AudioDeviceInfo dev, int wantSr, int n) {
        String why = "";
        if (NativeAudio.OK) {
            int id = dev == null ? 0 : dev.getId();
            for (boolean ex : new boolean[]{true, false}) {
                for (int pre : new int[]{MediaRecorder.AudioSource.UNPROCESSED, MediaRecorder.AudioSource.VOICE_RECOGNITION}) {
                    int r = NativeAudio.open(id, wantSr, n, pre, ex);
                    if (r == 0) { finishNative(); return null; }
                    why = "AAudio " + r;
                }
            }
        }
        // 標準: ch を番号で指定して開く
        int mask = n >= 31 ? 0x7fffffff : (1 << n) - 1;
        for (int src : new int[]{MediaRecorder.AudioSource.UNPROCESSED, MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC}) {
            for (int enc : new int[]{AudioFormat.ENCODING_PCM_FLOAT, AudioFormat.ENCODING_PCM_16BIT}) {
                try {
                    int bytes = (enc == AudioFormat.ENCODING_PCM_FLOAT ? 4 : 2) * n * Math.max(1024, wantSr / 20);
                    AudioRecord r = new AudioRecord.Builder()
                            .setAudioSource(src)
                            .setAudioFormat(new AudioFormat.Builder()
                                    .setSampleRate(wantSr).setChannelIndexMask(mask).setEncoding(enc).build())
                            .setBufferSizeInBytes(bytes)
                            .build();
                    if (r.getState() != AudioRecord.STATE_INITIALIZED) { r.release(); why += " / 標準 init"; continue; }
                    if (dev != null) r.setPreferredDevice(dev);
                    r.startRecording();
                    if (r.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) { r.release(); why += " / 標準 start"; continue; }
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
                    why += " / 標準 " + e.getClass().getSimpleName();
                }
            }
        }
        return why;
    }

    private void finishNative() {
        nat = true;
        isFloat = true;
        ch = NativeAudio.info(0);
        sr = NativeAudio.info(1);
        natShare = NativeAudio.info(2);
        natPerf = NativeAudio.info(3);
        natBurst = Math.max(16, NativeAudio.info(4));
        bufFrames = NativeAudio.info(5);
        natDev = NativeAudio.info(6);
        source = -1;
        running = true;
        thread = new Thread(this::loop, "audio");
        thread.setPriority(Thread.MAX_PRIORITY);
        thread.start();
    }

    public int channels() { return ch; }
    public int sampleRate() { return sr; }
    public String formatName() { return isFloat ? "float" : "16bit"; }
    public double bufferMs() { return bufFrames * 1000.0 / sr; }
    public String sourceName() {
        switch (source) {
            case MediaRecorder.AudioSource.UNPROCESSED: return "加工なし";
            case MediaRecorder.AudioSource.VOICE_RECOGNITION: return "音声認識用";
            case MediaRecorder.AudioSource.MIC: return "通常マイク";
            case -1: return "低遅延モード";
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
        if (nat) { NativeAudio.close(); nat = false; }
    }

    private void loop() {
        final int chunk = nat ? natBurst : Math.max(64, sr / 400);   // 1回に読む量
        final float[] fb = new float[chunk * ch];
        final short[] sb = new short[chunk * ch];
        final int blk = Math.max(1, Math.round(sr / 1000f));   // 1ms ごとに針を進める
        final double h = (double) blk / sr, k1 = WN * WN, k2 = 2 * ZETA * WN;
        final double fall = Math.pow(10, -20.0 / 20 * h / 1.5);  // ピークは1.5秒で20dB下がる
        double yL = 0, vL = 0, yR = 0, vR = 0, sL = 0, sR = 0, mL = 0, mR = 0, pL = 0, pR = 0;
        int n = 0, reads = 0;
        final int AV = 20;                                 // 先読み用の入力の平均 (20ms)
        final double[] rbL = new double[AV], rbR = new double[AV];
        double raL = 0, raR = 0;
        int rp = 0;
        long total = 0;
        AudioTimestamp ts = new AudioTimestamp();
        long[] nts = new long[2];

        while (running) {
            int frames;
            if (nat) {
                frames = NativeAudio.read(fb, chunk);
                if (frames < 0) { overruns++; continue; }
                if (frames == 0) continue;
            } else {
                int got = isFloat ? rec.read(fb, 0, fb.length, AudioRecord.READ_BLOCKING)
                                  : rec.read(sb, 0, sb.length);
                if (got <= 0) { if (got < 0) overruns++; continue; }
                frames = got / ch;
            }
            final double gl = gainL, gr = gainR;
            final boolean mo = mono;
            // L と R に使う ch。2ch 以下ならいつもどおり 1ch目と2ch目
            final int ia = ch > 2 ? Math.max(0, Math.min(ch - 1, selA)) : 0;
            final int ib = ch > 2 ? Math.max(0, Math.min(ch - 1, selB)) : (ch > 1 ? 1 : -1);
            if (ch > 2) {
                // 全chのピーク (一覧表示用)
                final double dk = Math.pow(10, -20.0 / 20 * frames / (double) sr / 1.5);
                for (int c = 0; c < ch && c < chPk.length; c++) {
                    double m = 0;
                    for (int i = 0; i < frames; i++) {
                        double v = isFloat ? fb[i * ch + c] : sb[i * ch + c] / 32768.0;
                        if (v < 0) v = -v;
                        if (v > m) m = v;
                    }
                    chPk[c] = (float) Math.max(m, chPk[c] * dk);
                }
            }
            long w = wpos;
            for (int i = 0; i < frames; i++) {
                double l, r;
                if (isFloat) { l = fb[i * ch + ia]; r = ib >= 0 ? fb[i * ch + ib] : 0; }
                else { l = sb[i * ch + ia] / 32768.0; r = ib >= 0 ? sb[i * ch + ib] / 32768.0 : 0; }
                ring[(int) (w & MASK)] = (float) l; ring[(int) ((w + 1) & MASK)] = (float) r; w += 2;
                if (mo) r = l;                                     // LL: 1ch目を R にも
                l *= gl; r *= gr;
                double al = Math.abs(l), ar = Math.abs(r);
                sL += al; sR += ar;
                if (al > mL) mL = al;
                if (ar > mR) mR = ar;
                if (++n >= blk) {
                    vL += (k1 * (sL / n - yL) - k2 * vL) * h; yL += vL * h;
                    vR += (k1 * (sR / n - yR) - k2 * vR) * h; yR += vR * h;
                    raL += sL / n - rbL[rp]; rbL[rp] = sL / n;
                    raR += sR / n - rbR[rp]; rbR[rp] = sR / n;
                    if (++rp >= AV) rp = 0;
                    pL = Math.max(mL, pL * fall); pR = Math.max(mR, pR * fall);
                    sL = sR = mL = mR = 0; n = 0;
                }
            }
            wpos = w;
            vuL = Math.max(0, yL); vuR = Math.max(0, yR); pkL = pL; pkR = pR;
            sYL = yL; sVL = vL; sUL = Math.max(0, raL / AV);
            sYR = yR; sVR = vR; sUR = Math.max(0, raR / AV);
            total += frames;

            // 入力の遅れの推定: 最後に受け取った音が、何ミリ秒前に入ってきたものか
            if (++reads % 20 == 0) {
                long pos = 0, tns = 0;
                boolean ok;
                if (nat) { ok = NativeAudio.timestamp(nts); pos = nts[0]; tns = nts[1]; overruns = NativeAudio.info(7); }
                else { ok = rec.getTimestamp(ts, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS; pos = ts.framePosition; tns = ts.nanoTime; }
                if (ok) {
                    long capNs = tns + (long) ((total - pos) * 1e9 / sr);
                    latencyMs = (System.nanoTime() - capNs) / 1e6;
                }
            }
        }
    }
}
