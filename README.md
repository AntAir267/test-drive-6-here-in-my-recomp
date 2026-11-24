# Test Drive 6 - Here In My Recomp

An unofficial native Linux and Windows version of **Test Drive 6** (Pitbull Syndicate / Infogrames, 1999,
PC), made by static recompilation: the game's own TD6.exe is translated into C on your computer
and built against a new stand-in for Windows and DirectX 7 on SDL3 and Vulkan. The game plays as
it did; everything new is an option, and with every option off it runs exactly as shipped
(checked frame by frame against the original code).

> **Alpha.** This is an early test version. It has only been tested on the author's own computer
> (listed under [Tested on](#tested-on)); expect problems on others, and please report them in
> Issues.

**Bring your own game.** This repository holds only the translator and the runtime, never any of
the game's code or data. You need your own copy of the **PC CD** of Test Drive 6 (the CD, or an
image of it); `tools/install.py` copies the game's files from it and checks TD6.exe before
anything is built.

## What's new

All optional, in `~/.local/share/td6-native/options.txt` (written with comments on first start):

- Any resolution, window or full screen (Alt+Enter), widescreen at the screen's own shape (Hor+:
  a wider view, not a stretched one), the menus' frame and backgrounds across the screen and the
  race HUD at its edges
- Correct speed at any refresh rate (the races as the game timed them; the menus paced at 60)
- Controllers: menus and races by pad (Xbox layout ready: triggers for the pedals, the stick for
  steering), the keyboard beside it, hot-plugging, Xbox names on the controller screen, rumble
  (the game's own force feedback, which it never got to play on most pads), with a strength
- Movies (Bink) and music (WMA) through FFmpeg, 3D sound on a software mixer

## Getting it

- **Players (Linux):** download the AppImage from
  [Releases](https://github.com/AntAir267/test-drive-6-here-in-my-recomp/releases), make it
  executable and open it. It holds everything needed but the game (the translator, a compiler,
  the runtime and a small FFmpeg for the movies and music): it asks for your Test Drive 6 CD (the
  disc, a copy of its files, or a disc image: .iso, or a .zip holding one), copies the game's
  files from it, builds the game on your computer once (about a minute), and plays it; after
  that it opens straight into the game. It needs 64-bit Linux from 2020 or later (glibc 2.31), a
  graphics driver with Vulkan 1.3, and about 500 MB free. `tools/release/linux.sh` makes it.
- **Players (Windows):** download the zip from
  [Releases](https://github.com/AntAir267/test-drive-6-here-in-my-recomp/releases), unzip it
  anywhere and open `td6-recomp.exe` in its folder; it does the same as the Linux one (a CD in
  the drive, or a disc image Windows has mounted, is found by itself). It needs 64-bit Windows
  10 or 11, a graphics driver with Vulkan 1.3, and about 1 GB free. The program isn't signed,
  so Windows may warn about it first (More info, then Run anyway).
- **Developers:** see [BUILDING.md](BUILDING.md). In short: install the packages it lists, build
  SDL3 (see [third_party/README.md](third_party/README.md)), then

      tools/install.py /path/to/your/CD-or-image     # the CD, a copy of it, an .iso or a .zip of one
      make                                           # translates TD6.exe and builds build/td6
      build/td6

## Tested on

- **Linux:** a handheld laptop with AMD graphics (RDNA 3.5); Ubuntu 26.04 LTS, Linux 7.0, KDE
  Plasma 6.6 (Wayland), Mesa (RADV); and a Steam Deck.
- **Windows:** not yet on a Windows PC. The Windows build was made and checked on the Linux
  machine above under Wine (Proton 10): the game runs and computes exactly what the Linux build
  does; the first-run build step could not run there (Wine lacks a file call the compiler uses).

## Status

Every front-end screen and race mode works (single race, tournament, cop chase, challenges,
practice, garage, options, replays, results). Not in this release of the game, so not here
either: split screen (the PC version's two-player screens exist but are unreachable). The
design notes and the plan are in [DESIGN.md](DESIGN.md).

## Layout

- `third_party/antistatic/` the [AntiStatic Recomp Toolkit](https://github.com/AntAir267/AntiStatic-Recomp-Toolkit)
  (a git submodule): the translator (Python + Capstone) and the Windows/DirectX 7 stand-in the
  translated code runs on, on SDL3 and Vulkan
- `tools/install.py` the game's files from the CD, `tools/release/` the AppImage and the Windows
  zip (still the scripts of the 0.1 releases, which predate the toolkit: being moved onto it)
- `launcher/` the AppImage's program: first-run setup, then the game
- `builds/` what is known about TD6.exe (addresses only, and its hash)
- `runtime/` TD6's own part of the runtime: its settings for the toolkit (`port.h`), binkw32 and
  wmaudsdk stand-ins on FFmpeg, widescreen, controllers, smooth motion; `runtime/win/` what the
  Windows build needs
- `ref/`, `tests/` the original code in Unicorn, to check the translation against (developers)
- `gen/`, `build/` the generated C and the build (from your TD6.exe; never committed)

## Credits and licence

See [CREDITS.md](CREDITS.md). This project's own code is released into the public domain
([LICENSE](LICENSE)); the libraries it uses keep their own licences. The translator and runtime
are the [AntiStatic Recomp Toolkit](https://github.com/AntAir267/AntiStatic-Recomp-Toolkit) (public
domain), grown out of [1NSANE: Recompiled](https://github.com/AntAir267/1nsane-recompiled)'s.

Test Drive is a trademark of its owners. This project is not made, endorsed or supported by
Pitbull Syndicate, Infogrames, Atari, Accolade or the game's current owners.
