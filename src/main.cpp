// KursorX - zmiana kursora myszy (.cur / .ani) i animowana tapeta pulpitu (wideo, GIF, obraz).
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>

#include <memory>
#include <string>
#include <vector>

#include "cursor.h"
#include "media.h"
#include "util.h"

using namespace Gdiplus;

// ---------------------------------------------------------------- pomocnicze

std::wstring DataDir() {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
    std::wstring dir = std::wstring(appdata) + L"\\KursorX";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

namespace {

// ---------------------------------------------------------------- ustawienia

struct Settings {
    std::wstring wallpaper;  // tapeta, ktora ma sie uruchamiac z aplikacja
    std::wstring cursorPath;
    int role = 0;
    int fit = kFill;
    bool mute = true;
    bool pauseFull = true;
    bool autostart = false;
    bool startTray = true;
    bool resume = true;
    bool closeToTray = true;
};

Settings g_set;

std::wstring IniPath() { return DataDir() + L"\\settings.ini"; }

std::wstring GetS(const wchar_t* key) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"app", key, L"", buf, 2048, IniPath().c_str());
    return buf;
}
int GetI(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"app", key, def, IniPath().c_str()); }
void PutS(const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(L"app", key, v.c_str(), IniPath().c_str()); }
void PutI(const wchar_t* key, int v) { PutS(key, std::to_wstring(v)); }

void LoadSettings() {
    g_set.wallpaper = GetS(L"wallpaper");
    g_set.cursorPath = GetS(L"cursor");
    g_set.role = Mn(Mx(GetI(L"role", 0), 0), kCursorRoleCount - 1);
    g_set.fit = Mn(Mx(GetI(L"fit", kFill), 0), 2);
    g_set.mute = GetI(L"mute", 1);
    g_set.pauseFull = GetI(L"pauseFull", 1);
    g_set.autostart = GetI(L"autostart", 0);
    g_set.startTray = GetI(L"startTray", 1);
    g_set.resume = GetI(L"resume", 1);
    g_set.closeToTray = GetI(L"closeToTray", 1);
}

void SaveSettings() {
    PutS(L"wallpaper", g_set.wallpaper);
    PutS(L"cursor", g_set.cursorPath);
    PutI(L"role", g_set.role);
    PutI(L"fit", g_set.fit);
    PutI(L"mute", g_set.mute);
    PutI(L"pauseFull", g_set.pauseFull);
    PutI(L"autostart", g_set.autostart);
    PutI(L"startTray", g_set.startTray);
    PutI(L"resume", g_set.resume);
    PutI(L"closeToTray", g_set.closeToTray);
}

void ApplyAutostart() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (g_set.autostart) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"" + (g_set.startTray ? L" --tray" : L"");
        RegSetValueExW(k, L"KursorX", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"KursorX");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- identyfikatory

enum Page { kPageCursor, kPageWallpaper, kPageSettings, kPageCount };

enum Id {
    ID_TAB = 100,  // + Page
    ID_ROLE = 200,  // + indeks roli
    ID_ZOOM = 300,  // + 0..3
    ID_FIT = 310,   // + FitMode
    ID_PICK_CURSOR = 400,
    ID_APPLY,
    ID_APPLY_ALL,
    ID_RESET,
    ID_PICK_WALL,
    ID_SET_WALL,
    ID_STOP_WALL,
    ID_PAUSE_WALL,
    ID_T_MUTE,
    ID_T_PAUSEFS,
    ID_T_AUTOSTART,
    ID_T_STARTTRAY,
    ID_T_RESUME,
    ID_T_CLOSETRAY,
};

enum : UINT_PTR { TIMER_CURSOR = 1, TIMER_WATCH = 2 };
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr int kZooms[4] = {1, 2, 4, 8};

enum class WK { Primary, Secondary, Ghost, Chip, Tab, Toggle };

struct Widget {
    int id;
    WK kind;
    RectF r;
    std::wstring text;
};

// ---------------------------------------------------------------- stan

HINSTANCE g_inst;
HWND g_hwnd, g_prevHost;
HICON g_icon;
float g_scale = 1.0f;
UINT g_msgShow, g_msgTaskbar;
bool g_inTray = false;

int g_page = kPageCursor;
std::vector<Widget> g_widgets;
int g_hot = -1, g_down = -1;
bool g_tracking = false;

CursorData g_cur;
std::unique_ptr<Bitmap> g_frame;
int g_step = 0;
int g_zoom = 2;  // indeks w kZooms

std::wstring g_wallSel;
MediaView g_prevView;
Wallpaper g_wall;
bool g_userPaused = false, g_autoPaused = false;

std::wstring g_status = L"Wybierz plik kursora (.cur / .ani) albo przeciągnij go na okno.";
int g_statusKind = 0;  // 0 info, 1 ok, 2 blad

// geometria (w pikselach ekranu), liczona w Relayout()
struct Geo {
    RectF sidebar, title, sub, left, right, card, info, status;
} g_geo;

HDC g_bbDc;
HBITMAP g_bbBmp, g_bbOld;
int g_bbW, g_bbH;

std::unique_ptr<FontFamily> g_fam;
std::unique_ptr<Font> g_fTitle, g_fH, g_fBody, g_fBold, g_fSmall;

float S(float v) { return v * g_scale; }

// tymczasowy pedzel przekazywany jako wskaznik: g.FillRectangle(SB(kolor), r)
struct SB {
    SolidBrush b;
    explicit SB(Color c) : b(c) {}
    operator const Brush*() const { return &b; }
};

const Color cBg(255, 14, 16, 21), cSide(255, 18, 20, 27), cCard(255, 23, 26, 35), cCard2(255, 31, 35, 46),
    cBorder(255, 40, 45, 60), cText(255, 233, 236, 243), cMuted(255, 140, 148, 168), cAcc1(255, 124, 92, 255),
    cAcc2(255, 45, 212, 255), cGood(255, 61, 220, 151), cBad(255, 255, 107, 107);

// ---------------------------------------------------------------- rysowanie

