package jp.mok.vutest;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

// L/R の VU 針 (ブラウザ版と同じ目盛りと角度) と、その下にピークの細いバーを描く
public class MeterView extends View {

    // 1kHz -20dBFS (振幅0.1) の絶対値の平均 = 0VU
    private static final double VU_CAL = 20 * Math.log10(1 / (0.1 * 2 / Math.PI));
    private static final double PK_MIN = -60, PK_MAX = 0;

    // ブラウザ版と同じ: 窓 280x145、軸は窓の上から181、左端133度〜右端47度
    private static final float FW = 280, FH = 145, CY = 181, ARC_R = 140, START_DEG = 133, END_DEG = 47;
    private static final double[][] SCALE = {
            {-20, 0.030}, {-10, 0.135}, {-7, 0.222}, {-5, 0.303}, {-3, 0.418}, {-2, 0.489},
            {-1, 0.567}, {0, 0.654}, {1, 0.765}, {2, 0.888}, {3, 1.000}};

    private double vuL, vuR, pkL, pkR;
    private int ch;
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float d;

    public MeterView(Context c) {
        super(c);
        d = c.getResources().getDisplayMetrics().density;
    }

    public void set(double vuL, double vuR, double pkL, double pkR, int ch) {
        setValues(vuL, vuR, pkL, pkR, ch);
        invalidate();
    }

    // 値だけ入れる (直接描く方式から使う)
    public void setValues(double vuL, double vuR, double pkL, double pkR, int ch) {
        this.vuL = vuL; this.vuR = vuR; this.pkL = pkL; this.pkR = pkR; this.ch = ch;
    }

    // 電圧(0VU=1) → 振れ幅(0〜1)。目盛りの間は直線でつなぐ
    private static double frac(double v) {
        if (!(v > 0)) return 0;
        double pa = 0, pf = 0;
        for (double[] m : SCALE) {
            double a = Math.pow(10, m[0] / 20);
            if (v <= a) return pf + (m[1] - pf) * (v - pa) / (a - pa);
            pa = a; pf = m[1];
        }
        return 1;
    }

    private static double angleOf(double amp) {   // amp: 絶対値の平均
        double v = amp * Math.pow(10, VU_CAL / 20);
        return START_DEG + frac(v) * (END_DEG - START_DEG);
    }

    @Override
    protected void onDraw(Canvas c) { c.drawColor(Color.rgb(17, 17, 17)); drawTo(c, getWidth(), getHeight()); }

    public void drawTo(Canvas c, float w, float h) {
        boolean side = w > h * 1.2f;               // 横長なら左右に、縦長なら上下に並べる
        float boxW = side ? w / 2 : w, boxH = side ? h : h / 2;
        float s = Math.min((boxW - 12 * d) / FW, (boxH - 40 * d) / FH);   // 窓の大きさ
        for (int i = 0; i < 2; i++) {
            float ox = side ? i * boxW : 0, oy = side ? 0 : i * boxH;
            float fx = ox + (boxW - FW * s) / 2, fy = oy + 6 * d;
            drawMeter(c, i == 0 ? "L" : "R", i == 0 ? vuL : vuR, i == 0 ? pkL : pkR, fx, fy, s);
        }
        if (ch == 1) {
            p.setColor(Color.rgb(140, 140, 140));
            p.setTextSize(12 * d);
            p.setTextAlign(Paint.Align.LEFT);
            c.drawText("1chで受け取っています (Rは Lと同じものを表示)", 8 * d, h - 4 * d, p);
        }
    }

    private void drawMeter(Canvas c, String name, double vu, double pk, float fx, float fy, float s) {
        float cx = fx + FW / 2 * s, cy = fy + CY * s;

        c.save();
        c.clipRect(fx, fy, fx + FW * s, fy + FH * s);
        p.setStyle(Paint.Style.FILL);
        p.setColor(Color.rgb(238, 226, 190));
        c.drawRect(fx, fy, fx + FW * s, fy + FH * s, p);

        // 目盛りの弧 (0VUより上は赤)
        RectF arc = new RectF(cx - ARC_R * s, cy - ARC_R * s, cx + ARC_R * s, cy + ARC_R * s);
        float a0 = (float) angleOf(Math.pow(10, -VU_CAL / 20));          // 0VU の角度
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(2 * s);
        p.setColor(Color.rgb(30, 30, 30));
        c.drawArc(arc, -START_DEG, START_DEG - a0, false, p);
        p.setColor(Color.rgb(200, 40, 30));
        p.setStrokeWidth(5 * s);
        c.drawArc(arc, -a0, a0 - END_DEG, false, p);

        // 目盛りと数字
        p.setTextAlign(Paint.Align.CENTER);
        p.setTextSize(13 * s);
        for (double[] m : SCALE) {
            double deg = START_DEG + m[1] * (END_DEG - START_DEG), r = Math.toRadians(deg);
            float cs = (float) Math.cos(r), sn = (float) Math.sin(r);
            int col = m[0] >= 0 ? Color.rgb(200, 40, 30) : Color.rgb(30, 30, 30);
            p.setColor(col);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(2.2f * s);
            float len = (m[0] >= 0 ? 13 : 10) * s;
            c.drawLine(cx + cs * ARC_R * s, cy - sn * ARC_R * s,
                       cx + cs * (ARC_R * s + len), cy - sn * (ARC_R * s + len), p);
            p.setStyle(Paint.Style.FILL);
            float tr = ARC_R * s + 26 * s;
            c.drawText(String.valueOf((int) Math.abs(m[0])), cx + cs * tr, cy - sn * tr + 5 * s, p);
        }
        p.setColor(Color.rgb(30, 30, 30));
        p.setTextSize(16 * s);
        p.setTextAlign(Paint.Align.LEFT);
        c.drawText(name, fx + 10 * s, fy + FH * s - 10 * s, p);
        p.setTextAlign(Paint.Align.RIGHT);
        c.drawText("VU", fx + FW * s - 10 * s, fy + FH * s - 10 * s, p);

        // 針
        double r = Math.toRadians(angleOf(vu));
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(2f * s);
        p.setColor(Color.rgb(20, 20, 20));
        float nl = (ARC_R + 10) * s;
        c.drawLine(cx, cy, cx + (float) Math.cos(r) * nl, cy - (float) Math.sin(r) * nl, p);
        c.restore();

        // 窓の枠
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(1 * d);
        p.setColor(Color.rgb(90, 90, 90));
        c.drawRect(fx, fy, fx + FW * s, fy + FH * s, p);

        // ピーク
        p.setStyle(Paint.Style.FILL);
        float pt = fy + FH * s + 8 * d, ph = 6 * d, bw = FW * s - 50 * d;
        p.setColor(Color.rgb(40, 40, 40));
        c.drawRect(fx, pt, fx + bw, pt + ph, p);
        double pdb = pk > 1e-7 ? 20 * Math.log10(pk) : -99;
        float px = fx + (float) ((Math.max(PK_MIN, Math.min(PK_MAX, pdb)) - PK_MIN) / (PK_MAX - PK_MIN)) * bw;
        p.setColor(pdb > -1 ? Color.rgb(230, 70, 60) : Color.rgb(200, 200, 90));
        c.drawRect(fx, pt, px, pt + ph, p);
        p.setColor(Color.rgb(170, 170, 170));
        p.setTextSize(11 * d);
        p.setTextAlign(Paint.Align.RIGHT);
        c.drawText(pdb < -90 ? "PK --" : String.format("PK %.1f", pdb), fx + FW * s, pt + ph + 1 * d, p);
    }
}
