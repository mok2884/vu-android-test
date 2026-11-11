// VU WIN : Windows 用、VU の針だけを全画面で、できるだけ遅れなく表示する
// 音: WASAPI (低遅延の共有モード / 専有モード)
// 画面: DXGI flip model + tearing (ゲームと同じ、画面の書き換えを待たない描き方) + Direct2D
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
#include <cmath>
#include <cstdio>

template<class T> static void SafeRelease(T*& p){ if(p){ p->Release(); p=nullptr; } }

// ===== VU の針 (ほかの版と同じ: 0.3秒で99%、約1%行き過ぎ、1kHz -20dBFS = 0VU) =====
static const double ZETA = 0.81, RISE = 0.3;
static double WN = 0;
static const double VU_CAL = 20.0 * std::log10(1.0 / (0.1 * 2.0 / 3.14159265358979));
static void initWN(){
    double z = ZETA, wd = std::sqrt(1 - z*z), x = 0;
    for (; x < 50; x += 0.0005) {
        double y = 1 - std::exp(-z*x) * (std::cos(wd*x) + z/wd*std::sin(wd*x));
        if (y >= 0.99) break;
    }
    WN = x / RISE;
}

// ===== 共有する値 =====
static std::atomic<float> gVuL{0}, gVuR{0}, gPkL{0}, gPkR{0};
static std::atomic<double> gGain{1.0};
static std::atomic<bool> gMono{false};
static std::atomic<bool> gRun{false};
static std::atomic<double> gInLatMs{-1};   // 入力の遅れ (音が入ってから受け取るまで、ms)
static std::wstring gStatus = L"";
static CRITICAL_SECTION gStatusCs;

// ===== 音の入力 =====
struct DevInfo { std::wstring id, name; };
static std::vector<DevInfo> gDevs;
static int gDevIndex = 0;
static bool gExclusive = false;
static std::thread gAudioThread;

