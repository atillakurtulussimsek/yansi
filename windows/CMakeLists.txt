cmake_minimum_required(VERSION 3.20)
project(ayna_win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Tek dosya, ek DLL gerektirmesin
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(ayna-win
    src/main.cpp
    src/net.cpp
    src/capture.cpp
    src/convert.cpp
    src/encoder.cpp
)

target_compile_definitions(ayna-win PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000006
)

if(MSVC)
    target_compile_options(ayna-win PRIVATE /W3 /utf-8 /permissive-)
endif()

target_link_libraries(ayna-win PRIVATE
    d3d11 dxgi
    mfplat mfuuid mf
    ws2_32 avrt winmm ole32 oleaut32
)
