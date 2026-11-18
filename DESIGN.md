# TD6 native: design notes

A native Linux port of Test Drive 6 (Pitbull Syndicate / Infogrames, 1999, PC) by static
recompilation of the original TD6.exe, the same method as 1NSANE: Recompiled (github.com/AntAir267/1nsane-recompiled), whose
translator and runtime this project forks. "Bring your own game": the repository holds only our
tools and runtime; each user's own TD6.exe is translated and compiled on their machine.

## Decisions

### 1. Goal: faithful by default, extras as options (as 1nsane-native)
- Default is the game as shipped: same physics, AI, timing and look; original bugs kept except
  crashes. Extras are options, off = as shipped.
- Targets (Ant's brief, 2026-10-24): runs natively on Linux (KDE Plasma Wayland; the handheld's
  2560x1600 at 200 % and the docked 2560x1440 at 165 Hz), any resolution with Hor+ widescreen and
  menus/HUD scaled sensibly (menus are 640x480 as shipped), correct timing at high refresh rates,
  full gamepad support in menus and races (Xbox layout, analog triggers, hot-plug, button
  prompts), movies and music.

### 2. Translation: fork 1nsane-native's translator (decided 2026-10-24)
Why: TD6.exe is the same kind of program as 1NSANE's Game.exe (32-bit MSVC, statically linked
CRT, DirectX 7) and the translator handles it almost unchanged. tools/ is a copy of
1nsane-native's pe.py, x86.py, discover.py, codegen.py, fpustack.py, recomp.py (+ lockstep,
memdiff, profile, survey), with:
- codegen: pushf/popf (the direction flag kept round inline memsets, 0x41b100), pusha/popa in
  16- and 32-bit forms, cpuid and rdtsc (runtime helpers, runtime/x86.c);
- builds/td6_1999.py: the build's knowledge (addresses only).
Result: 1390 functions, 0 untranslated instructions, ~436k lines of C.

TD6.exe (790,528 bytes, sha256 58940a17..., linked 1999-11-01, no relocations, base 0x400000,
entry 0x461ff0 = WinMainCRTStartup): .text 0x401000 (441 KB), .rdata 0x46d000, .data 0x470000
(2 MB virtual, 236 KB initialised), .rsrc 0x660000. No DRM or packing. CRT: MSVC 6 static
(memcpy 0x464780, memmove 0x4688e0, SEH 0x460a00-0x460d00). Imports: KERNEL32 95, USER32 40,
GDI32 13, ADVAPI32 7, ole32 2, WINMM 6, DDRAW 4 (DirectDrawCreate/Ex, both enumerators),
DINPUT 1 (DirectInputCreateEx), DSOUND #1 #2 (Create, EnumerateA), COMCTL32 PropertySheetA,
binkw32 9, wmaudsdk #1 (WMAudioCreateReader). Interfaces by IID in .rdata: IDirectDraw7,
IDirect3D7 (HAL, T&L HAL, RGB and reference devices), gamma control, IDirectInput7A /
IDirectInputDevice7A (system keyboard, mouse), IDirectSound3DBuffer/Listener, IDirectSoundNotify,
IKsPropertySet (EAX probe).

