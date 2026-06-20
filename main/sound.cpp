#include "sound.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cmath>

static const char* TAG = "Sound";

Sound::Sound() : xm_ctx(nullptr), xm_pool(nullptr), play_callback(nullptr), play_chunk_samples(0), play_task_handle(nullptr), playing(false), gptimer(nullptr), dac_handle(nullptr), i2s_tx_handle(nullptr), master_volume(1.0f) {
    ESP_LOGI(TAG, "Initializing libxm (Sound constructor)");
}

Sound::~Sound() {
    stop_playing();
    if (xm_pool) {
        heap_caps_free(xm_pool);
        xm_pool = nullptr;
    }
    ESP_LOGI(TAG, "Sound destroyed");
}

bool Sound::load(const uint8_t* data, size_t size) {
    ESP_LOGI(TAG, "Loading file from memory (size: %zu bytes)", size);
    
    xm_prescan_data_t* prescan = static_cast<xm_prescan_data_t*>(__builtin_alloca(XM_PRESCAN_DATA_SIZE));
    if (!xm_prescan_module(reinterpret_cast<const char*>(data), size, prescan)) {
        ESP_LOGE(TAG, "Failed to prescan module");
        return false;
    }

    uint32_t ctx_size = xm_size_for_context(prescan);
    ESP_LOGI(TAG, "Context size required: %lu bytes", (unsigned long)ctx_size);

    xm_pool = static_cast<char*>(heap_caps_malloc(ctx_size, MALLOC_CAP_DEFAULT));
    if (!xm_pool) {
        ESP_LOGE(TAG, "Failed to allocate memory for context");
        return false;
    }

    xm_ctx = xm_create_context(xm_pool, prescan, reinterpret_cast<const char*>(data), size);
    if (!xm_ctx) {
        ESP_LOGE(TAG, "Failed to create context");
        heap_caps_free(xm_pool);
        xm_pool = nullptr;
        return false;
    }

    xm_set_sample_rate(xm_ctx, 48000);

    ESP_LOGI(TAG, "Successfully loaded module from memory");
    return true;
}

void Sound::generate(float* output, uint16_t num_samples) {
    ESP_LOGI(TAG, "Generating %u samples to memory", num_samples);
    if (xm_ctx) {
        xm_generate_samples(xm_ctx, output, num_samples);
        if (master_volume != 1.0f) {
            for (uint16_t i = 0; i < num_samples * 2; ++i) {
                output[i] *= master_volume;
            }
        }
    } else {
        ESP_LOGE(TAG, "Cannot generate samples, context not initialized");
    }
}

void Sound::generate16(int16_t* output, uint16_t num_samples) {
    if (!xm_ctx) {
        ESP_LOGE(TAG, "Cannot generate samples, context not initialized");
        return;
    }
    
    float* temp_buf = static_cast<float*>(heap_caps_malloc(num_samples * 2 * sizeof(float), MALLOC_CAP_DEFAULT));
    if (!temp_buf) return;
    
    xm_generate_samples(xm_ctx, temp_buf, num_samples);
    
    for (uint32_t i = 0; i < (uint32_t)num_samples * 2; ++i) {
        float v = temp_buf[i] * master_volume;
        
        // Fast cubic soft clipping approximation: x - (x^3 / 3) for x in [-1, 1]
        // Output range is [-2/3, 2/3]. We multiply by 1.5 to map to [-1, 1]
        // This is FMA-friendly and avoids division and tanhf
        float v_clamp = v;
        if (v_clamp > 1.0f) v_clamp = 1.0f;
        else if (v_clamp < -1.0f) v_clamp = -1.0f;
        
        float soft_v = v_clamp - 0.3333333f * v_clamp * v_clamp * v_clamp;
        float scaled = soft_v * 49150.5f; // 32767.0f * 1.5f
        
        int32_t sample_int = static_cast<int32_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        
        if (sample_int > 32767)  sample_int = 32767;
        if (sample_int < -32768) sample_int = -32768;
        
        output[i] = static_cast<int16_t>(sample_int);
    }
    
    heap_caps_free(temp_buf);
}

