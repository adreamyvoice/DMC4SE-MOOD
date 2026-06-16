# MistressDMC — DMC4SE MOOD
# Native Windows build (Visual Studio / MSVC, or MinGW). 32-bit, because the game
# is 32-bit. Produces dinput8.dll.
#
#   cmake -B build -A Win32
#   cmake --build build --config Release
#   -> build/Release/dinput8.dll
cmake_minimum_required(VERSION 3.15)
project(MistressDMC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(dinput8 SHARED
    src/dllmain.cpp
    src/cheats_generated.cpp
    src/dinput8.def
    ${TP}/imgui/imgui.cpp
    ${TP}/imgui/imgui_draw.cpp
    ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp
    ${TP}/imgui/backends/imgui_impl_dx10.cpp
    ${TP}/imgui/backends/imgui_impl_win32.cpp
    ${TP}/kiero/kiero.cpp
    ${TP}/minhook/src/hook.c
    ${TP}/minhook/src/buffer.c
    ${TP}/minhook/src/trampoline.c
    ${TP}/minhook/src/hde/hde32.c
)

target_include_directories(dinput8 PRIVATE
    ${TP}/imgui ${TP}/imgui/backends ${TP}/kiero ${TP} ${TP}/minhook/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(dinput8 PRIVATE
    d3d10 dxgi d3dcompiler dxguid ole32 imm32 gdi32 user32 dwmapi winmm)

# Name the output exactly dinput8.dll (no "lib" prefix on MinGW).
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")

if(MSVC)
    target_compile_definitions(dinput8 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(dinput8 PRIVATE /permissive- /MP)
else()
    # MinGW
    target_compile_options(dinput8 PRIVATE -O2 -fpermissive)
    target_link_options(dinput8 PRIVATE -static -static-libgcc -static-libstdc++)
endif()
