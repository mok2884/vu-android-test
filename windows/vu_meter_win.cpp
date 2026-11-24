// VU METER (Windows 版)
// 2連 (INPUT 1 の L/R) と 4連 (INPUT 1 と INPUT 2 の L/R) を切り替える。ラウドネス・TP・スコープ・RTA は INPUT 1
// 配置は Claude の Design「VUメーター 配置案」。VU の針は遅れの小さい描き方 (画面の書き換えを待たない)
// 音: WASAPI (機器の今の周波数で専有 → 低遅延の共有 → 共有)
// 画面: DXGI flip model + tearing + Direct2D
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
//  入力 (INPUT 1 / INPUT 2)。それぞれ別の機器を別のスレッドで受け取る
//  INPUT 1 は VU・ラウドネス・TP・スコープ・RTA。INPUT 2 は VU だけ
// ============================================================
static CRITICAL_SECTION gStatusCs;

// 処理したあとの INPUT 1 の音 (スコープと RTA 用)。音の側が書き、画面の側が新しいほうから読む
static const int RING = 1 << 17;
static float gRingL[RING], gRingR[RING];
static std::atomic<unsigned long long> gRingW{0};
static std::atomic<int> gSr{48000};

struct Input {
    // 音のスレッドと共有する値
    std::atomic<float> vuL{0}, vuR{0};
    std::atomic<double> gainL{1.0}, gainR{1.0};
    std::atomic<bool> mono{false};
    std::atomic<int> lrShift{0};
    std::atomic<bool> run{false};
    std::thread th;
    std::wstring shortInfo, longInfo;     // 「専有 48k」、「専有 / 48k 2ch 24bit / 1回 3.0ms」 (gStatusCs で守る)
    // 画面の値 (開くたびに初期値)
    int dev = -1;                          // gDevs の番号。-1 = なし
    bool exclusive = true;
    double c = 20, f = 0, cR = 20, fR = 0, gainLdB = 0, gainRdB = 0, gainROff = 0;
    int shift = 0;
};
static Input gIn[2];

static void setInfo(int i, const std::wstring& s, const std::wstring& l){ EnterCriticalSection(&gStatusCs); gIn[i].shortInfo = s; gIn[i].longInfo = l; LeaveCriticalSection(&gStatusCs); }
static void getInfo(int i, std::wstring& s, std::wstring& l){ EnterCriticalSection(&gStatusCs); s = gIn[i].shortInfo; l = gIn[i].longInfo; LeaveCriticalSection(&gStatusCs); }

struct DevInfo { std::wstring id, name; };
static std::vector<DevInfo> gDevs;
static const PROPERTYKEY PK_FriendlyName = { {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14 };

static void listDevices(){
    std::wstring cur[2];
    for (int i = 0; i < 2; i++) if (gIn[i].dev >= 0 && gIn[i].dev < (int)gDevs.size()) cur[i] = gDevs[gIn[i].dev].id;
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
    for (int i = 0; i < 2; i++) {
        int found = -1;
        for (int k = 0; k < (int)gDevs.size(); k++) if (!cur[i].empty() && gDevs[k].id == cur[i]) found = k;
        gIn[i].dev = found;
    }
}

static inline double readSample(const BYTE* p, bool isFloat, int bits){
    if (isFloat) return bits == 64 ? *(const double*)p : *(const float*)p;
    if (bits == 16) return *(const short*)p / 32768.0;
    if (bits == 24) { int v = (p[0] | (p[1]<<8) | (p[2]<<16)); if (v & 0x800000) v |= ~0xffffff; return v / 8388608.0; }
    if (bits == 32) return *(const int*)p / 2147483648.0;
    return 0;
}
static std::wstring kHzText(int sr){ wchar_t b[16]; if (sr % 1000 == 0) swprintf(b, 16, L"%dk", sr / 1000); else swprintf(b, 16, L"%.1fk", sr / 1000.0); return b; }
static const wchar_t* exclReason(HRESULT hr){
    switch ((unsigned)hr) {
    case 0x8889000A: return L"ほかのアプリが使用中";
    case 0x8889000E: return L"Windowsの設定で専有が許可されていない";
    case 0x88890008: return L"機器がこの形式を受け付けない";
    case 0x88890004: return L"機器が抜かれた";
    }
    return nullptr;
}

static void audioMain(int idx, std::wstring devId, bool exclusive){
    Input& in = gIn[idx];
    const bool primary = idx == 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioCaptureClient* cc = nullptr;
    WAVEFORMATEX* fmt = nullptr; WAVEFORMATEXTENSIBLE exf{}; HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::wstring mode = L"", why = L""; double periodMs = 0;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (SUCCEEDED(hr)) hr = en->GetDevice(devId.c_str(), &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&fmt);
    bool ok = false;
    if (SUCCEEDED(hr) && exclusive) {
        // 専有モード: 機器の今の周波数 (Windows の「既定の形式」、ふつうは専用ソフトで決めたもの) のまま開く。
        // ほかの周波数は試さない (機器の時計を勝手に変えないため)。24bit → 16bit の順
        const int rate = (int)fmt->nSamplesPerSec;
        REFERENCE_TIME defP = 0, minP = 0; ac->GetDevicePeriod(&defP, &minP);
        int tries[][2] = { {32,24}, {24,24}, {16,16} };
        HRESULT firstErr = S_OK;
        for (auto& t : tries) {
            ZeroMemory(&exf, sizeof(exf));
            exf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; exf.Format.nChannels = fmt->nChannels >= 2 ? 2 : 1;
            exf.Format.nSamplesPerSec = rate; exf.Format.wBitsPerSample = (WORD)t[0];
            exf.Format.nBlockAlign = exf.Format.nChannels * t[0] / 8; exf.Format.nAvgBytesPerSec = rate * exf.Format.nBlockAlign;
            exf.Format.cbSize = 22; exf.Samples.wValidBitsPerSample = (WORD)t[1];
            exf.dwChannelMask = exf.Format.nChannels == 2 ? (SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT) : SPEAKER_FRONT_CENTER;
            exf.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            REFERENCE_TIME p = minP;
            hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p, p, (WAVEFORMATEX*)&exf, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 fr = 0; ac->GetBufferSize(&fr); ac->Release(); ac = nullptr;
                p = (REFERENCE_TIME)(10000.0 * 1000 * fr / rate + 0.5);
                dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
                hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p, p, (WAVEFORMATEX*)&exf, nullptr);
            }
            if (SUCCEEDED(hr)) { ok = true; mode = L"専有"; periodMs = p / 10000.0; break; }
            if (firstErr == S_OK) firstErr = hr;
            ac->Release(); ac = nullptr; dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        }
        if (!ok) {
            const wchar_t* r = exclReason(firstErr);
            wchar_t b[96]; if (r) swprintf(b, 96, L"%ls", r); else swprintf(b, 96, L"0x%08X", (unsigned)firstErr);
            why = b; mode = L"専有できず→";
        }
    }
    const WAVEFORMATEX* use = ok ? (WAVEFORMATEX*)&exf : fmt;
    if (!ok && ac && fmt) {
        IAudioClient3* ac3 = nullptr;
        if (SUCCEEDED(ac->QueryInterface(__uuidof(IAudioClient3), (void**)&ac3))) {
            UINT32 defF=0, fund=0, minF=0, maxF=0;
            if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(fmt, &defF, &fund, &minF, &maxF)) &&
                SUCCEEDED(ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minF, fmt, nullptr))) {
                ok = true; periodMs = 1000.0 * minF / fmt->nSamplesPerSec; mode += L"共有";
            }
            ac3->Release();
        }
        if (!ok) {
            hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, fmt, nullptr);
            if (SUCCEEDED(hr)) { ok = true; REFERENCE_TIME d=0,m=0; ac->GetDevicePeriod(&d,&m); periodMs = d/10000.0; mode += L"共有"; }
        }
        use = fmt;
    }
    if (ok) ok = SUCCEEDED(ac->SetEventHandle(ev)) && SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc));
    if (!ok) { setInfo(idx, L"開けません", L"入力を開けませんでした"); goto done; }
    {
        const int ch = use->nChannels, sr = use->nSamplesPerSec, bits = use->wBitsPerSample, stride = use->nBlockAlign;
        bool isFloat = use->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        int valid = bits;
        if (use->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)use;
            isFloat = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            valid = x->Samples.wValidBitsPerSample;
        }
        if (primary) gSr = sr;
        {
            wchar_t s[64], l[256];
            std::wstring shortMode = mode.find(L"専有できず") != std::wstring::npos ? L"共有" : mode;
            swprintf(s, 64, L"%ls %ls", shortMode.c_str(), kHzText(sr).c_str());
            if (!why.empty()) swprintf(l, 256, L"専有できず (%ls) → 共有 / %ls %dch %dbit%ls / 1回 %.1fms", why.c_str(), kHzText(sr).c_str(), ch, valid, isFloat ? L"f" : L"", periodMs);
            else swprintf(l, 256, L"%ls / %ls %dch %dbit%ls / 1回 %.1fms", mode.c_str(), kHzText(sr).c_str(), ch, valid, isFloat ? L"f" : L"", periodMs);
            setInfo(idx, s, l);
        }
        DWORD task = 0; HANDLE avrt = nullptr;
        typedef HANDLE (WINAPI *AvSetFn)(LPCWSTR, LPDWORD);
        HMODULE hav = LoadLibraryW(L"avrt.dll");
        if (hav) { AvSetFn f = (AvSetFn)GetProcAddress(hav, "AvSetMmThreadCharacteristicsW"); if (f) avrt = f(L"Pro Audio", &task); }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        // VU (1msごとに針の式を進める)
        const int blk = std::max(1, sr / 1000);
        const double h = (double)blk / sr, k1 = VU_WN*VU_WN, k2 = 2*VU_ZETA*VU_WN;
        double yL=0,vL=0,yR=0,vR=0,sL=0,sR=0; int n=0;
        // ラウドネス (10msごと)。INPUT 1 だけ
        double shB[3], shA[2], hpB[3], hpA[2]; makeKWeight(sr, shB, shA, hpB, hpA);
        Biquad kShL, kShR, kHpL, kHpR; kShL.set(shB, shA); kShR.set(shB, shA); kHpL.set(hpB, hpA); kHpR.set(hpB, hpA);
        const int subN = (int)std::lround(sr * 0.01);
        double subSum = 0, subTp = 0; int subCnt = 0;
        TpState tpL, tpR;
        if (primary) { EnterCriticalSection(&gLoudCs); gL.ringN = 0; gL.ringPos = 0; gL.tick = 0; gL.runBlocks = 0; LeaveCriticalSection(&gLoudCs); }
        // L/R 時間補正 (最大4サンプル)
        double hL[8] = {}, hR[8] = {}; int hp = 0;
        unsigned long long w = gRingW.load();

        ac->Start();
        while (in.run.load()) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            UINT32 pk = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk > 0) {
                BYTE* data = nullptr; UINT32 fr = 0; DWORD fl = 0;
                if (FAILED(cc->GetBuffer(&data, &fr, &fl, nullptr, nullptr))) break;
                const bool silent = (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const double gl = in.gainL.load(), gr = in.gainR.load();
                const bool mono = in.mono.load();
                const int shift = in.lrShift.load();
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
                    sL += std::fabs(l); sR += std::fabs(r);
                    if (++n >= blk) {
                        vL += (k1*(sL/n - yL) - k2*vL)*h; yL += vL*h;
                        vR += (k1*(sR/n - yR) - k2*vR)*h; yR += vR*h;
                        sL=sR=0; n=0;
                    }
                    if (primary) {
                        gRingL[w & (RING-1)] = (float)l; gRingR[w & (RING-1)] = (float)r; w++;
                        double kl = kHpL.run(kShL.run(l)), kr = kHpR.run(kShR.run(r));
                        subSum += kl*kl + kr*kr;
                        double t = std::max(tpL.run(l), tpR.run(r)); if (t > subTp) subTp = t;
                        if (++subCnt >= subN) { loudPush(subSum / subCnt, subTp); subSum = 0; subTp = 0; subCnt = 0; }
                    }
                }
                cc->ReleaseBuffer(fr);
                if (primary) gRingW.store(w);
                in.vuL.store((float)std::max(0.0, yL)); in.vuR.store((float)std::max(0.0, yR));
            }
        }
        ac->Stop();
        if (hav) { typedef BOOL (WINAPI *AvRevFn)(HANDLE); AvRevFn rf = (AvRevFn)GetProcAddress(hav, "AvRevertMmThreadCharacteristics"); if (rf && avrt) rf(avrt); FreeLibrary(hav); }
    }