void MakeFonts() {
    g_fam = std::make_unique<FontFamily>(L"Segoe UI");
    if (!g_fam->IsAvailable()) g_fam.reset(FontFamily::GenericSansSerif()->Clone());
    auto mk = [&](float px, int style) { return std::make_unique<Font>(g_fam.get(), px * g_scale, style, UnitPixel); };
    g_fTitle = mk(26, FontStyleBold);
    g_fH = mk(16, FontStyleBold);
    g_fBody = mk(14, FontStyleRegular);
    g_fBold = mk(14, FontStyleBold);
    g_fSmall = mk(12, FontStyleRegular);
}

void RoundPath(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(Graphics& g, const Brush& b, RectF r, float rad) {
    GraphicsPath p;
    RoundPath(p, r, rad);
    g.FillPath(&b, &p);
}

void StrokeRound(Graphics& g, const Pen& pen, RectF r, float rad) {
    r.X += 0.5f;
    r.Y += 0.5f;
    r.Width -= 1;
    r.Height -= 1;
    GraphicsPath p;
    RoundPath(p, r, rad);
    g.DrawPath(&pen, &p);
}

void Text(Graphics& g, const std::wstring& s, RectF r, Font* f, Color c, StringAlignment h = StringAlignmentNear,
          StringAlignment v = StringAlignmentNear, bool wrap = false) {
    StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(v);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, f, r, &fmt, &b);
}

Color Lighten(Color c, int a) {
    return Color(c.GetA(), Mn(255, c.GetR() + a), Mn(255, c.GetG() + a), Mn(255, c.GetB() + a));
}

bool WidgetOn(int id) {
    if (id >= ID_ROLE && id < ID_ROLE + kCursorRoleCount) return id - ID_ROLE == g_set.role;
    if (id >= ID_ZOOM && id < ID_ZOOM + 4) return id - ID_ZOOM == g_zoom;
    if (id >= ID_FIT && id < ID_FIT + 3) return id - ID_FIT == g_set.fit;
    if (id >= ID_TAB && id < ID_TAB + kPageCount) return id - ID_TAB == g_page;
    switch (id) {
        case ID_T_MUTE: return g_set.mute;
        case ID_T_PAUSEFS: return g_set.pauseFull;
        case ID_T_AUTOSTART: return g_set.autostart;
        case ID_T_STARTTRAY: return g_set.startTray;
        case ID_T_RESUME: return g_set.resume;
        case ID_T_CLOSETRAY: return g_set.closeToTray;
    }
    return false;
}

bool WidgetEnabled(int id) {
    switch (id) {
        case ID_APPLY:
        case ID_APPLY_ALL: return !g_cur.path.empty();
        case ID_SET_WALL: return !g_wallSel.empty();
        case ID_STOP_WALL:
        case ID_PAUSE_WALL: return g_wall.Running();
    }
    return true;
}

void DrawWidget(Graphics& g, const Widget& w, bool hot, bool down) {
    bool on = WidgetOn(w.id), en = WidgetEnabled(w.id);
    RectF r = w.r;
    if (down && en) {
        r.Inflate(-S(1), -S(1));
    }
    float rad = S(10);
    switch (w.kind) {
        case WK::Primary: {
            if (!en) {
                FillRound(g, SolidBrush(cCard2), r, rad);
                Text(g, w.text, r, g_fBold.get(), Color(255, 90, 96, 112), StringAlignmentCenter, StringAlignmentCenter);
                break;
            }
            int l = hot ? 22 : 0;
            LinearGradientBrush br(r, Lighten(cAcc1, l), Lighten(cAcc2, l), LinearGradientModeHorizontal);
            FillRound(g, br, r, rad);
            Text(g, w.text, r, g_fBold.get(), Color(255, 255, 255, 255), StringAlignmentCenter, StringAlignmentCenter);
            break;
        }
        case WK::Secondary: {
            FillRound(g, SolidBrush(hot && en ? Lighten(cCard2, 8) : cCard2), r, rad);
            StrokeRound(g, Pen(hot && en ? cAcc1 : cBorder, 1.0f), r, rad);
            Text(g, w.text, r, g_fBold.get(), en ? cText : Color(255, 90, 96, 112), StringAlignmentCenter,
                 StringAlignmentCenter);
            break;
        }
        case WK::Ghost: {
            if (hot) FillRound(g, SolidBrush(cCard2), r, rad);
            Text(g, w.text, r, g_fBold.get(), hot ? cText : cMuted, StringAlignmentCenter, StringAlignmentCenter);
            break;
        }
        case WK::Chip: {
            if (on) {
                FillRound(g, SolidBrush(Color(60, 124, 92, 255)), r, rad - 2);
                StrokeRound(g, Pen(cAcc1, 1.5f), r, rad - 2);
            } else {
                FillRound(g, SolidBrush(hot ? Lighten(cCard2, 10) : cCard2), r, rad - 2);
                StrokeRound(g, Pen(cBorder, 1.0f), r, rad - 2);
            }
            Text(g, w.text, r, on ? g_fBold.get() : g_fBody.get(), on ? Color(255, 255, 255, 255) : cText,
                 StringAlignmentCenter, StringAlignmentCenter);
            break;
        }
        case WK::Tab: {
            if (on) {
                FillRound(g, SolidBrush(cCard), r, rad);
                LinearGradientBrush br(RectF(r.X, r.Y + S(9), S(4), r.Height - S(18)), cAcc1, cAcc2,
                                       LinearGradientModeVertical);
                FillRound(g, br, RectF(r.X, r.Y + S(9), S(4), r.Height - S(18)), S(2));
            } else if (hot) {
                FillRound(g, SolidBrush(Color(255, 24, 27, 36)), r, rad);
            }
            Text(g, w.text, RectF(r.X + S(18), r.Y, r.Width - S(18), r.Height), on ? g_fBold.get() : g_fBody.get(),
                 on ? cText : cMuted, StringAlignmentNear, StringAlignmentCenter);
            break;
        }
        case WK::Toggle: {
            size_t bar = w.text.find(L'|');
            std::wstring title = w.text.substr(0, bar), desc = bar == std::wstring::npos ? L"" : w.text.substr(bar + 1);
            float sw = S(44), sh = S(24);
            RectF label(r.X, r.Y, r.Width - sw - S(16), r.Height);
            Text(g, title, RectF(label.X, label.Y + S(6), label.Width, S(22)), g_fBold.get(), cText);
            Text(g, desc, RectF(label.X, label.Y + S(28), label.Width, r.Height - S(28)), g_fSmall.get(), cMuted,
                 StringAlignmentNear, StringAlignmentNear, true);
            RectF pill(r.GetRight() - sw, r.Y + S(8), sw, sh);
            if (on) {
                LinearGradientBrush br(pill, cAcc1, cAcc2, LinearGradientModeHorizontal);
                FillRound(g, br, pill, sh / 2);
            } else {
                FillRound(g, SolidBrush(hot ? Lighten(cCard2, 14) : Lighten(cCard2, 4)), pill, sh / 2);
                StrokeRound(g, Pen(cBorder, 1.0f), pill, sh / 2);
            }
            float k = sh - S(8);
            float kx = on ? pill.GetRight() - k - S(4) : pill.X + S(4);
            g.FillEllipse(SB(Color(255, 255, 255, 255)), kx, pill.Y + S(4), k, k);
            break;
        }
    }
}

