#include "textbox.hpp"

#include "bitmap.hpp"
#include "lcd.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_LCD

namespace Textbox {
namespace {

int safe_scale(int scale)
{
    return scale > 0 ? scale : 1;
}

int glyph_width(int scale)
{
    return TextBox::kGlyphHeight * safe_scale(scale);
}

int glyph_height(int scale)
{
    return TextBox::kGlyphWidth * safe_scale(scale);
}

void measure_text(const char *text, int scale, int *width, int *height)
{
    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
    if (!text || !*text) {
        return;
    }

    int columns = 1;
    int rows = 0;
    int max_rows = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') {
            max_rows = std::max(max_rows, rows);
            rows = 0;
            ++columns;
            continue;
        }
        ++rows;
    }
    max_rows = std::max(max_rows, rows);
    if (max_rows == 0) {
        return;
    }

    const int normalized_scale = safe_scale(scale);
    const int char_width = glyph_width(normalized_scale);
    const int char_height = glyph_height(normalized_scale);
    if (width) {
        *width = columns * char_width + (columns - 1);
    }
    if (height) {
        *height = max_rows * char_height + (max_rows - 1);
    }
}

void render_char(uint16_t *pixels, int bitmap_width, int x, int y, char ch, uint16_t foreground, int scale)
{
    const unsigned char *glyph = Bitmap::rasters[(ch >= 32 && ch <= 126) ? ch - 32 : 0];
    const int normalized_scale = safe_scale(scale);
    const int char_width = glyph_width(normalized_scale);
    const int char_height = glyph_height(normalized_scale);

    for (int out_y = 0; out_y < char_height; ++out_y) {
        const int glyph_x = out_y / normalized_scale;
        for (int out_x = 0; out_x < char_width; ++out_x) {
            const int glyph_row = out_x / normalized_scale;
            const bool is_set = (glyph[glyph_row] & (0x80 >> glyph_x)) != 0;
            if (is_set) {
                pixels[(y + out_y) * bitmap_width + x + out_x] = foreground;
            }
        }
    }
}

} // namespace

TextBox::TextBox(int x, int y, uint16_t foreground, uint16_t background, int scale)
    : x_(x), y_(y), foreground_(foreground), background_(background), scale_(safe_scale(scale))
{
}

void TextBox::draw(const char *text) const
{
    TextBitmap bitmap = render_text(text, foreground_, background_, scale_);
    if (!bitmap.valid()) {
        return;
    }
    draw_bitmap(x_, y_, bitmap.width, bitmap.height, bitmap.pixels.get());
}

void TextBox::draw_char(int x, int y, char ch) const
{
    const int width = glyph_width(scale_);
    const int height = glyph_height(scale_);
    std::unique_ptr<uint16_t[]> pixels(new (std::nothrow) uint16_t[static_cast<std::size_t>(width) * height]);
    if (!pixels) {
        return;
    }

    std::fill_n(pixels.get(), static_cast<std::size_t>(width) * height, background_);
    render_char(pixels.get(), width, 0, 0, ch, foreground_, scale_);
    draw_bitmap(x, y, width, height, pixels.get());
}

int measure_text_width(const char *text, int scale)
{
    int width = 0;
    measure_text(text, scale, &width, nullptr);
    return width;
}

int measure_text_height(const char *text, int scale)
{
    int height = 0;
    measure_text(text, scale, nullptr, &height);
    return height;
}

TextBitmap render_text(const char *text, uint16_t foreground, uint16_t background, int scale)
{
    TextBitmap bitmap;
    measure_text(text, scale, &bitmap.width, &bitmap.height);
    if (bitmap.width <= 0 || bitmap.height <= 0) {
        return bitmap;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(bitmap.width) * bitmap.height;
    bitmap.pixels.reset(new (std::nothrow) uint16_t[pixel_count]);
    if (!bitmap.pixels) {
        bitmap.width = 0;
        bitmap.height = 0;
        return bitmap;
    }
    std::fill_n(bitmap.pixels.get(), pixel_count, background);

    const int normalized_scale = safe_scale(scale);
    const int char_width = glyph_width(normalized_scale);
    const int char_height = glyph_height(normalized_scale);
    int x = 0;
    int y = 0;
    for (const char *p = text; p && *p; ++p) {
        if (*p == '\n') {
            x += char_width + 1;
            y = 0;
            continue;
        }
        render_char(bitmap.pixels.get(), bitmap.width, x, y, *p, foreground, normalized_scale);
        y += char_height + 1;
    }

    return bitmap;
}

void draw_text(int x, int y, const char *text, uint16_t foreground, uint16_t background, int scale)
{
    TextBitmap bitmap = render_text(text, foreground, background, scale);
    if (!bitmap.valid()) {
        return;
    }
    draw_bitmap(x, y, bitmap.width, bitmap.height, bitmap.pixels.get());
}

} // namespace Textbox

#endif // CONFIG_ENABLE_LCD
