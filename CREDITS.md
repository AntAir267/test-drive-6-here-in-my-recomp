# Credits

- **Test Drive 6** (1999) by Pitbull Syndicate, published by Infogrames. None of the game is in
  this repository: each player translates their own copy.
- The translator and runtime come from [1NSANE: Recompiled](https://github.com/AntAir267/1nsane-recompiled)
  (public domain).

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
