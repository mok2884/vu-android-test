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
public class MainActivity extends Activity {

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

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
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
        root.addView(grow);
        setGain(0);

        info = new TextView(this);
        info.setTextColor(Color.rgb(170, 170, 170));
        info.setTextSize(12);
        info.setPadding(0, dp(6), 0, dp(6));
        root.addView(info);

        meter = new MeterView(this);
        root.addView(meter, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

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

    private void setGain(double db) {
        gainDb = db;
        gainText.setText("IN " + (db >= 0 ? "+" : "") + String.format("%.1f", db) + " dB");
        if (engine != null) engine.gain = Math.pow(10, db / 20);
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

    private void toggle() {
        if (engine != null) {
            engine.stop();
            engine = null;
            startBtn.setText("START");
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
        e.gain = Math.pow(10, gainDb / 20);
        engine = e;
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
            if (e != null) {
                meter.set(e.vuL, e.vuR, e.pkL, e.pkR, e.channels());
                if (t - lastInfo > 500_000_000L) {
                    lastInfo = t;
                    double lat = e.latencyMs;
                    info.setText(infoHead + "\n入力の遅れ(推定) " + (lat > 0 ? String.format("%.1f", lat) + "ms" : "--") +
                            (e.overruns > 0 ? " / 取りこぼし " + e.overruns : ""));
                }
            } else {
                meter.set(0, 0, 0, 0, 0);
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
