#!/bin/sh
# Cross-kompilacja pod Windows (MinGW-w64). Wynik: build/KursorX.exe
set -e
mkdir -p build
CXX=${CXX:-x86_64-w64-mingw32-g++}
RC=${RC:-x86_64-w64-mingw32-windres}
$RC src/app.rc -I src -O coff -o build/app.o
$CXX -O2 -std=c++17 -Wall -DUNICODE -D_UNICODE -municode -mwindows -static -s \
  src/main.cpp src/cursor.cpp src/media.cpp build/app.o -o build/KursorX.exe \
  -lgdiplus -lcomdlg32 -lcomctl32 -lshell32 -ladvapi32 -lgdi32 -lole32 -ldwmapi -lmfplay -lmfuuid -luuid -lshlwapi