void DrawCard(Graphics& g, RectF r) {
    FillRound(g, SolidBrush(cCard), r, S(14));
    StrokeRound(g, Pen(cBorder, 1.0f), r, S(14));
}

void DrawChecker(Graphics& g, RectF r) {
    SolidBrush a(Color(255, 27, 31, 41)), b(Color(255, 32, 36, 48));
    float c = S(16);
    g.FillRectangle(&a, r);
    for (float y = r.Y, j = 0; y < r.GetBottom(); y += c, ++j)
        for (float x = r.X + ((int)j % 2 ? c : 0); x < r.GetRight(); x += 2 * c)
            g.FillRectangle(&b, x, y, Mn(c, r.GetRight() - x), Mn(c, r.GetBottom() - y));
}

void DrawPlaceholder(Graphics& g, RectF card, const std::wstring& line1, const std::wstring& line2) {
    RectF in = card;
    in.Inflate(-S(24), -S(24));
    Pen dash(Color(255, 58, 64, 84), 1.5f);
    dash.SetDashStyle(DashStyleDash);
    GraphicsPath p;
    RoundPath(p, in, S(12));
    g.DrawPath(&dash, &p);
    float cy = card.Y + card.Height / 2;
    Text(g, line1, RectF(card.X, cy - S(34), card.Width, S(30)), g_fH.get(), cText, StringAlignmentCenter, StringAlignmentCenter);
    Text(g, line2, RectF(card.X + S(30), cy, card.Width - S(60), S(50)), g_fBody.get(), cMuted, StringAlignmentCenter,
         StringAlignmentNear, true);
}

std::wstring CursorInfo() {
    if (g_cur.path.empty()) return L"";
    wchar_t b[160];
    wsprintfW(b, L"%d×%d px  •  punkt aktywny (%d, %d)", g_cur.width, g_cur.height, g_cur.hotX, g_cur.hotY);
    std::wstring s = b;
    if (g_cur.Animated()) s += L"  •  animacja: " + std::to_wstring(g_cur.steps) + L" kroków";
    return s;
}

void DrawCursorPage(Graphics& g) {
    RectF card = g_geo.card;
    DrawCard(g, card);
    if (g_frame && !g_cur.path.empty()) {
        GraphicsPath clip;
        RoundPath(clip, card, S(14));
        GraphicsContainer c = g.BeginContainer();
        g.SetClip(&clip, CombineModeIntersect);
        DrawChecker(g, card);
        float zoom = (float)kZooms[g_zoom];
        float maxZ = Mn((card.Width - S(30)) / g_cur.width, (card.Height - S(30)) / g_cur.height);
        zoom = Mx(1.0f, Mn(zoom * g_scale, (float)(int)maxZ));
        float dw = g_cur.width * zoom, dh = g_cur.height * zoom;
        float dx = card.X + (card.Width - dw) / 2, dy = card.Y + (card.Height - dh) / 2;
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(g_frame.get(), RectF(dx, dy, dw, dh), 0, 0, (REAL)g_cur.width, (REAL)g_cur.height, UnitPixel);
        // znacznik punktu aktywnego
        float hx = dx + (g_cur.hotX + 0.5f) * zoom, hy = dy + (g_cur.hotY + 0.5f) * zoom, rr = S(6);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        Pen mark(Color(230, 45, 212, 255), 1.5f);
        g.DrawEllipse(&mark, hx - rr, hy - rr, rr * 2, rr * 2);
        g.DrawLine(&mark, hx - rr - S(3), hy, hx + rr + S(3), hy);
        g.DrawLine(&mark, hx, hy - rr - S(3), hx, hy + rr + S(3));
        g.EndContainer(c);
        Text(g, L"Najedź kursorem na podgląd, by zobaczyć go na żywo", RectF(card.X + S(14), card.GetBottom() - S(30), card.Width - S(28), S(22)),
             g_fSmall.get(), Color(200, 140, 148, 168));
    } else {
        DrawPlaceholder(g, card, L"Brak kursora", L"Przeciągnij tutaj plik .cur lub .ani albo kliknij „Wybierz plik…”");
    }
    if (!g_cur.path.empty()) {
        Text(g, FileName(g_cur.path), RectF(g_geo.info.X, g_geo.info.Y, g_geo.info.Width, S(22)), g_fBold.get(), cText);
        Text(g, CursorInfo(), RectF(g_geo.info.X, g_geo.info.Y + S(22), g_geo.info.Width, S(20)), g_fSmall.get(), cMuted);
    }

    DrawCard(g, g_geo.right);
    Text(g, L"Rola kursora", RectF(g_geo.right.X + S(18), g_geo.right.Y + S(14), g_geo.right.Width, S(26)), g_fH.get(), cText);
    Text(g, L"Który kursor Windows ma zostać zastąpiony?",
         RectF(g_geo.right.X + S(18), g_geo.right.GetBottom() - S(52), g_geo.right.Width - S(36), S(40)), g_fSmall.get(),
         cMuted, StringAlignmentNear, StringAlignmentNear, true);
}

