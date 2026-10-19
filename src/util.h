#pragma once
#include <windows.h>
#include <string>

template <class T> inline T Mn(T a, T b) { return a < b ? a : b; }
template <class T> inline T Mx(T a, T b) { return a > b ? a : b; }

inline std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

inline std::wstring Ext(const std::wstring& p) {
    size_t i = p.find_last_of(L'.');
    if (i == std::wstring::npos) return L"";
    std::wstring e = p.substr(i);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

// %APPDATA%\KursorX (tworzony przy pierwszym wywolaniu)
std::wstring DataDir();
