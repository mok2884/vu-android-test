// VU METER (Windows 版)
// HTML 版 (vu_meter.html) と同じ画面・同じ計算。VU の針だけは、遅れの小さい描き方 (VU WIN と同じ) で動かす
// 音: WASAPI (専有モード → 低遅延の共有モード → 共有モード)
// 画面: DXGI flip model + tearing (画面の書き換えを待たない) + Direct2D
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#undef DrawText
#include <d2d1.h>
#include <dwrite.h>
#include <mmsystem.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <complex>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef FONT_FAMILY
#define FONT_FAMILY L"Arial"
#endif
template<class T> static void SafeRelease(T*& p){ if(p){ p->Release(); p=nullptr; } }
static const double PI = 3.14159265358979323846;

// ============================================================
//  VU の針 (HTML 版と同じ: 0.3秒で99%、約1.3%行き過ぎ、1kHz -20dBFS = 0VU)
// ============================================================
static const double VU_ZETA = 0.81, VU_RISE = 0.3;
static double VU_WN = 0;
static const double VU_CAL = 20.0 * std::log10(1.0 / (0.1 * 2.0 / PI));
static void initWN(){
    double z = VU_ZETA, wd = std::sqrt(1 - z*z), x = 0;
    for (; x < 50; x += 0.0005) {
        double y = 1 - std::exp(-z*x) * (std::cos(wd*x) + z/wd*std::sin(wd*x));
        if (y >= 0.99) break;
    }
    VU_WN = x / VU_RISE;
}

// ============================================================
//  ラウドネス (ITU-R BS.1770 / HTML 版と同じ計算)
// ============================================================
static const double LOUD_TARGET = -24, LOUD_MIN = -50, LOUD_MAX = -10;
static const int LOUD_BAR_SEC = 3, LOUD_HIST_MIN = 12, LOUD_HIST_BARS = LOUD_HIST_MIN * 60 / LOUD_BAR_SEC;
static const int GATE_BINS = 800;

struct Biquad {
    double b0=0,b1=0,b2=0,a1=0,a2=0,x1=0,x2=0,y1=0,y2=0;
    void set(const double b[3], const double a[2]){ b0=b[0]; b1=b[1]; b2=b[2]; a1=a[0]; a2=a[1]; x1=x2=y1=y2=0; }
    inline double run(double x){ double y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2; x2=x1; x1=x; y2=y1; y1=y; return y; }
};
static void makeKWeight(double rate, double shB[3], double shA[2], double hpB[3], double hpA[2]){
    double K = std::tan(PI * 1681.974450955533 / rate), Q = 0.7071752369554196;
    double Vh = std::pow(10.0, 3.999843853973347 / 20), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1 + K/Q + K*K;
    shB[0] = (Vh + Vb*K/Q + K*K)/a0; shB[1] = 2*(K*K - Vh)/a0; shB[2] = (Vh - Vb*K/Q + K*K)/a0;
    shA[0] = 2*(K*K - 1)/a0; shA[1] = (1 - K/Q + K*K)/a0;
    K = std::tan(PI * 38.13547087602444 / rate); Q = 0.5003270373238773; a0 = 1 + K/Q + K*K;
    hpB[0] = 1; hpB[1] = -2; hpB[2] = 1;
    hpA[0] = 2*(K*K - 1)/a0; hpA[1] = (1 - K/Q + K*K)/a0;
}
static inline double toLkfs(double e){ return e > 0 ? -0.691 + 10*std::log10(e) : -120; }

// トゥルーピーク (4倍に細かくして、点と点の間の山まで見る。HTML 版と同じフィルター)
static const int TP_OS = 4, TP_TAPS = 12, TP_HOLD_BLOCKS = 1000;   // 10ms x 1000 = 10秒
static double TP_PH[TP_OS][TP_TAPS];
static void initTP(){
    const int N = TP_OS * TP_TAPS; const double beta = 5.0, fc = 0.92;
    auto i0 = [](double x){ double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x/(2*k))*(x/(2*k)); s += t; } return s; };
    std::vector<double> h(N);
    for (int n = 0; n < N; n++) {
        double t = (double)(n - N/2) / TP_OS;
        double sinc = t == 0 ? 1 : std::sin(PI*fc*t) / (PI*fc*t);
        double r = (double)(n - N/2) / (N/2);
        double w = std::fabs(r) <= 1 ? i0(beta*std::sqrt(1 - r*r)) / i0(beta) : 0;
        h[n] = fc * sinc * w;
    }
    for (int p = 0; p < TP_OS; p++) {
        double s = 0;
        for (int j = 0; j < TP_TAPS; j++) { TP_PH[p][j] = h[p + TP_OS*j]; s += TP_PH[p][j]; }
        for (int j = 0; j < TP_TAPS; j++) TP_PH[p][j] /= s;
    }
}
struct TpState {
    double buf[TP_TAPS*2] = {}; int pos = 0;
    inline double run(double v){
        buf[pos] = v; buf[pos + TP_TAPS] = v; pos = (pos + 1) % TP_TAPS;
        double peak = 0;
        for (int p = 0; p < TP_OS; p++) {
            double acc = 0; const double* c = TP_PH[p];
            for (int j = 0; j < TP_TAPS; j++) acc += c[j] * buf[pos + TP_TAPS - 1 - j];
            double a = acc < 0 ? -acc : acc; if (a > peak) peak = a;
        }
        return peak;
    }
};

// 画面と音の処理で共有するラウドネスの状態 (gLoudCs で守る)
struct LoudState {
    double ring[300] = {}; int ringN = 0, ringPos = 0;   // 10msごとのパワー (直近3秒)
    double M = -120, S = -120;
    bool hasI = false; double I = 0;
    bool run = false; int runBlocks = 0;
    int tick = 0, histCnt = 0;
    std::vector<float> hist;                             // 3秒ごとに確定したバー
    double gateCnt[GATE_BINS] = {}, gateSum[GATE_BINS] = {};
    double tpRecent[TP_HOLD_BLOCKS] = {}; int tpPos = 0; // 10msごとの最大値 (直近10秒)
    double tpMax = 0;
};
static LoudState gL;
static CRITICAL_SECTION gLoudCs;

static void loudResetLocked(){
    std::memset(gL.gateCnt, 0, sizeof(gL.gateCnt)); std::memset(gL.gateSum, 0, sizeof(gL.gateSum));
    gL.hasI = false; gL.runBlocks = 0; gL.tpMax = 0;
}
static void loudAddBlock(double e){
    double l = toLkfs(e);
    if (l <= -70) return;
    int idx = std::min(GATE_BINS - 1, (int)std::floor((l + 70) * 10));
    gL.gateCnt[idx]++; gL.gateSum[idx] += e;
    double n = 0, s = 0;
    for (int i = 0; i < GATE_BINS; i++) { n += gL.gateCnt[i]; s += gL.gateSum[i]; }
    if (!n) return;
    double rel = toLkfs(s / n) - 10;
    int from = std::max(0, (int)std::ceil((rel + 70) * 10 - 1e-9));
    n = 0; s = 0;
    for (int i = from; i < GATE_BINS; i++) { n += gL.gateCnt[i]; s += gL.gateSum[i]; }
    if (n) { gL.I = toLkfs(s / n); gL.hasI = true; }
}
// 10msぶんのパワーとトゥルーピークを受け取る
static void loudPush(double e, double tp){
    EnterCriticalSection(&gLoudCs);
    gL.ring[gL.ringPos] = e; gL.ringPos = (gL.ringPos + 1) % 300; if (gL.ringN < 300) gL.ringN++;
    double s40 = 0, s300 = 0;
    for (int k = 0; k < gL.ringN; k++) {
        double v = gL.ring[(gL.ringPos - 1 - k + 300) % 300];
        s300 += v; if (k < 40) s40 += v;
    }
    int n40 = std::min(40, gL.ringN);
    gL.M = toLkfs(s40 / n40); gL.S = toLkfs(s300 / gL.ringN);
    gL.tpRecent[gL.tpPos] = tp; gL.tpPos = (gL.tpPos + 1) % TP_HOLD_BLOCKS;
    if (gL.run && tp > gL.tpMax) gL.tpMax = tp;
    if (++gL.tick >= 10) {
        gL.tick = 0;
        if (++gL.histCnt >= LOUD_BAR_SEC * 10) {
            gL.histCnt = 0;
            gL.hist.push_back((float)gL.S);
            if ((int)gL.hist.size() > LOUD_HIST_BARS - 1) gL.hist.erase(gL.hist.begin());
        }
        if (gL.run && ++gL.runBlocks >= 4) loudAddBlock(s40 / n40);
    }
    LeaveCriticalSection(&gLoudCs);
}

// ============================================================
//  共有する値
// ============================================================
static std::atomic<float> gVuL{0}, gVuR{0};
static std::atomic<double> gGainL{1.0}, gGainR{1.0};
static std::atomic<bool> gMono{false};
static std::atomic<int> gLrShift{0};
static std::atomic<bool> gRun{false};
static std::atomic<int> gSr{48000};
static std::wstring gStatus = L"";
static CRITICAL_SECTION gStatusCs;
static void setStatus(const std::wstring& s){ EnterCriticalSection(&gStatusCs); gStatus = s; LeaveCriticalSection(&gStatusCs); }

