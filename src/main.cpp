// KursorX - wybierz plik kursora (.cur / .ani), zobacz podglad, ustaw lub przywroc domyslny.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>

namespace {

enum : int {
    ID_BROWSE = 100,
    ID_APPLY,
    ID_RESET,
    ID_ROLE,
    ID_PATH,
    ID_STATUS,
    ID_PREVIEW,
    ID_TIMER = 1,
};

struct Role {
    const wchar_t* label;
    const wchar_t* regName;
};

const Role kRoles[] = {
    {L"Normalny (strzalka)", L"Arrow"},
    {L"Pomoc", L"Help"},
    {L"Dzialanie w tle", L"AppStarting"},
    {L"Zajety", L"Wait"},
    {L"Celownik", L"Crosshair"},
    {L"Tekst (I-beam)", L"IBeam"},
    {L"Niedostepny", L"No"},
    {L"Zmiana rozmiaru pion", L"SizeNS"},
    {L"Zmiana rozmiaru poziom", L"SizeWE"},
    {L"Zmiana rozmiaru diag. 1", L"SizeNWSE"},
    {L"Zmiana rozmiaru diag. 2", L"SizeNESW"},
    {L"Przenies", L"SizeAll"},
    {L"Strzalka w gore", L"UpArrow"},
    {L"Lacze (raczka)", L"Hand"},
};

HINSTANCE g_inst;
HWND g_main, g_path, g_role, g_preview, g_status, g_apply;
HCURSOR g_cursor = nullptr;  // aktualnie podgladany kursor
int g_step = 0;              // klatka animacji
std::wstring g_file;

void SetStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

void FreeCursor() {
    if (g_cursor) DestroyCursor(g_cursor);
    g_cursor = nullptr;
    g_step = 0;
}

bool LoadPreview(const std::wstring& file) {
    HCURSOR c = (HCURSOR)LoadImageW(nullptr, file.c_str(), IMAGE_CURSOR, 0, 0,
                                    LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!c) return false;
    FreeCursor();
    g_cursor = c;
    g_file = file;
    SetWindowTextW(g_path, file.c_str());
    EnableWindow(g_apply, TRUE);
    InvalidateRect(g_preview, nullptr, TRUE);
    return true;
}

bool PickFile(HWND owner, std::wstring& out) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Kursory (*.cur;*.ani)\0*.cur;*.ani\0Wszystkie pliki\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Wybierz plik kursora";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

std::wstring DataDir() {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return L"";
    std::wstring dir = std::wstring(appdata) + L"\\KursorX";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool SetRegCursor(const wchar_t* name, const std::wstring& value) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, REG_EXPAND_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

void ReloadSystemCursors() {
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
}

void OnApply() {
    int sel = (int)SendMessageW(g_role, CB_GETCURSEL, 0, 0);
    if (sel < 0 || g_file.empty()) return;

    // Kopia do %APPDATA%, zeby kursor dzialal nawet po usunieciu oryginalu.
    std::wstring dir = DataDir();
    if (dir.empty()) { SetStatus(L"Nie mozna utworzyc folderu danych."); return; }
    size_t slash = g_file.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? g_file : g_file.substr(slash + 1);
    std::wstring dest = dir + L"\\" + kRoles[sel].regName + L"_" + name;
    if (!CopyFileW(g_file.c_str(), dest.c_str(), FALSE)) {
        SetStatus(L"Nie udalo sie skopiowac pliku kursora.");
        return;
    }
    if (!SetRegCursor(kRoles[sel].regName, dest)) {
        SetStatus(L"Nie udalo sie zapisac w rejestrze.");
        return;
    }
    ReloadSystemCursors();
    SetStatus(L"Ustawiono kursor: " + std::wstring(kRoles[sel].label));
}

void OnReset() {
    int sel = (int)SendMessageW(g_role, CB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    int ans = MessageBoxW(g_main,
                          L"Tak - przywroc domyslny kursor tylko dla wybranej roli.\n"
                          L"Nie - przywroc wszystkie kursory do domyslnych.",
                          L"Przywroc domyslny", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (ans == IDCANCEL) return;
    if (ans == IDYES) {
        SetRegCursor(kRoles[sel].regName, L"");
    } else {
        for (const Role& r : kRoles) SetRegCursor(r.regName, L"");
    }
    ReloadSystemCursors();
    SetStatus(L"Przywrocono domyslny kursor.");
}

void PaintPreview(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);

    // tlo w szachownice, zeby bylo widac przezroczystosc
    HBRUSH light = CreateSolidBrush(RGB(235, 235, 235));
    HBRUSH dark = CreateSolidBrush(RGB(205, 205, 205));
    const int cell = 12;
    for (int y = 0; y < rc.bottom; y += cell)
        for (int x = 0; x < rc.right; x += cell) {
            RECT r{x, y, x + cell, y + cell};
            FillRect(mem, &r, ((x / cell + y / cell) & 1) ? dark : light);
        }
    DeleteObject(light);
    DeleteObject(dark);

    if (g_cursor) {
        // Kursor rysowany w 4x skali przez bufor pomocniczy dla lepszej widocznosci.
        int w = GetSystemMetrics(SM_CXCURSOR), h = GetSystemMetrics(SM_CYCURSOR);
        HDC small = CreateCompatibleDC(dc);
        HBITMAP sb = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ so = SelectObject(small, sb);
        HBRUSH bg = CreateSolidBrush(RGB(220, 220, 220));
        RECT r{0, 0, w, h};
        FillRect(small, &r, bg);
        DeleteObject(bg);
        if (!DrawIconEx(small, 0, 0, g_cursor, w, h, g_step, nullptr, DI_NORMAL) && g_step != 0) {
            g_step = 0;
            DrawIconEx(small, 0, 0, g_cursor, w, h, 0, nullptr, DI_NORMAL);
        }
        int dw = w * 4, dh = h * 4;
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem, (rc.right - dw) / 2, (rc.bottom - dh) / 2, dw, dh, small, 0, 0, w, h,
                   SRCCOPY);
        SelectObject(small, so);
        DeleteObject(sb);
        DeleteDC(small);
    } else {
        SetBkMode(mem, TRANSPARENT);
        DrawTextW(mem, L"Brak podgladu - wybierz plik kursora", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: PaintPreview(h); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (g_cursor) {
                ++g_step;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Browse() {
    std::wstring f;
    if (!PickFile(g_main, f)) return;
    if (!LoadPreview(f)) SetStatus(L"Nie mozna wczytac tego pliku jako kursora.");
    else SetStatus(L"Wczytano. Wybierz role i kliknij \"Ustaw\".");
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            g_main = h;
            HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int cw,
                          int ch, int id) {
                HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, cw, ch, h,
                                         (HMENU)(INT_PTR)id, g_inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
                return c;
            };
            mk(L"BUTTON", L"Wybierz plik...", BS_PUSHBUTTON | WS_TABSTOP, 10, 10, 120, 26, ID_BROWSE);
            g_path = mk(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER, 140, 12, 330, 22, ID_PATH);
            g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"KursorXPreview", L"", WS_CHILD | WS_VISIBLE,
                                        10, 46, 460, 200, h, (HMENU)ID_PREVIEW, g_inst, nullptr);
            mk(L"STATIC", L"Rola kursora:", 0, 10, 260, 90, 20, 0);
            g_role = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 100, 256, 220, 200, ID_ROLE);
            for (const Role& r : kRoles) SendMessageW(g_role, CB_ADDSTRING, 0, (LPARAM)r.label);
            SendMessageW(g_role, CB_SETCURSEL, 0, 0);
            g_apply = mk(L"BUTTON", L"Ustaw", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 330, 254, 65, 26, ID_APPLY);
            mk(L"BUTTON", L"Domyslny", BS_PUSHBUTTON | WS_TABSTOP, 400, 254, 70, 26, ID_RESET);
            g_status = mk(L"STATIC", L"Wybierz plik .cur lub .ani", 0, 10, 292, 460, 20, ID_STATUS);
            DragAcceptFiles(h, TRUE);
            SetTimer(g_preview, ID_TIMER, 60, nullptr);
            return 0;
        }
        case WM_DROPFILES: {
            wchar_t buf[MAX_PATH];
            if (DragQueryFileW((HDROP)w, 0, buf, MAX_PATH) && !LoadPreview(buf))
                SetStatus(L"Nie mozna wczytac tego pliku jako kursora.");
            DragFinish((HDROP)w);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_BROWSE: Browse(); break;
                case ID_APPLY: OnApply(); break;
                case ID_RESET: OnReset(); break;
            }
            return 0;
        case WM_DESTROY:
            FreeCursor();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW pc{};
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.lpszClassName = L"KursorXPreview";
    RegisterClassW(&pc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KursorXMain";
    RegisterClassW(&wc);

    RECT r{0, 0, 480, 322};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND w = CreateWindowW(wc.lpszClassName, L"KursorX",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                           CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst,
                           nullptr);
    ShowWindow(w, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