void DrawWallpaperPage(Graphics& g) {
    RectF card = g_geo.card;
    DrawCard(g, card);
    if (!g_prevView.IsOpen())
        DrawPlaceholder(g, card, L"Brak tapety",
                        L"Przeciągnij tutaj film (MP4, MOV, WMV, AVI), GIF lub obraz albo kliknij „Wybierz plik…”");
    if (!g_wallSel.empty()) {
        std::wstring name = FileName(g_wallSel);
        bool active = g_wall.Running() && g_wall.Path() == g_wallSel;
        float badgeW = active ? S(120) : 0;
        Text(g, name, RectF(g_geo.info.X, g_geo.info.Y, g_geo.info.Width - badgeW - S(8), S(22)), g_fBold.get(), cText);
        Text(g, g_prevView.Info(), RectF(g_geo.info.X, g_geo.info.Y + S(22), g_geo.info.Width, S(20)), g_fSmall.get(), cMuted);
        if (active) {
            RectF b(g_geo.info.GetRight() - badgeW, g_geo.info.Y + S(1), badgeW, S(22));
            FillRound(g, SolidBrush(Color(50, 61, 220, 151)), b, S(11));
            Text(g, g_userPaused || g_autoPaused ? L"● Wstrzymana" : L"● Aktywna", b, g_fSmall.get(), cGood,
                 StringAlignmentCenter, StringAlignmentCenter);
        }
    }

    RectF rt = g_geo.right;
    DrawCard(g, rt);
    Text(g, L"Opcje tapety", RectF(rt.X + S(18), rt.Y + S(14), rt.Width, S(26)), g_fH.get(), cText);
    Text(g, L"Dopasowanie", RectF(rt.X + S(18), rt.Y + S(52), rt.Width, S(20)), g_fSmall.get(), cMuted);
    float y = rt.Y + S(210);
    Text(g, L"Tapeta działa, dopóki KursorX jest uruchomiony (ikona przy zegarze). Zamknięcie okna chowa aplikację do zasobnika.",
         RectF(rt.X + S(18), Mx(y, rt.GetBottom() - S(92)), rt.Width - S(36), S(80)), g_fSmall.get(), cMuted,
         StringAlignmentNear, StringAlignmentNear, true);
}

void DrawSettingsPage(Graphics& g) {
    RectF card(g_geo.left.X, g_geo.left.Y, g_geo.right.GetRight() - g_geo.left.X, S(4 * 66 + 24));
    DrawCard(g, card);
    RectF about(card.X, card.GetBottom() + S(18), card.Width, S(80));
    Text(g, L"KursorX 2.0", RectF(about.X + S(4), about.Y, about.Width, S(24)), g_fH.get(), cText);
    Text(g,
         L"Pliki kursorów są kopiowane do %APPDATA%\\KursorX, a ścieżki zapisywane w rejestrze użytkownika, więc zmiany "
         L"przetrwają restart. Przycisk „Domyślny” czyści te wpisy i przywraca kursory Windows.",
         RectF(about.X + S(4), about.Y + S(28), about.Width - S(8), S(60)), g_fSmall.get(), cMuted, StringAlignmentNear,
         StringAlignmentNear, true);
}

void DrawSidebar(Graphics& g) {
    g.FillRectangle(SB(cSide), g_geo.sidebar);
    g.FillRectangle(SB(cBorder), RectF(g_geo.sidebar.GetRight() - 1, 0, 1, g_geo.sidebar.Height));
    // logo
    RectF lg(S(22), S(26), S(36), S(36));
    LinearGradientBrush br(lg, cAcc1, cAcc2, LinearGradientModeForwardDiagonal);
    FillRound(g, br, lg, S(10));
    PointF pts[] = {{lg.X + S(11), lg.Y + S(8)},  {lg.X + S(11), lg.Y + S(27)}, {lg.X + S(16), lg.Y + S(22)},
                    {lg.X + S(20), lg.Y + S(30)}, {lg.X + S(23), lg.Y + S(28.5f)}, {lg.X + S(19), lg.Y + S(21)},
                    {lg.X + S(26), lg.Y + S(21)}};
    g.FillPolygon(SB(Color(255, 255, 255, 255)), pts, 7);
    Text(g, L"KursorX", RectF(S(68), S(26), S(130), S(36)), g_fH.get(), cText, StringAlignmentNear, StringAlignmentCenter);
    Text(g, L"v2.0", RectF(S(22), g_geo.sidebar.Height - S(40), S(100), S(20)), g_fSmall.get(), Color(255, 90, 96, 112));
}

void DrawStatus(Graphics& g) {
    RectF r = g_geo.status;
    g.FillRectangle(SB(cSide), r);
    g.FillRectangle(SB(cBorder), RectF(r.X, r.Y, r.Width, 1));
    Color c = g_statusKind == 1 ? cGood : g_statusKind == 2 ? cBad : cMuted;
    g.FillEllipse(SB(c), r.X + S(32), r.Y + r.Height / 2 - S(4), S(8), S(8));
    Text(g, g_status, RectF(r.X + S(50), r.Y, r.Width - S(80), r.Height), g_fBody.get(), cText, StringAlignmentNear,
         StringAlignmentCenter);
}

