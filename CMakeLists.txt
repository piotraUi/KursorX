cmake_minimum_required(VERSION 3.16)
project(KursorX CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(KursorX WIN32 src/main.cpp src/app.rc)
target_compile_definitions(KursorX PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(KursorX PRIVATE user32 gdi32 comdlg32 comctl32 advapi32 shell32)
if(MINGW)
  target_link_options(KursorX PRIVATE -static -municode)
endif()