void Sound::generate8(int8_t* output, uint16_t num_samples) {
    if (!xm_ctx) {
        ESP_LOGE(TAG, "Cannot generate samples, context not initialized");
        return;
    }
    
    float* temp_buf = static_cast<float*>(heap_caps_malloc(num_samples * 2 * sizeof(float), MALLOC_CAP_DEFAULT));
    if (!temp_buf) return;
    
    xm_generate_samples(xm_ctx, temp_buf, num_samples);
    
    for (uint32_t i = 0; i < (uint32_t)num_samples * 2; ++i) {
        float v = temp_buf[i] * master_volume;
        
        float v_clamp = v;
        if (v_clamp > 1.0f) v_clamp = 1.0f;
        else if (v_clamp < -1.0f) v_clamp = -1.0f;
        
        float soft_v = v_clamp - 0.3333333f * v_clamp * v_clamp * v_clamp;
        float scaled = soft_v * 190.5f; // 127.0f * 1.5f
        
        int32_t sample_int = static_cast<int32_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        
        if (sample_int > 127)  sample_int = 127;
        if (sample_int < -128) sample_int = -128;
        
        output[i] = static_cast<int8_t>(sample_int);
    }
    
    heap_caps_free(temp_buf);
}

void Sound::start_playing(uint16_t chunk_samples, std::function<void(int16_t*, uint16_t)> callback) {
    if (playing) {
        stop_playing();
    }
    
    play_chunk_samples = chunk_samples;
    play_callback = callback;
    playing = true;
    
    xTaskCreatePinnedToCore(play_task, "Sound_Play", 4096, this, 5, (TaskHandle_t*)&play_task_handle, 1);

    gptimer_config_t timer_config = {};
    timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_config.direction = GPTIMER_COUNT_UP;
    timer_config.resolution_hz = 1000000; // 1MHz resolution
    gptimer_new_timer(&timer_config, &gptimer);

    uint64_t chunk_time_us = ((uint64_t)chunk_samples * 125ULL) / 6ULL;
    gptimer_alarm_config_t alarm_config = {};
    alarm_config.alarm_count = chunk_time_us;
    alarm_config.reload_count = 0;
    alarm_config.flags.auto_reload_on_alarm = true;
    gptimer_set_alarm_action(gptimer, &alarm_config);

    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = timer_isr_callback;
    gptimer_register_event_callbacks(gptimer, &cbs, this);

    gptimer_enable(gptimer);
    gptimer_start(gptimer);
}

void Sound::stop_playing() {
    if (playing) {
        playing = false;
        
        if (gptimer) {
            gptimer_stop(gptimer);
            gptimer_disable(gptimer);
            gptimer_del_timer(gptimer);
            gptimer = nullptr;
        }

        // Wake up task in case it's waiting
        if (play_task_handle) {
            xTaskNotifyGive((TaskHandle_t)play_task_handle);
        }

        while (play_task_handle != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

bool IRAM_ATTR Sound::timer_isr_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    Sound* sound = static_cast<Sound*>(user_ctx);
    BaseType_t high_task_wakeup = pdFALSE;
    
    if (sound->play_task_handle) {
        vTaskNotifyGiveFromISR((TaskHandle_t)sound->play_task_handle, &high_task_wakeup);
    }
    
    return high_task_wakeup == pdTRUE;
}

void Sound::play_task(void* arg) {
    Sound* sound = static_cast<Sound*>(arg);
    int16_t* buffer = static_cast<int16_t*>(heap_caps_malloc(sound->play_chunk_samples * 2 * sizeof(int16_t), MALLOC_CAP_DEFAULT));
    
    while (sound->playing) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!sound->playing) break;

        if (sound->xm_ctx) {
            sound->generate16(buffer, sound->play_chunk_samples);
            if (sound->play_callback) {
                sound->play_callback(buffer, sound->play_chunk_samples);
            }
        }
    }
    
    if (buffer) {
        heap_caps_free(buffer);
    }
    
    sound->play_task_handle = nullptr;
    vTaskDelete(NULL);
}