// 処理したあとの音 (スコープと RTA 用)。音の側が書き、画面の側が新しいほうから読む
static const int RING = 1 << 17;
static float gRingL[RING], gRingR[RING];
static std::atomic<unsigned long long> gRingW{0};

// ============================================================
//  音の入力
// ============================================================
struct DevInfo { std::wstring id, name; };
static std::vector<DevInfo> gDevs;
static int gDevIndex = 0;
static bool gExclusive = true;
static bool gPower = false;
static std::thread gAudioThread;
static const PROPERTYKEY PK_FriendlyName = { {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14 };

static void listDevices(){
    std::wstring cur = (gDevIndex >= 0 && gDevIndex < (int)gDevs.size()) ? gDevs[gDevIndex].id : L"";
    gDevs.clear();
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        std::wstring defId;
        IMMDevice* def = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &def))) { LPWSTR s=nullptr; def->GetId(&s); if(s){defId=s; CoTaskMemFree(s);} def->Release(); }
        for (UINT i = 0; i < n; i++) {
            IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d))) continue;
            DevInfo di; LPWSTR s=nullptr; d->GetId(&s); if (s) { di.id = s; CoTaskMemFree(s); }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PK_FriendlyName, &v)) && v.vt == VT_LPWSTR) di.name = v.pwszVal;
                PropVariantClear(&v); ps->Release();
            }
            if (di.id == defId) gDevs.insert(gDevs.begin(), di); else gDevs.push_back(di);
            d->Release();
        }
        col->Release();
    }
    en->Release();
    gDevIndex = 0;
    for (int i = 0; i < (int)gDevs.size(); i++) if (gDevs[i].id == cur) gDevIndex = i;
}

static inline double readSample(const BYTE* p, bool isFloat, int bits){
    if (isFloat) return bits == 64 ? *(const double*)p : *(const float*)p;
    if (bits == 16) return *(const short*)p / 32768.0;
    if (bits == 24) { int v = (p[0] | (p[1]<<8) | (p[2]<<16)); if (v & 0x800000) v |= ~0xffffff; return v / 8388608.0; }
    if (bits == 32) return *(const int*)p / 2147483648.0;
    return 0;
}

static void audioMain(std::wstring devId, bool exclusive){
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioCaptureClient* cc = nullptr;
    WAVEFORMATEX* fmt = nullptr; WAVEFORMATEXTENSIBLE exf{}; HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::wstring mode = L"?"; double periodMs = 0;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (SUCCEEDED(hr)) hr = en->GetDevice(devId.c_str(), &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&fmt);
    bool ok = false;
    if (SUCCEEDED(hr) && exclusive) {
        REFERENCE_TIME defP = 0, minP = 0; ac->GetDevicePeriod(&defP, &minP);
        int tries[][2] = { {32,24}, {24,24}, {16,16} };
        for (auto& t : tries) {
            ZeroMemory(&exf, sizeof(exf));
            exf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; exf.Format.nChannels = fmt->nChannels >= 2 ? 2 : 1;
            exf.Format.nSamplesPerSec = 48000; exf.Format.wBitsPerSample = (WORD)t[0];
            exf.Format.nBlockAlign = exf.Format.nChannels * t[0] / 8; exf.Format.nAvgBytesPerSec = 48000 * exf.Format.nBlockAlign;
            exf.Format.cbSize = 22; exf.Samples.wValidBitsPerSample = (WORD)t[1];
            exf.dwChannelMask = exf.Format.nChannels == 2 ? (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT) : SPEAKER_FRONT_CENTER;
            exf.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, minP, (WAVEFORMATEX*)&exf, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 fr = 0; ac->GetBufferSize(&fr); ac->Release(); ac = nullptr;
                minP = (REFERENCE_TIME)(10000.0 * 1000 * fr / 48000 + 0.5);
                dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
                hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minP, minP, (WAVEFORMATEX*)&exf, nullptr);
            }
            if (SUCCEEDED(hr)) { ok = true; mode = L"専有"; periodMs = minP / 10000.0; break; }
            ac->Release(); ac = nullptr; dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        }
        if (!ok) mode = L"専有できず→";
    }
    const WAVEFORMATEX* use = ok ? (WAVEFORMATEX*)&exf : fmt;
    if (!ok && ac && fmt) {
        IAudioClient3* ac3 = nullptr;
        if (SUCCEEDED(ac->QueryInterface(__uuidof(IAudioClient3), (void**)&ac3))) {
            UINT32 defF=0, fund=0, minF=0, maxF=0;
            if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(fmt, &defF, &fund, &minF, &maxF)) &&
                SUCCEEDED(ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minF, fmt, nullptr))) {
                ok = true; periodMs = 1000.0 * minF / fmt->nSamplesPerSec; mode = (mode == L"?" ? L"" : mode) + L"低遅延共有";
            }
            ac3->Release();
        }
        if (!ok) {
            hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, fmt, nullptr);
            if (SUCCEEDED(hr)) { ok = true; REFERENCE_TIME d=0,m=0; ac->GetDevicePeriod(&d,&m); periodMs = d/10000.0; mode = (mode == L"?" ? L"" : mode) + L"共有"; }
        }
        use = fmt;
    }
    if (ok) ok = SUCCEEDED(ac->SetEventHandle(ev)) && SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc));
    if (!ok) { setStatus(L"入力を開けませんでした"); goto done; }
    {
        const int ch = use->nChannels, sr = use->nSamplesPerSec, bits = use->wBitsPerSample, stride = use->nBlockAlign;
        bool isFloat = use->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        int valid = bits;
        if (use->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)use;
            isFloat = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            valid = x->Samples.wValidBitsPerSample;
        }
        gSr = sr;
        wchar_t buf[256];
        swprintf(buf, 256, L"%ls / %gk %dch %dbit%ls / 1回 %.1fms", mode.c_str(), sr / 1000.0, ch, valid, isFloat ? L"f" : L"", periodMs);
        setStatus(buf);
        DWORD task = 0; HANDLE avrt = nullptr;
        typedef HANDLE (WINAPI *AvSetFn)(LPCWSTR, LPDWORD);
        HMODULE hav = LoadLibraryW(L"avrt.dll");
        if (hav) { AvSetFn f = (AvSetFn)GetProcAddress(hav, "AvSetMmThreadCharacteristicsW"); if (f) avrt = f(L"Pro Audio", &task); }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        // VU (1msごとに針の式を進める)
        const int blk = std::max(1, sr / 1000);
        const double h = (double)blk / sr, k1 = VU_WN*VU_WN, k2 = 2*VU_ZETA*VU_WN;
        double yL=0,vL=0,yR=0,vR=0,sL=0,sR=0; int n=0;
        // ラウドネス (10msごと)
        double shB[3], shA[2], hpB[3], hpA[2]; makeKWeight(sr, shB, shA, hpB, hpA);
        Biquad kShL, kShR, kHpL, kHpR; kShL.set(shB, shA); kShR.set(shB, shA); kHpL.set(hpB, hpA); kHpR.set(hpB, hpA);
        const int subN = (int)std::lround(sr * 0.01);
        double subSum = 0, subTp = 0; int subCnt = 0;
        TpState tpL, tpR;
        EnterCriticalSection(&gLoudCs); gL.ringN = 0; gL.ringPos = 0; gL.tick = 0; gL.runBlocks = 0; LeaveCriticalSection(&gLoudCs);
        // L/R 時間補正 (最大4サンプル)
        double hL[8] = {}, hR[8] = {}; int hp = 0;
        unsigned long long w = gRingW.load();

        ac->Start();
        while (gRun.load()) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            UINT32 pk = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk > 0) {
                BYTE* data = nullptr; UINT32 fr = 0; DWORD fl = 0;
                if (FAILED(cc->GetBuffer(&data, &fr, &fl, nullptr, nullptr))) break;
                const bool silent = (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const double gl = gGainL.load(), gr = gGainR.load();
                const bool mono = gMono.load();
                const int shift = gLrShift.load();
                const int bytesPer = bits / 8;
                for (UINT32 i = 0; i < fr; i++) {
                    double s0 = 0, s1 = 0;
                    if (!silent) {
                        const BYTE* p = data + (size_t)i * stride;
                        s0 = readSample(p, isFloat, bits);
                        s1 = ch > 1 ? readSample(p + bytesPer, isFloat, bits) : 0;   // 1chの入力: Rは無音 (HTML版と同じ)
                    }
                    double l = s0 * gl, r = (mono ? s0 : s1) * gr;
                    hL[hp] = l; hR[hp] = r;
                    if (shift > 0) l = hL[(hp - shift) & 7];
                    else if (shift < 0) r = hR[(hp + shift) & 7];
                    hp = (hp + 1) & 7;
                    // VU
                    sL += std::fabs(l); sR += std::fabs(r);
                    if (++n >= blk) {
                        vL += (k1*(sL/n - yL) - k2*vL)*h; yL += vL*h;
                        vR += (k1*(sR/n - yR) - k2*vR)*h; yR += vR*h;
                        sL=sR=0; n=0;
                    }
                    // スコープ・RTA 用
                    gRingL[w & (RING-1)] = (float)l; gRingR[w & (RING-1)] = (float)r; w++;
                    // ラウドネスとトゥルーピーク
                    double kl = kHpL.run(kShL.run(l)), kr = kHpR.run(kShR.run(r));
                    subSum += kl*kl + kr*kr;
                    double t = std::max(tpL.run(l), tpR.run(r)); if (t > subTp) subTp = t;
                    if (++subCnt >= subN) { loudPush(subSum / subCnt, subTp); subSum = 0; subTp = 0; subCnt = 0; }
                }
                cc->ReleaseBuffer(fr);
                gRingW.store(w);
                gVuL.store((float)std::max(0.0, yL)); gVuR.store((float)std::max(0.0, yR));
            }
        }
        ac->Stop();
        if (hav) { typedef BOOL (WINAPI *AvRevFn)(HANDLE); AvRevFn rf = (AvRevFn)GetProcAddress(hav, "AvRevertMmThreadCharacteristics"); if (rf && avrt) rf(avrt); FreeLibrary(hav); }
    }
