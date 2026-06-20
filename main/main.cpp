#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <xm.h>

#include <string>
#include "esp_littlefs.h"
#include "lcd.hpp"
#include "textbox.hpp"
#include "sound.hpp"
#include "http_server.hpp"

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
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/lfs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;
    esp_vfs_littlefs_register(&conf);

    init_lcd();

    fill_screen(kBackground);

    // Initialize libxm, load file and generate output
    static Sound sound;
    const size_t xm_size = roadblast_xm_end - roadblast_xm_start;
    if (sound.load(roadblast_xm_start, xm_size)) {
        sound.set_volume(0.2f);
        sound.init_internal_dac();
        const uint16_t chunk_samples = 1024; // increased chunk size for better FreeRTOS task timing
        sound.start_playing(chunk_samples, [](int16_t* buffer, uint16_t samples) {
            sound.output_internal_dac(buffer, samples);
        });
    }

    auto render_message = [](const std::string& msg) {
        fill_screen(kBackground);
        const int text_width = Textbox::measure_text_width(msg.c_str(), kTextScale);
        const int text_height = Textbox::measure_text_height(msg.c_str(), kTextScale);
        int x = std::max(0, (kScreenWidth - text_width) / 2);
        int y = std::max(0, (kScreenHeight - text_height) / 2);

        Textbox::TextBitmap text_bitmap = Textbox::render_text(msg.c_str(), kWhite, kBackground, kTextScale);
        if (text_bitmap.valid()) {
            draw_bitmap(x, y, text_width, text_height, text_bitmap.pixels.get());
        }
    };

    render_message(kMessage);

    static HttpServer http_server;
    HttpServer::Callbacks callbacks;
    callbacks.on_start_playing = []() {
        const uint16_t chunk_samples = 1024;
        sound.start_playing(chunk_samples, [](int16_t* buffer, uint16_t samples) {
            sound.output_internal_dac(buffer, samples);
        });
    };
    callbacks.on_stop_playing = []() {
        sound.stop_playing();
    };
    callbacks.on_display_string = [render_message](const std::string& msg) {
        render_message(msg);
    };

    // Host AP mode. This will be accessible from the AP and serve index.html at /
    http_server.start(HttpServer::Mode::AP, callbacks, "ESP32-AP", "");

    wait_forever();
}
