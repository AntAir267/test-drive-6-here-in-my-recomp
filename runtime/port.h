/* port.h: Test Drive 6 for the AntiStatic runtime (third_party/antistatic). The game as the
   program sees it under Windows, and the port's names on the host; none of the game's code or
   data. */
#pragma once

#define PORT_TITLE "Test Drive 6"
#define PORT_SLUG "td6-native"
#define PORT_ENV "TD6"                      /* TD6_KEYS, TD6_PAD, TD6_ENHANCED, ... */

/* the game installed as C:\TD6, started there as "C:\TD6\TD6.exe" NoProps: its own switch for
   no properties dialog (0x40bbb0) */
#define PORT_VROOT "C:\\TD6"
#define PORT_START_DIR ""
#define PORT_EXE "TD6.exe"
#define PORT_CMDLINE_ARGS "NoProps"
#define PORT_VOLUME_LABEL "TD6"

/* the program's image ends at 0x660000 + its resources; allocations from 0x2400000 as the port
   always made them (the hash baselines depend on where the heap lands) */
#define PORT_ARENA_START 0x02400000u

/* The machine the program sees (TD6's own runtime's answers, kept so its checks still hold): one
   Pentium III, family 6 model 7 stepping 3 with MMX and SSE ("KNI" in TD6.log; 0x40fa30 asks
   CPUID leaves 0, 1 and 3), 1 GHz time-stamp counter, 1 MHz performance counter, 512 MB
   (GlobalMemoryStatus in port_td6.c), and --test runs on 2000-01-01 12:00 UTC */
#define PORT_CPUID_SIGNATURE 0x673
#define PORT_CPUID_MAX_LEAF 1
#define PORT_CPUID_FEATURES 0x0383fbffu      /* FPU ... APIC, CMOV, MMX, FXSR, SSE */
#define PORT_CPU_COUNT 1
#define PORT_CPU_REVISION 0x0703
#define PORT_TSC_HZ 1e9
#define PORT_QPC_HZ 1e6
#define PORT_MEMORY_MB 512
#define PORT_COMPUTER_NAME "INSANE"
#define PORT_TEST_DATE 946728000ull

/* widescreen (runtime/d3d7/vk.c): a full-view background quad drawn with depth on counts as the
   frame's background (TD6's menus), and the race HUD's middle 30 % (its time and countdown) stays
   centred; the front end's frame: port_ws_menu_place in widescreen.c */
#define PORT_WS_BG_DEPTH 1
#define PORT_WS_HUD_MIDDLE 0.3f