done:
    SafeRelease(cc); SafeRelease(ac); SafeRelease(dev); SafeRelease(en);
    if (fmt) CoTaskMemFree(fmt);
    CloseHandle(ev);
    gVuL = 0; gVuR = 0;
    CoUninitialize();
}

static void stopAudio(){ gRun = false; if (gAudioThread.joinable()) gAudioThread.join(); gVuL = 0; gVuR = 0; }
static void startAudio(){
    stopAudio();
    if (gDevs.empty()) { setStatus(L"入力機器がありません"); return; }
    gDevIndex = (gDevIndex % (int)gDevs.size() + (int)gDevs.size()) % (int)gDevs.size();
    setStatus(L"");
    gRun = true;
    gAudioThread = std::thread(audioMain, gDevs[gDevIndex].id, gExclusive);
}
static void setPower(bool on){ gPower = on; if (on) startAudio(); else { stopAudio(); setStatus(L""); } }

// ============================================================
//  画面の配置 (HTML 版の各部品の位置。ブラウザで測った値、単位は CSS の px)
// ============================================================
struct Rc { float x, y, w, h; float r() const { return x + w; } float b() const { return y + h; } bool in(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; } };
static const float PANEL_W = 987, PANEL_H = 788.8f;
static const Rc R_CONSOLE{0,0,987,406.8f}, R_RTAPANEL{0,421.8f,987,367};
static const Rc R_POWER{45.6f,83.4f,140,35}, R_MIC{255.6f,60.4f,200,29}, R_LR{461.6f,60.4f,40,27}, R_LL{507.6f,60.4f,40,27};
static const Rc R_GC{155.6f,156.4f,130,16}, R_GF{155.6f,176.4f,130,16}, R_GCR{307.6f,156.4f,130,16}, R_GFR{307.6f,176.4f,130,16};
static const Rc R_METER[2] = { {27,244.4f,259.6f,135.4f}, {306.6f,244.4f,259.6f,135.4f} };
static const Rc R_SCOPEG{642.1f,29,130,16}, R_SCOPE{586.2f,103,241.8f,241.8f};
static const Rc R_LRM{680.4f,358.8f,28,21}, R_LRP{754.4f,358.8f,28,21};
static const Rc R_TRACK[2] = { {866,136.8f,20,220}, {922,136.8f,20,220} };
static const Rc R_TABRTA{113.5f,456.3f,104,28}, R_TABLOUD{225.5f,456.3f,104,28}, R_START{719.5f,456.3f,74,28}, R_RESET{799.5f,456.3f,74,28};
static const Rc R_RTAWIN{113.5f,511.8f,760,260};
static const float VU_K = 0.92f, SCOPE_K = 1.09f;

// ============================================================
//  画面の操作部品の値 (HTML 版と同じ。開くたびに初期値)
// ============================================================
static double gC = 20, gF = 0, gCR = 20, gFR = 0, gScopeG = 1.0;
static double gainLdB = 0, gainRdB = 0, gainROffset = 0;
static int lrShift = 0;
static bool rtaMode = true;      // true = RTA, false = LOUDNESS の履歴
static void applyGains(){ gGainL = std::pow(10.0, gainLdB/20); gGainR = std::pow(10.0, gainRdB/20); }
static double jsRound(double v){ return std::floor(v + 0.5); }
static void setRSliders(double db){
    db = std::max(-21.0, std::min(21.0, db));
    double c = std::max(-20.0, std::min(20.0, jsRound(db)));
    double f = std::max(-1.0, std::min(1.0, jsRound((db - c) * 10) / 10));
    gCR = c + 20; gFR = f;
}
static void onMainGain(){ gainLdB = (gC - 20) + gF; setRSliders(gainLdB + gainROffset); gainRdB = (gCR - 20) + gFR; applyGains(); }
static void onRGain(){ gainRdB = (gCR - 20) + gFR; gainROffset = gainRdB - gainLdB; applyGains(); }

