# Building from source

You need your own copy of Test Drive 6 for PC: the installed game folder (TD6.exe with its .zip
archives, objects, param, Movies and Music), or the CD's `InstData/GameData/Common` folder, which
is the same thing.

## The game's files

`tools/install.py SOURCE` puts them in place: SOURCE is the CD (where it is mounted), a copy of
its files, a disc image (`.iso`, or a `.zip` holding one) or a Windows install's folder. It copies
the game data (the CD's `InstData/GameData/Common`, as the Windows installer does; nothing from the
CD is run) and checks TD6.exe against the release the translator knows.

The program looks for the game in `~/.local/share/td6-native/game/` (pass `GAME=<folder>` to make
and `--game <folder>` to the program for another place; `tools/install.py --to <folder>` too). It never writes there: saves, TD6.log
and the registry file go to `~/.local/share/td6-native/`.

## Linux

Packages (Debian/Ubuntu names): `build-essential pkg-config python3 python3-venv libvulkan-dev
glslang-tools zlib1g-dev libavformat-dev libavcodec-dev libswscale-dev libswresample-dev cmake`
(and SDL3's own build needs, as its docs list for your system: X11/Wayland, ALSA/PulseAudio/PipeWire
and udev development packages).

1. Python with Capstone for the translator, in the repository's `.venv`:

       python3 -m venv .venv
       .venv/bin/pip install capstone

2. SDL3, built from source and installed into `third_party/sdl3/` (static library and headers):
   see [third_party/README.md](third_party/README.md).
3. `make` (or `make JOBS=4`): translates TD6.exe (seconds) and compiles the C (a few minutes).
4. `build/td6 --window`, or headless: `build/td6 --test --frames N` with `TD6_DUMP_FRAMES=<dir>`
   for frame dumps.
