package jp.mok.vutest;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Bundle;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;

import org.json.JSONArray;
import org.json.JSONObject;

// HTML版のメーターを画面いっぱいに表示する。音の受け取りとVUの針の計算はアプリでやる
public class MainActivity extends Activity {

    private AudioManager am;
    private WebView web;
    private volatile Engine engine;
    // スライダーと LR/LL は、入力を開き直しても引き継ぐ
    private volatile double gL = 1, gR = 1;
    private volatile boolean mono = false;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);   // 画面を消さない
        am = (AudioManager) getSystemService(AUDIO_SERVICE);

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

        web = new WebView(this);
        web.setBackgroundColor(Color.rgb(5, 5, 5));
        WebSettings ws = web.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setMediaPlaybackRequiresUserGesture(false);
        web.addJavascriptInterface(new Bridge(), "NativeAudio");
        setContentView(web);
        hideBars();

        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, 1);
        }
        web.loadUrl("file:///android_asset/vu_meter.html");
    }

    // 上と下のバーを隠して、メーターを画面いっぱいにする
    private void hideBars() {
        web.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override
    public void onWindowFocusChanged(boolean f) {
        super.onWindowFocusChanged(f);
        if (f) hideBars();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] p, int[] r) {
        web.reload();      // 許可されたら、機器の一覧を作り直す
    }

    @Override
    protected void onDestroy() {
        Engine e = engine;
        engine = null;
        if (e != null) e.stop();
        super.onDestroy();
    }

    private static String typeName(int t) {
        switch (t) {
            case AudioDeviceInfo.TYPE_BUILTIN_MIC: return "本体マイク";
            case AudioDeviceInfo.TYPE_USB_DEVICE: return "USB";
            case AudioDeviceInfo.TYPE_USB_HEADSET: return "USB";
            case AudioDeviceInfo.TYPE_WIRED_HEADSET: return "マイク端子";
            case AudioDeviceInfo.TYPE_BLUETOOTH_SCO: return "Bluetooth";
            default: return "入力";
        }
    }

    // HTML から呼ばれる窓口 (HTML の中では NativeAudio という名前)
    private class Bridge {

        // 入力機器の一覧。USB機器を先頭にする
        @JavascriptInterface
        public String devices() {
            JSONArray usb = new JSONArray(), other = new JSONArray();
            try {
                for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                    int t = d.getType();
                    if (t == AudioDeviceInfo.TYPE_TELEPHONY || t == AudioDeviceInfo.TYPE_FM_TUNER) continue;
                    JSONObject o = new JSONObject();
                    o.put("id", d.getId());
                    o.put("name", typeName(t) + " " + d.getProductName());
                    if (t == AudioDeviceInfo.TYPE_USB_DEVICE || t == AudioDeviceInfo.TYPE_USB_HEADSET) usb.put(o);
                    else other.put(o);
                }
                for (int i = 0; i < other.length(); i++) usb.put(other.get(i));
            } catch (Exception ignored) {}
            return usb.toString();
        }

        // 入力を開く。まず低遅延モード、だめなら標準
        @JavascriptInterface
        public synchronized String start(int deviceId) {
            stopEngine();
            JSONObject o = new JSONObject();
            try {
                if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                    o.put("ok", false); o.put("err", "マイクの許可がありません");
                    return o.toString();
                }
                AudioDeviceInfo dev = null;
                for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) if (d.getId() == deviceId) dev = d;
                Engine e = new Engine();
                e.gainL = gL; e.gainR = gR; e.mono = mono;
                String err = e.startNative(dev, 48000);
                if (err != null) {
                    e = new Engine();
                    e.gainL = gL; e.gainR = gR; e.mono = mono;
                    err = e.start(dev, 48000);
                }
                if (err != null) { o.put("ok", false); o.put("err", err); return o.toString(); }
                engine = e;
                o.put("ok", true);
                o.put("sr", e.sampleRate());
                o.put("ch", e.channels());
                o.put("mode", e.isNative() ? "LOWLAT" : "STD");
            } catch (Exception ex) {
                try { o.put("ok", false); o.put("err", String.valueOf(ex.getMessage())); } catch (Exception ignored) {}
            }
            return o.toString();
        }

        @JavascriptInterface
        public synchronized void stop() { stopEngine(); }

        private void stopEngine() {
            Engine e = engine;
            engine = null;
            if (e != null) e.stop();
        }

        // 音そのもの (L,R を交互に並べた float を base64 にしたもの)
        @JavascriptInterface
        public String pull(int frames) {
            Engine e = engine;
            return e == null ? "" : e.pull(frames);
        }

        // VUの針の位置 (絶対値の平均。0VU = 振幅0.1の正弦波)。"L,R"
        @JavascriptInterface
        public String vu() {
            Engine e = engine;
            return e == null ? "0,0" : e.vuL + "," + e.vuR;
        }

        @JavascriptInterface
        public void setGains(double dbL, double dbR) {
            gL = Math.pow(10, dbL / 20); gR = Math.pow(10, dbR / 20);
            Engine e = engine;
            if (e != null) { e.gainL = gL; e.gainR = gR; }
        }

        @JavascriptInterface
        public void setMono(boolean m) {
            mono = m;
            Engine e = engine;
            if (e != null) e.mono = m;
        }

        // 入力の遅れ(推定、ms)
        @JavascriptInterface
        public double latency() {
            Engine e = engine;
            return e == null ? -1 : e.latencyMs;
        }
    }
}