// ============================================================
//  画面 (DXGI / Direct2D)
// ============================================================
static HWND gWnd; static bool gFull = true; static RECT gWinRect = {80,60,1380,1100};
static ID3D11Device* gDev = nullptr; static ID3D11DeviceContext* gCtx = nullptr;
static IDXGISwapChain1* gSwap = nullptr; static bool gTearing = false;
static HANDLE gWait = nullptr;
static IDXGIFactory2* gFac = nullptr;
static UINT swapFlags(){ return (gTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; }
static ID2D1Factory* gD2 = nullptr; static ID2D1RenderTarget* gRT = nullptr; static IDWriteFactory* gDW = nullptr;
static ID2D1BitmapRenderTarget* gLayer = nullptr;   // 針以外の全部 (1秒60回描き直す)
static ID2D1BitmapRenderTarget* gScope = nullptr;   // スコープ (残像を残すので、消さずに描き足す)
static ID2D1Bitmap* gFace = nullptr;                // VU の文字盤
static ID2D1SolidColorBrush* gB = nullptr;          // 色を変えて使い回す
static ID2D1PathGeometry* gNeedle = nullptr;
static int gW = 0, gH = 0;
static float gFaceS = 0, gScopeS = 0;
static bool gLayerDirty = true;
static double gFps = 0; static int gHz = 60;
static bool gHelp = true;

// 表示の倍率と位置 (ピンチで拡大縮小、2本指で移動)
static float vS = 1, vX = 0, vY = 0;
static float fitScale(){ return std::min(gW / PANEL_W, gH / PANEL_H) * 0.94f; }
static void resetView(){ vS = fitScale(); vX = (gW - PANEL_W * vS) / 2; vY = (gH - PANEL_H * vS) / 2; gLayerDirty = true; }
static D2D1_MATRIX_3X2_F viewM(){ return D2D1::Matrix3x2F(vS, 0, 0, vS, vX, vY); }

static D2D1_COLOR_F rgb(unsigned c, float a = 1){ return D2D1::ColorF(((c>>16)&255)/255.f, ((c>>8)&255)/255.f, (c&255)/255.f, a); }
static ID2D1SolidColorBrush* br(unsigned c, float a = 1){ gB->SetColor(rgb(c, a)); return gB; }

// 文字の書式 (大きさ・太さ・寄せ方ごとに1回だけ作る)
struct Fmt { float size; bool bold; int align; bool trim; IDWriteTextFormat* f; };
static std::vector<Fmt> gFmts;
static IDWriteTextFormat* fmt(float size, bool bold, int align, bool trim = false){
    for (auto& x : gFmts) if (x.size == size && x.bold == bold && x.align == align && x.trim == trim) return x.f;
    IDWriteTextFormat* f = nullptr;
    gDW->CreateTextFormat(FONT_FAMILY, nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", &f);
    f->SetTextAlignment(align == 0 ? DWRITE_TEXT_ALIGNMENT_LEADING : align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    // ブラウザの「普通の行の高さ」と同じ位置に文字の下端 (ベースライン) が来るようにする
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.15f, size * 0.921f);
    if (trim) {
        IDWriteInlineObject* sign = nullptr; gDW->CreateEllipsisTrimmingSign(f, &sign);
        DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; f->SetTrimming(&t, sign); SafeRelease(sign);
    }
    gFmts.push_back({size, bold, align, trim, f});
    return f;
}
// x: 寄せ方の基準 (左寄せなら左端、中央なら中心、右寄せなら右端)、base: 文字の下端
static void textB(ID2D1RenderTarget* rt, const wchar_t* s, float size, bool bold, int align, float x, float base, ID2D1Brush* b, float maxW = 0){
    IDWriteTextFormat* f = fmt(size, bold, align, maxW > 0);
    float w = maxW > 0 ? maxW : 4000;
    float l = align == 0 ? x : align == 1 ? x - w/2 : x - w;
    float top = base - size * 0.921f;
    rt->DrawText(s, (UINT32)wcslen(s), f, D2D1::RectF(l, top, l + w, top + size * 1.15f), b, D2D1_DRAW_TEXT_OPTIONS_NONE);
}
// HTML の部品の上端から書く (普通の行の高さ)
static void textT(ID2D1RenderTarget* rt, const wchar_t* s, float size, bool bold, int align, float x, float top, ID2D1Brush* b, float maxW = 0){
    textB(rt, s, size, bold, align, x, top + size * 0.921f, b, maxW);
}
// ボタンの中央に書く
static void textMid(ID2D1RenderTarget* rt, const wchar_t* s, float size, bool bold, const Rc& r, ID2D1Brush* b){
    textB(rt, s, size, bold, 1, r.x + r.w/2, r.y + r.h/2 + size * 0.3465f, b);
}
static float textWidth(const wchar_t* s, float size, bool bold){
    IDWriteTextLayout* lay = nullptr; float w = 0;
    if (SUCCEEDED(gDW->CreateTextLayout(s, (UINT32)wcslen(s), fmt(size, bold, 0), 4000, 100, &lay))) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; lay->Release(); }
    return w;
}
static D2D1_RECT_F RF(const Rc& r){ return D2D1::RectF(r.x, r.y, r.r(), r.b()); }
static void fillRR(ID2D1RenderTarget* rt, const Rc& r, float rad, unsigned c, float a = 1){ rt->FillRoundedRectangle(D2D1::RoundedRect(RF(r), rad, rad), br(c, a)); }
static void strokeRR(ID2D1RenderTarget* rt, const Rc& r, float rad, float w, unsigned c){
    Rc q{ r.x + w/2, r.y + w/2, r.w - w, r.h - w }; rt->DrawRoundedRectangle(D2D1::RoundedRect(RF(q), rad, rad), br(c), w);
}

// ===== VU の文字盤 (HTML 版の drawVUBase と同じ形) =====
static const float VU_W = 280, VU_H = 145, VCX = 140, VCY = 181, START_DEG = 133, END_DEG = 47, VU_ARC_R = 140, VU_ARC_DROP = 30, VU_LABEL_GAP = 26;
static const double VU_SCALE[][2] = { {-20,0.030},{-10,0.135},{-7,0.222},{-5,0.303},{-3,0.418},{-2,0.489},{-1,0.567},{0,0.654},{1,0.765},{2,0.888},{3,1.000} };
static double vuFrac(double v){
    if (!(v > 0)) return 0;
    double pa = 0, pf = 0;
    for (auto& m : VU_SCALE) { double a = std::pow(10.0, m[0]/20); if (v <= a) return pf + (m[1]-pf)*(v-pa)/(a-pa); pa = a; pf = m[1]; }
    return 1;
}
static double dbToAngle(double db){ return START_DEG + vuFrac(std::pow(10.0, db/20)) * (END_DEG - START_DEG); }
static double vuArcRad(){ double a = START_DEG*PI/180; return std::hypot(std::cos(a)*VU_ARC_R, std::sin(a)*VU_ARC_R + VU_ARC_DROP); }
static double vuArcDist(double deg){ double s = std::sin(deg*PI/180), R = vuArcRad(); return -VU_ARC_DROP*s + std::sqrt(R*R - VU_ARC_DROP*VU_ARC_DROP*(1 - s*s)); }
static D2D1_POINT_2F vuPt(double deg, double extra){ double r = vuArcDist(deg) + extra, a = deg*PI/180; return D2D1::Point2F((float)(VCX + std::cos(a)*r), (float)(VCY - std::sin(a)*r)); }

static void buildFace(){
    SafeRelease(gFace);
    float px = VU_W * VU_K * vS;    // 実際に表示される大きさ (画面のピクセル)
    float k = VU_K * vS;
    ID2D1BitmapRenderTarget* bt = nullptr;
    if (FAILED(gRT->CreateCompatibleRenderTarget(D2D1::SizeF(px, VU_H * k), &bt))) return;
    bt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    bt->BeginDraw();
    bt->Clear(rgb(0xfdfae1));
    bt->SetTransform(D2D1::Matrix3x2F::Scale(k, k));
    const unsigned BLACK = 0x222222, RED = 0xdd0000, SOFT = 0x9a9a90;
    auto arcLine = [&](double d0, double d1, double extra, unsigned c, float w){
        ID2D1PathGeometry* g = nullptr; gD2->CreatePathGeometry(&g); ID2D1GeometrySink* s = nullptr; g->Open(&s);
        for (int i = 0; i <= 80; i++) { auto p = vuPt(d0 + (d1 - d0)*i/80, extra); if (i == 0) s->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW); else s->AddLine(p); }
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        bt->DrawGeometry(g, br(c), w); g->Release();
    };
    arcLine(START_DEG, dbToAngle(0), 0, BLACK, 1.5f);
    arcLine(dbToAngle(0), dbToAngle(3), 2.5, RED, 5);
    for (auto& m : VU_SCALE) {
        double v = m[0], deg = dbToAngle(v); unsigned c = v >= 0 ? RED : BLACK;
        bt->DrawLine(vuPt(deg, 0), vuPt(deg, v >= 0 ? 13 : 10), br(c), 2.2f);
        wchar_t t[8]; swprintf(t, 8, L"%d", (int)std::fabs(v));
        auto p = vuPt(deg, VU_LABEL_GAP);
        float half = textWidth(t, 15, true) / 2;
        textB(bt, t, 15, true, 1, std::max(4 + half, std::min(VU_W - 4 - half, p.x)), p.y + 5, br(c));
    }
    for (int pc : {0, 20, 40, 60, 80, 100}) {
        double deg = START_DEG + vuFrac(pc / 100.0) * (END_DEG - START_DEG);
        auto d = vuPt(deg, -5);
        bt->FillEllipse(D2D1::Ellipse(d, 0.9f, 0.9f), br(SOFT));
        auto t = vuPt(deg, -14);
        wchar_t s[8]; swprintf(s, 8, pc == 100 ? L"100%%" : L"%d", pc);
        textB(bt, s, 8, false, 1, t.x, t.y + 3, br(SOFT));
    }
    auto t = vuPt(dbToAngle(-20), -24); textB(bt, L"\x2212", 13, true, 1, t.x + 6, t.y + 6, br(BLACK));
    t = vuPt(dbToAngle(3), -24);        textB(bt, L"+", 13, true, 1, t.x - 6, t.y + 6, br(RED));
    textB(bt, L"VU", 18, true, 1, VCX, VU_H - 14, br(BLACK));
    bt->EndDraw();
    bt->GetBitmap(&gFace);
    SafeRelease(bt);
    gFaceS = vS;
}

// ===== スコープ (HTML 版と同じ描き方: 残像を薄く消して、点と点を明るさ6段階の線でつなぐ) =====
static const int SCOPE_POINTS = 1024; static const float SCOPE_SCALE = 160, SCOPE_FADE = 0.18f, SCOPE_DOT_ALPHA = 0.5f, SCOPE_BEAM_LEN = 1.5f;
static void buildScope(){
    SafeRelease(gScope);
    float px = std::max(220.f, std::round(220 * SCOPE_K * vS));
    if (FAILED(gRT->CreateCompatibleRenderTarget(D2D1::SizeF(px, px), &gScope))) return;
    gScope->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    gScope->BeginDraw(); gScope->Clear(rgb(0)); gScope->EndDraw();
    gScopeS = vS;
}
static void drawScope(){
    if (!gScope) return;
    float P = gScope->GetSize().width, RES = P / 220;
    float buf[2][SCOPE_POINTS];   // [0]=L+R (縦) [1]=R-L (横)
    unsigned long long w = gRingW.load();
    for (int i = 0; i < SCOPE_POINTS; i++) {
        long long idx = (long long)w - SCOPE_POINTS + i;
        float l = 0, r = 0; if (idx >= 0) { l = gRingL[idx & (RING-1)]; r = gRingR[idx & (RING-1)]; }
        buf[0][i] = l + r; buf[1][i] = r - l;
    }
    gScope->BeginDraw();
    gScope->SetTransform(D2D1::Matrix3x2F::Identity());
    gScope->FillRectangle(D2D1::RectF(0, 0, P, P), br(0x000a00, SCOPE_FADE));
    float c = std::round(110 * RES), lw = std::max(1.f, std::round(RES));
    gScope->DrawLine(D2D1::Point2F(c, 0), D2D1::Point2F(c, P), br(0x1a3a1a), lw);
    gScope->DrawLine(D2D1::Point2F(0, c), D2D1::Point2F(P, c), br(0x1a3a1a), lw);
    const float K = (float)gScopeG * SCOPE_SCALE * RES, C = 110 * RES;
    const float D = std::max(1.f, std::round(1 * RES)), O = ((int)D % 2) ? 0.5f : 0, beam = SCOPE_BEAM_LEN * RES;
    const float levels[6] = { 1, 0.6f, 0.36f, 0.2f, 0.1f, 0.05f };
    ID2D1PathGeometry* g[6] = {}; ID2D1GeometrySink* s[6] = {};
    for (int k = 0; k < 6; k++) { gD2->CreatePathGeometry(&g[k]); g[k]->Open(&s[k]); }
    float px = std::round(C + buf[1][0] * K) + O, py = std::round(C - buf[0][0] * K) + O;
    for (int i = 1; i < SCOPE_POINTS; i++) {
        float x = std::round(C + buf[1][i] * K) + O, y = std::round(C - buf[0][i] * K) + O;
        float len = std::hypot(x - px, y - py), b = beam / std::max(len, beam);
        int k = 0; while (k < 6 && b < levels[k] * 0.8f) k++;
        if (k < 6) {
            if (len < 0.5f) { s[k]->BeginFigure(D2D1::Point2F(x, y - 0.5f), D2D1_FIGURE_BEGIN_HOLLOW); s[k]->AddLine(D2D1::Point2F(x, y + 0.5f)); }
            else { s[k]->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_HOLLOW); s[k]->AddLine(D2D1::Point2F(x, y)); }
            s[k]->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        px = x; py = y;
    }
    for (int k = 0; k < 6; k++) { s[k]->Close(); s[k]->Release(); gScope->DrawGeometry(g[k], br(0x00ff66, SCOPE_DOT_ALPHA * levels[k]), D); g[k]->Release(); }
    gScope->EndDraw();
}