void DrawAll(Graphics& g, RectF clip) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(cBg);
    DrawSidebar(g);

    static const wchar_t* titles[] = {L"Kursor myszy", L"Animowana tapeta", L"Ustawienia"};
    static const wchar_t* subs[] = {L"Wybierz plik kursora, obejrzyj podgląd i ustaw go w systemie.",
                                    L"Film, GIF albo obraz jako żywe tło pulpitu.",
                                    L"Uruchamianie z systemem i zachowanie aplikacji."};
    Text(g, titles[g_page], g_geo.title, g_fTitle.get(), cText, StringAlignmentNear, StringAlignmentCenter);
    Text(g, subs[g_page], g_geo.sub, g_fBody.get(), cMuted);

    if (g_page == kPageCursor) DrawCursorPage(g);
    else if (g_page == kPageWallpaper) DrawWallpaperPage(g);
    else DrawSettingsPage(g);

    for (size_t i = 0; i < g_widgets.size(); ++i) {
        const Widget& w = g_widgets[i];
        if (w.r.Width <= 0) continue;
        DrawWidget(g, w, (int)i == g_hot, (int)i == g_down && (int)i == g_hot);
    }
    DrawStatus(g);
    (void)clip;
}

void EnsureBackBuffer(HDC ref, int w, int h) {
    if (g_bbDc && g_bbW == w && g_bbH == h) return;
    if (g_bbDc) {
        SelectObject(g_bbDc, g_bbOld);
        DeleteObject(g_bbBmp);
        DeleteDC(g_bbDc);
    }
    g_bbDc = CreateCompatibleDC(ref);
    g_bbBmp = CreateCompatibleBitmap(ref, w, h);
    g_bbOld = (HBITMAP)SelectObject(g_bbDc, g_bbBmp);
    g_bbW = w;
    g_bbH = h;
}

void OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g_hwnd, &ps);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        EnsureBackBuffer(dc, rc.right, rc.bottom);
        {
            Graphics g(g_bbDc);
            RECT p = ps.rcPaint;
            g.SetClip(Rect(p.left, p.top, p.right - p.left, p.bottom - p.top));
            DrawAll(g, RectF((REAL)p.left, (REAL)p.top, (REAL)(p.right - p.left), (REAL)(p.bottom - p.top)));
        }
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, g_bbDc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    }
    EndPaint(g_hwnd, &ps);
}

void Redraw() { InvalidateRect(g_hwnd, nullptr, FALSE); }

void SetStatus(const std::wstring& s, int kind = 0) {
    g_status = s;
    g_statusKind = kind;
    RECT r = {(LONG)g_geo.status.X, (LONG)g_geo.status.Y, (LONG)(g_geo.status.X + g_geo.status.Width),
              (LONG)(g_geo.status.Y + g_geo.status.Height)};
    InvalidateRect(g_hwnd, &r, FALSE);
}

// ---------------------------------------------------------------- uklad

void Add(int id, WK kind, RectF r, const std::wstring& text = L"") { g_widgets.push_back({id, kind, r, text}); }

void UpdatePreviewWindow() {
    bool vis = g_page == kPageWallpaper && g_prevView.IsOpen() && IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd);
    if (!g_prevHost) return;
    if (vis) {
        RectF c = g_geo.card;
        int in = (int)S(6);
        int x = (int)c.X + in, y = (int)c.Y + in, w = (int)c.Width - 2 * in, h = (int)c.Height - 2 * in;
        MoveWindow(g_prevHost, x, y, w, h, TRUE);
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, (int)S(18), (int)S(18));
        SetWindowRgn(g_prevHost, rgn, TRUE);
        ShowWindow(g_prevHost, SW_SHOWNA);
    } else {
        ShowWindow(g_prevHost, SW_HIDE);
    }
    g_prevView.SetPaused(!vis);
}

