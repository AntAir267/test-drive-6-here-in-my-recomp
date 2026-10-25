Third-party sources fetched and built locally (not committed):

- SDL3 (zlib licence), 3.4.18 or later: the official source release from
  github.com/libsdl-org/SDL, unpacked here as SDL3-<version>/ and installed into sdl3/ as a
  static library with its headers:

      cmake -S SDL3-3.4.18 -B SDL3-build -DSDL_SHARED=OFF -DSDL_STATIC=ON \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/sdl3"
      cmake --build SDL3-build -j8 && cmake --install SDL3-build

  (or point the Makefile at another copy: `make SDL3=/path/to/sdl3`).
