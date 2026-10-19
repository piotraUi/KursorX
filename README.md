# KursorX

Aplikacja Win32 (C++17, GDI+ / Media Foundation) z nowoczesnym ciemnym interfejsem.

**Kursor** – wybierasz `.cur` / `.ani` (przycisk lub przeciągnięcie), widzisz podgląd z animacją
(z prawdziwym czasem klatek) i punktem aktywnym, wskazujesz rolę kursora i klikasz *Ustaw*.
*Przywróć domyślny* czyści wpisy i wraca do kursorów Windows.

**Tapeta** – film (MP4/MOV/WMV/AVI…), GIF lub obraz jako żywe tło pulpitu, na każdym monitorze,
za ikonami. Tryby dopasowania, wyciszanie, auto-pauza w grach/pełnym ekranie. Aplikacja działa
w zasobniku, może startować z Windows i sama wznawiać tapetę.

Kursory są kopiowane do `%APPDATA%\KursorX`, a ścieżki zapisywane w `HKCU\Control Panel\Cursors`.

## Budowanie

    ./build.sh                       # MinGW-w64 -> build/KursorX.exe
    cmake -B build && cmake --build build --config Release   # MSVC / MinGW