done:
    SafeRelease(cc); SafeRelease(ac); SafeRelease(dev); SafeRelease(en);
    if (fmt) CoTaskMemFree(fmt);
    CloseHandle(ev);
    in.vuL = 0; in.vuR = 0;
    CoUninitialize();
}

static bool gPower = false, g4 = false;
static void stopInput(int i){ gIn[i].run = false; if (gIn[i].th.joinable()) gIn[i].th.join(); gIn[i].vuL = 0; gIn[i].vuR = 0; }
static void startInput(int i){
    stopInput(i);
    if (!gPower || (i == 1 && !g4)) { setInfo(i, L"", L""); return; }
    if (gIn[i].dev < 0 || gIn[i].dev >= (int)gDevs.size()) { setInfo(i, L"", i == 0 ? L"入力機器がありません" : L"機器を選んでください"); return; }
    setInfo(i, L"", L"");
    gIn[i].run = true;
    gIn[i].th = std::thread(audioMain, i, gDevs[gIn[i].dev].id, gIn[i].exclusive);
}
static void setPower(bool on){ gPower = on; startInput(0); startInput(1); }

// レベル (HTML 版と同じ: 左のスライダーは L と R の両方、右は R だけ)
static double jsRound(double v){ return std::floor(v + 0.5); }
static void applyGains(int i){ gIn[i].gainL = std::pow(10.0, gIn[i].gainLdB/20); gIn[i].gainR = std::pow(10.0, gIn[i].gainRdB/20); }
static void setRSliders(Input& in, double db){
    db = std::max(-21.0, std::min(21.0, db));
    double c = std::max(-20.0, std::min(20.0, jsRound(db)));
    double f = std::max(-1.0, std::min(1.0, jsRound((db - c) * 10) / 10));
    in.cR = c + 20; in.fR = f;
}
static void onMainGain(int i){ Input& in = gIn[i]; in.gainLdB = (in.c - 20) + in.f; setRSliders(in, in.gainLdB + in.gainROff); in.gainRdB = (in.cR - 20) + in.fR; applyGains(i); }
static void onRGain(int i){ Input& in = gIn[i]; in.gainRdB = (in.cR - 20) + in.fR; in.gainROff = in.gainRdB - in.gainLdB; applyGains(i); }

// ============================================================
//  画面の配置 (Design の「VUメーター 配置案」と同じ。1440x900 の枠の中の位置、単位は px)
// ============================================================
struct Rc { float x, y, w, h; float r() const { return x + w; } float b() const { return y + h; } bool in(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; } };
static const float PANEL_W = 1440, PANEL_H = 900;
static const Rc P_TOP{20,20,1400,60};
// 2連
static const Rc P_MET2{20,94,1400,360}, P_BOT2{20,468,1400,412};
static const Rc VU2[2] = { {68,202,440,228}, {540,202,440,228} };
static const Rc SCOPE2{1012,170,260,260}, SCOPEG2{1172,151,70,16};
static const Rc TRACK2[2] = { {1306,210,24,220}, {1350,210,24,220} };
static const Rc RTAWIN2{44,530,1352,334};
// 4連
static const Rc P_MET4{20,94,1400,226}, P_BOT4{20,334,1400,546};
static const Rc VU4[4] = { {60,152,286,148}, {360,152,286,148}, {692,152,286,148}, {992,152,286,148} };
static const Rc SCOPE4{44,382,330,330}, SCOPEG4{234,356,70,16};
static const Rc TRACK4[2] = { {1324,172,20,128}, {1360,172,20,128} };
static const Rc RTAWIN4{398,394,998,470};
// 上の帯
static const Rc B_POWER{42,32,120,36}, B_2{176,34,52,32}, B_4{232,34,72,32}, B_SET{312,34,100,32};
// 入力の設定の窓 (2連でも4連でも同じ。左は開けておく)
static const Rc W_SET{404,566,1016,310};
static const Rc W_CLOSE{1380,570,32,32};