// ===== RTA (HTML 版と同じ: 高域は 8192点、低域は 32768点の FFT、Blackman の窓、バンド内のパワーを合算) =====
static const int RTA_BANDS_PER_OCT = 6; static const double RTA_TOP = 0, RTA_BOTTOM = -80, RTA_ATTACK = 0.05, RTA_RELEASE = 0.3, RTA_SPLIT_HZ = 250;
static const double RTA_NORM = 2 / 0.3046;
struct Band { double fc, f1, f2, p, raw; };
static std::vector<Band> gBands;
struct Fft {
    int N = 0; std::vector<double> win; std::vector<std::complex<double>> tw, a; std::vector<int> rev; std::vector<double> pow;
    void init(int n){
        N = n; win.resize(n); tw.resize(n/2); a.resize(n); rev.resize(n); pow.assign(n/2, 0);
        for (int i = 0; i < n; i++) win[i] = 0.42 - 0.5*std::cos(2*PI*i/n) + 0.08*std::cos(4*PI*i/n);
        for (int i = 0; i < n/2; i++) tw[i] = std::polar(1.0, -2*PI*i/n);
        int bits = 0; while ((1 << bits) < n) bits++;
        for (int i = 0; i < n; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[i] = r; }
    }
    // 直近 N サンプル (L と R の平均) のパワー (|X/N|^2)
    void run(){
        unsigned long long w = gRingW.load();
        for (int i = 0; i < N; i++) {
            long long idx = (long long)w - N + i; double x = 0;
            if (idx >= 0) x = 0.5 * ((double)gRingL[idx & (RING-1)] + gRingR[idx & (RING-1)]);
            a[rev[i]] = x * win[i];
        }
        for (int len = 2; len <= N; len <<= 1) {
            int step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < len/2; j++) {
                    std::complex<double> u = a[i+j], v = a[i+j+len/2] * tw[j*step];
                    a[i+j] = u + v; a[i+j+len/2] = u - v;
                }
        }
        for (int k = 0; k < N/2; k++) pow[k] = std::norm(a[k]) / ((double)N * N);
    }
    double band(double f1, double f2, double sr) const {
        double binHz = sr / N, lo = f1 / binHz, hi = f2 / binHz;
        int iEnd = std::min(N/2 - 1, (int)std::floor(hi + 0.5));
        double p = 0;
        for (int i = std::max(1, (int)std::floor(lo + 0.5)); i <= iEnd; i++) {
            double w = std::min(hi, i + 0.5) - std::max(lo, i - 0.5);
            if (w > 0) p += pow[i] * w;
        }
        return p * RTA_NORM;
    }
};
static Fft gFftHi, gFftLo;
static double rtaHiT = -1e9, rtaLoT = -1e9, rtaLastT = 0;
static void initRta(){
    gFftHi.init(8192); gFftLo.init(32768);
    int n = RTA_BANDS_PER_OCT;
    int kMin = (int)jsRound(n * std::log2(20.0 / 1000)), kMax = (int)jsRound(n * std::log2(20000.0 / 1000));
    double half = std::pow(2.0, 1.0 / (2*n));
    for (int k = kMin; k <= kMax; k++) { double fc = 1000 * std::pow(2.0, (double)k / n); gBands.push_back({fc, fc/half, fc*half, 0, 0}); }
}
static double nowMs(){ static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / f.QuadPart; }

static float freqX(double f){ return (float)(30 + std::log10(f / 20) / std::log10(20000.0 / 20) * (760 - 60)); }
static double rtaSnap(double f){ return 1000 * std::pow(2.0, jsRound(3 * std::log2(f / 1000)) / 3); }

static void drawRta(ID2D1RenderTarget* rt){
    const double now = nowMs(), sr = gSr.load();
    bool hiNew = false, loNew = false;
    if (gPower) {
        if (now - rtaHiT >= 30) { rtaHiT = now; gFftHi.run(); hiNew = true; }
        if (now - rtaLoT >= 100) { rtaLoT = now; gFftLo.run(); loNew = true; }
    }
    const struct { double f; const wchar_t* t; } labels[] = { {20,L"20"},{31.5,L"31.5"},{63,L"63"},{125,L"125"},{250,L"250"},{500,L"500"},{1000,L"1k"},{2000,L"2k"},{4000,L"4k"},{8000,L"8k"},{16000,L"16k"} };
    for (int i = 1; i < 11; i++) {
        float x = freqX(rtaSnap(labels[i].f));
        rt->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, 220), br(0xffffff, labels[i].f == 1000 ? 0.25f : 0.12f), 1);
    }
    double dt = std::min(0.1, (now - rtaLastT) / 1000); rtaLastT = now;
    float step = (float)((760 - 60) / (std::log2(1000.0) * RTA_BANDS_PER_OCT)), barW = std::max(1.f, step * 0.8f);
    for (auto& b : gBands) {
        bool low = b.fc < RTA_SPLIT_HZ;
        if (low ? loNew : hiNew) b.raw = low ? gFftLo.band(b.f1, b.f2, sr) : gFftHi.band(b.f1, b.f2, sr);
        double tau = b.raw > b.p ? RTA_ATTACK : RTA_RELEASE;
        b.p += (b.raw - b.p) * (1 - std::exp(-dt / tau));
        double db = 10 * std::log10(b.p + 1e-12);
        float h = (float)std::max(0.0, std::min(220.0, (db - RTA_BOTTOM) / (RTA_TOP - RTA_BOTTOM) * 220));
        float x = freqX(b.fc);
        if (h > 0) rt->FillRectangle(D2D1::RectF(x - barW/2, 220 - h, x + barW/2, 220), br(0x777777));
    }
    for (int k = -17; k <= 13; k++) {
        float x = std::round(freqX(1000 * std::pow(2.0, k / 3.0))) + 0.5f;
        rt->DrawLine(D2D1::Point2F(x, 221), D2D1::Point2F(x, k % 3 == 0 ? 229.f : 225.f), br(0xffffff, 0.4f), 1);
    }
    for (auto& l : labels) textB(rt, l.t, 12, false, 1, freqX(rtaSnap(l.f)), 245, br(0xaaaaaa));
}

// ラウドネスの色 (HTML 版と同じグラデーション)
static const struct { double lk; unsigned c; } LOUD_COLORS[] = {
    {-49,0x2d7d96},{-43,0x37a55f},{-35,0x55af55},{-28,0x96c33c},{-24,0xe8ca2d},{-19,0xecaa2d},{-14,0xde3a2a} };
static ID2D1LinearGradientBrush* loudGrad(ID2D1RenderTarget* rt, float yBot, float yTop){
    D2D1_GRADIENT_STOP st[7];
    for (int i = 0; i < 7; i++) { st[i].position = (float)std::max(0.0, std::min(1.0, (LOUD_COLORS[i].lk - LOUD_MIN) / (LOUD_MAX - LOUD_MIN))); st[i].color = rgb(LOUD_COLORS[i].c); }
    ID2D1GradientStopCollection* col = nullptr; rt->CreateGradientStopCollection(st, 7, &col);
    ID2D1LinearGradientBrush* g = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, yBot), D2D1::Point2F(0, yTop)), col, &g);
    SafeRelease(col); return g;
}

static void drawLoudHistory(ID2D1RenderTarget* rt, const std::vector<float>& hist, double S){
    const float pitch = 3, x1 = 760 - 10, x0 = x1 - LOUD_HIST_BARS * pitch, yTop = 10, yBot = 220;
    auto yOf = [&](double v){ return (float)(yBot - (v - LOUD_MIN) / (LOUD_MAX - LOUD_MIN) * (yBot - yTop)); };
    for (int v = (int)LOUD_MIN; v <= (int)LOUD_MAX; v += 10) {
        rt->DrawLine(D2D1::Point2F(x0, yOf(v)), D2D1::Point2F(x1, yOf(v)), br(0xffffff, 0.12f), 1);
        wchar_t t[8]; swprintf(t, 8, L"%d", v); textB(rt, t, 12, false, 2, x0 - 6, yOf(v) + 4, br(0xaaaaaa));
    }
    std::vector<double> vals(hist.begin(), hist.end()); vals.push_back(S);
    std::vector<D2D1_POINT_2F> pts(vals.size());
    for (size_t k = 0; k < vals.size(); k++)
        pts[k] = D2D1::Point2F(x1 - (float)(vals.size() - 1 - k + 0.5) * pitch, yOf(std::max(LOUD_MIN, std::min(LOUD_MAX, vals[k]))));
    ID2D1PathGeometry* g = nullptr; gD2->CreatePathGeometry(&g); ID2D1GeometrySink* s = nullptr; g->Open(&s);
    s->BeginFigure(D2D1::Point2F(pts[0].x, yBot), D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine(pts[0]);
    for (size_t k = 1; k < pts.size(); k++) {
        D2D1_POINT_2F m = D2D1::Point2F((pts[k-1].x + pts[k].x) / 2, (pts[k-1].y + pts[k].y) / 2);
        s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pts[k-1], m));
    }
    auto last = pts.back();
    s->AddLine(last); s->AddLine(D2D1::Point2F(x1, last.y)); s->AddLine(D2D1::Point2F(x1, yBot));
    s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
    ID2D1LinearGradientBrush* grad = loudGrad(rt, yBot, yTop);
    rt->FillGeometry(g, grad); SafeRelease(grad); g->Release();
    rt->DrawLine(D2D1::Point2F(x0, yOf(LOUD_TARGET)), D2D1::Point2F(x1, yOf(LOUD_TARGET)), br(0xffffff, 0.8f), 1);
    textB(rt, L"-24", 12, false, 2, x0 - 6, yOf(LOUD_TARGET) + 4, br(0xffffff));
    for (int m = 0; m <= LOUD_HIST_MIN; m += 3) {
        wchar_t t[16]; if (m == 0) wcscpy(t, L"0"); else swprintf(t, 16, L"-%d分", m);
        textB(rt, t, 12, false, 1, x1 - m * 60 / LOUD_BAR_SEC * pitch, 245, br(0xaaaaaa));
    }
}

