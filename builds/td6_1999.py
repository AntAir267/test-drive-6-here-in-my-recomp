"""Per-build knowledge for Test Drive 6 (PC, 1999; TD6.exe 790,528 bytes, unpatched, build "001C").

Addresses only: no code or data from the game lives in this repository.
"""
NAME = 'td6-1999'
SHA256 = '58940a17dd716bce30e8c5dee335def87817664bd7a1a06119249518513c9bc7'

# Functions reached only in ways discovery cannot see (found at run time, added here).
EXTRA_ENTRIES = [
    # functions Ghidra found that discovery reaches only by jumps from other functions (their code
    # would otherwise be translated inside those), or not at all (unused CRT routines)
    0x401870, 0x401b60, 0x40a320, 0x40a540, 0x40e3e0, 0x40e6c0, 0x418d50, 0x41cce0, 0x41d100,
    0x426e30, 0x434390, 0x43a1e0, 0x445050, 0x45a750, 0x45fb35, 0x460aed, 0x460d44, 0x462153,
    0x467f89, 0x468ed0, 0x46908b,
    # the vertex-type handlers' table 0x49a570 (called through at 0x434a17, jumped through at 0x434a4c)
    0x434a60, 0x434b30, 0x434c00,
]

# Addresses that look like code pointers but are not function entries.
NOT_ENTRIES = []

# Jump tables discovery cannot bound: jmp address -> (table address, entries)
JUMP_TABLES = {
    0x434a4c: (0x49a570, 7),    # the vertex-type handlers (0x49a570: functions, also called by 0x4349e0): a tail call
    0x4170be: (0x417b70, 11),   # cmp edx,ebp with ebp = 10
    0x42fcd9: (0x42fd74, 8),    # z mode 0..7 (or 5)
    # CRT memcpy (0x464780) and memmove (0x4688e0): byte tails with negative indexes, read from
    # their true starts (index -4..-1 and -7..0)
    0x4647d4: (0x4648c8, 4),
    0x464942: (0x4649f4, 8),
    0x468934: (0x468a28, 4),
    0x468aa2: (0x468b54, 8),
}

# Jumps into the middle of another function (exception handling, longjmp).
NONLOCAL_JUMPS = [
    0x460a87,   # C++ EH / SEH: continue after a handler (mov esp, [ebx-4]; jmp eax)
    0x460cbc,   # SEH: __except handler (mov esp, [ebx+0x1c]; jmp [ebx+0x18])
]

# Imports that never return.
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess']

TABLES = []
# Functions the runtime may hook: hook_<name> is a function pointer, NULL unless set, run at the
# function's entry. It may adjust the arguments and return 0 (the function then runs), or run the
# original itself (F_<addr>_orig) and/or do the call's work and return 1.
HOOKS = {
    # widescreen (widescreen.c): a viewport's perspective projection being built (ECX matrix, EDX
    # viewport; near, far, fov); a race frame (physics ticks and drawing); the race HUD drawn
    0x4032a0: 'perspective',
    0x445850: 'race_frame',
    0x453cb0: 'race_hud',
    # the view cone the visibility tests use (0x432b90, 0x432d00): built from the screen's size
    # (ECX width, EDX height) and again when a camera changes its focal length
    0x433b80: 'view_cone',
    0x433c60: 'view_cone_again',
    # controllers (padgame.c): a race's controls applied (ECX = the joystick + 1, 0 the keyboard)
    0x409a50: 'controls_apply',
    0x408d00: 'race_keys',      # the keyboard's controls read (a race tick's with the keyboard chosen)
    0x41d100: 'front_frame',    # the front end's frame (widescreen: its banner and bottom bar)
    # VIDEO OPTIONS' RESOLUTION line written and drawn (ECX the item, EDX and one stack argument
    # passed on; callee pops 4): resolution = native shows the picture's real size
    0x422fa0: 'resolution_text',
    # smooth motion (camera.c): a race frame's drawing starting, after its physics ticks (0x445850
    # calls it once a frame; it picks the track sections to draw)
    0x457580: 'race_draw',
}
CONSTS = {}
# Fixed memory operands the runtime may point elsewhere (rm_<name>): the race frame's tick fraction
# (0x445850: the low 16 bits of the clock in ticks times 1/65536 at 0x46d4f8, stored at 0x62e164
# for drawing everything extrapolated): camera.c's smooth_camera points it at 1.25/65536
MEM_OVERRIDES = {'tick_fraction': (0x4458ba, 0x46d4f8)}
INLINE = {}
# Game data the runtime reads: emitted as GAME_<NAME> in gen/image.h.
DATA = {
    'game_state': 0x63b740,     # the main loop's state (0x459740): 0 legal screens and movies, 1 front end,
                                # 2 race (0x445850: physics in 30 Hz ticks, drawn extrapolated), 3 screenshot
}
