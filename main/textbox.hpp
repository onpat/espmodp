#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

#include "lcd.hpp"

namespace Textbox {

struct TextBitmap {
    int width = 0;
    int height = 0;
    std::unique_ptr<uint16_t[]> pixels;

    bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
};

#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_LCD

class TextBox {
public:
    static constexpr int kGlyphWidth = 8;
    static constexpr int kGlyphHeight = 13;

    TextBox(int x, int y, uint16_t foreground, uint16_t background, int scale = 1);

    void draw(const char *text) const;
    void draw_char(int x, int y, char ch) const;

private:
    static constexpr int to_font_index(char ch)
    {
        return (ch >= 32 && ch <= 126) ? ch - 32 : 0;
    }

    int x_;
    int y_;
    uint16_t foreground_;
    uint16_t background_;
    int scale_;
};

int measure_text_width(const char *text, int scale = 1);
int measure_text_height(const char *text, int scale = 1);
TextBitmap render_text(const char *text, uint16_t foreground, uint16_t background, int scale = 1);
void draw_text(int x, int y, const char *text, uint16_t foreground, uint16_t background, int scale = 1);

#else

class TextBox {
public:
    TextBox(int x, int y, uint16_t foreground, uint16_t background, int scale = 1) {}
    void draw(const char *text) const {}
    void draw_char(int x, int y, char ch) const {}
};

inline int measure_text_width(const char *text, int scale = 1) { return 0; }
inline int measure_text_height(const char *text, int scale = 1) { return 0; }
inline TextBitmap render_text(const char *text, uint16_t foreground, uint16_t background, int scale = 1) { return TextBitmap{}; }
inline void draw_text(int x, int y, const char *text, uint16_t foreground, uint16_t background, int scale = 1) {}

#endif // CONFIG_ENABLE_LCD

} // namespace Textbox