static double gScopeG = 1.0;
static bool rtaMode = true;      // true = RTA, false = LOUDNESS の履歴
static bool gSet = false;        // 入力の設定の窓

// ============================================================
//  画面 (DXGI / Direct2D)
// ============================================================
static HWND gWnd; static bool gFull = true; static RECT gWinRect = {80,60,1500,960};
static ID3D11Device* gDev = nullptr; static ID3D11DeviceContext* gCtx = nullptr;
static IDXGISwapChain1* gSwap = nullptr; static bool gTearing = false;
static HANDLE gWait = nullptr;
static IDXGIFactory2* gFac = nullptr;
static UINT swapFlags(){ return (gTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; }
static ID2D1Factory* gD2 = nullptr; static ID2D1RenderTarget* gRT = nullptr; static IDWriteFactory* gDW = nullptr;
static ID2D1BitmapRenderTarget* gLayer = nullptr;   // 針以外の全部 (1秒60回描き直す)
static ID2D1BitmapRenderTarget* gScope = nullptr;   // スコープ (残像を残すので、消さずに描き足す)
static ID2D1Bitmap* gFace = nullptr;                // VU の文字盤
static ID2D1SolidColorBrush* gB = nullptr;
static ID2D1PathGeometry* gNeedle = nullptr;
static int gW = 0, gH = 0;
static float gFaceKey = 0, gScopeKey = 0;
static bool gLayerDirty = true;
static double gFps = 0; static int gHz = 60;
static double gWorkMs = 0, gLayerMs = 0;   // 1回の描画にかかった時間、針以外の部分を描くのにかかった時間 (ms、ならし)
static bool heavy(){ return gFps > 0 && gWorkMs > 1000.0 / gFps * 0.6; }   // 描くのに時間がかかって回数が出ていない
static bool gHelp = true;

static float vS = 1, vX = 0, vY = 0;
static float fitScale(){ return std::min(gW / PANEL_W, gH / PANEL_H); }
static void resetView(){ vS = fitScale(); vX = (gW - PANEL_W * vS) / 2; vY = (gH - PANEL_H * vS) / 2; gLayerDirty = true; }
static D2D1_MATRIX_3X2_F viewM(){ return D2D1::Matrix3x2F(vS, 0, 0, vS, vX, vY); }

static D2D1_COLOR_F rgb(unsigned c, float a = 1){ return D2D1::ColorF(((c>>16)&255)/255.f, ((c>>8)&255)/255.f, (c&255)/255.f, a); }
static ID2D1SolidColorBrush* br(unsigned c, float a = 1){ gB->SetColor(rgb(c, a)); return gB; }

struct Fmt { float size; bool bold; int align; bool trim; IDWriteTextFormat* f; };
static std::vector<Fmt> gFmts;
static IDWriteTextFormat* fmt(float size, bool bold, int align, bool trim = false){
    for (auto& x : gFmts) if (x.size == size && x.bold == bold && x.align == align && x.trim == trim) return x.f;
    IDWriteTextFormat* f = nullptr;
    gDW->CreateTextFormat(FONT_FAMILY, nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", &f);
    f->SetTextAlignment(align == 0 ? DWRITE_TEXT_ALIGNMENT_LEADING : align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
static void textT(ID2D1RenderTarget* rt, const wchar_t* s, float size, bool bold, int align, float x, float top, ID2D1Brush* b, float maxW = 0){
    textB(rt, s, size, bold, align, x, top + size * 0.921f, b, maxW);
}
static std::vector<std::pair<std::wstring, float>> gTW;   // 文字の幅を覚えておく (毎回はかると重い)
static float textWidth(const wchar_t* s, float size, bool bold){
    std::wstring key = std::wstring(s) + (bold ? L"\x01" : L"\x02") + std::to_wstring((int)(size * 10));
    for (auto& e : gTW) if (e.first == key) return e.second;
    IDWriteTextLayout* lay = nullptr; float w = 0;
    if (SUCCEEDED(gDW->CreateTextLayout(s, (UINT32)wcslen(s), fmt(size, bold, 0), 4000, 100, &lay))) { DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; lay->Release(); }
    if (gTW.size() > 400) gTW.clear();
    gTW.push_back({key, w});
    return w;
}
static D2D1_RECT_F RF(const Rc& r){ return D2D1::RectF(r.x, r.y, r.r(), r.b()); }
static void fillRR(ID2D1RenderTarget* rt, const Rc& r, float rad, unsigned c, float a = 1){ rt->FillRoundedRectangle(D2D1::RoundedRect(RF(r), rad, rad), br(c, a)); }
static void strokeRR(ID2D1RenderTarget* rt, const Rc& r, float rad, float w, unsigned c){
    Rc q{ r.x + w/2, r.y + w/2, r.w - w, r.h - w }; rt->DrawRoundedRectangle(D2D1::RoundedRect(RF(q), rad, rad), br(c), w);
}
static void panel(ID2D1RenderTarget* rt, const Rc& r){ fillRR(rt, r, 12, 0x1a1a1a); strokeRR(rt, r, 12, 2, 0x333333); }

// ===== 押せる場所の一覧 (描くたびに作り直す) =====
enum { H_NONE, H_POWER, H_2, H_4, H_SET, H_CLOSE, H_TABRTA, H_TABLOUD, H_START, H_RESET, H_SCOPEG,
       H_DEV, H_LR, H_LL, H_EXCL, H_C, H_F, H_CR, H_FR, H_LRM, H_LRP, H_WINDOW };
struct Hit { Rc r; int id; int in; };
static std::vector<Hit> gHits;
static int gWinHitStart = -1;    // 設定の窓の部品が gHits の何番目から始まるか (-1 = 窓なし)
static void hit(const Rc& r, int id, int in = 0){ gHits.push_back({r, id, in}); }
static bool isSlider(int id){ return id == H_SCOPEG || id == H_C || id == H_F || id == H_CR || id == H_FR; }

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

// 窓の中身の幅 (枠の1pxを除く) が wDesign のときの文字盤
static void buildFace(float wDesign){
    SafeRelease(gFace);
    float k = (wDesign - 2) / VU_W * vS;
    ID2D1BitmapRenderTarget* bt = nullptr;
    if (FAILED(gRT->CreateCompatibleRenderTarget(D2D1::SizeF(VU_W * k, VU_H * k), &bt))) return;
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
    gFaceKey = wDesign * vS;
}

// ===== スコープ (HTML 版と同じ描き方) =====
static const int SCOPE_POINTS = 1024; static const float SCOPE_SCALE = 160, SCOPE_FADE = 0.18f, SCOPE_DOT_ALPHA = 0.5f, SCOPE_BEAM_LEN = 1.5f;
static void buildScope(float wDesign){
    SafeRelease(gScope);
    float px = std::max(220.f, std::round((wDesign - 2) * vS));
    if (FAILED(gRT->CreateCompatibleRenderTarget(D2D1::SizeF(px, px), &gScope))) return;
    gScope->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    gScope->BeginDraw(); gScope->Clear(rgb(0)); gScope->EndDraw();
    gScopeKey = wDesign * vS;
}
static void drawScope(){
    if (!gScope) return;
    float P = gScope->GetSize().width, RES = P / 220;
    static float buf[2][SCOPE_POINTS];
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

// ===== RTA (HTML 版と同じ計算。窓の大きさに合わせて横に広げる) =====
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
static double rtaSnap(double f){ return 1000 * std::pow(2.0, jsRound(3 * std::log2(f / 1000)) / 3); }

// W x H の窓に描く。周波数の数字は下から15px、バーは上から H-40 まで
static void drawRta(ID2D1RenderTarget* rt, float W, float H){
    auto freqX = [&](double f){ return (float)(30 + std::log10(f / 20) / std::log10(20000.0 / 20) * (W - 60)); };
    const float yB = H - 40;
    const double now = nowMs(), sr = gSr.load();
    bool hiNew = false, loNew = false;
    if (gPower) {
        if (now - rtaHiT >= 30) { rtaHiT = now; gFftHi.run(); hiNew = true; }
        if (now - rtaLoT >= 100) { rtaLoT = now; gFftLo.run(); loNew = true; }
    }
    const struct { double f; const wchar_t* t; } labels[] = { {20,L"20"},{31.5,L"31.5"},{63,L"63"},{125,L"125"},{250,L"250"},{500,L"500"},{1000,L"1k"},{2000,L"2k"},{4000,L"4k"},{8000,L"8k"},{16000,L"16k"} };
    for (int i = 1; i < 11; i++) {
        float x = freqX(rtaSnap(labels[i].f));
        rt->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, yB), br(0xffffff, labels[i].f == 1000 ? 0.25f : 0.12f), 1);
    }
    double dt = std::min(0.1, (now - rtaLastT) / 1000); rtaLastT = now;
    float step = (float)((W - 60) / (std::log2(1000.0) * RTA_BANDS_PER_OCT)), barW = std::max(1.f, step * 0.8f);
    for (auto& b : gBands) {
        bool low = b.fc < RTA_SPLIT_HZ;
        if (low ? loNew : hiNew) b.raw = low ? gFftLo.band(b.f1, b.f2, sr) : gFftHi.band(b.f1, b.f2, sr);
        if (!gPower) b.raw = 0;
        double tau = b.raw > b.p ? RTA_ATTACK : RTA_RELEASE;
        b.p += (b.raw - b.p) * (1 - std::exp(-dt / tau));
        double db = 10 * std::log10(b.p + 1e-12);
        float h = (float)std::max(0.0, std::min((double)yB, (db - RTA_BOTTOM) / (RTA_TOP - RTA_BOTTOM) * yB));
        float x = freqX(b.fc);
        if (h > 0) rt->FillRectangle(D2D1::RectF(x - barW/2, yB - h, x + barW/2, yB), br(0x777777));
    }
    for (int k = -17; k <= 13; k++) {
        float x = std::round(freqX(1000 * std::pow(2.0, k / 3.0))) + 0.5f;
        rt->DrawLine(D2D1::Point2F(x, yB + 1), D2D1::Point2F(x, k % 3 == 0 ? yB + 9 : yB + 5), br(0xffffff, 0.4f), 1);
    }
    for (auto& l : labels) textB(rt, l.t, 12, false, 1, freqX(rtaSnap(l.f)), H - 15, br(0xaaaaaa));
}

static const struct { double lk; unsigned c; } LOUD_COLORS[] = {
    {-49,0x2d7d96},{-43,0x37a55f},{-35,0x55af55},{-28,0x96c33c},{-24,0xe8ca2d},{-19,0xecaa2d},{-14,0xde3a2a} };
struct GradCache { ID2D1RenderTarget* rt; float yBot, yTop; ID2D1LinearGradientBrush* g; };
static std::vector<GradCache> gGrads;
static void releaseGrads(){ for (auto& c : gGrads) SafeRelease(c.g); gGrads.clear(); }
static ID2D1LinearGradientBrush* makeLoudGrad(ID2D1RenderTarget* rt, float yBot, float yTop);
// 同じ形のグラデーションは作り置きを使う (毎回作ると重い)
static ID2D1LinearGradientBrush* loudGrad(ID2D1RenderTarget* rt, float yBot, float yTop){
    for (auto& c : gGrads) if (c.rt == rt && c.yBot == yBot && c.yTop == yTop) return c.g;
    ID2D1LinearGradientBrush* g = makeLoudGrad(rt, yBot, yTop);
    gGrads.push_back({rt, yBot, yTop, g});
    return g;
}
static ID2D1LinearGradientBrush* makeLoudGrad(ID2D1RenderTarget* rt, float yBot, float yTop){
    D2D1_GRADIENT_STOP st[7];
    for (int i = 0; i < 7; i++) { st[i].position = (float)std::max(0.0, std::min(1.0, (LOUD_COLORS[i].lk - LOUD_MIN) / (LOUD_MAX - LOUD_MIN))); st[i].color = rgb(LOUD_COLORS[i].c); }
    ID2D1GradientStopCollection* col = nullptr; rt->CreateGradientStopCollection(st, 7, &col);
    ID2D1LinearGradientBrush* g = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, yBot), D2D1::Point2F(0, yTop)), col, &g);
    SafeRelease(col); return g;
}
static void drawLoudHistory(ID2D1RenderTarget* rt, const std::vector<float>& hist, double S, float W, float H){
    const float x1 = W - 10, x0 = 50, pitch = (x1 - x0) / LOUD_HIST_BARS, yTop = 10, yBot = H - 40;
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
    rt->FillGeometry(g, grad); g->Release();
    rt->DrawLine(D2D1::Point2F(x0, yOf(LOUD_TARGET)), D2D1::Point2F(x1, yOf(LOUD_TARGET)), br(0xffffff, 0.8f), 1);
    textB(rt, L"-24", 12, false, 2, x0 - 6, yOf(LOUD_TARGET) + 4, br(0xffffff));
    for (int m = 0; m <= LOUD_HIST_MIN; m += 3) {
        wchar_t t[16]; if (m == 0) wcscpy(t, L"0"); else swprintf(t, 16, L"-%d分", m);
        textB(rt, t, 12, false, 1, x1 - m * 60 / LOUD_BAR_SEC * pitch, H - 15, br(0xaaaaaa));
    }
}

