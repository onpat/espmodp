#include "lcd.hpp"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

#include "sdkconfig.h"

#ifdef CONFIG_ENABLE_LCD

static const char *TAG = "lcd";

namespace {
constexpr gpio_num_t kResetGpio = static_cast<gpio_num_t>(CONFIG_LCD_RESET_GPIO);
constexpr gpio_num_t kDcGpio = static_cast<gpio_num_t>(CONFIG_LCD_DC_GPIO);
constexpr gpio_num_t kCsGpio = static_cast<gpio_num_t>(CONFIG_LCD_CS_GPIO);
constexpr gpio_num_t kBacklightGpio = static_cast<gpio_num_t>(CONFIG_LCD_BACKLIGHT_GPIO);
constexpr gpio_num_t kSclkGpio = static_cast<gpio_num_t>(CONFIG_LCD_SCLK_GPIO);
constexpr gpio_num_t kMosiGpio = static_cast<gpio_num_t>(CONFIG_LCD_MOSI_GPIO);
constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr uint16_t kPanelWidth = 170;
constexpr uint16_t kPanelHeight = 320;
constexpr uint32_t kMaxTransferWidth = 240;
constexpr uint32_t kMaxTransferHeight = 320;
constexpr uint32_t kPixelClockHz = 40 * 1000 * 1000;

esp_lcd_panel_io_handle_t s_io_handle = nullptr;
esp_lcd_panel_handle_t s_panel_handle = nullptr;
SemaphoreHandle_t s_transfer_done_sem = nullptr;

bool on_color_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t task_woken = pdFALSE;
    if (s_transfer_done_sem) {
        xSemaphoreGiveFromISR(s_transfer_done_sem, &task_woken);
    }
    return task_woken == pdTRUE;
}

void wait_for_color_transfer()
{
    if (s_transfer_done_sem) {
        xSemaphoreTake(s_transfer_done_sem, portMAX_DELAY);
    }
}
} // namespace

void init_lcd()
{
    gpio_config_t bk_gpio_config = {};
    bk_gpio_config.pin_bit_mask = 1ULL << static_cast<uint32_t>(kBacklightGpio);
    bk_gpio_config.mode = GPIO_MODE_OUTPUT;
    bk_gpio_config.pull_up_en = GPIO_PULLUP_DISABLE;
    bk_gpio_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    bk_gpio_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level(kBacklightGpio, 0));

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = static_cast<int>(kMosiGpio);
    bus_config.miso_io_num = -1;
    bus_config.sclk_io_num = static_cast<int>(kSclkGpio);
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    bus_config.max_transfer_sz = kMaxTransferWidth * kMaxTransferHeight * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(kSpiHost, &bus_config, SPI_DMA_CH_AUTO));

    if (!s_transfer_done_sem) {
        s_transfer_done_sem = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(s_transfer_done_sem ? ESP_OK : ESP_ERR_NO_MEM);
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = kCsGpio;
    io_config.dc_gpio_num = kDcGpio;
    io_config.spi_mode = 3;
    io_config.pclk_hz = kPixelClockHz;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.on_color_trans_done = on_color_trans_done;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)kSpiHost,
        &io_config,
        &s_io_handle));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = kResetGpio;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel_handle, 35, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));
    ESP_ERROR_CHECK(gpio_set_level(kBacklightGpio, 1));

    ESP_LOGI(TAG, "LCD initialized");
}

void draw_bitmap(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (!s_panel_handle) {
        ESP_LOGE(TAG, "LCD panel not initialized");
        return;
    }
    if (!pixels || width <= 0 || height <= 0) {
        ESP_LOGE(TAG, "Invalid bitmap");
        return;
    }

    const size_t pixel_count = static_cast<size_t>(width) * height;
    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(
        pixel_count * 2,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate bitmap buffer");
        return;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        const uint16_t color = pixels[i];
        buffer[i * 2] = static_cast<uint8_t>((color >> 8) & 0xFF);
        buffer[i * 2 + 1] = static_cast<uint8_t>(color & 0xFF);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        s_panel_handle,
        x,
        y,
        x + width,
        y + height,
        buffer));
    wait_for_color_transfer();

    free(buffer);
}

void fill_screen(uint16_t color)
{
    draw_rectangle(0, 0, kPanelWidth, kPanelHeight, color);
}

void draw_rectangle(int x, int y, int width, int height, uint16_t color)
{
    if (!s_panel_handle) {
        ESP_LOGE(TAG, "LCD panel not initialized");
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    const int chunk_lines = 8;
    const int lines_to_alloc = (height < chunk_lines) ? height : chunk_lines;
    const size_t pixel_count = static_cast<size_t>(width) * lines_to_alloc;
    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(
        pixel_count * 2,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate rectangle buffer");
        return;
    }

    const uint8_t lo = static_cast<uint8_t>(color & 0xFF);
    const uint8_t hi = static_cast<uint8_t>((color >> 8) & 0xFF);
    for (size_t i = 0; i < pixel_count * 2; i += 2) {
        buffer[i] = hi;
        buffer[i + 1] = lo;
    }

    for (int row = 0; row < height; row += lines_to_alloc) {
        int lines = (row + lines_to_alloc > height) ? (height - row) : lines_to_alloc;
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
            s_panel_handle,
            x,
            y + row,
            x + width,
            y + row + lines,
            buffer));
        wait_for_color_transfer();
    }

    free(buffer);
}

#endif // CONFIG_ENABLE_LCD
