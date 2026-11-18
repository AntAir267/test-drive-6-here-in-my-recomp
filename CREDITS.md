# Credits

- **Test Drive 6** (1999) by Pitbull Syndicate, published by Infogrames. None of the game is in
  this repository: each player translates their own copy.
- The translator and runtime are the [AntiStatic Recomp Toolkit](https://github.com/AntAir267/AntiStatic-Recomp-Toolkit)
  (public domain), grown out of [1NSANE: Recompiled](https://github.com/AntAir267/1nsane-recompiled)'s.

Libraries the build uses (none is in this repository; see BUILDING.md and third_party/README.md):

- [SDL3](https://github.com/libsdl-org/SDL) (zlib licence): window, input, controllers, audio
  output. Linked statically.
- [FFmpeg](https://ffmpeg.org) (LGPL 2.1 or later): libavcodec, libavformat, libswscale and
  libswresample decode the game's Bink movies and WMA music. The system's own shared libraries
  are used.
- [Capstone](https://www.capstone-engine.org) (BSD licence): the translator's disassembler.
- [Unicorn](https://www.unicorn-engine.org) (GPL 2): only in the developers' reference build
  (`make build/td6_ref`), which runs the original code to check the translation; never part of
  the game.
- Vulkan (the system's loader and driver), zlib.

The AppImage also carries (their licences are in it, under usr/lib/td6-recomp/licenses):

- [FFmpeg](https://ffmpeg.org) 8.0.1 (LGPL 2.1 or later), built small and static: only the Bink
  and WMA decoders, with its source tarball and build options.
- [Zig](https://ziglang.org) 0.17.0 (MIT licence; LLVM's libc++, libc++abi and libunwind inside it):
  compiles the game on the player's computer.
- [Python](https://www.python.org) 3.14 (PSF licence), from python-build-standalone: runs the
  translator and the installer.
- SDL3 and Capstone, as above.

The Windows zip carries the same, in their Windows builds (Zig for Windows, Python from
python-build-standalone for Windows, Capstone's Windows wheel), and [zlib](https://zlib.net)
(zlib licence) and the MinGW-w64 runtime (in Zig; its COPYING is in the zip) linked into the game.