// ===== 操作部品 =====
static void drawRange(ID2D1RenderTarget* rt, const Rc& r, double t, bool grey){
    float cy = r.y + r.h/2, x0 = r.x + 8, x1 = r.r() - 8, tx = x0 + (float)std::max(0.0, std::min(1.0, t)) * (x1 - x0);
    unsigned fillC = grey ? 0x5a5a5a : 0x0075ff, restC = grey ? 0x9a9a9a : 0xefefef;
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + 1, cy - 3, r.r() - 1, cy + 3), 3, 3), br(restC));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x + 1, cy - 3, tx, cy + 3), 3, 3), br(fillC));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 7, 7), br(fillC));
}
static void drawKbd(ID2D1RenderTarget* rt, const wchar_t* k, float x, float cy){
    float w = textWidth(k, 10, true) + 8;
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy - 7, x + w, cy + 7), 3, 3), br(0x666666), 1);
    textB(rt, k, 10, true, 1, x + w/2, cy + 3.5f, br(0xaaaaaa));
}
// ボタンの文字 (キーの印つき)。キーの印は文字の右
static void btnLabel(ID2D1RenderTarget* rt, const Rc& r, const wchar_t* s, float size, unsigned col, const wchar_t* kbd){
    float tw = textWidth(s, size, true), kw = kbd ? textWidth(kbd, 10, true) + 8 + 6 : 0;
    float x = r.x + (r.w - tw - kw) / 2, cy = r.y + r.h / 2;
    textB(rt, s, size, true, 0, x, cy + size * 0.3465f, br(col));
    if (kbd) drawKbd(rt, kbd, x + tw + 6, cy);
}
static void drawPBtn(ID2D1RenderTarget* rt, const Rc& r, const wchar_t* s, bool on, const wchar_t* kbd = nullptr){
    fillRR(rt, r, 6, on ? 0x0a7a3c : 0x2a2a2a); strokeRR(rt, r, 6, 1, on ? 0x16c060 : 0x555555);
    btnLabel(rt, r, s, 12, on ? 0xffffff : 0xdddddd, kbd);
}
static void drawSelBtn(ID2D1RenderTarget* rt, const Rc& r, const wchar_t* s, bool on, const wchar_t* kbd = nullptr, bool green = false){
    fillRR(rt, r, 4, green ? 0x0a7a3c : on ? 0x555555 : 0x333333); strokeRR(rt, r, 4, 1, green ? 0x16c060 : on ? 0x999999 : 0x555555);
    btnLabel(rt, r, s, 13, (on || green) ? 0xffffff : 0xaaaaaa, kbd);
}
static void drawCheck(ID2D1RenderTarget* rt, float x, float y, bool on){
    Rc r{x, y, 16, 16};
    if (on) { fillRR(rt, r, 3, 0x0a7a3c); rt->DrawLine(D2D1::Point2F(x + 3.5f, y + 8.5f), D2D1::Point2F(x + 6.5f, y + 11.5f), br(0xffffff), 2); rt->DrawLine(D2D1::Point2F(x + 6.5f, y + 11.5f), D2D1::Point2F(x + 12.5f, y + 4.5f), br(0xffffff), 2); }
    else { fillRR(rt, r, 3, 0x333333); strokeRR(rt, r, 3, 1, 0x777777); }
}
static void drawDropdown(ID2D1RenderTarget* rt, const Rc& r, const wchar_t* s){
    fillRR(rt, r, 4, 0x333333); strokeRR(rt, r, 4, 1, 0x555555);
    textB(rt, s, 14, false, 0, r.x + 9, r.y + r.h/2 + 14 * 0.3465f, br(0xffffff), r.w - 34);
    float ax = r.r() - 16, ay = r.y + r.h/2;
    rt->DrawLine(D2D1::Point2F(ax - 4, ay - 2), D2D1::Point2F(ax, ay + 2), br(0xffffff), 1.6f);
    rt->DrawLine(D2D1::Point2F(ax, ay + 2), D2D1::Point2F(ax + 4, ay - 2), br(0xffffff), 1.6f);
}
static void badge(ID2D1RenderTarget* rt, float x, float y, int shift){
    wchar_t s[32]; swprintf(s, 32, shift > 0 ? L"L/R補正 +%d" : L"L/R補正 %d", shift);
    float tw = textWidth(s, 11, true);
    Rc bd{ x, y, tw + 12, 17 };
    fillRR(rt, bd, 3, 0xe8c430);
    textB(rt, s, 11, true, 0, x + 6, y + 12.5f, br(0x1a1400));
}