static const PROPERTYKEY PK_FriendlyName = { {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14 };

static void listDevices(){
    gDevs.clear();
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        // 既定の入力を先頭に
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
}

static void setStatus(const std::wstring& s){ EnterCriticalSection(&gStatusCs); gStatus = s; LeaveCriticalSection(&gStatusCs); }

// 1サンプル読み出し (float / 16 / 24 / 32bit 整数)
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
        // 専有モード: 48k / 2ch で、24bit(32bitの箱) → 16bit の順に試す
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
        if (!ok) mode = L"専有×→共有";
    }
    const WAVEFORMATEX* use = ok ? (WAVEFORMATEX*)&exf : fmt;
    if (SUCCEEDED(hr) || !ok) {
        if (!ok && fmt) {
            // 低遅延の共有モード (Windows 10 以降)。だめなら普通の共有モード
            IAudioClient3* ac3 = nullptr;
            if (SUCCEEDED(ac->QueryInterface(__uuidof(IAudioClient3), (void**)&ac3))) {
                UINT32 defF=0, fund=0, minF=0, maxF=0;
                if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(fmt, &defF, &fund, &minF, &maxF)) &&
                    SUCCEEDED(ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minF, fmt, nullptr))) {
                    ok = true; periodMs = 1000.0 * minF / fmt->nSamplesPerSec; mode = (mode == L"?" ? L"" : mode + L" ") + L"低遅延共有";
                }
                ac3->Release();
            }
            if (!ok) {
                hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, fmt, nullptr);
                if (SUCCEEDED(hr)) { ok = true; REFERENCE_TIME d=0,m=0; ac->GetDevicePeriod(&d,&m); periodMs = d/10000.0; mode = (mode == L"?" ? L"" : mode + L" ") + L"共有"; }
            }
            use = fmt;
        }
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
        REFERENCE_TIME lat = 0; ac->GetStreamLatency(&lat);
        wchar_t buf[256];
        swprintf(buf, 256, L"%ls / %d Hz / %dch / %dbit%ls / 1回 %.1f ms", mode.c_str(), sr, ch, valid, isFloat ? L" float" : L"", periodMs);
        setStatus(buf);
        DWORD task = 0; HANDLE avrt = nullptr;
        typedef HANDLE (WINAPI *AvSetFn)(LPCWSTR, LPDWORD);
        HMODULE hav = LoadLibraryW(L"avrt.dll");
        if (hav) { AvSetFn f = (AvSetFn)GetProcAddress(hav, "AvSetMmThreadCharacteristicsW"); if (f) avrt = f(L"Pro Audio", &task); }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const int blk = std::max(1, sr / 1000);
        const double h = (double)blk / sr, k1 = WN*WN, k2 = 2*ZETA*WN;
        const double fall = std::pow(10.0, -20.0/20.0 * h / 1.5);
        double yL=0,vL=0,yR=0,vR=0,sL=0,sR=0,mL=0,mR=0,pL=0,pR=0; int n=0;
        ac->Start();
        while (gRun.load()) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            UINT32 pk = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk > 0) {
                BYTE* data = nullptr; UINT32 fr = 0; DWORD fl = 0; UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(cc->GetBuffer(&data, &fr, &fl, &devPos, &qpcPos))) break;
                // 入力の遅れ: この塊の最後の音が入った時刻と、今の時刻の差
                if (qpcPos && !(fl & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                    LARGE_INTEGER now, qf; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&qf);
                    double nowHns = (double)now.QuadPart * 1e7 / qf.QuadPart;
                    double lastHns = (double)qpcPos + (double)fr * 1e7 / sr;
                    double ms = (nowHns - lastHns) / 1e4;
                    double old = gInLatMs.load();
                    if (ms > -50 && ms < 2000) gInLatMs.store(old < 0 ? ms : old * 0.95 + ms * 0.05);
                }
                const bool silent = (fl & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const double g = gGain.load(); const bool mono = gMono.load() || ch == 1;
                const int bytesPer = bits / 8;
                for (UINT32 i = 0; i < fr; i++) {
                    double l = 0, r = 0;
                    if (!silent) {
                        const BYTE* p = data + (size_t)i * stride;
                        l = readSample(p, isFloat, bits);
                        r = ch > 1 ? readSample(p + bytesPer, isFloat, bits) : l;
                    }
                    if (mono) r = l;
                    l *= g; r *= g;
                    double al = std::fabs(l), ar = std::fabs(r);
                    sL += al; sR += ar; if (al > mL) mL = al; if (ar > mR) mR = ar;
                    if (++n >= blk) {
                        vL += (k1*(sL/n - yL) - k2*vL)*h; yL += vL*h;
                        vR += (k1*(sR/n - yR) - k2*vR)*h; yR += vR*h;
                        pL = std::max(mL, pL*fall); pR = std::max(mR, pR*fall);
                        sL=sR=mL=mR=0; n=0;
                    }
                }
                cc->ReleaseBuffer(fr);
                gVuL.store((float)std::max(0.0, yL)); gVuR.store((float)std::max(0.0, yR));
                gPkL.store((float)pL); gPkR.store((float)pR);
            }
        }
        ac->Stop();
        if (hav) { typedef BOOL (WINAPI *AvRevFn)(HANDLE); AvRevFn rf = (AvRevFn)GetProcAddress(hav, "AvRevertMmThreadCharacteristics"); if (rf && avrt) rf(avrt); FreeLibrary(hav); }
    }
done:
    SafeRelease(cc); SafeRelease(ac); SafeRelease(dev); SafeRelease(en);
    if (fmt) CoTaskMemFree(fmt);
    CloseHandle(ev);
    gVuL = 0; gVuR = 0; gPkL = 0; gPkR = 0; gInLatMs = -1;
    CoUninitialize();
}

static void stopAudio(){ gRun = false; if (gAudioThread.joinable()) gAudioThread.join(); }
static void startAudio(){
    stopAudio();
    if (gDevs.empty()) { setStatus(L"入力機器がありません"); return; }
    gDevIndex = (gDevIndex % (int)gDevs.size() + (int)gDevs.size()) % (int)gDevs.size();
    gRun = true;
    gAudioThread = std::thread(audioMain, gDevs[gDevIndex].id, gExclusive);
}

