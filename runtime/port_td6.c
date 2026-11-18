/* Test Drive 6 on the AntiStatic runtime: the port's own set-up and frames, through the
   runtime core's port hooks (weak defaults in third_party/antistatic/runtime/win32). */
#include <stdlib.h>
#include "runtime.h"
#include "image.h"
#include "options.h"
#include "widescreen.h"

void bink_init(void);
void wma_init(void);
void wma_run_due(double now);
void padgame_init(void);
void padgame_frame(unsigned long frame);
void camera_init(void);
void camera_frame(void);

/* the display layer (DirectDraw and Direct3D 7 on Vulkan), before DirectSound and DirectInput */
void port_graphics_init(void)
{
    ddraw_init();
}

/* the game's own stand-ins and extras, once, before the program starts (TD6's order: the Bink
   and WMA readers, widescreen, the controller code) */
void game_init(void)
{
    bink_init();
    wma_init();
    widescreen_init();
    padgame_init();
    camera_init();
}

/* each presented frame: in --test runs (with TD6_TEST_MUSIC=1) the music's samples come at frame
   ends on the virtual clock (wma.c) */
void game_frame(unsigned long frame)
{
    (void)frame;
    if (deterministic)
        wma_run_due(frame_clock_ms());
    camera_frame();
}

/* The front end reads no clock: its animations (the rev needle, the menu cursor, the car turning
   in the showroom) move a step a frame, so outside races frames are paced at menu_fps
   (options.txt, default 60) as TD6's own runtime did; races (the main loop's state 2, 0x63b740)
   keep the frame limit as set (none by default), as their physics run on the clock. The core's
   limiter calls this each frame outside test runs. */
double port_frame_limit_ms(double ms)
{
    static double menu_ms = -1;
    if (menu_ms < 0) {
        int mf = atoi(option_str("menu_fps", "60"));
        menu_ms = mf >= 10 ? 1000.0 / mf : 0;
    }
    if (R32(GAME_GAME_STATE) != 2 && menu_ms > ms)
        return menu_ms;
    return ms;
}

/* the controllers' frame (pad.c, after the scripted pad's presses): menus by pad, the race's
   layout, the bet screen, rumble strength */
void port_pad_frame(unsigned long frame)
{
    padgame_frame(frame);
}

/* GlobalMemoryStatus as TD6's own runtime answered: 512 MB, plenty for 1999 and no overflow in
   its arithmetic */
API(KERNEL32, GlobalMemoryStatus, 1)
{
    uint32_t p = ARG(0);
    W32(p, 32);
    W32(p + 4, 30);                      /* memory load % */
    W32(p + 8, 512u << 20);
    W32(p + 12, 384u << 20);
    W32(p + 16, 1024u << 20);
    W32(p + 20, 1024u << 20);
    W32(p + 24, 0x7ffe0000u);
    W32(p + 28, 0x7f000000u);
    return 0;
}