void Relayout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    float W = (float)rc.right, H = (float)rc.bottom;
    g_widgets.clear();

    float sb = S(210);
    g_geo.sidebar = RectF(0, 0, sb, H);
    g_geo.status = RectF(sb, H - S(44), W - sb, S(44));
    static const wchar_t* tabs[] = {L"Kursor", L"Tapeta", L"Ustawienia"};
    for (int i = 0; i < kPageCount; ++i) Add(ID_TAB + i, WK::Tab, RectF(S(12), S(110) + i * S(46), sb - S(24), S(40)), tabs[i]);

    float cx = sb + S(32), cw = W - cx - S(32);
    float top = S(100), bottom = H - S(44) - S(20);
    g_geo.title = RectF(cx, S(22), cw, S(38));
    g_geo.sub = RectF(cx, S(62), cw, S(22));
    float rightW = S(300), gap = S(20), leftW = cw - rightW - gap;
    g_geo.left = RectF(cx, top, leftW, bottom - top);
    g_geo.right = RectF(cx + leftW + gap, top, rightW, bottom - top);

    float btnH = S(42), bg = S(10);
    float rows = btnH * 2 + bg;
    float infoH = S(46);
    g_geo.card = RectF(g_geo.left.X, g_geo.left.Y, g_geo.left.Width, g_geo.left.Height - rows - infoH - S(24));
    g_geo.info = RectF(g_geo.left.X + S(4), g_geo.card.GetBottom() + S(10), g_geo.left.Width - S(8), infoH);
    float by = g_geo.left.GetBottom() - rows;
    float bw = (g_geo.left.Width - bg) / 2;
    auto btn = [&](int id, WK k, int col, int row, const wchar_t* t) {
        Add(id, k, RectF(g_geo.left.X + col * (bw + bg), by + row * (btnH + bg), bw, btnH), t);
    };

    if (g_page == kPageCursor) {
        btn(ID_PICK_CURSOR, WK::Secondary, 0, 0, L"Wybierz plik…");
        btn(ID_APPLY, WK::Primary, 1, 0, L"Ustaw kursor");
        btn(ID_APPLY_ALL, WK::Secondary, 0, 1, L"Ustaw we wszystkich rolach");
        btn(ID_RESET, WK::Ghost, 1, 1, L"Przywróć domyślny");
        static const wchar_t* zl[] = {L"×1", L"×2", L"×4", L"×8"};
        for (int i = 0; i < 4; ++i)
            Add(ID_ZOOM + i, WK::Chip, RectF(g_geo.card.GetRight() - S(14) - (4 - i) * S(44) + S(4), g_geo.card.Y + S(14), S(40), S(28)), zl[i]);
        float cw2 = (g_geo.right.Width - S(36) - S(8)) / 2, ch = S(38);
        for (int i = 0; i < kCursorRoleCount; ++i)
            Add(ID_ROLE + i, WK::Chip,
                RectF(g_geo.right.X + S(18) + (i % 2) * (cw2 + S(8)), g_geo.right.Y + S(54) + (i / 2) * (ch + S(8)), cw2, ch),
                kCursorRoles[i].label);
    } else if (g_page == kPageWallpaper) {
        btn(ID_PICK_WALL, WK::Secondary, 0, 0, L"Wybierz plik…");
        btn(ID_SET_WALL, WK::Primary, 1, 0, L"Ustaw jako tapetę");
        btn(ID_STOP_WALL, WK::Secondary, 0, 1, L"Zatrzymaj tapetę");
        btn(ID_PAUSE_WALL, WK::Ghost, 1, 1, g_userPaused ? L"Wznów" : L"Wstrzymaj");
        static const wchar_t* fl[] = {L"Wypełnij", L"Dopasuj", L"Rozciągnij"};
        float fw = (g_geo.right.Width - S(36) - 2 * S(8)) / 3;
        for (int i = 0; i < 3; ++i)
            Add(ID_FIT + i, WK::Chip, RectF(g_geo.right.X + S(18) + i * (fw + S(8)), g_geo.right.Y + S(76), fw, S(36)), fl[i]);
        float tw = g_geo.right.Width - S(36);
        Add(ID_T_MUTE, WK::Toggle, RectF(g_geo.right.X + S(18), g_geo.right.Y + S(124), tw, S(40)), L"Wycisz dźwięk");
        Add(ID_T_PAUSEFS, WK::Toggle, RectF(g_geo.right.X + S(18), g_geo.right.Y + S(166), tw, S(40)),
            L"Pauzuj w grach");
    } else {
        float x = g_geo.left.X + S(20), w = g_geo.right.GetRight() - g_geo.left.X - S(40), y = g_geo.left.Y + S(12);
        const struct { int id; const wchar_t* t; } rowsT[] = {
            {ID_T_AUTOSTART, L"Uruchamiaj z Windows|Aplikacja startuje po zalogowaniu, dzięki czemu tapeta wraca sama."},
            {ID_T_STARTTRAY, L"Start w zasobniku|Przy starcie z systemem nie pokazuj okna, tylko ikonę przy zegarze."},
            {ID_T_RESUME, L"Wznów tapetę po uruchomieniu|Ostatnio ustawiona tapeta włącza się automatycznie."},
            {ID_T_CLOSETRAY, L"Zamknięcie okna chowa do zasobnika|Gdy tapeta działa, krzyżyk nie kończy programu."}};
        for (auto& r : rowsT) {
            Add(r.id, WK::Toggle, RectF(x, y, w, S(54)), r.t);
            y += S(66);
        }
    }
    UpdatePreviewWindow();
    Redraw();
}

// ---------------------------------------------------------------- akcje

bool PickFile(const wchar_t* title, const wchar_t* filter, std::wstring& out) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

void ScheduleCursorFrame() {
    KillTimer(g_hwnd, TIMER_CURSOR);
    if (g_cur.Animated()) SetTimer(g_hwnd, TIMER_CURSOR, (UINT)g_cur.DelayMs(g_step), nullptr);
}

bool LoadCursorPath(const std::wstring& path) {
    if (!g_cur.Load(path)) {
        SetStatus(L"Nie można wczytać tego pliku jako kursora.", 2);
        return false;
    }
    g_step = 0;
    g_frame.reset(g_cur.RenderStep(0));
    g_set.cursorPath = path;
    ScheduleCursorFrame();
    SetStatus(L"Wczytano " + FileName(path) + L". Wybierz rolę i kliknij „Ustaw kursor”.", 1);
    Relayout();
    return true;
}

bool LoadWallpaperPreview(const std::wstring& path) {
    MediaKind k = KindOf(path);
    if (k == MediaKind::Unknown) {
        SetStatus(L"Nieobsługiwany format. Użyj MP4, MOV, WMV, AVI, GIF, PNG lub JPG.", 2);
        return false;
    }
    g_prevView.Close();
    if (!g_prevView.Open(g_prevHost, path, true, g_set.fit)) {
        g_wallSel.clear();
        SetStatus(L"Nie udało się otworzyć pliku (brak kodeka?). Spróbuj MP4 (H.264) lub GIF.", 2);
        Relayout();
        return false;
    }
    g_wallSel = path;
    SetStatus(L"Wczytano " + FileName(path) + L". Kliknij „Ustaw jako tapetę”.", 1);
    Relayout();
    return true;
}

void ApplyPause() { g_wall.SetPaused(g_userPaused || g_autoPaused); }

bool StartWallpaper(const std::wstring& path) {
    std::wstring err;
    if (!g_wall.Start(path, g_set.mute, g_set.fit, err)) {
        SetStatus(err, 2);
        return false;
    }
    g_userPaused = g_autoPaused = false;
    g_set.wallpaper = path;
    return true;
}

void ShowWindowFromTray() {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
    Relayout();
}

void AddTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    lstrcpyW(nid.szTip, L"KursorX");
    g_inTray = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void RemoveTray() {
    if (!g_inTray) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_inTray = false;
}

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Otwórz KursorX");
    if (g_wall.Running()) {
        AppendMenuW(m, MF_STRING, 2, g_userPaused ? L"Wznów tapetę" : L"Wstrzymaj tapetę");
        AppendMenuW(m, MF_STRING, 3, L"Zatrzymaj tapetę");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Zakończ");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case 1: ShowWindowFromTray(); break;
        case 2: g_userPaused = !g_userPaused; ApplyPause(); Relayout(); break;
        case 3: g_wall.Stop(); g_set.wallpaper.clear(); SaveSettings(); Relayout(); break;
        case 4: DestroyWindow(g_hwnd); break;
    }
}