Build config notes (builds/td6_1999.py): jump tables the bound search misreads (0x434a4c's
`and eax,0xffff` before a `cmp ax,7; jb`, 0x4170be's register bound), the CRT memcpy/memmove
byte tails indexed -4..-1 and -7..0 (read from their true starts; 1nsane-native's gog_1_0.py
lacks the -7..0 one at 0x4b8262, a latent bug there for small backward memmoves), the two SEH
continuations, and 21 entries Ghidra found that discovery only reached by jumps.

Cross-check (2026-10-24): of Ghidra's 1117 functions (a full decompile of the same TD6.exe) all are found but two
unused CRT routines (0x468e90, 0x468f10: bit-test charset scans, never called).
REA pass (2026-10-25, rea-agents 6.7.0, Ghidra 12.1.4 on the same TD6.exe): 1118 functions
(evidence ev_405b791f...), the same two the only ones not translated, and REA finds no
references to either (ev_378fac38..., ev_89193358...); 277 of ours are entries Ghidra does not
list (code pointers in data: vtables, CRT, callbacks). Snapshot in work/rea/td6.rea-snapshot
(reopen with it for later bug hunts).
Force feedback (2026-10-25, corrected twice: the first REA reading and a second static one missed
the creation path): the game has working force feedback for devices from DirectInput's
force-feedback enumeration (joystick record + 0x2b0, 0x40a880). At a race's start it creates its
effects in the joystick's record (+ 0x250 a square wave, + 0x254 and + 0x258 constant forces,
+ 0x25c friction; CreateEffect calls at 0x431660-0x4317c3, GUIDs found by EnumEffects with
0x409fe0) and plays them for the cars that have a joystick (0x5853d8, set from the chosen
controller by 0x4317e0): slot 0 all race long (0x431810 from each tick; its gain update 0x409e30
ignores the strength it is given, so a steady buzz at the game's scale, 0.75), slots 1 and 2 on
collisions (0x431970 car against car, the hit's strength / 10; 0x43e9ed hits on the scenery with a
duration). Friction is not supported by our DirectInput (the slot stays empty). The port's
rumble option makes controllers force-feedback devices, and dinput.c plays the effects on the
motors (constant forces the big one, the square wave the small one).

### 3. Runtime: fork of 1nsane-native's runtime
Copied: the Windows/DirectX 7 stand-in core (kernel32, user32/gdi, advapi32 registry file,
ole32, winmm, files, sync, mem, fpu, ddraw, d3d, the Vulkan renderer vk.c/vkthread.c, dinput,
dsound, pad, win, options, swrast, prof). Left out: everything 1NSANE-specific (Enhanced menus,
opponents, draw distance, mods, car classes, prompts, fan cars, IFC21, video.c's DirectShow).
vk.c still carries 1NSANE's ray tracing, smoke and temporal AA code, dormant (nothing calls
it); prune or reuse later. font.h and widescreen.c are inert stand-ins for now.

New for TD6:
- bink.c: binkw32 on FFmpeg's libavcodec (Bink 1 "f"; the BINK record's fields the game reads,
  copy formats 1-5, line-doubled files, the soundtrack decoded at open and played through the
  mixer). TD6's player: 0x40c480-0x40cd20.
- wma.c: wmaudsdk's reader on libavcodec (WMA 2). The game's protocol (0x40e6c0, callback
  vtable 0x46d390): GetAttributeByName(L"Duration") read as milliseconds; OnSample into an
  8-second ring; after 6 s it starts a DirectSound buffer with position notifications it
  refills itself. Delivery: 6.5 s at once, then real time 6.5 s ahead; --test runs deliver at
  frame ends on the virtual clock.
- dsound.c: IDirectSoundNotify (SetNotificationPositions; events set as the play position
  passes; DSBPN_OFFSETSTOP on Stop). Still missing: 3D buffers and the listener (the game
  carries on without them, logging "not implemented").
- advapi32.c: RegEnumValueA / RegEnumKeyExA / RegDeleteKeyA (TD6 reads its settings by
  enumerating HKCU\Software\Infogrames\Test Drive 6\0.1).
- The command line is `"C:\TD6\TD6.exe" NoProps`: the game's own switch for no properties
  dialog. Its switches (0x40bbb0, from the command line or a command file): Log, NoProps, Window,
  NoPrimaryTest, MatchBitDepth (and comment prefixes // rem ; \ #). It reads TD6.ini too.
- GDI text is used only by a debug info screen (0x443de0) and the fps overlay: stubs.
- swrast's swr_* renamed swrast_* (swr_init is libswresample's: it broke the movie sound).

### 4. The game's files
The game reads its data from the installed archives (LEGALS.ZIP, loading.zip, static.zip,
frontend.zip, sky.zip, traffic.zip, LEVELS\level%03d.zip, LEVELS\texture%d.zip, cars\*.zip,
sound\Sound.zip) plus loose objects\, param\, Movies\ and Music\. The installer copies the
disc's InstData\GameData\Common as it is.
- A copy with each zip unpacked to a folder cannot run: the game looks for the zips.
- The port's game copy: ~/.local/share/td6-native/game, the disc's InstData/GameData/Common.
  Never written: saves, TD6.log and registry.txt go to the user folder ~/.local/share/td6-native/.
- tools/install.py (2026-10-25): from the CD, a copy of it, an ISO (its own ISO 9660 / Joliet
  reader, no 7-Zip needed), a zip holding the ISO, or a Windows install: copies Common, checks
  TD6.exe's hash against builds/*.py. Checked: from the disc zip and from the unpacked disc, both
  byte-identical to the game copy. A launcher doing this on first start (as 1nsane-native's
  AppImage) is the next step for other players.

