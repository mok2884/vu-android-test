package jp.mok.vutest;

import android.os.Build;

// 低遅延モード (AAudio) の入口。Android 8 以上だけ
public class NativeAudio {
    public static final boolean OK;
    static {
        boolean ok = false;
        if (Build.VERSION.SDK_INT >= 26) {
            try { System.loadLibrary("vuaaudio"); ok = true; } catch (Throwable t) { ok = false; }
        }
        OK = ok;
    }
    public static native int open(int deviceId, int sampleRate, int channels, int preset, boolean exclusive);
    public static native int info(int what);
    public static native int read(float[] buf, int frames);
    public static native boolean timestamp(long[] out);
    public static native void close();
}