// ===== 画面 =====
static HWND gWnd; static bool gFull = true; static RECT gWinRect = {100,100,1380,580};
static ID3D11Device* gDev = nullptr; static ID3D11DeviceContext* gCtx = nullptr;
static IDXGISwapChain1* gSwap = nullptr; static bool gTearing = false;
static HANDLE gWait = nullptr;          // 描いた絵の順番待ちを1枚までにするための合図
static bool gExclFull = false;          // ゲーム式の全画面 (Windowsのまとめ役を通さない)
static wchar_t gDiag[256] = L"";        // 画面まわりの診断 (なぜ待ちなしにならないか)
static wchar_t gGpu[128] = L"?";
static bool gTearOn = false;           // 今のスワップチェーンが待ちなし用か
static wchar_t gErr[256] = L"";
static IDXGIFactory2* gFac = nullptr;
static UINT swapFlags(){ return (gTearOn ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; }
static ID2D1Factory* gD2 = nullptr; static ID2D1RenderTarget* gRT = nullptr;
static ID2D1Bitmap* gFace = nullptr; static IDWriteFactory* gDW = nullptr;
static int gW = 0, gH = 0;

// VU の文字盤の形 (HTML 版と同じ: 窓 280x145、軸は窓の上から181、左端133度〜右端47度、弧は軸の30下が中心)
static const float FW = 280, FH = 145, PIVX = 140, PIVY = 181, START_DEG = 133, END_DEG = 47, ARC_R = 140, ARC_DROP = 30;
static const double SC[][2] = { {-20,0.030},{-10,0.135},{-7,0.222},{-5,0.303},{-3,0.418},{-2,0.489},{-1,0.567},{0,0.654},{1,0.765},{2,0.888},{3,1.0} };
static double fracOf(double v){
    if (!(v > 0)) return 0;
    double pa = 0, pf = 0;
    for (auto& m : SC) { double a = std::pow(10.0, m[0]/20); if (v <= a) return pf + (m[1]-pf)*(v-pa)/(a-pa); pa = a; pf = m[1]; }
    return 1;
}
static double angleOfAmp(double amp){ return START_DEG + fracOf(amp * std::pow(10.0, VU_CAL/20)) * (END_DEG - START_DEG); }
static double angleOfDb(double db){ return START_DEG + fracOf(std::pow(10.0, db/20)) * (END_DEG - START_DEG); }
static float arcRad(){ double a = START_DEG*3.14159265/180; return (float)std::hypot(std::cos(a)*ARC_R, std::sin(a)*ARC_R + ARC_DROP); }
static float arcDist(double deg){ double s = std::sin(deg*3.14159265/180); float R = arcRad(); return (float)(-ARC_DROP*s + std::sqrt(R*R - ARC_DROP*ARC_DROP*(1 - s*s))); }
static D2D1_POINT_2F facePt(double deg, float extra, float k){
    float r = arcDist(deg) + extra; double a = deg*3.14159265/180;
    return D2D1::Point2F((float)(PIVX + std::cos(a)*r)*k, (float)(PIVY - std::sin(a)*r)*k);
}

// 毎回作らずに使い回す色と文字の設定
static ID2D1SolidColorBrush *bInk=nullptr,*bTxt=nullptr,*bFrame=nullptr,*bBar=nullptr,*bBarBg=nullptr,*bHot=nullptr;
static IDWriteTextFormat *tfInfo=nullptr, *tfLab=nullptr; static float tfForH = -1, tfForFh = -1;
static void releaseTargets(){
    SafeRelease(gFace); SafeRelease(bInk); SafeRelease(bTxt); SafeRelease(bFrame); SafeRelease(bBar); SafeRelease(bBarBg); SafeRelease(bHot);
    SafeRelease(gRT);
}
static void makeBrushes(){
    if (!gRT) return;
    gRT->CreateSolidColorBrush(D2D1::ColorF(0x111111), &bInk); gRT->CreateSolidColorBrush(D2D1::ColorF(0x9a9a9a), &bTxt);
    gRT->CreateSolidColorBrush(D2D1::ColorF(0x555555), &bFrame); gRT->CreateSolidColorBrush(D2D1::ColorF(0xc8c85a), &bBar);
    gRT->CreateSolidColorBrush(D2D1::ColorF(0x262626), &bBarBg); gRT->CreateSolidColorBrush(D2D1::ColorF(0xe6463c), &bHot);
}

// 文字盤を1枚の絵にしておく (毎回描かない)
static void buildFace(float k){
    SafeRelease(gFace);
    ID2D1BitmapRenderTarget* bt = nullptr;
    if (FAILED(gRT->CreateCompatibleRenderTarget(D2D1::SizeF(FW*k, FH*k), &bt))) return;
    ID2D1SolidColorBrush *cream=nullptr,*ink=nullptr,*red=nullptr,*soft=nullptr;
    bt->CreateSolidColorBrush(D2D1::ColorF(0xfdfae1), &cream); bt->CreateSolidColorBrush(D2D1::ColorF(0x222222), &ink);
    bt->CreateSolidColorBrush(D2D1::ColorF(0xdd0000), &red); bt->CreateSolidColorBrush(D2D1::ColorF(0x9a9a90), &soft);
    bt->BeginDraw();
    bt->FillRectangle(D2D1::RectF(0,0,FW*k,FH*k), cream);
    auto arc = [&](double d0, double d1, float extra, ID2D1Brush* br, float w){
        ID2D1PathGeometry* g=nullptr; gD2->CreatePathGeometry(&g); ID2D1GeometrySink* s=nullptr; g->Open(&s);
        for (int i = 0; i <= 80; i++) { double d = d0 + (d1-d0)*i/80; auto p = facePt(d, extra, k); if (i==0) s->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW); else s->AddLine(p); }
        s->EndFigure(D2D1_FIGURE_END_OPEN); s->Close(); s->Release();
        bt->DrawGeometry(g, br, w*k); g->Release();
    };
    arc(START_DEG, angleOfDb(0), 0, ink, 1.5f);
    arc(angleOfDb(0), angleOfDb(3), 2.5f, red, 5.0f);
    IDWriteTextFormat* tf = nullptr; IDWriteTextFormat* tfVu = nullptr;
    gDW->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15*k, L"", &tf);
    gDW->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18*k, L"", &tfVu);
    tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); tfVu->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (auto& m : SC) {
        double deg = angleOfDb(m[0]); bool hot = m[0] >= 0; ID2D1Brush* br = hot ? red : ink;
        bt->DrawLine(facePt(deg, 0, k), facePt(deg, hot ? 13.f : 10.f, k), br, 2.2f*k);
        wchar_t t[8]; swprintf(t, 8, L"%d", (int)std::fabs(m[0]));
        auto p = facePt(deg, 26, k);
        float x = std::max(14*k, std::min((FW-14)*k, p.x));
        bt->DrawText(t, (UINT32)wcslen(t), tf, D2D1::RectF(x-20*k, p.y-9*k, x+20*k, p.y+12*k), br);
    }
    bt->DrawText(L"VU", 2, tfVu, D2D1::RectF(PIVX*k-40*k, (FH-34)*k, PIVX*k+40*k, FH*k), ink);
    bt->EndDraw();
    bt->GetBitmap(&gFace);
    SafeRelease(tf); SafeRelease(tfVu); SafeRelease(cream); SafeRelease(ink); SafeRelease(red); SafeRelease(soft); SafeRelease(bt);
}

