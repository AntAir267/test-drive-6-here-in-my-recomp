#!/bin/bash
# tools/release/windows.sh OUT_DIR: the Windows download (a zip), with no part of the game in it,
# cross-built on Linux with Zig. Run it after linux.sh in the same checkout (its gen/: the
# translation of the player-to-be's TD6.exe, whose declarations the runtime is compiled against;
# FUNCS_SHA256).
#
# Everything is built with the Linux Zig 0.17.0 for x86_64-windows-gnu, from sources in
# third_party/ (README.md): SDL3, zlib and a small FFmpeg in third_party/win/, Vulkan's import
# library made from the loader's function list. The players' Zig (the Windows build of the same
# Zig, in the zip) compiles their translation with the same flags as on Linux (launcher.c). The
# zip is one folder: the launcher with its bundle beside it.
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$(realpath -m "${1:?usage: windows.sh OUT_DIR}")
TP=$ROOT/third_party
W=$(readlink -f "$TP/win")
Z=$(readlink -f "$TP/zig-x86_64-linux-0.17.0")/zig
ZW=$W/zig-x86_64-windows-0.17.0
T=x86_64-windows-gnu
for need in "$Z" "$W/sdl3/lib/libSDL3.a" "$W/ffmpeg/lib/libavcodec.a" "$W/zlib/lib/libz.a" \
            "$W/vulkan/lib/libvulkan-1.a" "$ZW/zig.exe" "$W/python-win/python/python.exe" \
            "$W/capstone-win/capstone" gen/funcs.h; do
    [ -e "$need" ] || { echo "missing $need (third_party/README.md; run linux.sh first for gen/)"; exit 1; }
done

R=$ROOT/build/win
rm -rf "$R"; mkdir -p "$R/obj" "$OUT"
TK=third_party/antistatic                  # (the AntiStatic toolkit: translator and most of the runtime)
RT_INC="-Iruntime -I$TK/runtime/cpu -I$TK/runtime/win32 -I$TK/runtime/d3d7"
INC="$RT_INC -Igen -I$W/sdl3/include -I$W/ffmpeg/include -I$W/zlib/include -I$W/vulkan/include"
COMMON="-std=gnu11 -g -fno-strict-aliasing -fno-omit-frame-pointer"