// ===== 操作部品の描き方 =====
static void drawRange(ID2D1RenderTarget* rt, const Rc& r, double t, bool grey){
    float cy = r.y + r.h/2, x0 = r.x + 8, x1 = r.r() - 8, tx = x0 + (float)t * (x1 - x0);
    unsigned fillC = grey ? 0x444444 : 0x0075ff, restC = grey ? 0xb3b3b3 : 0xefefef, edgeC = grey ? 0x8a8a8a : 0xb8b8b8;
    D2D1_ROUNDED_RECT tr = D2D1::RoundedRect(D2D1::RectF(r.x + 1, cy - 4, r.r() - 1, cy + 4), 4, 4);
    rt->FillRoundedRectangle(tr, br(restC)); rt->DrawRoundedRectangle(tr, br(edgeC), 0.8f);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + 1, cy - 4, tx, cy + 4), 4, 4), br(fillC));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 8, 8), br(fillC));
}
static void drawPBtn(ID2D1RenderTarget* rt, const Rc& r, const wchar_t* s, bool on){
    fillRR(rt, r, 6, on ? 0x0a7a3c : 0x2a2a2a); strokeRR(rt, r, 6, 1, on ? 0x16c060 : 0x555555);
    textMid(rt, s, 12, true, r, br(on ? 0xffffff : 0xdddddd));
}
static void drawSelBtn(ID2D1RenderTarget* rt, const Rc& r, const wchar_t* s, bool on){
    fillRR(rt, r, 4, on ? 0x555555 : 0x333333); strokeRR(rt, r, 4, 1, on ? 0x888888 : 0x555555);
    textMid(rt, s, 13, true, r, br(on ? 0xffffff : 0x999999));
}

static int monitorHz(){
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(MonitorFromWindow(gWnd, MONITOR_DEFAULTTONEAREST), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return (int)dm.dmDisplayFrequency;
    return 60;
}

// ===== 針以外の全部を描く (1秒60回) =====
static void drawLayer(){
    ID2D1RenderTarget* rt = gLayer;
    if (!rt) return;
    if (std::fabs(gFaceS - vS) > 1e-4f || !gFace) buildFace();
    if (std::fabs(gScopeS - vS) > 1e-4f || !gScope) buildScope();
    if (gPower) drawScope();

    // ラウドネスの値を写す (音の側と取り合いにならないよう、短く)
    double M, S, I; bool hasI, run; double tpRecent = 0, tpMax; std::vector<float> hist;
    EnterCriticalSection(&gLoudCs);
    M = gL.M; S = gL.S; I = gL.I; hasI = gL.hasI; run = gL.run; tpMax = gL.tpMax; hist = gL.hist;
    for (int i = 0; i < TP_HOLD_BLOCKS; i++) if (gL.tpRecent[i] > tpRecent) tpRecent = gL.tpRecent[i];
    LeaveCriticalSection(&gLoudCs);

    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(rgb(0x050505));
    rt->SetTransform(viewM());

    // 2つの箱
    for (const Rc* p : { &R_CONSOLE, &R_RTAPANEL }) { fillRR(rt, *p, 12, 0x1a1a1a); strokeRR(rt, *p, 12, 2, 0x333333); }

    // 電源、入力の選択
    fillRR(rt, R_POWER, 4, gPower ? 0xdd0000 : 0x444444);
    textMid(rt, gPower ? L"POWER ON" : L"POWER OFF", 13.333f, true, R_POWER, br(0xffffff));
    textT(rt, L"SELECT INPUT DEVICE:", 12, false, 1, 401.55f, 41.4f, br(0x00ff00));
    fillRR(rt, R_MIC, 4, 0x333333); strokeRR(rt, R_MIC, 4, 1, 0x555555);
    {
        std::wstring dn = gDevs.empty() ? L"-- Device List --" : gDevs[gDevIndex].name;
        textB(rt, dn.c_str(), 13.333f, false, 0, R_MIC.x + 9, R_MIC.y + R_MIC.h/2 + 13.333f*0.3465f, br(0xffffff), R_MIC.w - 34);
        float ax = R_MIC.r() - 16, ay = R_MIC.y + R_MIC.h/2;
        ID2D1PathGeometry* g = nullptr; gD2->CreatePathGeometry(&g); ID2D1GeometrySink* s = nullptr; g->Open(&s);
        s->BeginFigure(D2D1::Point2F(ax - 4, ay - 2), D2D1_FIGURE_BEGIN_HOLLOW); s->AddLine(D2D1::Point2F(ax, ay + 2)); s->AddLine(D2D1::Point2F(ax + 4, ay - 2));
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        rt->DrawGeometry(g, br(0xffffff), 1.6f); g->Release();
    }
    drawSelBtn(rt, R_LR, L"LR", !gMono.load()); drawSelBtn(rt, R_LL, L"LL", gMono.load());
    {
        std::wstring st; EnterCriticalSection(&gStatusCs); st = gStatus; LeaveCriticalSection(&gStatusCs);
        if (gPower) {
            const wchar_t* mode = !gFull ? L"ウィンドウ" : (gTearing && gFps > gHz * 1.5) ? L"待ちなし" : L"待ちあり";
            wchar_t b[256]; swprintf(b, 256, L"%ls / %ls %.0f回/秒", st.c_str(), mode, gFps); st = b;
        }
        textT(rt, st.c_str(), 11, false, 1, 401.6f, 104.4f, br(0x888888), 360);
    }

    // レベル
    drawRange(rt, R_GC, gC / 40, false); drawRange(rt, R_GF, (gF + 1) / 2, false);
    drawRange(rt, R_GCR, gCR / 40, true); drawRange(rt, R_GFR, (gFR + 1) / 2, true);
    { wchar_t t[32]; swprintf(t, 32, L"%+.1f dB", gainLdB); if (gainLdB == 0) wcscpy(t, L"+0.0 dB");
      textT(rt, t, 12, false, 1, 220.6f, 194.4f, br(0x888888));
      swprintf(t, 32, L"R %+.1f dB", gainRdB); if (gainRdB == 0) wcscpy(t, L"R +0.0 dB");
      textT(rt, t, 12, false, 1, 372.6f, 194.4f, br(0x777777)); }

    // VU の窓 (針は毎回別に描く)
    textT(rt, L"L-VU", 16, true, 1, 156.75f, 218.4f, br(0xeeeeee));
    textT(rt, L"R-VU", 16, true, 1, 436.35f, 218.4f, br(0xeeeeee));
    for (int i = 0; i < 2; i++) {
        const Rc& m = R_METER[i];
        if (gFace) rt->DrawBitmap(gFace, D2D1::RectF(m.x + 1, m.y + 1, m.r() - 1, m.b() - 1));
        strokeRR(rt, m, 4, 1, 0x444444);
    }

    // スコープ
    drawRange(rt, R_SCOPEG, (gScopeG - 0.1) / 4.9, false);
    { wchar_t t[16]; swprintf(t, 16, L"x%.1f", gScopeG); textT(rt, t, 12, false, 1, 707.1f, 55, br(0x888888)); }
    textT(rt, L"PHASE SCOPE", 16, true, 1, 707.1f, 77, br(0xeeeeee));
    fillRR(rt, R_SCOPE, 4, 0x000000);
    if (gScope) { ID2D1Bitmap* b = nullptr; gScope->GetBitmap(&b); if (b) { rt->DrawBitmap(b, D2D1::RectF(R_SCOPE.x + 1, R_SCOPE.y + 1, R_SCOPE.r() - 1, R_SCOPE.b() - 1)); b->Release(); } }
    strokeRR(rt, R_SCOPE, 4, 1, 0x444444);
    {
        wchar_t v[8]; swprintf(v, 8, lrShift > 0 ? L"+%d" : L"%d", lrShift);
        bool act = lrShift != 0;
        if (act) {
            wchar_t s[32]; swprintf(s, 32, L"L/R補正 %ls", v);
            float tw = textWidth(s, 11, true);
            Rc bd{ R_SCOPE.x + 1 + 6, R_SCOPE.y + 1 + 5, tw + 10, 11 * 1.15f + 2 };
            fillRR(rt, bd, 3, 0xe8c430);
            textT(rt, s, 11, true, 0, bd.x + 5, bd.y + 1, br(0x1a1400));
        }
        unsigned col = act ? 0xe8c430 : 0x888888;
        textB(rt, L"L/R補正", 12, act, 0, 631.7f, 360.8f + 13.8f, br(col));
        drawSelBtn(rt, R_LRM, L"\x2212", false); drawSelBtn(rt, R_LRP, L"+", false);
        textB(rt, v, 12, act, 1, 731.4f, 362.3f + 10.9f, br(act ? 0xe8c430 : 0xaaaaaa));
    }

    // ラウドネスのバー
    textT(rt, L"LOUDNESS", 16, true, 1, 903.95f, 90.8f, br(0xeeeeee));
    {
        const wchar_t* nm[2] = { L"M", L"S" }; double val[2] = { M, S }; float cx[2] = { 876, 932 };
        ID2D1LinearGradientBrush* grad = loudGrad(rt, R_TRACK[0].b(), R_TRACK[0].y);
        for (int i = 0; i < 2; i++) {
            const Rc& t = R_TRACK[i];
            textT(rt, nm[i], 12, false, 1, cx[i], 116.8f, br(0xaaaaaa));
            rt->FillRectangle(RF(t), br(0x222222));
            double pct = std::max(0.0, std::min(1.0, (val[i] - LOUD_MIN) / (LOUD_MAX - LOUD_MIN)));
            if (pct > 0) {
                // グラデーションは枠の高さに固定して、バーの高さぶんだけ見せる
                rt->SetTransform(D2D1::Matrix3x2F::Translation(t.x - R_TRACK[0].x, 0) * viewM());
                rt->FillRectangle(D2D1::RectF(R_TRACK[0].x, (float)(t.b() - pct * t.h), R_TRACK[0].r(), t.b()), grad);
                rt->SetTransform(viewM());
            }
            float ty = (float)(t.b() - (LOUD_TARGET - LOUD_MIN) / (LOUD_MAX - LOUD_MIN) * t.h);
            rt->FillRectangle(D2D1::RectF(t.x - 3, ty - 1, t.r() + 3, ty), br(0xffffff));
            wchar_t s[16]; if (val[i] < -99) wcscpy(s, L"--.-"); else swprintf(s, 16, L"%.1f", val[i]);
            textT(rt, s, 15, false, 1, cx[i], 362.8f, br(0xcccccc));
        }
        SafeRelease(grad);
    }

    // 下の段: タブ、トゥルーピーク、積算
    drawPBtn(rt, R_TABRTA, L"RTA", rtaMode); drawPBtn(rt, R_TABLOUD, L"LOUDNESS", !rtaMode);
    {
        double lin[2] = { tpRecent, tpMax }; const wchar_t* lab[2] = { L"TP 10s", L"TP MAX" }; float y[2] = { 456.8f, 471.8f };
        for (int i = 0; i < 2; i++) {
            double db = lin[i] > 1e-6 ? 20*std::log10(lin[i]) : -1e9;
            wchar_t s[16]; if (db < -70) wcscpy(s, L"--.-"); else swprintf(s, 16, L"%.1f", db);
            unsigned col = db > 0 ? 0xc0504d : db > -1 ? 0xc9a227 : 0x888888;
            float base = y[i] + 10.16f;
            textB(rt, lab[i], 12, false, 0, 343.5f, base, br(0x888888));
            textB(rt, s, 12, false, 2, 429.5f, base, br(col));
            textB(rt, L" dBTP", 12, false, 0, 429.5f, base, br(0x888888));
        }
    }
    textB(rt, L"INTEGRATED", 11, false, 2, 701.5f, 438.8f + 9.3f, br(0xaaaaaa));
    {
        wchar_t s[16]; if (!hasI) wcscpy(s, L"--.-"); else swprintf(s, 16, L"%.1f", I);
        textB(rt, s, 50, true, 2, 651.5f, 494.05f, br(0x00ff00));
        textB(rt, L" LKFS", 18, false, 0, 651.5f, 494.05f, br(0x00ff00));
    }
    drawPBtn(rt, R_START, run ? L"STOP" : L"START", run); drawPBtn(rt, R_RESET, L"RESET", false);

    // RTA / ラウドネスの履歴の窓
    rt->FillRectangle(RF(R_RTAWIN), br(0x050505)); strokeRR(rt, R_RTAWIN, 0, 1, 0x444444);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(R_RTAWIN.x + 1, R_RTAWIN.y + 1) * viewM());
    if (rtaMode) { if (gPower) { rt->FillRectangle(D2D1::RectF(0, 0, 760, 260), br(0x050505)); drawRta(rt); } }
    else { rt->FillRectangle(D2D1::RectF(0, 0, 760, 260), br(0x050505)); drawLoudHistory(rt, hist, S); }

    // 使い方 (画面の左下)
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (gHelp) textB(rt, L"ピンチで拡大縮小・2本指で移動   0 表示を元に戻す   F 全画面/ウィンドウ   X 専有モード   H この表示を消す   Esc 終了",
                     12, false, 0, 16, (float)gH - 10, br(0x555555));
    rt->EndDraw();
    gLayerDirty = false;
}