static void createTargets(){
    releaseTargets();
    IDXGISurface* surf = nullptr;
    if (FAILED(gSwap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) return;
    D2D1_RENDER_TARGET_PROPERTIES pr = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    gD2->CreateDxgiSurfaceRenderTarget(surf, &pr, &gRT);
    surf->Release();
    makeBrushes();
}

static bool createSwap(bool tear){
    if (gWait) { CloseHandle(gWait); gWait = nullptr; }
    SafeRelease(gSwap);
    gTearOn = tear;
    DXGI_SWAP_CHAIN_DESC1 d{}; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.Flags = swapFlags();
    HRESULT hr = gFac->CreateSwapChainForHwnd(gDev, gWnd, &d, nullptr, nullptr, &gSwap);
    if (FAILED(hr)) { swprintf(gErr, 256, L"画面の作り直しに失敗 0x%08X", (unsigned)hr); return false; }
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(gSwap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        gWait = sc2->GetFrameLatencyWaitableObject();
        sc2->Release();
    }
    return true;
}

static bool initGraphics(){
    UINT fl = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, fl, nullptr, 0, D3D11_SDK_VERSION, &gDev, nullptr, &gCtx))) return false;
    IDXGIFactory2* f2 = nullptr; CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&f2);
    IDXGIFactory5* f5 = nullptr;
    if (f2 && SUCCEEDED(f2->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5))) {
        BOOL t = FALSE; if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof(t)))) gTearing = t != FALSE;
        f5->Release();
    }
    gFac = f2;
    if (!createSwap(gTearing)) return false;
    f2->MakeWindowAssociation(gWnd, DXGI_MWA_NO_ALT_ENTER);
    // 診断用: グラフィックの名前、モニターの数、待ちなしの対応
    {
        IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC ds{};
        if (SUCCEEDED(gDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&ds)))
            swprintf(gGpu, 128, L"%ls", ds.Description);
        int outs = 0; if (ad) { IDXGIOutput* o = nullptr; while (SUCCEEDED(ad->EnumOutputs(outs, &o))) { o->Release(); outs++; } }
        swprintf(gDiag, 256, L"グラフィック %ls / このグラフィックにつながるモニター %d / モニター全部 %d / 待ちなし対応 %ls",
            gGpu, outs, GetSystemMetrics(SM_CMONITORS), gTearing ? L"あり" : L"なし");
        SafeRelease(ad); SafeRelease(xd);
    }
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &gD2);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&gDW);
    createTargets();
    return gRT != nullptr;
}

