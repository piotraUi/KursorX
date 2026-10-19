#include "media.h"

#include <gdiplus.h>
#include <mfplay.h>
#include <propvarutil.h>

#include "util.h"

namespace {
HINSTANCE g_inst;
const wchar_t kHostClass[] = L"KXMedia";
const wchar_t kVideoClass[] = L"KXVideo";
constexpr UINT_PTR kFrameTimer = 1;
}  // namespace

MediaKind KindOf(const std::wstring& path) {
    std::wstring e = Ext(path);
    for (const wchar_t* i : {L".gif", L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff"})
        if (e == i) return MediaKind::Image;
    for (const wchar_t* v : {L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".asf", L".mkv", L".webm", L".mpg", L".mpeg"})
        if (e == v) return MediaKind::Video;
    return MediaKind::Unknown;
}

// ---------------------------------------------------------------- callback MFPlay

class VideoCallback : public IMFPMediaPlayerCallback {
public:
    explicit VideoCallback(MediaView* owner) : owner_(owner) {}
    virtual ~VideoCallback() = default;
    void Detach() { owner_ = nullptr; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* h) override {
        if (!owner_ || FAILED(h->hrEvent)) return;
        if (h->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) owner_->OnVideoReady();
        else if (h->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) owner_->OnVideoEnded();
    }

private:
    LONG ref_ = 1;
    MediaView* owner_;
};

// ---------------------------------------------------------------- MediaView

bool MediaView::Open(HWND host, const std::wstring& path, bool mute, int fit) {
    Close();
    host_ = host;
    path_ = path;
    mute_ = mute;
    fit_ = fit;
    paused_ = false;

    if (KindOf(path) == MediaKind::Video) {
        videoWnd_ = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 16, 16, host, nullptr, g_inst, this);
        cb_ = new VideoCallback(this);
        HRESULT hr = MFPCreateMediaPlayer(path.c_str(), TRUE, 0, cb_, videoWnd_, &player_);
        if (FAILED(hr) || !player_) {
            Close();
            return false;
        }
        player_->SetMute(mute_ ? TRUE : FALSE);
        player_->SetAspectRatioMode(MFVideoARMode_None);
        info_ = L"Wideo " + Ext(path).substr(1);
        for (auto& c : info_) c = (wchar_t)towupper(c);
        LayoutVideo();
        return true;
    }

    img_.reset(Gdiplus::Image::FromFile(path.c_str()));
    if (!img_ || img_->GetLastStatus() != Gdiplus::Ok) {
        Close();
        return false;
    }
    frames_ = 1;
    UINT dims = img_->GetFrameDimensionsCount();
    if (dims > 0) {
        GUID g;
        img_->GetFrameDimensionsList(&g, 1);
        if (g == Gdiplus::FrameDimensionTime) frames_ = Mx<UINT>(img_->GetFrameCount(&g), 1);
    }
    delays_.assign(frames_, 100);
    if (frames_ > 1) {
        UINT sz = img_->GetPropertyItemSize(PropertyTagFrameDelay);
        if (sz) {
            std::vector<BYTE> buf(sz);
            auto* item = (Gdiplus::PropertyItem*)buf.data();
            if (img_->GetPropertyItem(PropertyTagFrameDelay, sz, item) == Gdiplus::Ok) {
                const UINT32* v = (const UINT32*)item->value;
                for (UINT i = 0; i < frames_ && i < item->length / 4; ++i)
                    delays_[i] = v[i] <= 1 ? 100 : (int)v[i] * 10;
            }
        }
    }
    wchar_t buf[96];
    wsprintfW(buf, frames_ > 1 ? L"Animowany obraz • %d×%d • %u klatek" : L"Obraz • %d×%d", (int)img_->GetWidth(),
              (int)img_->GetHeight(), frames_);
    info_ = buf;
    frame_ = 0;
    InvalidateRect(host_, nullptr, FALSE);
    ScheduleFrame();
    return true;
}

void MediaView::Close() {
    if (host_) KillTimer(host_, kFrameTimer);
    if (cb_) cb_->Detach();
    if (player_) {
        player_->Shutdown();
        player_->Release();
        player_ = nullptr;
    }
    if (cb_) {
        cb_->Release();
        cb_ = nullptr;
    }
    if (videoWnd_) {
        DestroyWindow(videoWnd_);
        videoWnd_ = nullptr;
    }
    img_.reset();
    host_ = nullptr;
    native_ = {0, 0};
    info_.clear();
}

void MediaView::SetPaused(bool paused) {
    if (!host_ || paused == paused_) return;
    paused_ = paused;
    if (player_) {
        if (paused) player_->Pause();
        else player_->Play();
    } else {
        ScheduleFrame();
    }
}

void MediaView::SetMute(bool mute) {
    mute_ = mute;
    if (player_) player_->SetMute(mute ? TRUE : FALSE);
}

void MediaView::SetFit(int fit) {
    fit_ = fit;
    if (!host_) return;
    if (player_) LayoutVideo();
    else InvalidateRect(host_, nullptr, FALSE);
}

void MediaView::ScheduleFrame() {
    if (!host_ || !img_ || frames_ < 2 || paused_) return;
    SetTimer(host_, kFrameTimer, (UINT)delays_[frame_], nullptr);
}

void MediaView::OnTimer() {
    KillTimer(host_, kFrameTimer);
    if (!img_ || frames_ < 2 || paused_) return;
    frame_ = (frame_ + 1) % frames_;
    GUID t = Gdiplus::FrameDimensionTime;
    img_->SelectActiveFrame(&t, frame_);
    InvalidateRect(host_, nullptr, FALSE);
    ScheduleFrame();
}

void MediaView::OnSize() {
    if (player_) LayoutVideo();
    else if (host_) InvalidateRect(host_, nullptr, FALSE);
}

void MediaView::UpdateVideo() {
    if (player_) player_->UpdateVideo();
}

void MediaView::OnVideoReady() {
    SIZE s{};
    if (player_ && SUCCEEDED(player_->GetNativeVideoSize(&s, nullptr))) native_ = s;
    if (native_.cx > 0) {
        wchar_t buf[32];
        wsprintfW(buf, L" • %d×%d", (int)native_.cx, (int)native_.cy);
        info_ += buf;
    }
    LayoutVideo();
}

void MediaView::OnVideoEnded() {
    if (!player_) return;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = 0;
    player_->SetPosition(MFP_POSITIONTYPE_100NS, &pv);
    if (!paused_) player_->Play();
}

void MediaView::LayoutVideo() {
    if (!host_ || !videoWnd_) return;
    RECT rc;
    GetClientRect(host_, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    double vw = native_.cx > 0 ? native_.cx : W, vh = native_.cy > 0 ? native_.cy : H;
    double sx = W / vw, sy = H / vh, dw, dh;
    if (fit_ == kStretch) {
        dw = W;
        dh = H;
    } else {
        double s = fit_ == kFill ? Mx(sx, sy) : Mn(sx, sy);
        dw = vw * s;
        dh = vh * s;
    }
    MoveWindow(videoWnd_, (int)((W - dw) / 2), (int)((H - dh) / 2), (int)dw, (int)dh, TRUE);
    if (player_) player_->UpdateVideo();
}

void MediaView::Paint(HDC dc) {
    RECT rc;
    GetClientRect(host_, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    if (!img_) {  // film: tlo pod oknem wideo
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Gdiplus::Graphics g(mem);
        g.Clear(Gdiplus::Color(255, 0, 0, 0));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        float iw = (float)img_->GetWidth(), ih = (float)img_->GetHeight();
        float sx = W / iw, sy = H / ih;
        Gdiplus::RectF dst(0, 0, (float)W, (float)H);
        float srcX = 0, srcY = 0, srcW = iw, srcH = ih;
        if (fit_ == kFill) {
            float s = Mx(sx, sy);
            srcW = W / s;
            srcH = H / s;
            srcX = (iw - srcW) / 2;
            srcY = (ih - srcH) / 2;
        } else if (fit_ == kFit) {
            float s = Mn(sx, sy);
            dst = Gdiplus::RectF((W - iw * s) / 2, (H - ih * s) / 2, iw * s, ih * s);
        }
        Gdiplus::ImageAttributes attr;
        attr.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        g.DrawImage(img_.get(), dst, srcX, srcY, srcW, srcH, Gdiplus::UnitPixel, &attr);
    }
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------- okna

namespace {

MediaView* ViewOf(HWND h) { return (MediaView*)GetWindowLongPtrW(h, GWLP_USERDATA); }

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (MediaView* v = ViewOf(h)) v->Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (MediaView* v = ViewOf(h)) v->OnTimer();
            return 0;
        case WM_SIZE:
            if (MediaView* v = ViewOf(h)) v->OnSize();
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK VideoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            if (MediaView* v = ViewOf(h)) v->UpdateVideo();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool RegisterMediaClasses(HINSTANCE inst) {
    g_inst = inst;
    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = kHostClass;
    if (!RegisterClassW(&wc)) return false;
    wc.lpfnWndProc = VideoProc;
    wc.lpszClassName = kVideoClass;
    return RegisterClassW(&wc) != 0;
}

HWND CreateMediaHost(HWND parent, DWORD style, DWORD exStyle, int x, int y, int w, int h, MediaView* view) {
    return CreateWindowExW(exStyle, kHostClass, L"", style | WS_CLIPCHILDREN, x, y, w, h, parent, nullptr, g_inst, view);
}

// ---------------------------------------------------------------- tapeta na pulpicie

namespace {

struct DesktopHost {
    HWND parent = nullptr;
    HWND insertAfter = nullptr;  // != null: okno ma trafic tuz nad tym oknem (Win11 24H2)
};

DesktopHost FindDesktopHost() {
    DesktopHost out;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return out;
    // Prosi Explorera o utworzenie okna WorkerW, ktore siedzi pod ikonami pulpitu.
    DWORD_PTR res = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 1, SMTO_NORMAL, 1000, &res);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &res);

    HWND workerw = nullptr;
    EnumWindows(
        [](HWND top, LPARAM lp) -> BOOL {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))
                *(HWND*)lp = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return TRUE;
        },
        (LPARAM)&workerw);

    if (workerw) {
        out.parent = workerw;  // Windows 10 / starsze Windows 11
    } else {
        // Windows 11 24H2: WorkerW jest dzieckiem Progmana, a okno wstawiamy tuz nad nim.
        out.parent = progman;
        out.insertAfter = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    }
    return out;
}

}  // namespace

bool Wallpaper::Start(const std::wstring& path, bool mute, int fit, std::wstring& err) {
    Stop();
    DesktopHost host = FindDesktopHost();
    if (!host.parent) {
        err = L"Nie znaleziono okna pulpitu (Explorer nie działa?).";
        return false;
    }

    std::vector<RECT> mons;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT r, LPARAM lp) -> BOOL {
            ((std::vector<RECT>*)lp)->push_back(*r);
            return TRUE;
        },
        (LPARAM)&mons);

    for (const RECT& r : mons) {
        Surface s;
        s.view = std::make_unique<MediaView>();
        POINT p{r.left, r.top};
        ScreenToClient(host.parent, &p);
        s.hwnd = CreateMediaHost(host.parent, WS_CHILD | WS_VISIBLE, WS_EX_NOACTIVATE, p.x, p.y, r.right - r.left,
                                 r.bottom - r.top, s.view.get());
        if (!s.hwnd) continue;
        if (host.insertAfter)
            SetWindowPos(s.hwnd, host.insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!s.view->Open(s.hwnd, path, mute, fit)) {
            DestroyWindow(s.hwnd);
            s.hwnd = nullptr;
            surfaces_.push_back(std::move(s));
            Stop();
            err = L"Nie udało się odtworzyć pliku (brak kodeka?). Spróbuj MP4 (H.264) lub GIF.";
            return false;
        }
        surfaces_.push_back(std::move(s));
    }
    if (surfaces_.empty()) {
        err = L"Nie udało się utworzyć okna tapety.";
        return false;
    }
    path_ = path;
    mute_ = mute;
    fit_ = fit;
    paused_ = false;
    // Tylko pierwszy monitor gra dzwiek.
    for (size_t i = 1; i < surfaces_.size(); ++i) surfaces_[i].view->SetMute(true);
    return true;
}

void Wallpaper::Stop() {
    for (auto& s : surfaces_) {
        s.view->Close();
        if (s.hwnd) DestroyWindow(s.hwnd);
    }
    surfaces_.clear();
    path_.clear();
    // Odswiezenie pulpitu, zeby zniknely resztki obrazu.
    RedrawWindow(nullptr, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void Wallpaper::SetPaused(bool paused) {
    paused_ = paused;
    for (auto& s : surfaces_) s.view->SetPaused(paused);
}

void Wallpaper::SetMute(bool mute) {
    mute_ = mute;
    for (size_t i = 0; i < surfaces_.size(); ++i) surfaces_[i].view->SetMute(i == 0 ? mute : true);
}

void Wallpaper::SetFit(int fit) {
    fit_ = fit;
    for (auto& s : surfaces_) s.view->SetFit(fit);
}