### 5. Testing
As 1nsane-native: --test (virtual clock, scripted keys TD6_KEYS="frame:KEY*hold,...",
TD6_PAD), frame dumps (TD6_DUMP_FRAMES=<dir>, TD6_DUMP_EVERY), hashes (--hash). Planned: the
Unicorn lockstep reference (tools/lockstep.py, build/td6_ref) once races run, and Proton as the
original's reference.
Screenshots: the renderer's own frame dumps (a spectacle capture takes in every screen).

## Progress

- 2026-10-24: repo made; translator and runtime forked; TD6.exe translated whole. Headless
  --test boots: legal screens, Publish/Develop/Title movies with sound, the front end with its
  music (GW2.wma), SELECT PLAYER. In a window on Vulkan (RADV, 890M) the same menu draws right
  at 640x480; scripted keys reach it.
- 2026-10-25: races run and draw (Rome, practice). Fixes: a jump through a table of functions is
  a tail call (discover.py; the vertex-type handlers at 0x49a570); SetSurfaceDesc with a new
  size, pitch, pixel format and pointer on any system-memory surface (TD6 points a staging
  mipmap chain at each texture's decoded levels; every track texture was blank).
- 3D sound: IDirectSound3DListener / 3DBuffer on the software mixer (rolloff, side panning,
  Doppler; deferred settings and CommitDeferredSettings, as TD6 uses them).
- Reference build and lockstep (ref/, tests/, tools/lockstep.py; CPUID and RDTSC hooked in
  Unicorn): memory identical to the original through the menus (600 frames) and into a race
  with throttle and steering (1700 frames). --test runs have no music (the game's streaming
  thread is not repeatable; TD6_TEST_MUSIC=1 for it).
- Timing: races are right at any frame rate as shipped (0x445850: physics in 30 Hz ticks from
  timeGetTime, at most 4 a frame; the cars and camera drawn extrapolated by the tick's fraction,
  0x62e164). The front end reads no clock (its animations move a step a frame), so outside races
  frames are paced at menu_fps (60) by the runtime.
- Widescreen (widescreen = on | W:H) with resolution = native: the perspective projection of
  full-width viewports widened (0x4032a0 hook, Hor+), the renderer spreads the view; the flat
  projection of the HUD and menus (0x403380) unchanged: menus 4:3 in the middle, the race HUD's
  pieces (0x453cb0) to the nearest screen edge, the middle 30 % (time, countdown) centred.
  Front end (2026-10-25): the background (the frame's first quad over the whole view) across the
  screen; the top banner and the bottom bar with the rev counter reach the sides (inside
  0x41d100: pieces left of x 160 / right of 480 move with their edge, the tiles between
  stretched, the title's letters left in the middle); menus, panels and previews 4:3 in the
  middle. The showroom's 3D view is widened too: its camera is built at start-up before the
  screen's shape is known, so a full-width viewport's perspective (kept in its record, + 0x28)
  is widened again at the next frame when the shape changes (also after window resizes).
  The rev counter's needle (2026-11-05, Ant on the Steam Deck: "the speedometer glitches in the
  menu"): it was moved with the bar only when its box reached the bar's bottom (y 476), so at
  most angles it stayed behind while its dial went to the edge. Small pieces over the bar's end
  tiles (left: centre < 160, top >= 350; right: centre > 480, top >= 430) now go with them
  [verified: frame dumps at 1280x800 match the 4:3 needle at every angle; against the old
  renderer only the needle's pixels differ on the garage, class, buy and selected-car screens].
  The view cone (2026-10-25, Ant saw pop-in at the screen's edges): the game draws a level
  model or an object only when its bounding sphere is inside a cone (0x432b90 level models,
  0x432d00 the others) whose side planes (0x585518 x, 0x585520 z) come from half the width
  (0x585590) over a focal length (0x5855a8 = width x 0.5625), built at a race's start (0x433b80)
  and when a camera changes its focal length (0x433c60): about 83 degrees across, a little
  wider than the 75-degree camera at 4:3. Widened with the camera (widen_cone, hooks on both
  builders): at 21:9 (106 degrees) trees at the top corners were missing in 30 of 124 frames of
  an autopilot drive in Rome without it; at 16:9 (91 degrees) no frame of that drive changed.
  It changes only what is drawn (the draw lists, the texture cache): the cars' records are the
  same. The level is drawn by track section (0x457580: a window of up to 64 sections round the
  camera, from the VIDEO OPTIONS draw distance, 0x65bccc / 4096), not by angle.
- Controllers (pad.c, padgame.c): the joystick as TD6 reads it (X the left stick, Y the
  triggers together, Xbox button order); with the keyboard chosen in the game's options a race
  takes the XBOX layout (A hand brake, B/X gears, Y camera, LB rear view, RB horn) with every key
  still working; menus and the pause menu by D-pad / stick, A, B, Start; a controller plugged in
  later goes through the game's own joystick callback. CONTROLLER OPTIONS with a pad: its
  controls named as the pad's (LS LEFT/RIGHT, RT, LT, A, B, X, Y, LB, RB, BACK, START: the labels
  the game keeps in the joystick's record from DirectInput's object names), and the game's
  untouched joystick default (buttons 0-7, no triggers) made the XBOX layout in the profile, so
  choosing the controller there gives the same layout, shown and rebindable. Rumble (option
  "rumble": on, off, or a strength 25-100 % in place of the game's 75 %): the game's own force feedback (decision 2) on the pad - a light engine buzz all
  race, collisions on the big motor; with the XBOX layout the player's car is given joystick 1
  for it. Betting by pad (2026-11-05, Ant on the Deck could not bet): PLACE YOUR BET (menu
  screen 114 in 0x65e260, alternating with 125, its list of bets, as the menu loop works through
  the panels [verified: memory reads]) takes the bet only as typed characters (a digit: bet * 10
  + digit; Backspace: bet / 10 [verified: 500, 3 x Backspace, 0, then 7: 7]); up/down move
  between OK and CANCEL, left/right do nothing. With a controller, left/right (held: repeat) now
  step the bet by 100 between the minimum (0x65dbb0) and the lower of the maximum (0x65dbac)
  and the credits (0x65bcb8), typed as Backspaces and digits, one a presented frame (the menu
  takes one character a frame [verified]), and the title reads "PLACE YOUR BET - LEFT/RIGHT"
  (the menus' string table entry 102, 0x482570, pointed elsewhere while the screen is up)
  [verified: scripted pad, 500 -> 800, A: prize money screen, credits 16000 -> 15200, the
  title's pointer back]. VIDEO OPTIONS' RESOLUTION (2026-11-05, Ant on the Deck: it read 640 x
  480 while the picture is the screen's): with resolution = native the game's mode only sets its
  own coordinates and the picture fills the screen at the screen's resolution, so that line now
  shows the picture's size (hook on 0x422fa0, which formats the selected entry of the mode list
  at [0x577224] - width, height, depth at + 8, + 0xc, + 0x10 of a 12-byte entry, the selection
  the short at [0x65de9c + item * 4] [verified: disassembly; frame dump "1280, 800, 16 BIT" at
  1280x800 native, "640, 480, 16 BIT" with resolution = original]). Left/right there still
  change the game's mode, which in native only changes its coordinates, so the line stays the
  same. The race reads the controller used last (2026-10-25: on the handheld Handheld Daemon
  shows the built-in pad as two devices, "Generic X-Box pad" and a virtual "Xbox 360
  Controller"; joystick 1 could be the one not in use), and button presses are logged.
- Front-end walk (2026-10-25, tools/dev/walk.sh, headless): every screen reached draws and works:
  select/create player, main menu, race menu, garage (buy: class, car, paint shop, purchase),
  single race (bet, prize money, Rome), tournament (class, tournaments, next track, bet, Pelton
  Raceway), cop chase (Paris), challenge (Hong Kong), practice, options (video, audio, game,
  controller with key rebinding), the Fear Factory and credits movies, the pause menu (continue,
  exit and lose bet), back to the menus and a second race, the quit prompt. A race's end with
  TD6_AUTOPILOT=1 (tests: the player's car made an AI racer, 0x62e634's kind 1 -> 2): the
  results, the replay, back to the menus; a tournament race's results and standings (also at
  16:9). A race left alone times out (the checkpoint clock) and goes back to its menu. The race-start burnout fills the screen with white smoke:
  the game's own additive puffs (SRCALPHA/ONE, 0x4448f0), the same in both renderers.
- No split screen in this release: the two-player screens are in the executable (multiplayer race
  menu, screen 36 at 0x4855a0: single race / cop chase / quad race; 0x484e50, 0x486098) but the
  main menu's list (0x47ba90) leaves out MULTI PLAYER RACE and the only screen leading to them
  (79) is an empty stub (0x401a50). Unlocking them would be an extra of the port's own.

- The registry file (registry.txt) was never read or written (the fork lost 1nsane-native's
  settings_load / settings_save calls): now loaded at start and saved at exit, with the motors
  stopped at exit. TD6 keeps its Properties dialog's settings there as two hex "Set" values,
  HKCU\Software\Infogrames\Test Drive 6\0.1 (decoded by 0x40bce0) and its subkey per 3D device
  (0x40bf20). Global: bit 0 time demo playback, 1 begin in a window, 2 log file, 3 time demo, 4 skip
  front end, 5 ?, 6 no quick maths, 7 no EAX, 8-10 rasterizer choice, 11-12 force-feedback strength
  (25 % steps, 0x50ae50; default 75 %), 14 3D sound quality, 15 show overdraw, 16 ?, 18 0x50fe34
  (the sun and the smoke are drawn only when set; default on). Device: 0-1 no reflections, 2-3
  texture quality, 4-5 movies (any bit: no movies at all, the menus' previews too), 8-9 no
  W-buffer, 10-11 no triple buffering, 12-13 no AGP, 16-17 no mip-mapping, 22-23 no fog, 24 no
  VSync. Dialog pages and control IDs: work/dialogs.py reads them from the resources.

- Releases (2026-10-25): public repository AntAir267/test-drive-6-here-in-my-recomp. Linux
  AppImage (tools/release/linux.sh): launcher/launcher.c does the first-run setup (install.py,
  the translation, Zig compiling it against the bundled runtime, SDL3 and a small static FFmpeg
  8.0.1 with only Bink and WMA), all built for glibc 2.31; the player's build matches the gcc
  build's memory exactly. 0.1.0-alpha started the game headless (no --window): 0.1.1.
  Windows zip (tools/release/windows.sh, cross-built here with Zig): runtime/win/ from
  1NSANE: Recompiled (the low range 0x400000-0x70000000 reserved first thing, relaunching
  suspended if taken), SDL3 / zlib / FFmpeg cross-built, Vulkan's import library from a .def.
  Under Wine with tools/dev/wine_lowfree.c (Wine otherwise fills that range with its own
  allocations) the Windows game's memory equals Linux's at frame 2200 and Vulkan draws; Zig's
  cache locking (NtLockFile) fails under Wine, so the player's compile step is unchecked until
  run on real Windows.

## Open reports (parked 2026-10-25, Ant)

- Driving through objects "sometimes". Checked: a scripted quick race into an opponent and a
  wall is identical to the original code (lockstep, 1800 frames), and widescreen leaves the
  race's memory alone; physics are fixed 30 Hz ticks. Needs the place (track, what was hit)
  to script it there; may be the original game's.
- "A Panoz Roadster woman soundbite" while driving the Viper in a quick race. The only voice in
  the game's files is the race-start countdown (Sound.zip starta-startd, 11025 Hz, a woman's
  voice, played once at the start, rightly); the cars' sounds (cars\sound.zip, by 0x49a7f8)
  are engine loops. A voice mid-race would be a bug: needs when it played.

## Plan (next steps, in order)

0. Windows on a real PC: the first-run build (Zig's compile and link), a window, a controller.
1. A first place by autopilot for the high-score name entry (the autopilot finishes 3rd-6th).
2. Button prompts: the controller screen is done (Xbox names, the XBOX layout); left: while the
   keyboard is chosen, that screen lists keys only (the pad's XBOX layout is not shown there).
3. Menus at 16:9: done (results and standings checked too).
4. Sun in widescreen: by the code, right already (0x4473e0 projects it with the widened camera;
   it and the flare, 0x447740, are flat drawings of the race frame outside the HUD, which the
   renderer places from the widened view); not yet seen: the sun (1e6, 8e5, 1e6) sits at the top
   of the view, out of sight in the scripted drives.
5. Options page (the Properties dialog's settings: 3D device, rendering, audio, movie) and the
   first-run tool (from the CD or ISO); the in-game VIDEO OPTIONS resolution list against the
   port's resolution. Rumble: Ant to try on the real pad (strength done; maybe collisions
   only, without the engine's steady buzz). A launcher for first start (install.py's job).
