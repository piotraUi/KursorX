#include "cursor.h"

#include <gdiplus.h>

#include <algorithm>
#include <cstring>

#include "util.h"

const CursorRole kCursorRoles[] = {
    {L"Strzałka", L"Arrow"},
    {L"Pomoc", L"Help"},
    {L"Praca w tle", L"AppStarting"},
    {L"Zajęty", L"Wait"},
    {L"Celownik", L"Crosshair"},
    {L"Tekst", L"IBeam"},
    {L"Niedostępny", L"No"},
    {L"Rozmiar pionowo", L"SizeNS"},
    {L"Rozmiar poziomo", L"SizeWE"},
    {L"Rozmiar skos \\", L"SizeNWSE"},
    {L"Rozmiar skos /", L"SizeNESW"},
    {L"Przenieś", L"SizeAll"},
    {L"Strzałka w górę", L"UpArrow"},
    {L"Łącze (rączka)", L"Hand"},
};
const int kCursorRoleCount = (int)(sizeof(kCursorRoles) / sizeof(kCursorRoles[0]));

namespace {

DWORD Rd32(const BYTE* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24); }

bool ReadAll(const std::wstring& path, std::vector<BYTE>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64 << 20);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Czyta z naglowka RIFF/ACON liczbe krokow i opoznienia (w jiffies = 1/60 s).
void ParseAni(const std::vector<BYTE>& d, int& steps, std::vector<int>& delays) {
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) != 0 || memcmp(d.data() + 8, "ACON", 4) != 0) return;
    DWORD jifDefault = 6, cSteps = 0;
    std::vector<DWORD> rates;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const BYTE* c = d.data() + pos;
        DWORD len = Rd32(c + 4);
        size_t body = pos + 8;
        if (body + len > d.size()) break;
        if (!memcmp(c, "anih", 4) && len >= 36) {
            cSteps = Rd32(d.data() + body + 8);
            jifDefault = Rd32(d.data() + body + 28);
        } else if (!memcmp(c, "rate", 4)) {
            for (DWORD i = 0; i + 1 <= len / 4; ++i) rates.push_back(Rd32(d.data() + body + i * 4));
        }
        pos = body + len + (len & 1);
    }
    if (cSteps == 0) return;
    steps = (int)cSteps;
    delays.resize(cSteps);
    for (DWORD i = 0; i < cSteps; ++i) {
        DWORD j = i < rates.size() ? rates[i] : jifDefault;
        delays[i] = (int)Mx<DWORD>(j * 1000 / 60, 20);
    }
}

HBITMAP MakeDib(int w, int h, BYTE** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)bits, nullptr, 0);
}

}  // namespace

void CursorData::Reset() {
    if (handle) DestroyCursor(handle);
    handle = nullptr;
    width = height = hotX = hotY = 0;
    steps = 1;
    delaysMs.clear();
    path.clear();
}

bool CursorData::Load(const std::wstring& file) {
    HCURSOR c = (HCURSOR)LoadImageW(nullptr, file.c_str(), IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE);
    if (!c) return false;

    ICONINFO ii{};
    if (!GetIconInfo(c, &ii)) {
        DestroyCursor(c);
        return false;
    }
    BITMAP bm{};
    HBITMAP src = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    GetObjectW(src, sizeof(bm), &bm);
    int w = bm.bmWidth, h = bm.bmHeight;
    if (!ii.hbmColor) h /= 2;  // maska monochromatyczna: AND + XOR jedna pod druga
    int hx = (int)ii.xHotspot, hy = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    Reset();
    handle = c;
    width = w;
    height = h;
    hotX = hx;
    hotY = hy;
    path = file;

    if (Ext(file) == L".ani") {
        std::vector<BYTE> data;
        if (ReadAll(file, data)) ParseAni(data, steps, delaysMs);
    }
    return true;
}

int CursorData::DelayMs(int step) const {
    if (step >= 0 && step < (int)delaysMs.size()) return delaysMs[step];
    return 100;
}

Gdiplus::Bitmap* CursorData::RenderStep(int step) const {
    if (!handle || width <= 0 || height <= 0) return nullptr;
    const int w = width, h = height;
    BYTE *onBlack = nullptr, *onWhite = nullptr;
    HBITMAP bb = MakeDib(w, h, &onBlack), bw = MakeDib(w, h, &onWhite);
    if (!bb || !bw) {
        if (bb) DeleteObject(bb);
        if (bw) DeleteObject(bw);
        return nullptr;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    memset(onBlack, 0, (size_t)w * h * 4);
    memset(onWhite, 255, (size_t)w * h * 4);

    HGDIOBJ old = SelectObject(dc, bb);
    DrawIconEx(dc, 0, 0, handle, w, h, step, nullptr, DI_NORMAL);
    SelectObject(dc, bw);
    DrawIconEx(dc, 0, 0, handle, w, h, step, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    DeleteDC(dc);

    // Alfa wyliczana z dwoch renderow (na czarnym i bialym tle).
    auto* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
    Gdiplus::BitmapData bd;
    Gdiplus::Rect r(0, 0, w, h);
    if (bmp->LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) != Gdiplus::Ok) {
        delete bmp;
        DeleteObject(bb);
        DeleteObject(bw);
        return nullptr;
    }
    for (int y = 0; y < h; ++y) {
        BYTE* out = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
        const BYTE* pb = onBlack + (size_t)y * w * 4;
        const BYTE* pw = onWhite + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, out += 4, pb += 4, pw += 4) {
            int diff = ((pw[0] - pb[0]) + (pw[1] - pb[1]) + (pw[2] - pb[2])) / 3;
            int a = std::clamp(255 - diff, 0, 255);
            out[0] = (BYTE)Mn<int>(pb[0], a);
            out[1] = (BYTE)Mn<int>(pb[1], a);
            out[2] = (BYTE)Mn<int>(pb[2], a);
            out[3] = (BYTE)a;
        }
    }
    bmp->UnlockBits(&bd);
    DeleteObject(bb);
    DeleteObject(bw);
    return bmp;
}

namespace {

bool SetRegCursor(const wchar_t* name, const std::wstring& value) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, REG_EXPAND_SZ, (const BYTE*)value.c_str(),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

void Reload() { SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE); }

}  // namespace

bool ApplyCursor(int role, const std::wstring& file, bool all, std::wstring& err) {
    std::wstring dir = DataDir();
    if (dir.empty()) {
        err = L"Nie można utworzyć folderu danych.";
        return false;
    }
    // Kopia w %APPDATA%: kursor działa nawet po usunięciu oryginału.
    std::wstring tag = all ? L"all" : kCursorRoles[role].regName;
    std::wstring dest = dir + L"\\" + tag + L"_" + FileName(file);
    if (!CopyFileW(file.c_str(), dest.c_str(), FALSE)) {
        err = L"Nie udało się skopiować pliku kursora.";
        return false;
    }
    for (int i = 0; i < kCursorRoleCount; ++i) {
        if (!all && i != role) continue;
        if (!SetRegCursor(kCursorRoles[i].regName, dest)) {
            err = L"Nie udało się zapisać w rejestrze.";
            return false;
        }
    }
    Reload();
    return true;
}

void ResetCursor(int role) {
    for (int i = 0; i < kCursorRoleCount; ++i)
        if (role < 0 || i == role) SetRegCursor(kCursorRoles[i].regName, L"");
    Reload();
}
