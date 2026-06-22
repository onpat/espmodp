#pragma once

#include <cstdint>
#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_LCD

void draw_bitmap(int x, int y, int width, int height, const uint16_t *pixels);
void draw_bitmap_int16(int x, int y, int width, int height, const int16_t *pixels);

void init_lcd();
void fill_screen(uint16_t color);
void draw_rectangle(int x, int y, int width, int height, uint16_t color);

#else

inline void draw_bitmap(int x, int y, int width, int height, const uint16_t *pixels) {}
inline void draw_bitmap_int16(int x, int y, int width, int height, const int16_t *pixels) {}

inline void init_lcd() {}
inline void fill_screen(uint16_t color) {}
inline void draw_rectangle(int x, int y, int width, int height, uint16_t color) {}

#endif
