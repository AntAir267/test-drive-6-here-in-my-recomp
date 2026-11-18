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

0. The toolkit (translator and runtime) is a git submodule: clone with
   `git clone --recurse-submodules`, or run `git submodule update --init` in a clone.
1. Python with Capstone for the translator, in the repository's `.venv`:

       python3 -m venv .venv
       .venv/bin/pip install capstone

2. SDL3, built from source and installed into `third_party/sdl3/` (static library and headers):
   see [third_party/README.md](third_party/README.md).
3. `make` (or `make JOBS=4`): translates TD6.exe (seconds) and compiles the C (a few minutes).
4. `build/td6 --window`, or headless: `build/td6 --test --frames N` with `TD6_DUMP_FRAMES=<dir>`
   for frame dumps.

## The AppImage

`tools/release/linux.sh OUT_DIR`, in a checkout of its own (a git worktree: its build/ and gen/
become the release build), with the pieces third_party/README.md lists for the player download.
It builds the runtime with Zig for glibc 2.31, checks that the translation builds, and packs the
launcher, the runtime, Zig, Python with the translator and installer, FFmpeg and the licences.
The game it builds on the player's computer computes exactly what the gcc build does (memory
compared frame by frame through the menus and into a race; the only difference ever seen was
±1 in decoded movie sound, from the system's FFmpeg against the bundled one).

## The Windows zip

`tools/release/windows.sh OUT_DIR`, after linux.sh in the same worktree (it uses its gen/), with
the pieces third_party/README.md lists for it: a cross-build on Linux with Zig, no Windows
machine needed. It builds the runtime for x86_64-windows-gnu (runtime/win/ in place of the POSIX
pieces MinGW lacks: the program's low memory reserved before anything else, sleeps, spawning,
signals), links the game with it as the check, and packs the launcher, the runtime, Zig and
Python for Windows, FFmpeg and the licences into one folder, zipped.

Under Wine (tools/dev/wine_lowfree.c), the Windows game's memory matched the Linux build's
exactly at frame 2200 (menus, then a race), and it draws with Vulkan; the launcher copies and
translates the game there, but Zig's compile step needs a file-locking call Wine lacks, so the
whole first run is only checked on Windows itself.