void OnClick(int id) {
    if (id >= ID_TAB && id < ID_TAB + kPageCount) {
        g_page = id - ID_TAB;
        Relayout();
        return;
    }
    if (id >= ID_ROLE && id < ID_ROLE + kCursorRoleCount) {
        g_set.role = id - ID_ROLE;
        Redraw();
        return;
    }
    if (id >= ID_ZOOM && id < ID_ZOOM + 4) {
        g_zoom = id - ID_ZOOM;
        Redraw();
        return;
    }
    if (id >= ID_FIT && id < ID_FIT + 3) {
        g_set.fit = id - ID_FIT;
        g_prevView.SetFit(g_set.fit);
        g_wall.SetFit(g_set.fit);
        SaveSettings();
        Redraw();
        return;
    }
    std::wstring f, err;
    switch (id) {
        case ID_PICK_CURSOR:
            if (PickFile(L"Wybierz plik kursora", L"Kursory (*.cur;*.ani)\0*.cur;*.ani\0Wszystkie pliki\0*.*\0", f))
                LoadCursorPath(f);
            break;
        case ID_APPLY:
        case ID_APPLY_ALL: {
            bool all = id == ID_APPLY_ALL;
            if (ApplyCursor(g_set.role, g_cur.path, all, err)) {
                SaveSettings();
                SetStatus(all ? L"Ustawiono kursor we wszystkich rolach." : L"Ustawiono kursor: " + std::wstring(kCursorRoles[g_set.role].label), 1);
            } else {
                SetStatus(err, 2);
            }
            break;
        }
        case ID_RESET: {
            int ans = MessageBoxW(g_hwnd,
                                  L"Tak – przywróć domyślny kursor tylko dla wybranej roli.\nNie – przywróć wszystkie kursory do domyślnych.",
                                  L"Przywróć domyślny", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (ans == IDCANCEL) break;
            ResetCursor(ans == IDYES ? g_set.role : -1);
            SetStatus(L"Przywrócono domyślny kursor.", 1);
            break;
        }
        case ID_PICK_WALL:
            if (PickFile(L"Wybierz tapetę",
                         L"Wideo, GIF i obrazy\0*.mp4;*.m4v;*.mov;*.wmv;*.avi;*.mkv;*.webm;*.gif;*.png;*.jpg;*.jpeg;*.bmp\0Wszystkie pliki\0*.*\0", f))
                LoadWallpaperPreview(f);
            break;
        case ID_SET_WALL:
            if (StartWallpaper(g_wallSel)) {
                SaveSettings();
                SetStatus(L"Tapeta ustawiona. KursorX działa w tle – zamknięcie okna chowa go do zasobnika.", 1);
                Relayout();
            }
            break;
        case ID_STOP_WALL:
            g_wall.Stop();
            g_set.wallpaper.clear();
            SaveSettings();
            SetStatus(L"Tapeta zatrzymana.", 0);
            Relayout();
            break;
        case ID_PAUSE_WALL:
            g_userPaused = !g_userPaused;
            ApplyPause();
            Relayout();
            break;
        case ID_T_MUTE:
            g_set.mute = !g_set.mute;
            g_wall.SetMute(g_set.mute);
            SaveSettings();
            Redraw();
            break;
        case ID_T_PAUSEFS:
            g_set.pauseFull = !g_set.pauseFull;
            if (!g_set.pauseFull) { g_autoPaused = false; ApplyPause(); }
            SaveSettings();
            Redraw();
            break;
        case ID_T_AUTOSTART:
            g_set.autostart = !g_set.autostart;
            ApplyAutostart();
            SaveSettings();
            Redraw();
            break;
        case ID_T_STARTTRAY:
            g_set.startTray = !g_set.startTray;
            if (g_set.autostart) ApplyAutostart();
            SaveSettings();
            Redraw();
            break;
        case ID_T_RESUME:
            g_set.resume = !g_set.resume;
            SaveSettings();
            Redraw();
            break;
        case ID_T_CLOSETRAY:
            g_set.closeToTray = !g_set.closeToTray;
            SaveSettings();
            Redraw();
            break;
    }
}

int HitTest(int x, int y) {
    for (int i = (int)g_widgets.size() - 1; i >= 0; --i)
        if (g_widgets[i].r.Contains((REAL)x, (REAL)y) && WidgetEnabled(g_widgets[i].id)) return i;
    return -1;
}

void OnDrop(HDROP drop) {
    wchar_t buf[MAX_PATH];
    if (DragQueryFileW(drop, 0, buf, MAX_PATH)) {
        std::wstring e = Ext(buf);
        if (e == L".cur" || e == L".ani") {
            g_page = kPageCursor;
            LoadCursorPath(buf);
        } else {
            g_page = kPageWallpaper;
            LoadWallpaperPreview(buf);
        }
        Relayout();
    }
    DragFinish(drop);
}

void CheckFullscreen() {
    if (!g_set.pauseFull || !g_wall.Running()) return;
    QUERY_USER_NOTIFICATION_STATE s;
    if (SHQueryUserNotificationState(&s) != S_OK) return;
    bool full = s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE;
    if (full != g_autoPaused) {
        g_autoPaused = full;
        ApplyPause();
        if (IsWindowVisible(g_hwnd)) Redraw();
    }
}

HICON MakeIcon() {
    Bitmap bmp(64, 64, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        RectF r(2, 2, 60, 60);
        LinearGradientBrush br(r, cAcc1, cAcc2, LinearGradientModeForwardDiagonal);
        GraphicsPath p;
        RoundPath(p, r, 16);
        g.FillPath(&br, &p);
        PointF pts[] = {{19, 12}, {19, 46}, {28, 38}, {35, 52}, {41, 49}, {34, 36}, {46, 36}};
        g.FillPolygon(SB(Color(255, 255, 255, 255)), pts, 7);
    }
    HICON h = nullptr;
    bmp.GetHICON(&h);
    return h;
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_msgShow && g_msgShow) {
        ShowWindowFromTray();
        return 0;
    }
    if (m == g_msgTaskbar && g_msgTaskbar) {  // Explorer zostal zrestartowany
        g_inTray = false;
        AddTray();
        if (!g_set.wallpaper.empty()) StartWallpaper(g_set.wallpaper);
        return 0;
    }
    switch (m) {
        case WM_CREATE: {
            g_hwnd = h;
            HDC sdc = GetDC(nullptr);
            g_scale = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
            ReleaseDC(nullptr, sdc);
            MakeFonts();
            g_icon = MakeIcon();
            SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
            SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
            BOOL dark = TRUE;
            DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));
            COLORREF cap = RGB(18, 20, 27);
            DwmSetWindowAttribute(h, 35, &cap, sizeof(cap));
            g_prevHost = CreateMediaHost(h, WS_CHILD, 0, 0, 0, 10, 10, &g_prevView);
            DragAcceptFiles(h, TRUE);
            AddTray();
            SetTimer(h, TIMER_WATCH, 2500, nullptr);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize.x = (LONG)S(1000);
            mm->ptMinTrackSize.y = (LONG)S(700);
            return 0;
        }
        case WM_SIZE:
        case WM_SHOWWINDOW:
            if (g_hwnd && g_icon) Relayout();
            return 0;
        case WM_DPICHANGED: {
            g_scale = HIWORD(w) / 96.0f;
            MakeFonts();
            const RECT* r = (const RECT*)l;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Relayout();
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: OnPaint(); return 0;
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                g_tracking = true;
            }
            int hit = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (hit != g_hot) {
                g_hot = hit;
                Redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hot = -1;
            Redraw();
            return 0;
        case WM_LBUTTONDOWN:
            g_down = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (g_down >= 0) SetCapture(h);
            Redraw();
            return 0;
        case WM_LBUTTONUP: {
            int hit = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            int was = g_down;
            g_down = -1;
            ReleaseCapture();
            if (was >= 0 && hit == was) OnClick(g_widgets[hit].id);
            Redraw();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(h, &p);
                if (HitTest(p.x, p.y) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                // podglad "na zywo": nad kartą pokazujemy wczytany kursor
                if (g_page == kPageCursor && g_cur.handle && g_geo.card.Contains((REAL)p.x, (REAL)p.y)) {
                    SetCursor(g_cur.handle);
                    return TRUE;
                }
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (w == TIMER_CURSOR && g_cur.Animated()) {
                g_step = (g_step + 1) % g_cur.steps;
                g_frame.reset(g_cur.RenderStep(g_step));
                ScheduleCursorFrame();
                if (g_page == kPageCursor && IsWindowVisible(h)) {
                    RECT r = {(LONG)g_geo.card.X, (LONG)g_geo.card.Y, (LONG)g_geo.card.GetRight() + 1, (LONG)g_geo.card.GetBottom() + 1};
                    InvalidateRect(h, &r, FALSE);
                }
            } else if (w == TIMER_WATCH) {
                CheckFullscreen();
            }
            return 0;
        case WM_DROPFILES: OnDrop((HDROP)w); return 0;
        case WM_DISPLAYCHANGE:
            if (g_wall.Running()) StartWallpaper(g_wall.Path());
            return 0;
        case WM_TRAY:
            if (LOWORD(l) == WM_LBUTTONUP) ShowWindowFromTray();
            else if (LOWORD(l) == WM_RBUTTONUP) ShowTrayMenu();
            return 0;
        case WM_CLOSE:
            if (g_set.closeToTray && g_wall.Running()) {
                ShowWindow(h, SW_HIDE);
                return 0;
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            SaveSettings();
            KillTimer(h, TIMER_CURSOR);
            KillTimer(h, TIMER_WATCH);
            RemoveTray();
            g_prevView.Close();
            g_wall.Stop();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    g_inst = inst;
    g_msgShow = RegisterWindowMessageW(L"KursorX.Show");
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"KursorX.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"KursorXMain", nullptr);
        if (other) PostMessageW(other, g_msgShow, 0, 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gin;
    ULONG_PTR token;
    GdiplusStartup(&token, &gin, nullptr);
    int exitCode = 0;
    {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        RegisterMediaClasses(inst);
        LoadSettings();

        WNDCLASSW wc{};
        wc.lpfnWndProc = MainProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"KursorXMain";
        RegisterClassW(&wc);

        HDC sdc = GetDC(nullptr);
        float sc = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, sdc);
        HWND w = CreateWindowExW(0, wc.lpszClassName, L"KursorX", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                 CW_USEDEFAULT, (int)(1100 * sc), (int)(760 * sc), nullptr, nullptr, inst, nullptr);

        if (!g_set.cursorPath.empty() && g_cur.Load(g_set.cursorPath)) {
            g_frame.reset(g_cur.RenderStep(0));
            ScheduleCursorFrame();
        }
        if (g_set.resume && !g_set.wallpaper.empty() && GetFileAttributesW(g_set.wallpaper.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (StartWallpaper(g_set.wallpaper)) {
                g_wallSel = g_set.wallpaper;
                g_prevView.Open(g_prevHost, g_wallSel, true, g_set.fit);
            }
        }
        // argumenty: --tray (start w zasobniku), --page N, sciezka do pliku do wczytania
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--page" && i + 1 < argc) g_page = Mn(Mx(_wtoi(argv[++i]), 0), (int)kPageCount - 1);
            else if (a[0] != L'-') {
                std::wstring e = Ext(a);
                if (e == L".cur" || e == L".ani") { g_page = kPageCursor; LoadCursorPath(a); }
                else { g_page = kPageWallpaper; LoadWallpaperPreview(a); }
            }
        }
        if (argv) LocalFree(argv);
        bool hidden = wcsstr(cmd, L"--tray") != nullptr && g_wall.Running();
        ShowWindow(w, hidden ? SW_HIDE : show);
        if (!hidden) UpdateWindow(w);
        Relayout();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = (int)msg.wParam;
        g_frame.reset();
        g_prevView.Close();
        g_fTitle.reset(); g_fH.reset(); g_fBody.reset(); g_fBold.reset(); g_fSmall.reset();
        g_fam.reset();
    }
    GdiplusShutdown(token);
    CoUninitialize();
    CloseHandle(mutex);
    return exitCode;
}