bool Sound::init_internal_dac() {
    dac_continuous_config_t cont_cfg = {};
    cont_cfg.chan_mask = DAC_CHANNEL_MASK_ALL;
    cont_cfg.desc_num = 4;
    cont_cfg.buf_size = 2048;
    cont_cfg.freq_hz = dac_oversampling ? 96000 : 48000;
    cont_cfg.offset = 0;
    cont_cfg.clk_src = DAC_DIGI_CLK_SRC_DEFAULT;
    cont_cfg.chan_mode = DAC_CHANNEL_MODE_ALTER;

    if (dac_continuous_new_channels(&cont_cfg, &dac_handle) != ESP_OK) return false;
    if (dac_continuous_enable(dac_handle) != ESP_OK) return false;
    return true;
}

bool Sound::init_external_i2s(int bck_io_num, int ws_io_num, int data_out_num) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)bck_io_num,
            .ws = (gpio_num_t)ws_io_num,
            .dout = (gpio_num_t)data_out_num,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    if (i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(i2s_tx_handle) != ESP_OK) return false;
    return true;
}

inline uint32_t xorshift32() {
    static uint32_t x = 2463534242; // Seed
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

inline uint8_t processSampleWithDitherInt(int16_t sampleIn) {
    int32_t scaled = (sampleIn * 127) / 128 + 32768;
    int32_t noise = (int32_t)(xorshift32() % 256) - 128;
    scaled += noise;
    if (scaled > 65535) scaled = 65535;
    if (scaled < 0) scaled = 0;
    return (uint8_t)(scaled >> 8);
}

void Sound::output_internal_dac(int16_t* buffer, uint16_t samples) {
    if (!dac_handle) return;
    
    if (dac_oversampling) {
        uint8_t* dac_buf = (uint8_t*)heap_caps_malloc(samples * 4, MALLOC_CAP_DEFAULT);
        if (!dac_buf) return;
        
        uint32_t out_idx = 0;
        for (uint16_t i = 0; i < samples; ++i) {
            int16_t l_curr = buffer[i * 2];
            int16_t r_curr = buffer[i * 2 + 1];
            
            int16_t l_next = (i + 1 < samples) ? buffer[(i + 1) * 2] : l_curr;
            int16_t r_next = (i + 1 < samples) ? buffer[(i + 1) * 2 + 1] : r_curr;
            
            if (dac_dither) {
                dac_buf[out_idx++] = processSampleWithDitherInt(l_curr);
                dac_buf[out_idx++] = processSampleWithDitherInt(r_curr);
                
                int16_t l_mid = (l_curr + l_next) / 2;
                int16_t r_mid = (r_curr + r_next) / 2;
                
                dac_buf[out_idx++] = processSampleWithDitherInt(l_mid);
                dac_buf[out_idx++] = processSampleWithDitherInt(r_mid);
            } else {
                dac_buf[out_idx++] = (uint8_t)((l_curr + 32768) >> 8);
                dac_buf[out_idx++] = (uint8_t)((r_curr + 32768) >> 8);
                
                int16_t l_mid = (l_curr + l_next) / 2;
                int16_t r_mid = (r_curr + r_next) / 2;
                
                dac_buf[out_idx++] = (uint8_t)((l_mid + 32768) >> 8);
                dac_buf[out_idx++] = (uint8_t)((r_mid + 32768) >> 8);
            }
        }
        
        size_t bytes_loaded = 0;
        dac_continuous_write(dac_handle, dac_buf, samples * 4, &bytes_loaded, portMAX_DELAY);
        heap_caps_free(dac_buf);
    } else {
        uint8_t* dac_buf = (uint8_t*)heap_caps_malloc(samples * 2, MALLOC_CAP_DEFAULT);
        if (!dac_buf) return;
        
        for (uint16_t i = 0; i < samples * 2; ++i) {
            if (dac_dither) {
                dac_buf[i] = processSampleWithDitherInt(buffer[i]);
            } else {
                dac_buf[i] = (uint8_t)((buffer[i] + 32768) >> 8);
            }
        }
        
        size_t bytes_loaded = 0;
        dac_continuous_write(dac_handle, dac_buf, samples * 2, &bytes_loaded, portMAX_DELAY);
        heap_caps_free(dac_buf);
    }
}

void Sound::output_external_i2s(int16_t* buffer, uint16_t samples) {
    if (!i2s_tx_handle) return;
    
    size_t bytes_loaded = 0;
    i2s_channel_write(i2s_tx_handle, buffer, samples * 2 * sizeof(int16_t), &bytes_loaded, portMAX_DELAY);
}
