cmake_minimum_required(VERSION 3.16)
project(KursorX CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(KursorX WIN32 src/main.cpp src/cursor.cpp src/media.cpp src/app.rc)
target_compile_definitions(KursorX PRIVATE UNICODE _UNICODE)
target_link_libraries(KursorX PRIVATE user32 gdi32 gdiplus comdlg32 comctl32 advapi32 shell32 ole32 dwmapi mfplay mfuuid uuid)
if(MSVC)
  target_compile_options(KursorX PRIVATE /utf-8)
else()
  target_link_options(KursorX PRIVATE -static -municode)
endif()
