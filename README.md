# KursorX

Mała aplikacja Win32 (C++17) do zmiany kursora myszy.

- wybierasz plik `.cur` lub `.ani` (przycisk lub przeciągnij i upuść),
- widzisz powiększony podgląd (animowane kursory są animowane),
- wybierasz rolę kursora (strzałka, ręka, I-beam…) i klikasz **Ustaw**,
- **Domyślny** przywraca domyślny kursor (jednej roli albo wszystkich).

Plik jest kopiowany do `%APPDATA%\KursorX`, a ścieżka trafia do `HKCU\Control Panel\Cursors`, więc zmiana przetrwa restart.

## Budowanie

    cmake -B build && cmake --build build --config Release

(MSVC albo MinGW; na Linuksie: `-DCMAKE_TOOLCHAIN_FILE` dla `x86_64-w64-mingw32`.)
