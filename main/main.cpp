#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <xm.h>

#include "lcd.hpp"
#include "textbox.hpp"
#include "sound.hpp"

extern const uint8_t roadblast_xm_start[] asm("_binary_roadblast_xm_start");
extern const uint8_t roadblast_xm_end[]   asm("_binary_roadblast_xm_end");

namespace {
constexpr int kScreenWidth = 170;
constexpr int kScreenHeight = 320;
constexpr int kTextScale = 1;
constexpr uint16_t kBackground = 0x00F0;
constexpr uint16_t kWhite = 0xFFFF;
constexpr const char *kMessage = "hello, world!";

void wait_forever()
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
} // namespace

extern "C" void app_main(void)
{
    init_lcd();

    fill_screen(kBackground);

    // Initialize libxm, load file and generate output
    Sound sound;
    const size_t xm_size = roadblast_xm_end - roadblast_xm_start;
    if (sound.load(roadblast_xm_start, xm_size)) {
        sound.set_volume(0.2f);
        sound.init_internal_dac();
        const uint16_t chunk_samples = 1024; // increased chunk size for better FreeRTOS task timing
        sound.start_playing(chunk_samples, [&sound](int16_t* buffer, uint16_t samples) {
            sound.output_internal_dac(buffer, samples);
        });
    }

    const int text_width = Textbox::measure_text_width(kMessage, kTextScale);
    const int text_height = Textbox::measure_text_height(kMessage, kTextScale);

    int x = std::max(0, (kScreenWidth - text_width) / 2);
    int y = std::max(0, (kScreenHeight - text_height) / 2);

    Textbox::TextBitmap text_bitmap = Textbox::render_text(kMessage, kWhite, kBackground, kTextScale);
    if (!text_bitmap.valid()) {
        wait_forever();
    }

    draw_bitmap(x, y, text_width, text_height, text_bitmap.pixels.get());

    wait_forever();
}