static void resize(int w, int h){
    if (!gSwap || w <= 0 || h <= 0) return;
    gW = w; gH = h;
    releaseTargets();
    gSwap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags());
    createTargets();
}

// ゲーム式の全画面の切り替え
static void setExclusive(bool on){
    if (!gSwap) return;
    if (on && !gFull) return;
    releaseTargets();
    if (gExclFull) gSwap->SetFullscreenState(FALSE, nullptr);
    gExclFull = false;
    // ゲーム式は「待ちなし」用の設定と一緒に使えないので、画面を作り直す
    if (!createSwap(on ? false : gTearing)) createSwap(gTearing);
    HRESULT hr = S_OK;
    if (on && gSwap) {
        hr = gSwap->SetFullscreenState(TRUE, nullptr);
        BOOL fs = FALSE; gSwap->GetFullscreenState(&fs, nullptr);
        gExclFull = SUCCEEDED(hr) && fs;
        if (!gExclFull) {
            const wchar_t* why = L"";
            if (hr == (HRESULT)0x887A0022) why = L"(今は使えない)";
            else if (hr == (HRESULT)0x087A0001) why = L"(画面がこのアプリのものではない: ノートPCの切替グラフィックや複数モニターなど)";
            else if (hr == (HRESULT)0x887A0001) why = L"(使い方の誤り)";
            swprintf(gErr, 256, L"G: ゲーム式に切り替えられませんでした 0x%08X %ls", (unsigned)hr, why);
            createSwap(gTearing);   // 元の待ちなし用に戻す
        } else wcscpy(gErr, L"");
    } else if (!on) wcscpy(gErr, L"");
    if (gSwap) {
        RECT rc; GetClientRect(gWnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        releaseTargets();
        if (w > 0 && h > 0) { gW = w; gH = h; gSwap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags()); }
        createTargets();
    }
}

