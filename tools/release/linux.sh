#!/bin/bash
# tools/release/linux.sh OUT_DIR: the Linux download (an AppImage), with no part of the game in it.
#
# Run it in a checkout of its own (a git worktree): its build/ and gen/ become the release build.
# It needs the player-to-be's TD6.exe once, as every build does (GAME=..., default the usual game
# folder): the runtime (AntiStatic, third_party/antistatic, plus runtime/) is compiled against the
# declarations the translator writes (gen/funcs.h),
# and the players' translation must give the same (FUNCS_SHA256, checked by the launcher).
#
# Everything is built with Zig 0.17.0 for glibc 2.31 (third_party/zig-*, SDL3 from
# third_party/sdl3-rel, FFmpeg from third_party/ffmpeg-rel; third_party/README.md), so the
# download runs on Linux from 2020 on. The players' Zig compiles their translation with the same
# flags (launcher/launcher.c): no -frounding-math (clang's strict floating point is much slower;
# the game's results are the same without it, checked with --hash against the gcc build) and
# -fno-slp-vectorize (clang otherwise merges the memory helpers' stores).
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$(realpath -m "${1:?usage: linux.sh OUT_DIR}")
JOBS=${JOBS:-6}
GAME=${GAME:-$HOME/.local/share/td6-native/game}
TP=$ROOT/third_party
ZDIR=$(readlink -f "$TP/zig-x86_64-linux-0.17.0")
Z=$ZDIR/zig
SDL=$(readlink -f "$TP/sdl3-rel")
FF=$TP/ffmpeg-rel
PYS=$(readlink -f "$TP/python-standalone")/python
CAP=$(readlink -f "$TP/pack-venv")/lib/python3.14/site-packages
T=x86_64-linux-gnu.2.31
LIBDIR=/usr/lib/x86_64-linux-gnu
for need in "$Z" "$SDL/lib/libSDL3.a" "$FF/lib/libavcodec.a" "$PYS/bin/python3.14" "$CAP/capstone" \
            "$TP/appimagetool-x86_64.AppImage"; do
    [ -e "$need" ] || { echo "missing $need (third_party/README.md)"; exit 1; }
done

TOOLS=$ROOT/build/rel-tools
mkdir -p "$TOOLS" "$OUT"
printf '#!/bin/sh\nexec %s cc -target %s "$@"\n' "$Z" "$T" > "$TOOLS/zcc"
chmod +x "$TOOLS/zcc"
SYSINC="-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu"
TK=third_party/antistatic                  # (the AntiStatic toolkit: translator and most of the runtime)
RT_INC="-Iruntime -I$TK/runtime/cpu -I$TK/runtime/win32 -I$TK/runtime/d3d7"
COMMON="-std=gnu11 -g -fno-strict-aliasing -fno-omit-frame-pointer $RT_INC -Igen -I$SDL/include -I$FF/include $SYSINC"
FFLIBS="$FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libswscale.a $FF/lib/libswresample.a $FF/lib/libavutil.a"

echo "== the game built with Zig (the runtime is what the download carries; the rest checks it)"
make JOBS="$JOBS" GAME="$GAME" CC="$TOOLS/zcc -fPIE -pie -L$LIBDIR -Wl,--allow-shlib-undefined" SDL3="$SDL" \
     CFLAGS_COMMON="$COMMON" CFLAGS_GEN="$COMMON -O2 -w -DCPU_FAST_MEM -fno-slp-vectorize" \
     FFMPEG_CFLAGS="-I$FF/include" FFMPEG_LIBS="$FFLIBS"

A=$OUT/AppDir
B=$A/usr/lib/td6-recomp
rm -rf "$A"
mkdir -p "$B/lib/stubs" "$B/include" "$B/builds" "$B/tools" "$A/usr/bin"

echo "== the runtime and its libraries"
"$Z" ar rcs "$B/lib/libruntime.a" $(ls build/obj/tk_*.o build/obj/rt_*.o | grep -v _main_nomain)
cp "$SDL/lib/libSDL3.a" $FFLIBS "$B/lib/"
cp $TK/runtime/cpu/*.h $TK/runtime/win32/runtime.h runtime/port.h "$B/include/"
cp builds/td6_1999.py "$B/builds/"
sha256sum gen/funcs.h | cut -c1-64 > "$B/FUNCS_SHA256"
REL=$(cat tools/release/VERSION)
echo "td6-recomp $(date -u +%Y-%m-%d) $(git rev-parse --short HEAD)|$REL" > "$B/VERSION"

echo "== link-time stand-ins for the system's Vulkan and zlib (the players' own are used)"
for spec in vulkan:libvulkan.so.1 z:libz.so.1; do
    name=${spec%%:*} so=${spec#*:}
    nm -D --defined-only "$LIBDIR/$so" | awk '$2 ~ /^[TWi]$/ {print $3}' | sed 's/@.*//' | sort -u |
        sed 's/.*/void &(void) {}/' > "build/stub_$name.c"     # (its functions: only names matter)
    "$TOOLS/zcc" -shared -fPIC -o "$B/lib/stubs/lib$name.so" -Wl,-soname,"$so" "build/stub_$name.c"
