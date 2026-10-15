package jp.mok.vutest;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;

// L/R の VU バー(太い) とピークバー(細い) を描く
public class MeterView extends View {

    // 1kHz -20dBFS (振幅0.1) の絶対値の平均 = 0VU
    private static final double VU_CAL = 20 * Math.log10(1 / (0.1 * 2 / Math.PI));
    private static final double VU_MIN = -20, VU_MAX = 3, PK_MIN = -60, PK_MAX = 0;

    private double vuL, vuR, pkL, pkR;
    private int ch;
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float d;

    public MeterView(Context c) {
        super(c);
        d = c.getResources().getDisplayMetrics().density;
    }

    public void set(double vuL, double vuR, double pkL, double pkR, int ch) {
        this.vuL = vuL; this.vuR = vuR; this.pkL = pkL; this.pkR = pkR; this.ch = ch;
        invalidate();
    }

    private static double vuDb(double a) { return a > 1e-7 ? 20 * Math.log10(a) + VU_CAL : -99; }
    private static double pkDb(double a) { return a > 1e-7 ? 20 * Math.log10(a) : -99; }

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), hgt = getHeight();
        float left = 34 * d, right = w - 74 * d, bw = right - left;
        float rowH = Math.min(hgt / 2.4f, 120 * d);
        p.setTextSize(13 * d);

        drawRow(c, "L", vuL, pkL, 10 * d, rowH, left, bw, w);
        drawRow(c, "R", vuR, pkR, 10 * d + rowH * 1.15f, rowH, left, bw, w);

        p.setColor(Color.rgb(120, 120, 120));
        p.setTextAlign(Paint.Align.LEFT);
        p.setTextSize(12 * d);
        String note = ch == 1 ? "1chで受け取っています (Rは Lと同じものを表示)" :
                      ch >= 2 ? "太いバー: VU (-20〜+3)  細いバー: ピーク dBFS (-60〜0)" : "";
        c.drawText(note, left, 10 * d + rowH * 2.3f, p);
    }

    private void drawRow(Canvas c, String name, double vu, double pk, float top, float rowH,
                         float left, float bw, float w) {
        float vuH = rowH * 0.5f, pkH = rowH * 0.16f, gap = rowH * 0.08f;

        p.setTextAlign(Paint.Align.LEFT);
        p.setColor(Color.rgb(230, 230, 230));
        p.setTextSize(18 * d);
        c.drawText(name, 8 * d, top + vuH * 0.7f, p);

        // VU
        p.setColor(Color.rgb(40, 40, 40));
        c.drawRect(left, top, left + bw, top + vuH, p);
        double db = vuDb(vu);
        float x = left + (float) ((Math.max(VU_MIN, Math.min(VU_MAX, db)) - VU_MIN) / (VU_MAX - VU_MIN)) * bw;
        float x0 = left + (float) ((0 - VU_MIN) / (VU_MAX - VU_MIN)) * bw;
        p.setColor(Color.rgb(60, 200, 90));
        c.drawRect(left, top, Math.min(x, x0), top + vuH, p);
        if (x > x0) { p.setColor(Color.rgb(230, 70, 60)); c.drawRect(x0, top, x, top + vuH, p); }

        // 目盛り
        p.setTextSize(10 * d);
        p.setTextAlign(Paint.Align.CENTER);
        for (int v : new int[]{-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3}) {
            float tx = left + (float) ((v - VU_MIN) / (VU_MAX - VU_MIN)) * bw;
            p.setColor(v == 0 ? Color.WHITE : Color.rgb(140, 140, 140));
            c.drawRect(tx - 0.5f * d, top + vuH, tx + 0.5f * d, top + vuH + 4 * d, p);
            if (v != -2 && v != 2) c.drawText((v > 0 ? "+" : "") + v, tx, top + vuH + 14 * d, p);
        }

        // ピーク
        float pt = top + vuH + 18 * d + gap;
        p.setColor(Color.rgb(40, 40, 40));
        c.drawRect(left, pt, left + bw, pt + pkH, p);
        double pdb = pkDb(pk);
        float px = left + (float) ((Math.max(PK_MIN, Math.min(PK_MAX, pdb)) - PK_MIN) / (PK_MAX - PK_MIN)) * bw;
        p.setColor(pdb > -1 ? Color.rgb(230, 70, 60) : Color.rgb(200, 200, 90));
        c.drawRect(left, pt, px, pt + pkH, p);

        // 数字
        p.setTextAlign(Paint.Align.RIGHT);
        p.setTextSize(16 * d);
        p.setColor(Color.rgb(230, 230, 230));
        c.drawText(db < -90 ? "--" : String.format("%+.1f", db), w - 6 * d, top + vuH * 0.75f, p);
        p.setTextSize(12 * d);
        p.setColor(Color.rgb(160, 160, 160));
        c.drawText(pdb < -90 ? "--" : String.format("%.1f", pdb), w - 6 * d, pt + pkH, p);
    }
}
