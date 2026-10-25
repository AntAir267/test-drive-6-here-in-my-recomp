/* A sharper font for the game's letters: not made for TD6 yet. The renderer asks for the picture
   it would draw from (font_atlas); NULL keeps the game's own. */
#pragma once
#include <stdint.h>

static inline const uint8_t *font_atlas(int *size, uint32_t *gen) { (void)size; (void)gen; return 0; }
