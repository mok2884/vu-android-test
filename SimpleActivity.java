package jp.mok.vutest;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Choreographer;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.view.Display;
import android.widget.FrameLayout;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

// 確認用アプリ: 選んだ入力機器からステレオで受け取れるか、VUの遅れがどのくらいかを見る
public class SimpleActivity extends Activity {

    private AudioManager am;
    private Spinner devSpinner, srSpinner;
    private Button startBtn;
    private TextView info;
    private MeterView meter;
    private final List<AudioDeviceInfo> devices = new ArrayList<>();
    private final int[] RATES = {48000, 44100, 96000};
    private Engine engine;
    private String infoHead = "";
    private double gainDb = 0;
    private TextView gainText;
    private SeekBar gainBar;
    private boolean lowLat = false;
    private Button modeBtn;
    private MeterSurface surf;
    private boolean direct = false;
    private Button drawBtn;
    private Button multiBtn;
    private Spinner selLSp, selRSp;
    private TextView chList;
    private boolean multi = false;
    private int lookMs = 0;
    private int viewFrames = 0;
    private long viewFpsT = 0;
    private double viewFps = 0;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // 画面の書き換えがいちばん速いモードを選ぶ (同じ解像度の中で)
        try {
            Display dsp = getWindowManager().getDefaultDisplay();
            Display.Mode cur = dsp.getMode(), best = cur;
            for (Display.Mode m : dsp.getSupportedModes()) {
                if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate()) best = m;
            }
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
        } catch (Exception ignored) {}
        am = (AudioManager) getSystemService(AUDIO_SERVICE);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(17, 17, 17));
        int pad = dp(10);
        root.setPadding(pad, pad, pad, pad);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        devSpinner = new Spinner(this);
        row.addView(devSpinner, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        srSpinner = new Spinner(this);
        List<String> rs = new ArrayList<>();
        for (int r : RATES) rs.add((r / 1000.0 == r / 1000 ? String.valueOf(r / 1000) : String.valueOf(r / 1000.0)) + "k");
        ArrayAdapter<String> ra = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, rs);
        ra.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        srSpinner.setAdapter(ra);
        row.addView(srSpinner, new LinearLayout.LayoutParams(dp(90), LinearLayout.LayoutParams.WRAP_CONTENT));

        modeBtn = new Button(this);
        modeBtn.setText("標準");
        modeBtn.setOnClickListener(v -> {
            if (engine != null) return;          // 止めてから切り替える
            lowLat = !lowLat;
            modeBtn.setText(lowLat ? "低遅延" : "標準");
        });
        row.addView(modeBtn, new LinearLayout.LayoutParams(dp(100), LinearLayout.LayoutParams.WRAP_CONTENT));

        startBtn = new Button(this);
        startBtn.setText("START");
        startBtn.setOnClickListener(v -> toggle());
        row.addView(startBtn, new LinearLayout.LayoutParams(dp(110), LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(row);

        // 入力レベル調整: -20〜+20dB、0.5dB刻み
        LinearLayout grow = new LinearLayout(this);
        grow.setOrientation(LinearLayout.HORIZONTAL);
        grow.setGravity(Gravity.CENTER_VERTICAL);
        gainText = new TextView(this);
        gainText.setTextColor(Color.rgb(220, 220, 220));
        gainText.setTextSize(14);
        grow.addView(gainText, new LinearLayout.LayoutParams(dp(130), LinearLayout.LayoutParams.WRAP_CONTENT));
        gainBar = new SeekBar(this);
        gainBar.setMax(80);
        gainBar.setProgress(40);
        gainBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int v, boolean user) { setGain((v - 40) / 2.0); }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {}
        });
        grow.addView(gainBar, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        Button zero = new Button(this);
        zero.setText("0dB");
        zero.setOnClickListener(v -> gainBar.setProgress(40));
        grow.addView(zero, new LinearLayout.LayoutParams(dp(80), LinearLayout.LayoutParams.WRAP_CONTENT));
        drawBtn = new Button(this);
        drawBtn.setText("描画:通常");
        drawBtn.setOnClickListener(v -> setDirect(!direct));
        grow.addView(drawBtn, new LinearLayout.LayoutParams(dp(120), LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(grow);
        setGain(0);

        // 多chテスト (DM3 など): 多chで開いて、L と R に使う ch を選ぶ
        LinearLayout mrow = new LinearLayout(this);
        mrow.setOrientation(LinearLayout.HORIZONTAL);
        mrow.setGravity(Gravity.CENTER_VERTICAL);
        multiBtn = new Button(this);
        multiBtn.setText("多ch START");
        multiBtn.setOnClickListener(v -> toggleMulti());
        mrow.addView(multiBtn, new LinearLayout.LayoutParams(dp(150), LinearLayout.LayoutParams.WRAP_CONTENT));
        TextView tl = new TextView(this); tl.setText("  L=ch"); tl.setTextColor(Color.rgb(220, 220, 220));
        mrow.addView(tl);
        selLSp = new Spinner(this);
        mrow.addView(selLSp, new LinearLayout.LayoutParams(dp(80), LinearLayout.LayoutParams.WRAP_CONTENT));
        TextView tr = new TextView(this); tr.setText("  R=ch"); tr.setTextColor(Color.rgb(220, 220, 220));
        mrow.addView(tr);
        selRSp = new Spinner(this);
        mrow.addView(selRSp, new LinearLayout.LayoutParams(dp(80), LinearLayout.LayoutParams.WRAP_CONTENT));
        List<String> chs = new ArrayList<>();
        for (int k = 1; k <= 32; k++) chs.add(String.valueOf(k));
        ArrayAdapter<String> ca = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, chs);
        ca.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        selLSp.setAdapter(ca); selRSp.setAdapter(ca);
        selLSp.setSelection(0); selRSp.setSelection(1);
        AdapterView.OnItemSelectedListener selL = new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                Engine e = engine; if (e != null) { e.selA = selLSp.getSelectedItemPosition(); e.selB = selRSp.getSelectedItemPosition(); }
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        };
        selLSp.setOnItemSelectedListener(selL); selRSp.setOnItemSelectedListener(selL);
        root.addView(mrow);
        chList = new TextView(this);
        chList.setTextColor(Color.rgb(150, 220, 150));
        chList.setTextSize(11);
        chList.setTypeface(android.graphics.Typeface.MONOSPACE);
        root.addView(chList);

        info = new TextView(this);
        info.setTextColor(Color.rgb(170, 170, 170));
        info.setTextSize(12);
        info.setPadding(0, dp(6), 0, dp(6));
        root.addView(info);

        FrameLayout box = new FrameLayout(this);
        meter = new MeterView(this);
        surf = new MeterSurface(this, () -> engine);
        box.addView(surf, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        box.addView(meter, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        root.addView(box, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);

        devSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { if (engine == null) showDeviceInfo(); }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });

        am.registerAudioDeviceCallback(new AudioDeviceCallback() {
            @Override public void onAudioDevicesAdded(AudioDeviceInfo[] a) { refreshDevices(); }
            @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] r) { refreshDevices(); }
        }, new Handler(Looper.getMainLooper()));
        refreshDevices();

        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 1);
        }

        Choreographer.getInstance().postFrameCallback(frame);
    }

    // 描き方の切り替え: 通常(画面部品) / 直接(専用の描画係)
    private void setDirect(boolean d) {
        direct = d;
        drawBtn.setText(d ? "描画:直接" : "描画:通常");
        meter.setVisibility(d ? View.INVISIBLE : View.VISIBLE);
        surf.setActive(d);
    }

    private void setGain(double db) {
        gainDb = db;
        gainText.setText("IN " + (db >= 0 ? "+" : "") + String.format("%.1f", db) + " dB");
        if (engine != null) { engine.gainL = engine.gainR = Math.pow(10, db / 20); }
    }

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private static String typeName(int t) {
        switch (t) {
            case AudioDeviceInfo.TYPE_BUILTIN_MIC: return "本体マイク";
            case AudioDeviceInfo.TYPE_USB_DEVICE: return "USB";
            case AudioDeviceInfo.TYPE_USB_HEADSET: return "USBヘッドセット";
            case AudioDeviceInfo.TYPE_USB_ACCESSORY: return "USBアクセサリ";
            case AudioDeviceInfo.TYPE_WIRED_HEADSET: return "有線ヘッドセット";
            case AudioDeviceInfo.TYPE_BLUETOOTH_SCO: return "Bluetooth";
            case AudioDeviceInfo.TYPE_TELEPHONY: return "電話";
            default: return "type" + t;
        }
    }

    private static String join(int[] a) {
        if (a == null || a.length == 0) return "?";
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < a.length; i++) { if (i > 0) s.append(","); s.append(a[i]); }
        return s.toString();
    }

    private void refreshDevices() {
        AudioDeviceInfo keep = null;
        int sel = devSpinner.getSelectedItemPosition();
        if (sel >= 0 && sel < devices.size()) keep = devices.get(sel);
        devices.clear();
        List<String> names = new ArrayList<>();
        int usbIndex = -1, keepIndex = -1;
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            int t = d.getType();
            if (t == AudioDeviceInfo.TYPE_TELEPHONY || t == AudioDeviceInfo.TYPE_FM_TUNER) continue;
            if (keep != null && d.getId() == keep.getId()) keepIndex = devices.size();
            if (usbIndex < 0 && (t == AudioDeviceInfo.TYPE_USB_DEVICE || t == AudioDeviceInfo.TYPE_USB_HEADSET)) usbIndex = devices.size();
            devices.add(d);
            names.add(typeName(t) + " " + d.getProductName());
        }
        ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, names);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        devSpinner.setAdapter(a);
        if (keepIndex >= 0) devSpinner.setSelection(keepIndex);
        else if (usbIndex >= 0) devSpinner.setSelection(usbIndex);   // USB機器があれば最初から選んでおく
        if (engine == null) showDeviceInfo();
    }

    private AudioDeviceInfo selectedDevice() {
        int i = devSpinner.getSelectedItemPosition();
        return (i >= 0 && i < devices.size()) ? devices.get(i) : null;
    }

    // 機器が「対応している」と言っている ch 数と周波数
    private void showDeviceInfo() {
        AudioDeviceInfo d = selectedDevice();
        if (d == null) { info.setText("入力機器が見つかりません"); return; }
        info.setText("機器の申告: ch " + join(d.getChannelCounts()) + " / Hz " + join(d.getSampleRates()) +
                "\nSTARTを押すと、実際に受け取れた内容がここに出ます");
    }

    // 多chで開く。機器が申告しているいちばん多いch数 (2以下なら18) で試す
    private void toggleMulti() {
        if (engine != null) {
            engine.stop(); engine = null;
            startBtn.setText("START"); multiBtn.setText("多ch START"); chList.setText("");
            showDeviceInfo();
            return;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 1);
            return;
        }
        AudioDeviceInfo dev = selectedDevice();
        int rep = 0;
        if (dev != null) for (int c : dev.getChannelCounts()) rep = Math.max(rep, c);
        int n = rep > 2 ? rep : 18;
        int sr = RATES[Math.max(0, srSpinner.getSelectedItemPosition())];
        Engine e = new Engine();
        e.selA = selLSp.getSelectedItemPosition(); e.selB = selRSp.getSelectedItemPosition();
        e.gainL = e.gainR = Math.pow(10, gainDb / 20);
        String err = e.startMulti(dev, sr, n);
        if (err != null) { info.setText("多chで開けませんでした (申告 " + rep + "ch / " + n + "chで試した): " + err); return; }
        engine = e; multi = true;
        multiBtn.setText("多ch STOP"); startBtn.setText("STOP");
        infoHead = "多ch: 申告 " + rep + "ch / " + n + "chで頼んだ → 受け取り " + e.channels() + "ch / " + e.sampleRate() + "Hz / " + e.sourceName() +
                (e.isNative() ? "\n" + e.nativeInfo() : "");
        info.setText(infoHead);
    }

    private void toggle() {
        if (engine != null) {
            engine.stop();
            engine = null;
            startBtn.setText("START"); multiBtn.setText("多ch START"); chList.setText("");
            showDeviceInfo();
            return;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 1);
            return;
        }
        int sr = RATES[Math.max(0, srSpinner.getSelectedItemPosition())];
        Engine e = new Engine();
        String err = lowLat ? e.startNative(selectedDevice(), sr) : e.start(selectedDevice(), sr);
        if (err != null) { info.setText("開始できませんでした: " + err); return; }
        e.gainL = e.gainR = Math.pow(10, gainDb / 20);
        if (e.channels() == 1) e.mono = true;      // 1chのときは R にも同じものを出す
        engine = e;
        multi = false;
        startBtn.setText("STOP");
        AudioDeviceInfo routed = e.routedDevice();
        if (e.isNative()) {
            routed = null;
            for (AudioDeviceInfo dd : devices) if (dd.getId() == e.nativeDeviceId()) routed = dd;
        }
        infoHead = "受け取り: " + e.channels() + "ch / " + e.sampleRate() + "Hz / " + e.formatName() + " / " + e.sourceName() +
                "\n実際の入力: " + (routed == null ? "?" : typeName(routed.getType()) + " " + routed.getProductName()) +
                " / バッファ " + String.format("%.1f", e.bufferMs()) + "ms" +
                (e.isNative() ? "\n" + e.nativeInfo() : "");
        info.setText(infoHead);
    }

    private long lastInfo = 0;
    private final Choreographer.FrameCallback frame = new Choreographer.FrameCallback() {
        @Override public void doFrame(long t) {
            Engine e = engine;
            if (!direct) {
                if (e != null) meter.set(e.needleL(), e.needleR(), e.pkL, e.pkR, e.channels());
                else meter.set(0, 0, 0, 0, 0);
                viewFrames++;
                if (viewFpsT == 0) viewFpsT = t;
                if (t - viewFpsT >= 1_000_000_000L) { viewFps = viewFrames * 1e9 / (t - viewFpsT); viewFrames = 0; viewFpsT = t; }
            }
            if (e != null && t - lastInfo > 500_000_000L) {
                lastInfo = t;
                double lat = e.latencyMs;
                float hz = getWindowManager().getDefaultDisplay().getRefreshRate();
                if (multi && e.channels() > 2) {
                    StringBuilder sb2 = new StringBuilder("各chのピーク(dBFS):");
                    for (int c = 0; c < e.channels() && c < e.chPk.length; c++) {
                        double v = e.chPk[c];
                        String d = v > 1e-5 ? String.format("%4.0f", 20 * Math.log10(v)) : "  --";
                        sb2.append(c % 6 == 0 ? "\n" : "  ").append(String.format("%2d:", c + 1)).append(d);
                    }
                    chList.setText(sb2.toString());
                }
                info.setText(infoHead + "\n入力の遅れ(推定) " + (lat > 0 ? String.format("%.1f", lat) + "ms" : "--") +
                        (e.overruns > 0 ? " / 取りこぼし " + e.overruns : "") +
                        "\n画面 " + Math.round(hz) + "Hz / 描画 " + (direct ? "直接 " + Math.round(surf.fps) : "通常 " + Math.round(viewFps)) + "回/秒");
            }
            Choreographer.getInstance().postFrameCallback(this);
        }
    };

    @Override
    protected void onDestroy() {
        if (engine != null) engine.stop();
        engine = null;
        super.onDestroy();
    }
}
