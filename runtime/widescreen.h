/* Widescreen (widescreen.c): the game's cameras widened (Hor+), the renderer spreads them. */
#pragma once

void widescreen_init(void);
void widescreen_options(void);            /* the option changed */
int widescreen_on(void);
void widescreen_screen(int w, int h);     /* the renderer reports the picture area */
extern float ws_aspect;                    /* target width / height */
extern float ws_world_aspect;              /* y/x scale of the widened camera's projection */
extern int ws_race_depth, ws_hud_depth, ws_menu_depth;   /* where a frame's drawing is (menu: the front end) */