static int monitorHz(){
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(MonitorFromWindow(gWnd, MONITOR_DEFAULTTONEAREST), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) return (int)dm.dmDisplayFrequency;
    return 60;
}
static double waitThreshold(){ return std::min(gHz * 1.5, 200.0); }
// 電池かどうか (調べるのに時間がかかることがあるので、2秒に1回だけ調べる)
static bool onBattery(){
    static double last = -1e9; static bool bat = false;
    double now = nowMs();
    if (now - last > 2000) { last = now; SYSTEM_POWER_STATUS ps; bat = GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0; }
    return bat;
}

// ===== 部品のまとまり =====
static std::wstring devName(int i){ return (gIn[i].dev >= 0 && gIn[i].dev < (int)gDevs.size()) ? gDevs[gIn[i].dev].name : (i == 0 ? L"-- 入力機器なし --" : L"（なし）"); }
static std::wstring dbText(double v){ wchar_t b[24]; swprintf(b, 24, L"%+.1f dB", std::fabs(v) < 0.05 ? 0.0 : v); return b; }

// 上の帯に1つの入力のあらましを書く
static void statusLine(ID2D1RenderTarget* rt, int i, float base){
    std::wstring s, l; getInfo(i, s, l);
    textB(rt, i == 0 ? L"INPUT 1" : L"INPUT 2", 12, true, 0, 440, base, br(0x3ddc5a));
    textB(rt, devName(i).c_str(), 12, false, 0, 500, base, br(0xeeeeee), 230);
    std::wstring info = s.empty() ? (gPower ? L"--" : L"OFF") : s;
    info += gIn[i].mono ? L" · LL" : L" · LR";
    textB(rt, info.c_str(), 12, false, 0, 740, base, br(0x8a8a8a), 120);
    std::wstring g = L"L " + dbText(gIn[i].gainLdB).substr(0, dbText(gIn[i].gainLdB).size() - 3) + L" / R " + dbText(gIn[i].gainRdB);
    textB(rt, g.c_str(), 12, false, 0, 866, base, br(0xbbbbbb));
    if (gIn[i].shift != 0) badge(rt, 1010, base - 13, gIn[i].shift);
}

static void loudBars(ID2D1RenderTarget* rt, const Rc* tr, double M, double S, float nameTop, float valTop, float valSize){
    const wchar_t* nm[2] = { L"M", L"S" }; double val[2] = { M, S };
    ID2D1LinearGradientBrush* grad = loudGrad(rt, tr[0].b(), tr[0].y);
    for (int i = 0; i < 2; i++) {
        const Rc& t = tr[i]; float cx = t.x + t.w / 2;
        textT(rt, nm[i], 12, false, 1, cx, nameTop, br(0xaaaaaa));
        wchar_t s[16]; if (val[i] < -99) wcscpy(s, L"--.-"); else swprintf(s, 16, L"%.1f", val[i]);
        textT(rt, s, valSize, false, 1, cx, valTop, br(0xdddddd));
        rt->FillRectangle(RF(t), br(0x222222));
        double pct = std::max(0.0, std::min(1.0, (val[i] - LOUD_MIN) / (LOUD_MAX - LOUD_MIN)));
        if (pct > 0) {
            rt->SetTransform(D2D1::Matrix3x2F::Translation(t.x - tr[0].x, 0) * viewM());
            rt->FillRectangle(D2D1::RectF(tr[0].x, (float)(t.b() - pct * t.h), tr[0].r(), t.b()), grad);
            rt->SetTransform(viewM());
        }
        float ty = (float)(t.b() - (LOUD_TARGET - LOUD_MIN) / (LOUD_MAX - LOUD_MIN) * t.h);
        rt->FillRectangle(D2D1::RectF(t.x - 4, ty - 1, t.r() + 4, ty + 1), br(0xffffff));
    }
}

// タブ、TP、積算、START/STOP/RESET の1列。x0 = 左端、y = 上端
static void analysisRow(ID2D1RenderTarget* rt, float x0, float y, double tpRecent, double tpMax, bool hasI, double I, bool run, float lkSize){
    Rc tR{x0, y, 110, 32}, tL{x0 + 116, y, 140, 32};
    drawPBtn(rt, tR, L"RTA", rtaMode); drawPBtn(rt, tL, L"LOUDNESS", !rtaMode, L"R/L");
    hit(tR, H_TABRTA); hit(tL, H_TABLOUD);
    double lin[2] = { tpRecent, tpMax }; const wchar_t* lab[2] = { L"TP 10s", L"TP MAX" };
    for (int i = 0; i < 2; i++) {
        double db = lin[i] > 1e-6 ? 20*std::log10(lin[i]) : -1e9;
        wchar_t s[16]; if (db < -70) wcscpy(s, L"--.-"); else swprintf(s, 16, L"%.1f", db);
        unsigned col = db > 0 ? 0xc0504d : db > -1 ? 0xc9a227 : 0xcccccc;
        float base = y + 13 + i * 16;
        textB(rt, lab[i], 12, false, 0, x0 + 272, base, br(0x8a8a8a));
        textB(rt, s, 12, false, 2, x0 + 366, base, br(col));
        textB(rt, L" dBTP", 12, false, 0, x0 + 366, base, br(0x8a8a8a));
    }
    // スペースキーは、止まっているときは START、動いているときは RESET
    Rc rs{1276, y, 120, 32};
    Rc st = run ? Rc{1188, y, 80, 32} : Rc{1146, y, 122, 32};
    if (run) drawPBtn(rt, st, L"STOP", true); else drawPBtn(rt, st, L"START", false, L"SPACE");
    drawPBtn(rt, rs, L"RESET", false, run ? L"SPACE" : nullptr);
    hit(st, H_START); hit(rs, H_RESET);
    float right = st.x - 12;
    textB(rt, L"INTEGRATED", 11, false, 2, right, y - 1, br(0xaaaaaa));
    wchar_t s[16]; if (!hasI) wcscpy(s, L"--.-"); else swprintf(s, 16, L"%.1f", I);
    float lw = textWidth(L" LKFS", lkSize * 0.4f, false);
    textB(rt, s, lkSize, true, 2, right - lw, y + 30, br(0x3ddc5a));
    textB(rt, L" LKFS", lkSize * 0.4f, false, 0, right - lw, y + 30, br(0x3ddc5a));
}

static void rtaWindow(ID2D1RenderTarget* rt, const Rc& w, const std::vector<float>& hist, double S){
    rt->FillRectangle(RF(w), br(0x050505)); strokeRR(rt, w, 0, 1, 0x444444);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(w.x + 1, w.y + 1) * viewM());
    if (rtaMode) drawRta(rt, w.w - 2, w.h - 2); else drawLoudHistory(rt, hist, S, w.w - 2, w.h - 2);
    rt->SetTransform(viewM());
}

