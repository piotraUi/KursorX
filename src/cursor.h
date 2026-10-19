#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace Gdiplus { class Bitmap; }

// Wczytany plik kursora (.cur / .ani) wraz z danymi do podgladu.
struct CursorData {
    HCURSOR handle = nullptr;
    int width = 0, height = 0;
    int hotX = 0, hotY = 0;
    int steps = 1;  // liczba krokow animacji (1 = kursor statyczny)
    std::vector<int> delaysMs;
    std::wstring path;

    CursorData() = default;
    CursorData(const CursorData&) = delete;
    CursorData& operator=(const CursorData&) = delete;
    ~CursorData() { Reset(); }

    void Reset();
    bool Load(const std::wstring& file);
    bool Animated() const { return steps > 1; }
    int DelayMs(int step) const;
    // Klatka jako bitmapa z kanalem alfa (wolajacy usuwa). nullptr przy bledzie.
    Gdiplus::Bitmap* RenderStep(int step) const;
};

struct CursorRole {
    const wchar_t* label;
    const wchar_t* regName;
};
extern const CursorRole kCursorRoles[];
extern const int kCursorRoleCount;

// Kopiuje plik do %APPDATA%, wpisuje do rejestru i odswieza kursory systemu.
bool ApplyCursor(int role, const std::wstring& file, bool all, std::wstring& err);
// role < 0 => wszystkie role
void ResetCursor(int role);
