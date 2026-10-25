/* The host window (SDL3) as USER32 sees it, plus the keyboard tables shared with it. */
#pragma once
#include <stdint.h>

void host_window_create(const char *title, int w, int h);
void host_window_show(int visible);
void host_window_resize(int w, int h);
void host_window_title(const char *title);
void host_desktop_size(int *w, int *h);
void host_pump(void);
void host_cursor_visible(int visible);
void host_message_box(const char *caption, const char *text);
void host_show_bitmap(uint32_t dib_resource, int x, int y, int w, int h);
/* The program set a display mode (DirectDraw): size the host window for it (an integer
   multiple that fits the desktop) unless we are in borderless fullscreen. Fullscreen on the
   host is always a borderless window: the desktop mode is never changed. */
void host_display_mode(int w, int h);
/* The SDL window (created with SDL_WINDOW_VULKAN), or NULL when headless. Opaque here so
   files without SDL headers can include this one. */
void *host_sdl_window(void);
int host_vk_down(int vk);
int host_shift_down(void);

/* USER32's queue (user32.c) */
void post_message(uint32_t msg, uint32_t wparam, uint32_t lparam);

/* keyboard tables (keys.c) */
int vk_to_char(uint32_t vk, int shift);
uint32_t vk_to_scan(uint32_t vk);
uint32_t scan_to_vk(uint32_t scan);
const char *scan_name(uint32_t scan);