static void setFull(bool full){
    if (!full && gExclFull) setExclusive(false);
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

static bool gHelp = true;
static int monitorHz(){
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(gWnd, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return (int)dm.dmDisplayFrequency;
}
static double gFps = 0;
// 画面の描き方の表示。待ちなしのつもりでも、描ける回数がモニターと同じなら、Windowsがまとめている
static const wchar_t* modeName(){
    if (!gFull) return L"ウィンドウ(遅め)";
    if (gExclFull) return L"ゲーム式全画面";
    int hz = 0; { MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        if (GetMonitorInfoW(MonitorFromWindow(gWnd, MONITOR_DEFAULTTONEAREST), &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hz = dm.dmDisplayFrequency; }
    if (gTearOn && hz > 0 && gFps > hz * 1.5) return L"全画面・待ちなし";
    return L"全画面・待ちあり(Gでゲーム式に)";
}
static float gFaceK = 0;

static void render(){
    if (!gRT) return;
    float W = (float)gW, H = (float)gH;
    // 2つの窓を横に並べて、画面に収まるいちばん大きい倍率
    float gap = 0.05f;
    float k = std::min(W * 0.92f / (FW * (2 + gap)), H * 0.70f / FH);
    if (std::fabs(k - gFaceK) > 0.001f || !gFace) { gFaceK = k; buildFace(k); }
    float fw = FW*k, fh = FH*k, x0 = (W - fw*(2+gap))/2, y0 = (H - fh)/2 - H*0.04f;
    if (tfForH != H || tfForFh != fh) {
        SafeRelease(tfInfo); SafeRelease(tfLab); tfForH = H; tfForFh = fh;
        gDW->CreateTextFormat(L"Meiryo", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, std::max(12.f, H*0.018f), L"ja-jp", &tfInfo);
        gDW->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, std::max(14.f, fh*0.13f), L"", &tfLab);
        tfLab->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (!bInk || !tfInfo || !tfLab) return;
    IDWriteTextFormat* tf = tfInfo; IDWriteTextFormat* lab = tfLab;
    ID2D1SolidColorBrush *ink=bInk,*txt=bTxt,*frame=bFrame,*bar=bBar,*barbg=bBarBg,*hot=bHot;

    gRT->BeginDraw();
    gRT->Clear(D2D1::ColorF(0x1a1a1a));
    float vus[2] = { gVuL.load(), gVuR.load() }, pks[2] = { gPkL.load(), gPkR.load() };
    const wchar_t* names[2] = { L"L", L"R" };
    for (int i = 0; i < 2; i++) {
        float fx = x0 + i * fw * (1 + gap);
        gRT->DrawText(names[i], 1, lab, D2D1::RectF(fx, y0 - fh*0.2f, fx + fw, y0), txt);
        gRT->DrawBitmap(gFace, D2D1::RectF(fx, y0, fx + fw, y0 + fh));
        gRT->DrawRectangle(D2D1::RectF(fx, y0, fx + fw, y0 + fh), frame, 1.5f);
        // 針 (窓の中だけ描く)
        gRT->PushAxisAlignedClip(D2D1::RectF(fx, y0, fx + fw, y0 + fh), D2D1_ANTIALIAS_MODE_ALIASED);
        double a = angleOfAmp(vus[i]) * 3.14159265 / 180;
        D2D1_POINT_2F piv = D2D1::Point2F(fx + PIVX*k, y0 + PIVY*k);
        D2D1_POINT_2F tip = D2D1::Point2F(piv.x + (float)std::cos(a)*150*k, piv.y - (float)std::sin(a)*150*k);
        gRT->DrawLine(piv, tip, ink, 2.2f*k);
        gRT->PopAxisAlignedClip();
        // ピーク (細いバー、-60〜0 dBFS)
        float by = y0 + fh + fh*0.07f, bh = fh*0.045f;
        gRT->FillRectangle(D2D1::RectF(fx, by, fx + fw, by + bh), barbg);
        double pdb = pks[i] > 1e-7 ? 20*std::log10(pks[i]) : -99;
        float px = fx + (float)((std::max(-60.0, std::min(0.0, pdb)) + 60) / 60) * fw;
        gRT->FillRectangle(D2D1::RectF(fx, by, px, by + bh), pdb > -1 ? hot : bar);
    }
    // 状態の表示
    std::wstring st; EnterCriticalSection(&gStatusCs); st = gStatus; LeaveCriticalSection(&gStatusCs);
    wchar_t line[1024];
    double gdb = 20*std::log10(gGain.load());
    std::wstring dn = gDevs.empty() ? L"(なし)" : gDevs[gDevIndex].name;
    double lat = gInLatMs.load();
    wchar_t latS[32]; if (lat >= 0) swprintf(latS, 32, L"%.1f ms", lat); else wcscpy(latS, L"--");
    swprintf(line, 1024, L"%ls   |   %ls   |   入力の遅れ %ls   |   レベル %+.1f dB%ls   |   画面 %ls %.0f 回/秒   モニター %d Hz",
        dn.c_str(), st.c_str(), latS, gdb, (gMono.load() ? L"   |   LL" : L""),
        modeName(), gFps, monitorHz());
    gRT->DrawText(line, (UINT32)wcslen(line), tf, D2D1::RectF(W*0.03f, H*0.865f, W*0.97f, H*0.94f), txt);
    if (gDiag[0]) gRT->DrawText(gDiag, (UINT32)wcslen(gDiag), tf, D2D1::RectF(W*0.03f, H*0.80f, W*0.97f, H*0.83f), hot);
    if (gErr[0]) gRT->DrawText(gErr, (UINT32)wcslen(gErr), tf, D2D1::RectF(W*0.03f, H*0.83f, W*0.97f, H*0.86f), hot);
    if (gHelp) {
        const wchar_t* help = L"← → 入力機器   ↑ ↓ レベル(0.5dB)   0 レベルを0に   M モノラル(LL)   X 専有モード   F 全画面/ウィンドウ   G ゲーム式全画面   H この表示を消す   Esc 終了";
        gRT->DrawText(help, (UINT32)wcslen(help), tf, D2D1::RectF(W*0.03f, H*0.94f, W*0.97f, H*0.99f), frame);
    }
    HRESULT hr = gRT->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { createTargets(); gFaceK = 0; }
    // ゲーム式の全画面では、待たずに出す指定がいらない (いつも待たない)
    gSwap->Present(0, (gTearOn && gFull && !gExclFull) ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch (m) {
    case WM_SIZE: resize(LOWORD(l), HIWORD(l)); return 0;
    case WM_ACTIVATEAPP:
        if (!w && gExclFull && gSwap) { gSwap->SetFullscreenState(FALSE, nullptr); gExclFull = false; }
        return 0;
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: DestroyWindow(h); return 0;
        case VK_RIGHT: gDevIndex++; startAudio(); return 0;
        case VK_LEFT: gDevIndex--; startAudio(); return 0;
        case VK_UP: gGain = gGain.load() * std::pow(10.0, 0.5/20); return 0;
        case VK_DOWN: gGain = gGain.load() / std::pow(10.0, 0.5/20); return 0;
        case '0': gGain = 1.0; return 0;
        case 'M': gMono = !gMono.load(); return 0;
        case 'X': gExclusive = !gExclusive; startAudio(); return 0;
        case 'F': setFull(!gFull); return 0;
        case 'G': setExclusive(!gExclFull); return 0;
        case 'H': gHelp = !gHelp; return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&gStatusCs);
    initWN();
    timeBeginPeriod(1);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"VUWIN"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    gWnd = CreateWindowW(L"VUWIN", L"VU WIN", WS_OVERLAPPEDWINDOW, gWinRect.left, gWinRect.top, gWinRect.right-gWinRect.left, gWinRect.bottom-gWinRect.top, nullptr, nullptr, inst, nullptr);
    if (!gWnd || !initGraphics()) { MessageBoxW(nullptr, L"画面の準備ができませんでした", L"VU WIN", MB_OK); return 1; }
    ShowWindow(gWnd, SW_SHOW);
    setFull(true);
    listDevices();
    startAudio();
    LARGE_INTEGER fq, t0, t; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); int frames = 0;
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) goto quit; TranslateMessage(&msg); DispatchMessageW(&msg); }
        // 画面が次の絵を受け取れるまで待ってから描く (いちばん新しい針の位置で描くため)
        if (gWait) WaitForSingleObjectEx(gWait, 100, TRUE);
        render();
        frames++;
        QueryPerformanceCounter(&t);
        double dt = double(t.QuadPart - t0.QuadPart) / fq.QuadPart;
        if (dt >= 0.5) { gFps = frames / dt; frames = 0; t0 = t; }
        // 描き直しは1秒240回まで (CPUとGPUを使いすぎないように)
        {
            static LONGLONG next = 0;
            LONGLONG step = fq.QuadPart / 240;
            LARGE_INTEGER n; QueryPerformanceCounter(&n);
            if (next == 0 || n.QuadPart - next > step * 4) next = n.QuadPart;
            next += step;
            for (;;) {
                QueryPerformanceCounter(&n);
                LONGLONG left = next - n.QuadPart;
                if (left <= fq.QuadPart / 2000) break;   // 残り0.5ms以下なら描く (空回りしない)
                Sleep(1);
            }
        }
    }
quit:
    if (gExclFull && gSwap) gSwap->SetFullscreenState(FALSE, nullptr);
    stopAudio();
    timeEndPeriod(1);
    releaseTargets(); SafeRelease(gSwap); SafeRelease(gCtx); SafeRelease(gDev); SafeRelease(gD2); SafeRelease(gDW);
    CoUninitialize();
    return 0;
}
