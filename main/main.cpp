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
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_littlefs.h"
#include "driver/gpio.h"
#include "lcd.hpp"
#include "textbox.hpp"
#include "sound.hpp"
#include "playlist.hpp"
#include "http_server.hpp"
#include "esp_log.h"

namespace {
constexpr int kScreenWidth = 170;
constexpr int kScreenHeight = 320;
constexpr int kTextScale = 1;
constexpr uint16_t kBackground = 0x00F0;
constexpr uint16_t kWhite = 0xFFFF;
constexpr const char *kMessage = "hello, world!";

void wait_forever(Playlist& playlist)
{
    while (true) {
        playlist.update();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

} // namespace

extern "C" void app_main(void)
{
#ifdef CONFIG_STORAGE_SDCARD
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // 1-bit mode
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = (gpio_num_t)CONFIG_SD_CLK_GPIO;
    slot_config.cmd = (gpio_num_t)CONFIG_SD_CMD_GPIO;
    slot_config.d0 = (gpio_num_t)CONFIG_SD_D0_GPIO;
    slot_config.d3 = (gpio_num_t)CONFIG_SD_D3_GPIO;
#endif

#ifdef SDMMC_SLOT_FLAG_INTERNAL_PULLUP
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    // Software fix for missing pull-ups on the board (like T8 V1.7.1)
    gpio_set_pull_mode((gpio_num_t)CONFIG_SD_D0_GPIO, GPIO_PULLUP_ONLY);

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE("main", "Failed to mount SD card VFAT filesystem.");
    } else {
        sdmmc_card_print_info(stdout, card);
    }
#else
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/lfs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;
    esp_vfs_littlefs_register(&conf);
#endif

    init_lcd();

    fill_screen(kBackground);

    // Initialize libxm, load file and generate output
    static Sound sound;
    static Playlist playlist(sound);

#if defined(CONFIG_XM_SAMPLE_FLOAT)
    ESP_LOGI("main", "Current XM_SAMPLE_TYPE is Float");
#elif defined(CONFIG_XM_SAMPLE_INT16)
    ESP_LOGI("main", "Current XM_SAMPLE_TYPE is Int16");
#elif defined(CONFIG_XM_SAMPLE_INT8)
    ESP_LOGI("main", "Current XM_SAMPLE_TYPE is Int8");
#elif defined(CONFIG_XM_SAMPLE_DD4A)
    ESP_LOGI("main", "Current XM_SAMPLE_TYPE is DD4A");
#elif defined(CONFIG_XM_SAMPLE_DD8A)
    ESP_LOGI("main", "Current XM_SAMPLE_TYPE is DD8A");
#else
    ESP_LOGI("main", "Current XM_SAMPLE_TYPE is Unknown");
#endif

    // Initialize I2S first so the DAC receives clocks before I2C config
    sound.init_external_i2s(CONFIG_I2S_BCK_GPIO, CONFIG_I2S_WS_GPIO, CONFIG_I2S_DOUT_GPIO);
    // mod2 should pull up for i2c control!
    sound.init_pcm5122(CONFIG_I2C_SDA_GPIO, CONFIG_I2C_SCL_GPIO);
    sound.set_volume(0.1f);
    sound.set_pcm5122_dsp_program(Sound::Pcm5122DspProgram::FirInterpolation);

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
    callbacks.on_start_playing = [&]() {
        if (!playlist.is_playing() && playlist.get_current_index() >= 0) {
            playlist.resume();
        } else if (!playlist.is_playing() && playlist.get_items().size() > 0) {
            playlist.play(0);
        }
    };
    callbacks.on_stop_playing = [&]() {
        playlist.stop();
    };
    callbacks.on_display_string = [render_message](const std::string& msg) {
        render_message(msg);
    };
    callbacks.on_play_file = [&](const std::string& filename) {
        auto items = playlist.get_items();
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].filename == filename) {
                playlist.play(i);
                render_message("Playing: " + filename);
                return true;
            }
        }
        render_message("Failed: " + filename);
        return false;
    };
    callbacks.on_set_volume = [&](float vol) {
        sound.set_volume(vol);
    };
    callbacks.on_files_changed = [&]() {
        playlist.rescan();
    };
    callbacks.on_set_loop = [&](bool loop) {
        playlist.set_loop(loop);
    };
    callbacks.on_skip = [&]() {
        ESP_LOGI("Skip", "Skipped 1 seconds of the song!");
        sound.skip(1.0f);
    };
    callbacks.on_get_status = [&]() -> std::string {
        std::string json = "{";
        json += "\"is_playing\":" + std::string(playlist.is_playing() ? "true" : "false") + ",";
        json += "\"loop\":" + std::string(playlist.get_loop() ? "true" : "false") + ",";
        json += "\"playing_file\":\"";
        int idx = playlist.get_current_index();
        if (idx >= 0 && idx < (int)playlist.get_items().size()) {
            json += playlist.get_items()[idx].filename;
        }
        json += "\",";
        json += "\"current_time\":" + std::to_string(sound.get_current_time());
        json += "}";
        return json;
    };

    // Host AP mode. This will be accessible from the AP and serve index.html at /
    //http_server.start(HttpServer::Mode::AP, callbacks, "ESP32-AP", "");
    http_server.start(HttpServer::Mode::Client, callbacks, "4Open", "");

    playlist.rescan();
    playlist.play_next();

    wait_forever(playlist);
}