// ===== 1回の描画: 描いておいた絵 + 針 =====
static void render(){
    if (!gRT || !gLayer) return;
    static double lastLayer = 0; double now = nowMs();
    if (gLayerDirty || now - lastLayer >= 1000.0 / 60 * 0.85) { lastLayer = now; drawLayer(); }
    gRT->BeginDraw();
    gRT->SetTransform(D2D1::Matrix3x2F::Identity());
    ID2D1Bitmap* lb = nullptr; gLayer->GetBitmap(&lb);
    if (lb) { gRT->DrawBitmap(lb, D2D1::RectF(0, 0, (float)gW, (float)gH), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR); lb->Release(); }
    float vus[2] = { gVuL.load(), gVuR.load() };
    for (int i = 0; i < 2; i++) {
        const Rc& m = R_METER[i];
        gRT->SetTransform(viewM());
        gRT->PushAxisAlignedClip(D2D1::RectF(m.x + 1, m.y + 1, m.r() - 1, m.b() - 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        double db = std::max(-200.0, std::min(3.0, 20*std::log10(vus[i] > 0 ? vus[i] : 0.00001) + VU_CAL));
        double ang = dbToAngle(db);
        gRT->SetTransform(D2D1::Matrix3x2F::Rotation((float)(90 - ang)) * D2D1::Matrix3x2F::Scale(VU_K, VU_K) *
                          D2D1::Matrix3x2F::Translation(m.x + 1 + VCX * VU_K, m.y + 1 + VCY * VU_K) * viewM());
        gRT->FillGeometry(gNeedle, br(0x111111));
        gRT->SetTransform(viewM());
        gRT->PopAxisAlignedClip();
    }
    HRESULT hr = gRT->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { extern void createTargets(); createTargets(); }
    gSwap->Present(0, (gTearing && gFull) ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

static void releaseTargets(){ SafeRelease(gFace); SafeRelease(gScope); SafeRelease(gLayer); SafeRelease(gB); SafeRelease(gRT); gFaceS = gScopeS = 0; }
void createTargets(){
    releaseTargets();
    IDXGISurface* surf = nullptr;
    if (FAILED(gSwap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) return;
    D2D1_RENDER_TARGET_PROPERTIES pr = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    gD2->CreateDxgiSurfaceRenderTarget(surf, &pr, &gRT);
    surf->Release();
    if (!gRT) return;
    gRT->CreateSolidColorBrush(rgb(0), &gB);
    gRT->CreateCompatibleRenderTarget(D2D1::SizeF((float)std::max(1, gW), (float)std::max(1, gH)), &gLayer);
    if (gLayer) gLayer->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    gLayerDirty = true;
}
static bool createSwap(){
    if (gWait) { CloseHandle(gWait); gWait = nullptr; }
    SafeRelease(gSwap);
    DXGI_SWAP_CHAIN_DESC1 d{}; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.Flags = swapFlags();
    if (FAILED(gFac->CreateSwapChainForHwnd(gDev, gWnd, &d, nullptr, nullptr, &gSwap))) return false;
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(gSwap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) { sc2->SetMaximumFrameLatency(1); gWait = sc2->GetFrameLatencyWaitableObject(); sc2->Release(); }
    return true;
}
static bool initGraphics(){
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &gDev, nullptr, &gCtx))) return false;
    CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&gFac);
    if (!gFac) return false;
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(gFac->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5))) {
        BOOL t = FALSE; if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t)))) gTearing = t != FALSE;
        f5->Release();
    }
    RECT rc; GetClientRect(gWnd, &rc); gW = rc.right - rc.left; gH = rc.bottom - rc.top;
    if (!createSwap()) return false;
    gFac->MakeWindowAssociation(gWnd, DXGI_MWA_NO_ALT_ENTER);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &gD2);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&gDW);
    // 針の形 (根元6px、先2px、長さ150px。軸を原点に、上向き)
    gD2->CreatePathGeometry(&gNeedle);
    { ID2D1GeometrySink* s = nullptr; gNeedle->Open(&s);
      s->BeginFigure(D2D1::Point2F(-3, 0), D2D1_FIGURE_BEGIN_FILLED);
      s->AddLine(D2D1::Point2F(3, 0)); s->AddLine(D2D1::Point2F(1, -150)); s->AddLine(D2D1::Point2F(-1, -150));
      s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release(); }
    createTargets();
    resetView();
    return gRT != nullptr;
}
static void resize(int w, int h){
    if (!gSwap || w <= 0 || h <= 0) return;
    gW = w; gH = h;
    releaseTargets();
    gSwap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags());
    createTargets();
    resetView();
    gHz = monitorHz();
}
static void setFull(bool full){
    gFull = full;
    if (full) {
        GetWindowRect(gWnd, &gWinRect);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(gWnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(gWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(gWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(gWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(gWnd, HWND_NOTOPMOST, gWinRect.left, gWinRect.top, gWinRect.right - gWinRect.left, gWinRect.bottom - gWinRect.top, SWP_FRAMECHANGED);
    }
}

// ============================================================
//  マウス・タッチパッド・キー
// ============================================================
enum { H_NONE, H_POWER, H_MIC, H_LR, H_LL, H_GC, H_GF, H_GCR, H_GFR, H_SCOPEG, H_LRM, H_LRP, H_TABRTA, H_TABLOUD, H_START, H_RESET };
static int gDrag = H_NONE; static bool gPanning = false; static float gPanLX = 0, gPanLY = 0;
static int hitTest(float ux, float uy){
    struct { const Rc* r; int id; } list[] = { {&R_POWER,H_POWER},{&R_MIC,H_MIC},{&R_LR,H_LR},{&R_LL,H_LL},{&R_GC,H_GC},{&R_GF,H_GF},{&R_GCR,H_GCR},{&R_GFR,H_GFR},
        {&R_SCOPEG,H_SCOPEG},{&R_LRM,H_LRM},{&R_LRP,H_LRP},{&R_TABRTA,H_TABRTA},{&R_TABLOUD,H_TABLOUD},{&R_START,H_START},{&R_RESET,H_RESET} };
    for (auto& e : list) if (e.r->in(ux, uy)) return e.id;
    return H_NONE;
}
static double sliderVal(const Rc& r, float ux, double mn, double mx, double step){
    double t = std::max(0.0, std::min(1.0, (double)(ux - (r.x + 8)) / (r.w - 16)));
    double v = mn + t * (mx - mn);
    return std::max(mn, std::min(mx, jsRound((v - mn) / step) * step + mn));
}
static void dragSlider(int id, float ux){
    switch (id) {
    case H_GC: gC = sliderVal(R_GC, ux, 0, 40, 1); onMainGain(); break;
    case H_GF: gF = sliderVal(R_GF, ux, -1, 1, 0.1); onMainGain(); break;
    case H_GCR: gCR = sliderVal(R_GCR, ux, 0, 40, 1); onRGain(); break;
    case H_GFR: gFR = sliderVal(R_GFR, ux, -1, 1, 0.1); onRGain(); break;
    case H_SCOPEG: gScopeG = sliderVal(R_SCOPEG, ux, 0.1, 5.0, 0.1); break;
    }
    gLayerDirty = true;
}
static void deviceMenu(){
    listDevices();
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < (int)gDevs.size(); i++) AppendMenuW(m, MF_STRING | (i == gDevIndex ? MF_CHECKED : 0), 1000 + i, gDevs[i].name.c_str());
    if (gDevs.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 999, L"(入力機器がありません)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (gExclusive ? MF_CHECKED : 0), 2000, L"専有モード (X)");
    POINT p{ (LONG)(vX + R_MIC.x * vS), (LONG)(vY + R_MIC.b() * vS) }; ClientToScreen(gWnd, &p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, p.x, p.y, 0, gWnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + (int)gDevs.size()) { gDevIndex = cmd - 1000; if (gPower) startAudio(); }
    else if (cmd == 2000) { gExclusive = !gExclusive; if (gPower) startAudio(); }
    gLayerDirty = true;
}
static void click(int id){
    switch (id) {
    case H_POWER: setPower(!gPower); break;
    case H_MIC: deviceMenu(); break;
    case H_LR: gMono = false; break;
    case H_LL: gMono = true; break;
    case H_LRM: lrShift = std::max(-4, lrShift - 1); gLrShift = lrShift; break;
    case H_LRP: lrShift = std::min(4, lrShift + 1); gLrShift = lrShift; break;
    case H_TABRTA: rtaMode = true; break;
    case H_TABLOUD: rtaMode = false; break;
    case H_START: EnterCriticalSection(&gLoudCs); gL.run = !gL.run; gL.runBlocks = 0; LeaveCriticalSection(&gLoudCs); break;
    case H_RESET: EnterCriticalSection(&gLoudCs); loudResetLocked(); LeaveCriticalSection(&gLoudCs); break;
    }
    gLayerDirty = true;
}
static void zoomAt(float mx, float my, float f){
    float fit = fitScale();
    float ns = std::max(fit * 0.5f, std::min(fit * 8.f, vS * f)); f = ns / vS;
    vX = mx - (mx - vX) * f; vY = my - (my - vY) * f; vS = ns; gLayerDirty = true;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch (m) {
    case WM_SIZE: resize(LOWORD(l), HIWORD(l)); return 0;
    case WM_LBUTTONDOWN: {
        float mx = (float)GET_X_LPARAM(l), my = (float)GET_Y_LPARAM(l), ux = (mx - vX) / vS, uy = (my - vY) / vS;
        int id = hitTest(ux, uy);
        if (id >= H_GC && id <= H_SCOPEG) { gDrag = id; SetCapture(h); dragSlider(id, ux); }
        else if (id != H_NONE) click(id);
        else { gPanning = true; gPanLX = mx; gPanLY = my; SetCapture(h); }   // 何もない所をつかむと、表示を動かせる
        return 0; }
    case WM_MOUSEMOVE: {
        float mx = (float)GET_X_LPARAM(l), my = (float)GET_Y_LPARAM(l);
        if (gDrag) dragSlider(gDrag, (mx - vX) / vS);
        else if (gPanning) { vX += mx - gPanLX; vY += my - gPanLY; gPanLX = mx; gPanLY = my; gLayerDirty = true; }
        return 0; }
    case WM_LBUTTONUP: gDrag = H_NONE; gPanning = false; ReleaseCapture(); return 0;
    case WM_LBUTTONDBLCLK: {
        float ux = (GET_X_LPARAM(l) - vX) / vS, uy = (GET_Y_LPARAM(l) - vY) / vS;
        int id = hitTest(ux, uy);
        if (id == H_NONE) resetView(); else if (!(id >= H_GC && id <= H_SCOPEG)) click(id);
        return 0; }
    case WM_MOUSEWHEEL: {
        // タッチパッドのピンチは、Ctrl を押したホイールとして届く
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        float d = (float)GET_WHEEL_DELTA_WPARAM(w);
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) zoomAt((float)p.x, (float)p.y, std::exp(d / 120.f * std::log(1.15f)));
        else if (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) { vX += d * 0.5f; gLayerDirty = true; }
        else { vY += d * 0.5f; gLayerDirty = true; }
        return 0; }
    case WM_MOUSEHWHEEL: vX -= GET_WHEEL_DELTA_WPARAM(w) * 0.5f; gLayerDirty = true; return 0;
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: DestroyWindow(h); return 0;
        case 'F': setFull(!gFull); return 0;
        case 'X': gExclusive = !gExclusive; if (gPower) startAudio(); gLayerDirty = true; return 0;
        case '0': case VK_NUMPAD0: case VK_HOME: resetView(); return 0;
        case 'H': gHelp = !gHelp; gLayerDirty = true; return 0;
        case VK_OEM_PLUS: case VK_ADD: zoomAt(gW / 2.f, gH / 2.f, 1.15f); return 0;
        case VK_OEM_MINUS: case VK_SUBTRACT: zoomAt(gW / 2.f, gH / 2.f, 1 / 1.15f); return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&gStatusCs); InitializeCriticalSection(&gLoudCs);
    initWN(); initTP(); initRta(); onMainGain();
    timeBeginPeriod(1);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"VUMETERWIN"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    gWnd = CreateWindowW(L"VUMETERWIN", L"VU METER", WS_OVERLAPPEDWINDOW, gWinRect.left, gWinRect.top, gWinRect.right-gWinRect.left, gWinRect.bottom-gWinRect.top, nullptr, nullptr, inst, nullptr);
    if (!gWnd || !initGraphics()) { MessageBoxW(nullptr, L"画面の準備ができませんでした", L"VU METER", MB_OK); return 1; }
    ShowWindow(gWnd, SW_SHOW);
    setFull(true);
    gHz = monitorHz();
    listDevices();
    setPower(true);
    LARGE_INTEGER fq, t0, t; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); int frames = 0;
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) goto quit; TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (gWait) WaitForSingleObjectEx(gWait, 100, TRUE);
        render();
        frames++;
        QueryPerformanceCounter(&t);
        double dt = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (dt >= 0.5) { gFps = frames / dt; frames = 0; t0 = t; }
        // 描き直しは1秒240回まで
        static LONGLONG next = 0;
        LONGLONG step = fq.QuadPart / 240;
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        if (next == 0 || n.QuadPart - next > step * 4) next = n.QuadPart;
        next += step;
        for (;;) { QueryPerformanceCounter(&n); if (next - n.QuadPart <= fq.QuadPart / 2000) break; Sleep(1); }
    }
quit:
    stopAudio();
    timeEndPeriod(1);
    releaseTargets(); SafeRelease(gNeedle); SafeRelease(gSwap); SafeRelease(gCtx); SafeRelease(gDev); SafeRelease(gD2); SafeRelease(gDW);
    CoUninitialize();
    return 0;
}
