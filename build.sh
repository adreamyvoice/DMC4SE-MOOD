#!/bin/bash
set -e
cd "$(dirname "$0")"
export PATH="/opt/homebrew/bin:$PATH"

CXX=i686-w64-mingw32-g++
CC=i686-w64-mingw32-gcc
TP=third_party
OUT=build
mkdir -p "$OUT"

INC="-I$TP/imgui -I$TP/imgui/backends -I$TP/kiero -I$TP/minhook/include -I$TP -Isrc"
CFLAGS="-m32 -O2 -DNDEBUG"
CXXFLAGS="$CFLAGS -std=c++17 -fpermissive $INC ${MOOD_DEV:+-DMOOD_DEV}"   # MOOD_DEV=1 ./build.sh -> dev tools (logger, parry capture)

echo "== compiling MinHook (C) =="
MH_OBJ=""
for f in $TP/minhook/src/hook.c $TP/minhook/src/buffer.c $TP/minhook/src/trampoline.c \
         $TP/minhook/src/hde/hde32.c; do
    o="$OUT/$(basename "$f").o"
    $CC $CFLAGS -I$TP/minhook/include -c "$f" -o "$o"
    MH_OBJ="$MH_OBJ $o"
done

echo "== compiling ImGui + kiero + dllmain (C++) =="
CPP_OBJ=""
for f in $TP/imgui/imgui.cpp $TP/imgui/imgui_draw.cpp $TP/imgui/imgui_tables.cpp \
         $TP/imgui/imgui_widgets.cpp \
         $TP/imgui/backends/imgui_impl_dx10.cpp $TP/imgui/backends/imgui_impl_win32.cpp \
         $TP/kiero/kiero.cpp src/dllmain.cpp src/cheats_generated.cpp; do
    o="$OUT/$(basename "$f").o"
    $CXX $CXXFLAGS -c "$f" -o "$o"
    CPP_OBJ="$CPP_OBJ $o"
done

echo "== linking dinput8.dll =="
$CXX -m32 -shared -o "$OUT/dinput8.dll" \
    $CPP_OBJ $MH_OBJ src/dinput8.def \
    -static -static-libgcc -static-libstdc++ \
    -ld3d10 -ldxgi -ld3dcompiler -ldxguid -lole32 -limm32 -lgdi32 -luser32 -lkernel32 -ldwmapi -lwinmm

echo "== done: $OUT/dinput8.dll =="
file "$OUT/dinput8.dll"
i686-w64-mingw32-objdump -p "$OUT/dinput8.dll" | grep -A8 "Export Address Table" | head -10
