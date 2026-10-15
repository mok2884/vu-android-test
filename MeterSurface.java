package jp.mok.vutest;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Choreographer;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

// 「直接描く」方式: 画面部品の仕組みを通さず、専用の描画係が画面の書き換えごとに直接描く
public class MeterSurface extends SurfaceView implements SurfaceHolder.Callback {

    public interface Source { Engine engine(); }

    private final MeterView painter;      // 絵の描き方は通常方式と同じものを使う
    private final Source src;
    private HandlerThread th;
    private Handler h;
    private volatile boolean alive, active;
    private volatile int w, hgt;
    public volatile double fps = 0;
    private int frames = 0;
    private long fpsT = 0;

    public MeterSurface(Context c, Source src) {
        super(c);
        this.src = src;
        painter = new MeterView(c);
        getHolder().addCallback(this);
    }

    public void setActive(boolean a) { active = a; }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        // 画面の書き換えを、できるだけ速く (対応機種なら120回/秒) にしてほしいと頼む
        if (Build.VERSION.SDK_INT >= 30) {
            try { holder.getSurface().setFrameRate(120f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT); } catch (Exception ignored) {}
        }
        alive = true;
        th = new HandlerThread("draw", android.os.Process.THREAD_PRIORITY_DISPLAY);
        th.start();
        h = new Handler(th.getLooper());
        h.post(() -> Choreographer.getInstance().postFrameCallback(frame));
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int f, int width, int height) { w = width; hgt = height; }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        alive = false;
        if (th != null) { th.quitSafely(); try { th.join(300); } catch (InterruptedException ignored) {} th = null; }
    }

    private final Choreographer.FrameCallback frame = new Choreographer.FrameCallback() {
        @Override public void doFrame(long t) {
            if (!alive) return;
            if (active && w > 0) {
                Engine e = src.engine();
                if (e != null) painter.setValues(e.needleL(), e.needleR(), e.pkL, e.pkR, e.channels());
                else painter.setValues(0, 0, 0, 0, 0);
                Canvas c = null;
                try {
                    c = Build.VERSION.SDK_INT >= 26 ? getHolder().getSurface().lockHardwareCanvas() : getHolder().lockCanvas();
                    if (c != null) {
                        c.drawColor(Color.rgb(17, 17, 17));
                        painter.drawTo(c, w, hgt);
                    }
                } catch (Exception ignored) {
                } finally {
                    if (c != null) {
                        try {
                            if (Build.VERSION.SDK_INT >= 26) getHolder().getSurface().unlockCanvasAndPost(c);
                            else getHolder().unlockCanvasAndPost(c);
                        } catch (Exception ignored) {}
                    }
                }
                frames++;
                if (fpsT == 0) fpsT = t;
                if (t - fpsT >= 1_000_000_000L) { fps = frames * 1e9 / (t - fpsT); frames = 0; fpsT = t; }
            }
            Choreographer.getInstance().postFrameCallback(this);
        }
    };
}