static void scopeBox(ID2D1RenderTarget* rt, const Rc& sc, const Rc& sg, float titleSize, float titleTop, bool showIn1){
    textT(rt, L"PHASE SCOPE", titleSize, true, 0, sc.x, titleTop, br(0xeeeeee));
    if (showIn1) textT(rt, L"INPUT 1", 11, false, 0, sc.x + textWidth(L"PHASE SCOPE", titleSize, true) + 8, titleTop + 4, br(0x8a8a8a));
    drawRange(rt, sg, (gScopeG - 0.1) / 4.9, false); hit(sg, H_SCOPEG);
    wchar_t t[16]; swprintf(t, 16, L"x%.1f", gScopeG); textT(rt, t, 12, false, 2, sc.r(), titleTop + 3, br(0x8a8a8a));
    fillRR(rt, sc, 4, 0x000000);
    if (gScope) { ID2D1Bitmap* b = nullptr; gScope->GetBitmap(&b); if (b) { rt->DrawBitmap(b, D2D1::RectF(sc.x + 1, sc.y + 1, sc.r() - 1, sc.b() - 1)); b->Release(); } }
    strokeRR(rt, sc, 4, 1, 0x444444);
    if (gIn[0].shift != 0) badge(rt, sc.x + 7, sc.y + 6, gIn[0].shift);
}

static void vuBox(ID2D1RenderTarget* rt, const Rc& m){
    if (gFace) rt->DrawBitmap(gFace, D2D1::RectF(m.x + 1, m.y + 1, m.r() - 1, m.b() - 1));
    strokeRR(rt, m, 4, 1, 0x444444);
}

// 入力の設定の窓の、1つの入力のカード
static void inputCard(ID2D1RenderTarget* rt, int i, float cx, float cy){
    Input& in = gIn[i];
    Rc card{cx, cy, 482, 242};
    fillRR(rt, card, 8, 0x181818); strokeRR(rt, card, 8, 1, 0x3a3a3a);
    textB(rt, i == 0 ? L"INPUT 1" : L"INPUT 2", 13, true, 0, cx + 16, cy + 27, br(0x3ddc5a));
    std::wstring s, l; getInfo(i, s, l);
    if (i == 1 && !g4) l = L"4連のときに使う";
    textB(rt, l.c_str(), 11, false, 2, cx + 466, cy + 27, br(0x8a8a8a), 360);
    textB(rt, L"機器", 12, false, 0, cx + 16, cy + 60, br(0xaaaaaa));
    Rc dd{cx + 74, cy + 40, 392, 32}; drawDropdown(rt, dd, devName(i).c_str()); hit(dd, H_DEV, i);
    textB(rt, L"入れ方", 12, false, 0, cx + 16, cy + 102, br(0xaaaaaa));
    Rc lr{cx + 74, cy + 82, 44, 32}, ll{cx + 124, cy + 82, 44, 32};
    drawSelBtn(rt, lr, L"LR", !in.mono); drawSelBtn(rt, ll, L"LL", in.mono); hit(lr, H_LR, i); hit(ll, H_LL, i);
    drawCheck(rt, cx + 192, cy + 90, in.exclusive); textB(rt, L"専有", 12, false, 0, cx + 214, cy + 103, br(0xcccccc));
    hit(Rc{cx + 186, cy + 82, 70, 32}, H_EXCL, i);
    textB(rt, L"L / R", 12, false, 0, cx + 16, cy + 140, br(0xaaaaaa));
    Rc c1{cx + 74, cy + 128, 238, 16}, f1{cx + 320, cy + 128, 64, 16};
    drawRange(rt, c1, in.c / 40, false); drawRange(rt, f1, (in.f + 1) / 2, false); hit(c1, H_C, i); hit(f1, H_F, i);
    textB(rt, dbText(in.gainLdB).c_str(), 12, false, 0, cx + 394, cy + 140, br(0xcccccc));
    textB(rt, L"R だけ", 12, false, 0, cx + 16, cy + 168, br(0xaaaaaa));
    Rc c2{cx + 74, cy + 156, 238, 16}, f2{cx + 320, cy + 156, 64, 16};
    drawRange(rt, c2, in.cR / 40, true); drawRange(rt, f2, (in.fR + 1) / 2, true); hit(c2, H_CR, i); hit(f2, H_FR, i);
    textB(rt, (L"R " + dbText(in.gainRdB)).c_str(), 12, false, 0, cx + 394, cy + 168, br(0x9a9a9a));
    bool act = in.shift != 0;
    textB(rt, L"L/R補正", 12, act, 0, cx + 16, cy + 210, br(act ? 0xe8c430 : 0xaaaaaa));
    Rc m{cx + 74, cy + 190, 40, 32}, p{cx + 154, cy + 190, 40, 32};
    drawSelBtn(rt, m, L"\x2212", false); drawSelBtn(rt, p, L"+", false); hit(m, H_LRM, i); hit(p, H_LRP, i);
    wchar_t v[8]; swprintf(v, 8, in.shift > 0 ? L"+%d" : L"%d", in.shift);
    textB(rt, v, 13, act, 1, cx + 134, cy + 211, br(act ? 0xe8c430 : 0xaaaaaa));
    textB(rt, L"+ は R が遅れて届く機器用", 11, false, 0, cx + 206, cy + 210, br(0x8a8a8a));
}

static void settingsWindow(ID2D1RenderTarget* rt){
    const Rc& w = W_SET;
    // 影
    for (int k = 1; k <= 4; k++) fillRR(rt, Rc{w.x - k*3.f, w.y - k*2.f, w.w + k*6.f, w.h + k*6.f}, 10 + k*3.f, 0x000000, 0.12f);
    fillRR(rt, w, 10, 0x232323);
    rt->FillRectangle(D2D1::RectF(w.x + 1, w.y + 1, w.r() - 1, w.y + 40), br(0x2e2e2e));
    rt->DrawLine(D2D1::Point2F(w.x, w.y + 40.5f), D2D1::Point2F(w.r(), w.y + 40.5f), br(0x444444), 1);
    strokeRR(rt, w, 10, 1, 0x777777);
    gWinHitStart = (int)gHits.size();
    hit(w, H_WINDOW);
    textB(rt, L"入力の設定", 14, true, 0, w.x + 18, w.y + 25, br(0xeeeeee));
    textB(rt, L"メーターを見ながら合わせる", 11, false, 0, w.x + 108, w.y + 25, br(0x8a8a8a));
    rt->DrawLine(D2D1::Point2F(W_CLOSE.x + 10, W_CLOSE.y + 10), D2D1::Point2F(W_CLOSE.r() - 10, W_CLOSE.b() - 10), br(0xcccccc), 1.8f);
    rt->DrawLine(D2D1::Point2F(W_CLOSE.r() - 10, W_CLOSE.y + 10), D2D1::Point2F(W_CLOSE.x + 10, W_CLOSE.b() - 10), br(0xcccccc), 1.8f);
    hit(W_CLOSE, H_CLOSE);
    inputCard(rt, 0, w.x + 18, w.y + 54);
    inputCard(rt, 1, w.x + 18 + 482 + 16, w.y + 54);
}

