Third-party sources fetched and built locally (not committed):

- SDL3 (zlib licence), 3.4.18 or later: the official source release from
  github.com/libsdl-org/SDL, unpacked here as SDL3-<version>/ and installed into sdl3/ as a
  static library with its headers:

      cmake -S SDL3-3.4.18 -B SDL3-build -DSDL_SHARED=OFF -DSDL_STATIC=ON \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/sdl3"
      cmake --build SDL3-build -j8 && cmake --install SDL3-build

  (or point the Makefile at another copy: `make SDL3=/path/to/sdl3`).

For the player download (tools/release/linux.sh), also (the same pieces as 1NSANE: Recompiled's):

- Zig 0.17.0 (MIT licence): the official Linux x86_64 build from ziglang.org, unpacked as
  zig-x86_64-linux-0.17.0/. It builds the download, and the download carries it (without doc/)
  to compile the player's translation.
- SDL3 again, built with that Zig for glibc 2.31 into sdl3-rel/ (configure SDL3-<version> with
  CMAKE_C_COMPILER set to `zig cc -target x86_64-linux-gnu.2.31` and its ar and ranlib,
  CMAKE_C_FLAGS="-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu",
  SDL_SHARED=OFF, SDL_STATIC=ON, SDL_X11_XSCRNSAVER=OFF, CMAKE_BUILD_TYPE=Release).
- FFmpeg 8.0.1 (LGPL 2.1 or later): ffmpeg-8.0.1.tar.xz from ffmpeg.org (sha256
  05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41), unpacked as
  ffmpeg-8.0.1/ and built with that Zig into ffmpeg-rel/, only what TD6 uses (the Bink and WMA
  decoders and demuxers, swscale, swresample): configure in ffmpeg-build/ with
  --prefix=../ffmpeg-rel --cc, --ar and --ranlib the Zig ones, then the options the release
  script writes into the download's licenses/FFmpeg-BUILD.txt; then HAVE_SYSCTL 0 in config.h
  (glibc 2.31 has no sys/sysctl.h), make, make install. The download carries the tarball (the
  LGPL's source).
- python-standalone/: CPython 3.14 for any Linux with glibc 2.17 or later, from
  github.com/astral-sh/python-build-standalone (install_only_stripped), unpacked as python/: it
  runs the translator and the installer.
- pack-venv/: a Python virtual environment with Capstone 5.0.9 (manylinux), whose package the
  download puts beside that Python.
- appimagetool-x86_64.AppImage (github.com/AppImage/appimagetool) and appimage-runtime-x86_64,
  the runtime it puts at the front of an AppImage.

For the Windows zip (tools/release/windows.sh, cross-built on Linux), in win/:

- zig-x86_64-windows-0.17.0/: the official Windows build of the same Zig (ziglang.org, sha256
  b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428): the zip carries it.
- python-win/python/: CPython 3.14.8 for Windows from python-build-standalone (release 20261020,
  cpython-3.14.8+20261020-x86_64-pc-windows-msvc-install_only_stripped.tar.gz, sha256
  6761cf0dd740591df7643cf55fd03913c8ec7ddd88b419fe2f170abe3479e198).
- capstone-win/: capstone-5.0.9-py3-none-win_amd64.whl from PyPI, unpacked (sha256
  732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0).
- tools/: zig cc / ar / ranlib / rc wrappers for x86_64-windows-gnu and a CMake toolchain file
  using them; wcc-ff also drops the GNU ld options FFmpeg's configure adds for MinGW.
- sdl3/: SDL3 built with that toolchain (SDL_SHARED=OFF, SDL_STATIC=ON, SDL_TEST_LIBRARY=OFF).
- zlib/: zlib 1.3.1's sources compiled with the wrapper into zlib/lib/libz.a, its headers and
  LICENSE beside it.
- ffmpeg/: FFmpeg 8.0.1 as for Linux, with --target-os=mingw32 --arch=x86_64
  --enable-cross-compile --enable-w32threads and the wcc-ff wrapper (the zip's
  licenses/FFmpeg-BUILD.txt has the options).
- vulkan/: Vulkan's headers (from the system's) and lib/libvulkan-1.a, an import library made with
  `zig dlltool` from vulkan-1.def (the vk* functions the system's loader exports).

Testing the Windows build under Wine (Proton 10): Wine keeps the low memory the program needs for
its own allocations, so runs need tools/dev/wine_lowfree.c (LD_PRELOAD into Wine; it says why).