echo "== the runtime, for Windows (the toolkit's runtime/win standing in for the POSIX pieces)"
for f in $TK/runtime/cpu/fpu.c $TK/runtime/win32/*.c $TK/runtime/d3d7/*.c runtime/*.c $TK/runtime/win/platform.c; do
    n=$(basename "$f" .c); case $f in runtime/*) n=rt_$n;; *) n=tk_$n;; esac
    echo "$Z cc -target $T $COMMON -O2 -w -I$TK/runtime/win -include $TK/runtime/win/compat.h $INC -c $f -o $R/obj/$n.o"
done | xargs -P "${JOBS:-6}" -I{} sh -c '{}'
"$Z" ar rcs "$R/libruntime.a" "$R"/obj/tk_*.o "$R"/obj/rt_*.o

echo "== the icon and the resources (manifest: the program's settings for Windows)"
python3 - "$ROOT/assets/icon.png" "$R/td6.ico" <<'EOF'
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2], sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
EOF
cp runtime/win/td6.manifest "$R/"
{ cat runtime/win/td6.rc; echo '1 ICON "td6.ico"'; } > "$R/td6.rc"
(cd "$R" && "$Z" rc /fo td6.res td6.rc)

echo "== the game built with them (the check that the players' build links)"
GF="$COMMON -O2 -w -DCPU_FAST_MEM -fno-slp-vectorize $RT_INC -Igen"
for f in $(ls gen/code_*.c) gen/table.c gen/stubs.c; do
    echo "$Z cc -target $T $GF -c $f -o $R/obj/gen_$(basename "$f" .c).o"
done | xargs -P "${JOBS:-6}" -I{} sh -c '{}'
SYSLIBS="-luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi
         -lshell32 -ldinput8 -lbcrypt -lpthread -static -Wl,--stack,8388608 -Wl,--subsystem,windows -Wl,-e,mainCRTStartup"
WINLIBS="-lz -lvulkan-1 $SYSLIBS"
LIBDIRS="-L$W/sdl3/lib -L$W/ffmpeg/lib -L$W/zlib/lib -L$W/vulkan/lib"
FFL="-lSDL3 -lavformat -lavcodec -lswscale -lswresample -lavutil"
"$Z" cc -target $T -o "$R/td6.exe" "$R"/obj/gen_*.o -Wl,--whole-archive "$R/libruntime.a" -Wl,--no-whole-archive \
    "$R/td6.res" $LIBDIRS $FFL $WINLIBS

B=$OUT/td6-recomp
rm -rf "$B"
mkdir -p "$B/lib" "$B/include" "$B/builds" "$B/tools" "$B/licenses"

echo "== the bundle: the runtime and its libraries, the translator's files"
cp "$R/libruntime.a" "$R/td6.res" "$W/sdl3/lib/libSDL3.a" "$W/zlib/lib/libz.a" "$W/vulkan/lib/libvulkan-1.a" "$B/lib/"
cp "$W"/ffmpeg/lib/lib*.a "$B/lib/"
cp $TK/runtime/cpu/*.h $TK/runtime/win32/runtime.h runtime/port.h "$B/include/"
cp builds/td6_1999.py "$B/builds/"
sha256sum gen/funcs.h | cut -c1-64 > "$B/FUNCS_SHA256"
REL=$(cat tools/release/VERSION)
echo "td6-recomp $(date -u +%Y-%m-%d) $(git rev-parse --short HEAD) windows|$REL" > "$B/VERSION"
cp $TK/translator/*.py tools/install.py "$B/tools/"

echo "== Python (python-build-standalone) with Capstone, Zig for Windows"
cp -a "$W/python-win/python" "$B/python"
rm -rf "$B/python/Lib/test" "$B/python/Lib/idlelib" "$B/python/Lib/tkinter" "$B/python/Lib/turtledemo" \
       "$B/python/Lib/ensurepip" "$B/python/tcl" "$B/python/include" "$B/python/libs"
cp -r "$W/capstone-win/capstone" "$B/python/Lib/site-packages/"
cp -a "$ZW" "$B/zig"
rm -rf "$B/zig/doc"

echo "== the launcher"
"$Z" cc -target $T -O2 -I"$W/sdl3/include" -o "$B/td6-recomp.exe" launcher/launcher.c "$R/td6.res" \
    "$W/sdl3/lib/libSDL3.a" $SYSLIBS
rm -f "$B"/*.pdb

echo "== the licences (every part the download carries; CREDITS.md names them)"
L=$B/licenses
cp CREDITS.md LICENSE "$L/"
cp $TK/LICENSE "$L/AntiStatic-LICENSE.txt"
cp "$(readlink -f "$TP/sdl3-rel")"/../SDL3-*/LICENSE.txt "$L/SDL3-LICENSE.txt"
cp "$W/zlib/LICENSE" "$L/zlib-LICENSE.txt"
cp "$TP"/ffmpeg-8.0.1/COPYING.LGPLv2.1 "$L/FFmpeg-COPYING.LGPLv2.1"
cp "$TP"/ffmpeg-8.0.1.tar.xz "$L/"
cat > "$L/FFmpeg-BUILD.txt" <<EOF2
FFmpeg 8.0.1 (LGPL 2.1 or later), from ffmpeg-8.0.1.tar.xz beside this file (https://ffmpeg.org),
built as static libraries (lib/libav*.a, lib/libsw*.a; the game is linked with them on the
player's computer, so they can be replaced) with Zig 0.17.0 for x86_64-windows-gnu:

  configure --target-os=mingw32 --arch=x86_64 --enable-cross-compile --enable-static --disable-shared
    --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything
    --disable-x86asm --disable-debug --enable-w32threads
    --enable-decoder=bink,binkaudio_rdft,binkaudio_dct,wmav1,wmav2 --enable-demuxer=bink,asf
    --enable-protocol=file --enable-swscale --enable-swresample --disable-avdevice --disable-avfilter
EOF2
cp "$ZW/LICENSE" "$L/Zig-LICENSE.txt"
for x in libcxx libcxxabi libunwind; do cp "$ZW/lib/$x/LICENSE.TXT" "$L/LLVM-$x-LICENSE.txt"; done
cp "$ZW/lib/libc/mingw/COPYING" "$L/MinGW-w64-COPYING.txt" 2>/dev/null || true
cp "$W/python-win/python/LICENSE.txt" "$L/Python-LICENSE.txt"
cp "$W"/capstone-win/capstone-*.dist-info/LICENSE.TXT "$L/Capstone-LICENSE.txt"

echo "== zip"
ZIP=Test_Drive_6-Here_In_My_Recomp-$REL-windows-x86_64.zip
rm -f "$OUT/$ZIP"
(cd "$OUT" && zip -q -r -9 "$ZIP" td6-recomp)
ls -la "$OUT/$ZIP"