// ===== 針以外の全部を描く (1秒60回) =====
static void drawLayer(){
    ID2D1RenderTarget* rt = gLayer;
    if (!rt) return;
    const float faceW = g4 ? VU4[0].w : VU2[0].w, scopeW = g4 ? SCOPE4.w : SCOPE2.w;
    if (std::fabs(gFaceKey - faceW * vS) > 1e-3f || !gFace) buildFace(faceW);
    if (std::fabs(gScopeKey - scopeW * vS) > 1e-3f || !gScope) buildScope(scopeW);
    if (gPower) drawScope();

    double M, S, I; bool hasI, run; double tpRecent = 0, tpMax; std::vector<float> hist;
    EnterCriticalSection(&gLoudCs);
    M = gL.M; S = gL.S; I = gL.I; hasI = gL.hasI; run = gL.run; tpMax = gL.tpMax; hist = gL.hist;
    for (int i = 0; i < TP_HOLD_BLOCKS; i++) if (gL.tpRecent[i] > tpRecent) tpRecent = gL.tpRecent[i];
    LeaveCriticalSection(&gLoudCs);
    if (!gPower) { M = S = -120; }

    gHits.clear(); gWinHitStart = -1;
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(rgb(0x050505));
    rt->SetTransform(viewM());

    // 上の帯
    panel(rt, P_TOP);
    fillRR(rt, B_POWER, 4, gPower ? 0xdd0000 : 0x444444);
    btnLabel(rt, B_POWER, gPower ? L"POWER ON" : L"POWER OFF", 13, 0xffffff, nullptr); hit(B_POWER, H_POWER);
    drawSelBtn(rt, B_2, L"2連", !g4); drawSelBtn(rt, B_4, L"4連", g4, L"V"); hit(B_2, H_2); hit(B_4, H_4);
    drawSelBtn(rt, B_SET, L"入力の設定", false, nullptr, gSet); hit(B_SET, H_SET);
    rt->FillRectangle(D2D1::RectF(426, 35, 427, 65), br(0x333333));
    if (g4) { statusLine(rt, 0, 46); statusLine(rt, 1, 64); } else statusLine(rt, 0, 55);
    {
        // 待ちあり (Windows のまとめ役を通っている) は赤。描くのが重くて回数が出ないときは黄色
        const bool slow = gFull && !(gTearing && gFps > waitThreshold());
        const bool hv = slow && heavy();
        const bool wait = slow && !hv;
        wchar_t m[96];
        swprintf(m, 96, L"%ls %.0f回/秒 (1回 %.1fms)", !gFull ? L"ウィンドウ" : wait ? L"待ちあり" : hv ? L"描くのが重い" : L"待ちなし", gFps, gWorkMs);
        unsigned col = wait ? 0xe6463c : (gFull && (hv || gFps < 120)) ? 0xe8c430 : 0x8a8a8a;
        textB(rt, m, 12, wait || hv, 2, 1398, 55, br(col));
        if (onBattery()) { float mw = textWidth(m, 12, wait || hv); textB(rt, L"電池で動作中", 12, true, 2, 1398 - mw - 14, 55, br(0xec9a2d)); }
    }

    if (!g4) {
        panel(rt, P_MET2);
        textT(rt, L"L-VU", 16, true, 1, VU2[0].x + VU2[0].w/2, 176, br(0xeeeeee));
        textT(rt, L"R-VU", 16, true, 1, VU2[1].x + VU2[1].w/2, 176, br(0xeeeeee));
        vuBox(rt, VU2[0]); vuBox(rt, VU2[1]);
        scopeBox(rt, SCOPE2, SCOPEG2, 16, 146, false);
        textT(rt, L"LOUDNESS", 16, true, 1, 1340, 146, br(0xeeeeee));
        loudBars(rt, TRACK2, M, S, 171, 188, 15);
        panel(rt, P_BOT2);
        analysisRow(rt, 44, 486, tpRecent, tpMax, hasI, I, run, 40);
        rtaWindow(rt, RTAWIN2, hist, S);
    } else {
        panel(rt, P_MET4);
        textT(rt, L"INPUT 1", 15, true, 0, VU4[0].x, 110, br(0xeeeeee));
        textT(rt, L"INPUT 2", 15, true, 0, VU4[2].x, 110, br(0xeeeeee));
        const wchar_t* lr[4] = { L"L", L"R", L"L", L"R" };
        for (int i = 0; i < 4; i++) { textT(rt, lr[i], 13, true, 1, VU4[i].x + VU4[i].w/2, 133, br(0xbbbbbb)); vuBox(rt, VU4[i]); }
        rt->FillRectangle(D2D1::RectF(668, 124, 670, 300), br(0x333333));
        rt->FillRectangle(D2D1::RectF(1300, 124, 1302, 300), br(0x333333));
        textT(rt, L"LOUDNESS", 13, true, 1, 1352, 116, br(0xeeeeee));
        loudBars(rt, TRACK4, M, S, 136, 153, 13);
        panel(rt, P_BOT4);
        scopeBox(rt, SCOPE4, SCOPEG4, 15, 352, true);
        analysisRow(rt, 398, 350, tpRecent, tpMax, hasI, I, run, 34);
        rtaWindow(rt, RTAWIN4, hist, S);
    }
    if (gSet) settingsWindow(rt);

    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (gHelp) textB(rt, L"SPACE ラウドネス START / RESET   V 2連/4連   R・L RTA/LOUDNESS   ピンチで拡大縮小・2本指で移動   0 表示を元に戻す   F 全画面/ウィンドウ   H この表示を消す   Esc 終了",
                     11, false, 0, 12, (float)gH - 4, br(0x666666));
    rt->EndDraw();
    gLayerDirty = false;
}