done

echo "== the translator and installer on a standalone Python (glibc 2.17) with Capstone"
mkdir -p "$B/python/bin" "$B/python/lib"
cp -a "$PYS/bin/python3.14" "$B/python/bin/"
tar -C "$PYS/lib" --exclude=test --exclude=idlelib --exclude=tkinter --exclude=turtledemo --exclude=ensurepip \
    --exclude=pydoc_data --exclude='config-3.14-*' --exclude='site-packages/*' --exclude='_tkinter.*' \
    --exclude='_dbm.*' -cf - python3.14 | tar -C "$B/python/lib" -xf -
cp -r "$CAP/capstone" "$B/python/lib/python3.14/site-packages/"
find "$B/python/lib/python3.14/site-packages/capstone" -name __pycache__ -prune -exec rm -r {} +
cp $TK/translator/*.py tools/install.py "$B/tools/"

echo "== the launcher"
"$TOOLS/zcc" -O2 -fPIE -pie -I"$SDL/include" $SYSINC -o "$A/usr/bin/td6-recomp" launcher/launcher.c \
    "$SDL/lib/libSDL3.a" -lm -ldl -pthread

echo "== Zig"
cp -a "$ZDIR" "$B/zig"
rm -rf "$B/zig/doc"

echo "== the licences (every part the download carries; CREDITS.md names them)"
L=$B/licenses
mkdir -p "$L"
cp CREDITS.md LICENSE "$L/"
cp $TK/LICENSE "$L/AntiStatic-LICENSE.txt"
cp "$(readlink -f "$TP/sdl3-rel")"/../SDL3-*/LICENSE.txt "$L/SDL3-LICENSE.txt"
cp "$TP"/ffmpeg-8.0.1/COPYING.LGPLv2.1 "$L/FFmpeg-COPYING.LGPLv2.1"
cp "$TP"/ffmpeg-8.0.1.tar.xz "$L/"                  # (FFmpeg's source, as the LGPL asks: see FFmpeg-BUILD.txt)
cat > "$L/FFmpeg-BUILD.txt" <<EOF2
FFmpeg 8.0.1 (LGPL 2.1 or later), from ffmpeg-8.0.1.tar.xz beside this file (https://ffmpeg.org),
built as static libraries (lib/libav*.a, lib/libsw*.a; the game is linked with them on the
player's computer, so they can be replaced) with Zig 0.17.0 for x86_64-linux-gnu.2.31:

  configure --enable-static --disable-shared --disable-programs --disable-doc --disable-network
    --disable-autodetect --disable-everything --disable-x86asm --disable-debug --enable-pic
    --enable-decoder=bink,binkaudio_rdft,binkaudio_dct,wmav1,wmav2 --enable-demuxer=bink,asf
    --enable-protocol=file --enable-swscale --enable-swresample --disable-avdevice --disable-avfilter

and one change after configure: HAVE_SYSCTL 0 in config.h (glibc 2.31 has no sys/sysctl.h).
EOF2
cp "$ZDIR/LICENSE" "$L/Zig-LICENSE.txt"
for x in libcxx libcxxabi libunwind; do cp "$ZDIR/lib/$x/LICENSE.TXT" "$L/LLVM-$x-LICENSE.txt"; done
cp "$CAP"/capstone-*.dist-info/LICENSE.TXT "$L/Capstone-LICENSE.txt" 2>/dev/null || \
    cp "$CAP"/capstone-*.dist-info/licenses/* "$L/" 2>/dev/null || true
cp "$PYS/lib/python3.14/LICENSE.txt" "$L/Python-LICENSE.txt"

echo "== AppImage"
cat > "$A/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/td6-recomp" "$@"
EOF
chmod +x "$A/AppRun"
cat > "$A/td6-recomp.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Test Drive 6 - Here In My Recomp
Comment=The 1999 racer, rebuilt from your own CD
Exec=td6-recomp
Icon=td6-recomp
Categories=Game;SportsGame;
StartupWMClass=td6
EOF
cp assets/icon.png "$A/td6-recomp.png"
RUNTIME=()
[ -f "$TP/appimage-runtime-x86_64" ] && RUNTIME=(--runtime-file "$TP/appimage-runtime-x86_64")
ARCH=x86_64 "$TP/appimagetool-x86_64.AppImage" --no-appstream "${RUNTIME[@]}" "$A" \
    "$OUT/Test_Drive_6-Here_In_My_Recomp-$REL-x86_64.AppImage" >/dev/null
ls -la "$OUT/Test_Drive_6-Here_In_My_Recomp-$REL-x86_64.AppImage"