// ===== 1回の描画: 描いておいた絵 + 針 =====
static void createTargets();
static void render(){
    if (!gRT || !gLayer) return;
    static double lastLayer = 0; double now = nowMs();
    // 針以外は1秒60回。それを描くのが重いPCでは1秒30回にして、針の回数を守る
    const double layerGap = gLayerMs > 6 ? 1000.0 / 30 : 1000.0 / 60;
    if (gLayerDirty || now - lastLayer >= layerGap * 0.85) {
        lastLayer = now; drawLayer();
        double t = nowMs() - now; gLayerMs = gLayerMs * 0.9 + t * 0.1;
    }
    gRT->BeginDraw();
    gRT->SetTransform(D2D1::Matrix3x2F::Identity());
    ID2D1Bitmap* lb = nullptr; gLayer->GetBitmap(&lb);
    if (lb) { gRT->DrawBitmap(lb, D2D1::RectF(0, 0, (float)gW, (float)gH), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR); lb->Release(); }
    float vus[4] = { gIn[0].vuL.load(), gIn[0].vuR.load(), gIn[1].vuL.load(), gIn[1].vuR.load() };
    const Rc* rects = g4 ? VU4 : VU2; int n = g4 ? 4 : 2;
    for (int i = 0; i < n; i++) {
        const Rc& m = rects[i];
        // 設定の窓がかぶっているところには描かない (4連・2連とも VU には かぶらない配置だが念のため)
        gRT->SetTransform(viewM());
        gRT->PushAxisAlignedClip(D2D1::RectF(m.x + 1, m.y + 1, m.r() - 1, m.b() - 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        double db = std::max(-200.0, std::min(3.0, 20*std::log10(vus[i] > 0 ? vus[i] : 0.00001) + VU_CAL));
        double ang = dbToAngle(db);
        float k = (m.w - 2) / VU_W;
        gRT->SetTransform(D2D1::Matrix3x2F::Rotation((float)(90 - ang)) * D2D1::Matrix3x2F::Scale(k, k) *
                          D2D1::Matrix3x2F::Translation(m.x + 1 + VCX * k, m.y + 1 + VCY * k) * viewM());
        gRT->FillGeometry(gNeedle, br(0x111111));
        gRT->SetTransform(viewM());
        gRT->PopAxisAlignedClip();
    }
    HRESULT hr = gRT->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) createTargets();
    gSwap->Present(0, (gTearing && gFull) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    gWorkMs = gWorkMs * 0.95 + (nowMs() - now) * 0.05;
}

static void releaseTargets(){ releaseGrads(); SafeRelease(gFace); SafeRelease(gScope); SafeRelease(gLayer); SafeRelease(gB); SafeRelease(gRT); gFaceKey = gScopeKey = 0; }
static void createTargets(){
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
// 「待ちあり」に落ちたとき: 画面を作り直して「待ちなし」に戻す
static void rebuildSwap(){
    releaseTargets();
    if (!createSwap()) return;
    gFac->MakeWindowAssociation(gWnd, DXGI_MWA_NO_ALT_ENTER);
    RECT rc; GetClientRect(gWnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w > 0 && h > 0) { gW = w; gH = h; gSwap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags()); }
    createTargets();
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
static int gDrag = H_NONE, gDragIn = 0; static Rc gDragR{}; static bool gPanning = false; static float gPanLX = 0, gPanLY = 0;
static const Hit* hitTest(float ux, float uy){
    // 設定の窓の上なら、窓の中の部品だけを見る (窓の下に隠れている部品は押せない)
    int from = (int)gHits.size() - 1, to = 0;
    if (gWinHitStart >= 0) {
        if (W_SET.in(ux, uy)) to = gWinHitStart; else from = gWinHitStart - 1;
    }
    for (int k = from; k >= to; k--) if (gHits[k].r.in(ux, uy)) return &gHits[k];
    return nullptr;
}
static double sliderVal(const Rc& r, float ux, double mn, double mx, double step){
    double t = std::max(0.0, std::min(1.0, (double)(ux - (r.x + 8)) / (r.w - 16)));
    double v = mn + t * (mx - mn);
    return std::max(mn, std::min(mx, jsRound((v - mn) / step) * step + mn));
}
static void dragSlider(float ux){
    Input& in = gIn[gDragIn];
    switch (gDrag) {
    case H_C: in.c = sliderVal(gDragR, ux, 0, 40, 1); onMainGain(gDragIn); break;
    case H_F: in.f = sliderVal(gDragR, ux, -1, 1, 0.1); onMainGain(gDragIn); break;
    case H_CR: in.cR = sliderVal(gDragR, ux, 0, 40, 1); onRGain(gDragIn); break;
    case H_FR: in.fR = sliderVal(gDragR, ux, -1, 1, 0.1); onRGain(gDragIn); break;
    case H_SCOPEG: gScopeG = sliderVal(gDragR, ux, 0.1, 5.0, 0.1); break;
    }
    gLayerDirty = true;
}
static void deviceMenu(int i, const Rc& r){
    listDevices();
    HMENU m = CreatePopupMenu();
    if (i == 1) AppendMenuW(m, MF_STRING | (gIn[1].dev < 0 ? MF_CHECKED : 0), 999, L"（なし）");
    for (int k = 0; k < (int)gDevs.size(); k++) AppendMenuW(m, MF_STRING | (k == gIn[i].dev ? MF_CHECKED : 0), 1000 + k, gDevs[k].name.c_str());
    if (gDevs.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 998, L"(入力機器がありません)");
    POINT p{ (LONG)(vX + r.x * vS), (LONG)(vY + r.b() * vS) }; ClientToScreen(gWnd, &p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, p.x, p.y, 0, gWnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + (int)gDevs.size()) { gIn[i].dev = cmd - 1000; startInput(i); }
    else if (cmd == 999) { gIn[i].dev = -1; startInput(i); }
    gLayerDirty = true;
}
static void setMode4(bool on){ g4 = on; startInput(1); gLayerDirty = true; }
static void loudStartOrReset(){
    EnterCriticalSection(&gLoudCs);
    if (!gL.run) { gL.run = true; gL.runBlocks = 0; } else loudResetLocked();
    LeaveCriticalSection(&gLoudCs);
    gLayerDirty = true;
}
static void click(const Hit& h){
    Input& in = gIn[h.in];
    switch (h.id) {
    case H_POWER: setPower(!gPower); break;
    case H_2: if (g4) setMode4(false); break;
    case H_4: if (!g4) setMode4(true); break;
    case H_SET: gSet = !gSet; break;
    case H_CLOSE: gSet = false; break;
    case H_DEV: deviceMenu(h.in, h.r); break;
    case H_LR: in.mono = false; break;
    case H_LL: in.mono = true; break;
    case H_EXCL: in.exclusive = !in.exclusive; startInput(h.in); break;
    case H_LRM: in.shift = std::max(-4, in.shift - 1); in.lrShift = in.shift; break;
    case H_LRP: in.shift = std::min(4, in.shift + 1); in.lrShift = in.shift; break;
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
    case WM_SYSCOMMAND:
        // スクリーンセーバーとモニターの電源オフを止める
        if ((w & 0xFFF0) == SC_SCREENSAVE || (w & 0xFFF0) == SC_MONITORPOWER) return 0;
        break;
    case WM_LBUTTONDOWN: {
        float mx = (float)GET_X_LPARAM(l), my = (float)GET_Y_LPARAM(l), ux = (mx - vX) / vS, uy = (my - vY) / vS;
        const Hit* ht = hitTest(ux, uy);
        if (ht && isSlider(ht->id)) { gDrag = ht->id; gDragIn = ht->in; gDragR = ht->r; SetCapture(h); dragSlider(ux); }
        else if (ht && ht->id != H_WINDOW) { Hit copy = *ht; click(copy); }
        else if (!ht) { gPanning = true; gPanLX = mx; gPanLY = my; SetCapture(h); }
        return 0; }
    case WM_MOUSEMOVE: {
        float mx = (float)GET_X_LPARAM(l), my = (float)GET_Y_LPARAM(l);
        if (gDrag) dragSlider((mx - vX) / vS);
        else if (gPanning) { vX += mx - gPanLX; vY += my - gPanLY; gPanLX = mx; gPanLY = my; gLayerDirty = true; }
        return 0; }
    case WM_LBUTTONUP: gDrag = H_NONE; gPanning = false; ReleaseCapture(); return 0;
    case WM_LBUTTONDBLCLK: {
        float ux = (GET_X_LPARAM(l) - vX) / vS, uy = (GET_Y_LPARAM(l) - vY) / vS;
        const Hit* ht = hitTest(ux, uy);
        if (!ht) resetView();
        else if (!isSlider(ht->id) && ht->id != H_WINDOW && ht->id != H_DEV) { Hit copy = *ht; click(copy); }
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
        case VK_SPACE: if (!(l & 0x40000000)) loudStartOrReset(); return 0;
        case 'V': if (!(l & 0x40000000)) setMode4(!g4); return 0;
        case 'R': case 'L': if (!(l & 0x40000000)) { rtaMode = !rtaMode; gLayerDirty = true; } return 0;
        case 'F': setFull(!gFull); return 0;
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
    initWN(); initTP(); initRta(); onMainGain(0); onMainGain(1);
    timeBeginPeriod(1);
    // 開いている間は、スクリーンセーバーとスリープを止める
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"VUMETERWIN"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    gWnd = CreateWindowW(L"VUMETERWIN", L"VU METER", WS_OVERLAPPEDWINDOW, gWinRect.left, gWinRect.top, gWinRect.right-gWinRect.left, gWinRect.bottom-gWinRect.top, nullptr, nullptr, inst, nullptr);
    if (!gWnd || !initGraphics()) { MessageBoxW(nullptr, L"画面の準備ができませんでした", L"VU METER", MB_OK); return 1; }
    ShowWindow(gWnd, SW_SHOW);
    setFull(true);
    gHz = monitorHz();
    listDevices();
    if (!gDevs.empty()) gIn[0].dev = 0;
    setPower(true);
    LARGE_INTEGER fq, t0, t; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); int frames = 0;
    double slowSince = -1, lastFix = -1e9; int fixes = 0;
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) goto quit; TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (gWait) WaitForSingleObjectEx(gWait, 100, TRUE);
        render();
        frames++;
        QueryPerformanceCounter(&t);
        double dt = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (dt >= 0.5) {
            gFps = frames / dt; frames = 0; t0 = t;
            // 全画面なのに「待ちあり」が3秒続いたら、画面を作り直して「待ちなし」に戻す (続けて3回まで)
            double now = nowMs();
            bool slow = gFull && gTearing && gFps <= waitThreshold() && !heavy() && GetForegroundWindow() == gWnd;
            if (!slow) { slowSince = -1; if (gFps > waitThreshold()) fixes = 0; }
            else if (slowSince < 0) slowSince = now;
            else if (now - slowSince > 3000 && now - lastFix > 10000 && fixes < 3) { rebuildSwap(); lastFix = now; slowSince = -1; fixes++; }
        }
        static LONGLONG next = 0;
        LONGLONG step = fq.QuadPart / 240;
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        if (next == 0 || n.QuadPart - next > step * 4) next = n.QuadPart;
        next += step;
        for (;;) { QueryPerformanceCounter(&n); if (next - n.QuadPart <= fq.QuadPart / 2000) break; Sleep(1); }
    }
quit:
    stopInput(0); stopInput(1);
    SetThreadExecutionState(ES_CONTINUOUS);
    timeEndPeriod(1);
    releaseTargets(); SafeRelease(gNeedle); SafeRelease(gSwap); SafeRelease(gCtx); SafeRelease(gDev); SafeRelease(gD2); SafeRelease(gDW);
    CoUninitialize();
    return 0;
}
