#include "sound.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cmath>
#include <cstdio>
#include "driver/i2c_master.h"

extern "C" void xm_dump_sample_info(const xm_context_t*);

// Single source of truth for hardware output rate.
// When CONFIG_XM_SAMPLE_RATE is non-zero, libxm is compiled with that
// rate hardcoded and the I2S/DAC clock must match. When zero (runtime
// libxm), default both to 48000 and let xm_set_sample_rate() sync libxm.
constexpr uint32_t kOutputSampleRate =
#if CONFIG_XM_SAMPLE_RATE != 0
    CONFIG_XM_SAMPLE_RATE;
#else
    48000;
#endif

#if CONFIG_ENABLE_CYCLE_LOGGING
#define ENABLE_CYCLE_LOGGING
#endif

static const char* TAG = "Sound";

constexpr uint8_t kPcm5122Addr = 0x4C;

static i2c_master_bus_handle_t i2c_bus_handle = nullptr;
static i2c_master_dev_handle_t pcm5122_handle = nullptr;

static esp_err_t pcm5122_write_reg(uint8_t reg, uint8_t val) {
    if (!pcm5122_handle) return ESP_FAIL;
    uint8_t data[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(pcm5122_handle, data, 2, -1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C Write failed at reg 0x%02x: %s", reg, esp_err_to_name(err));
    }
    return err;
}

Sound::Sound() : xm_ctx(nullptr), xm_pool(nullptr), play_callback(nullptr), play_chunk_samples(0), play_task_handle(nullptr), playing(false), gptimer(nullptr), dac_handle(nullptr), i2s_tx_handle(nullptr), master_volume(1.0f) {
    ESP_LOGI(TAG, "Initializing libxm (Sound constructor)");
}

Sound::~Sound() {
    stop_playing();

    if (xm_pool) {
        heap_caps_free(xm_pool);
        xm_pool = nullptr;
    }
    if (temp_float_buf) {
        heap_caps_free(temp_float_buf);
        temp_float_buf = nullptr;
    }
    if (dac_buf) {
        heap_caps_free(dac_buf);
        dac_buf = nullptr;
    }
    ESP_LOGI(TAG, "Sound destroyed");
}

static size_t mem_stream_read(xm_stream_t* stream, void* dest, size_t length, size_t offset) {
    const uint8_t* data = static_cast<const uint8_t*>(stream->user_data);
    memcpy(dest, data + offset, length);
    return length;
}

bool Sound::load(const uint8_t* data, size_t size) {
    ESP_LOGI(TAG, "Loading file from memory (size: %zu bytes)", size);
    
    xm_stream_t stream;
    stream.read = mem_stream_read;
    stream.user_data = (void*)data;

    xm_prescan_data_t* prescan = static_cast<xm_prescan_data_t*>(__builtin_alloca(XM_PRESCAN_DATA_SIZE));
    if (!xm_prescan_module(&stream, size, prescan)) {
        ESP_LOGE(TAG, "Failed to prescan module");
        return false;
    }

    uint32_t ctx_size = xm_size_for_context(prescan);
    ESP_LOGI(TAG, "Context pool required: %lu bytes", (unsigned long)ctx_size);

    xm_ctx = nullptr; // Ensure we don't access it while reallocating
    if (xm_pool) {
        heap_caps_free(xm_pool);
        xm_pool = nullptr;
    }

    xm_pool = static_cast<char*>(heap_caps_malloc(ctx_size, MALLOC_CAP_DEFAULT));
    if (!xm_pool) {
        ESP_LOGE(TAG, "Failed to allocate memory for context");
        return false;
    }

    xm_ctx = xm_create_context(xm_pool, prescan, &stream, size);
    if (!xm_ctx) {
        ESP_LOGE(TAG, "Failed to create context");
        heap_caps_free(xm_pool);
        xm_pool = nullptr;
        return false;
    }

    uint32_t packed_size = xm_get_packed_data_size(xm_ctx);
    ESP_LOGI(TAG, "Packed sample data: %lu bytes | Total memory: %lu bytes",
             (unsigned long)packed_size,
             (unsigned long)(ctx_size + packed_size));

    fprintf(stderr, "POST-LOAD: num_samples=%u "
           "num_instruments=%u channels=%u\n",
           xm_get_number_of_samples(xm_ctx),
           xm_get_number_of_instruments(xm_ctx),
           xm_get_number_of_channels(xm_ctx));
    fflush(stderr);

    xm_dump_sample_info(xm_ctx);

    xm_set_sample_rate(xm_ctx, (uint16_t)kOutputSampleRate);

    ESP_LOGI(TAG, "Successfully loaded module from memory");
    return true;
}

static size_t file_stream_read(xm_stream_t* stream, void* dest, size_t length, size_t offset) {
    FILE* f = static_cast<FILE*>(stream->user_data);
    fseek(f, offset, SEEK_SET);

    /* Stage reads through an SRAM bounce buffer so fread never writes
     * directly to a PSRAM destination. */
    size_t total = 0;
    uint8_t buf[128];
    while (total < length) {
        size_t chunk = length - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        size_t n = fread(buf, 1, chunk, f);
        if (n == 0) break;
        memcpy((uint8_t*)dest + total, buf, n);
        total += n;
    }
    return total;
}

bool Sound::load_from_file(const char* filepath) {
    ESP_LOGI(TAG, "Loading file from filesystem: %s", filepath);

    FILE* f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    xm_stream_t stream;
    stream.read = file_stream_read;
    stream.user_data = f;

    xm_prescan_data_t* prescan = static_cast<xm_prescan_data_t*>(__builtin_alloca(XM_PRESCAN_DATA_SIZE));
    if (!xm_prescan_module(&stream, size, prescan)) {
        ESP_LOGE(TAG, "Failed to prescan module");
        fclose(f);
        return false;
    }

    uint32_t ctx_size = xm_size_for_context(prescan);
    ESP_LOGI(TAG, "Context pool required: %lu bytes", (unsigned long)ctx_size);

    xm_ctx = nullptr;
    if (xm_pool) {
        heap_caps_free(xm_pool);
        xm_pool = nullptr;
    }

    xm_pool = static_cast<char*>(heap_caps_malloc(ctx_size, MALLOC_CAP_DEFAULT));
    if (!xm_pool) {
        ESP_LOGE(TAG, "Failed to allocate memory for context");
        fclose(f);
        return false;
    }

    xm_ctx = xm_create_context(xm_pool, prescan, &stream, size);
    fclose(f);

    if (!xm_ctx) {
        ESP_LOGE(TAG, "Failed to create context");
        heap_caps_free(xm_pool);
        xm_pool = nullptr;
        return false;
    }

    uint32_t packed_size = xm_get_packed_data_size(xm_ctx);
    ESP_LOGI(TAG, "Packed sample data: %lu bytes | Total memory: %lu bytes",
             (unsigned long)packed_size,
             (unsigned long)(ctx_size + packed_size));

    ESP_LOGI(TAG, "Successfully loaded module from file");

    xm_set_sample_rate(xm_ctx, (uint16_t)kOutputSampleRate);

    fprintf(stderr, "POST-LOAD: num_samples=%u "
           "num_instruments=%u channels=%u\n",
           xm_get_number_of_samples(xm_ctx),
           xm_get_number_of_instruments(xm_ctx),
           xm_get_number_of_channels(xm_ctx));
    fflush(stderr);

    xm_dump_sample_info(xm_ctx);

    return true;
}

void IRAM_ATTR Sound::generate(float* output, uint16_t num_samples) {
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

#include "esp_cpu.h"

extern "C" {
    extern uint64_t g_cycles_tick;
    extern uint64_t g_cycles_fetch;
    extern uint64_t g_cycles_decode;
    extern uint64_t g_cycles_mix;
    extern uint64_t g_cycles_fetch_sample;
    extern uint64_t g_cycles_fetch_logic;
    extern uint64_t g_cycles_fetch_lerp;
    extern uint32_t g_fetch_cache_hits;
    extern uint32_t g_fetch_cache_misses;

    uint32_t IRAM_ATTR get_cycle_count(void) {
        return esp_cpu_get_cycle_count();
    }
}

void IRAM_ATTR Sound::generate16(int16_t* output, uint16_t num_samples) {
    if (!xm_ctx) {
        ESP_LOGE(TAG, "Cannot generate samples, context not initialized");
        return;
    }
    
    if (!temp_float_buf || temp_float_buf_samples < num_samples) {
        if (temp_float_buf) heap_caps_free(temp_float_buf);
        temp_float_buf = static_cast<float*>(heap_caps_malloc(num_samples * 2 * sizeof(float), MALLOC_CAP_DEFAULT));
        temp_float_buf_samples = num_samples;
    }
    float* temp_buf = temp_float_buf;
    if (!temp_buf) return;
    
#ifdef ENABLE_CYCLE_LOGGING
    g_cycles_tick = 0;
    g_cycles_fetch = 0;
    g_cycles_decode = 0;
    g_cycles_mix = 0;
    g_cycles_fetch_sample = 0;
    g_cycles_fetch_logic = 0;
    g_cycles_fetch_lerp = 0;
    g_fetch_cache_hits = 0;
    g_fetch_cache_misses = 0;
    uint64_t start_time = esp_timer_get_time();
#endif

    xm_generate_samples(xm_ctx, temp_buf, num_samples);
    
#ifdef ENABLE_CYCLE_LOGGING
    uint64_t end_time = esp_timer_get_time();

    static uint64_t last_log_time = 0;
    static uint32_t log_count = 0;
    log_count++;
    if (end_time - last_log_time > 1000000) {
        uint64_t total_cycles = g_cycles_tick + g_cycles_fetch + g_cycles_mix;
        if (total_cycles == 0) total_cycles = 1;
        ESP_LOGI(TAG, "generate16 [%lu]: num_samples=%u, generate_time=%llu us",
                 (unsigned long)log_count, num_samples, (unsigned long long)(end_time - start_time));
        ESP_LOGI(TAG, "  -> tick: %llu cyc (%llu%%), fetch: %llu cyc (%llu%%), decode: %llu cyc (%llu%% of fetch), mix: %llu cyc (%llu%%)",
                 (unsigned long long)g_cycles_tick, (unsigned long long)(g_cycles_tick * 100 / total_cycles),
                 (unsigned long long)g_cycles_fetch, (unsigned long long)(g_cycles_fetch * 100 / total_cycles),
                 (unsigned long long)g_cycles_decode, (unsigned long long)(g_cycles_fetch > 0 ? g_cycles_decode * 100 / g_cycles_fetch : 0),
                 (unsigned long long)g_cycles_mix, (unsigned long long)(g_cycles_mix * 100 / total_cycles));
        ESP_LOGI(TAG, "  -> fetch breakdown: sample: %llu cyc (%llu%% of fetch), logic: %llu cyc, lerp: %llu cyc, overhead: %llu cyc, cache hits: %lu, misses: %lu",
                 (unsigned long long)g_cycles_fetch_sample, (unsigned long long)(g_cycles_fetch > 0 ? g_cycles_fetch_sample * 100 / g_cycles_fetch : 0),
                 (unsigned long long)g_cycles_fetch_logic,
                 (unsigned long long)g_cycles_fetch_lerp,
                 (unsigned long long)(g_cycles_fetch > (g_cycles_fetch_sample + g_cycles_fetch_logic + g_cycles_fetch_lerp) ? g_cycles_fetch - g_cycles_fetch_sample - g_cycles_fetch_logic - g_cycles_fetch_lerp : 0),
                 (unsigned long)g_fetch_cache_hits, (unsigned long)g_fetch_cache_misses);
        last_log_time = end_time;
    }
#endif

    for (uint32_t i = 0; i < (uint32_t)num_samples * 2; ++i) {
#ifdef ENABLE_SOFTWARE_VOLUME
        float v = temp_buf[i] * master_volume;
        
        // Fast cubic soft clipping approximation: x - (x^3 / 3) for x in [-1, 1]
        // Output range is [-2/3, 2/3]. We multiply by 1.5 to map to [-1, 1]
        // This is FMA-friendly and avoids division and tanhf
        float v_clamp = v;
        if (v_clamp > 1.0f) v_clamp = 1.0f;
        else if (v_clamp < -1.0f) v_clamp = -1.0f;
        
        float soft_v = v_clamp - 0.3333333f * v_clamp * v_clamp * v_clamp;
        float scaled = soft_v * 49150.5f; // 32767.0f * 1.5f
#else
        float scaled = temp_buf[i] * 32767.0f;
#endif
        
        int32_t sample_int = static_cast<int32_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        
        if (sample_int > 32767)  sample_int = 32767;
        if (sample_int < -32768) sample_int = -32768;
        
        output[i] = static_cast<int16_t>(sample_int);
    }
}

void IRAM_ATTR Sound::generate8(int8_t* output, uint16_t num_samples) {
    if (!xm_ctx) {
        ESP_LOGE(TAG, "Cannot generate samples, context not initialized");
        return;
    }
    
    if (!temp_float_buf || temp_float_buf_samples < num_samples) {
        if (temp_float_buf) heap_caps_free(temp_float_buf);
        temp_float_buf = static_cast<float*>(heap_caps_malloc(num_samples * 2 * sizeof(float), MALLOC_CAP_DEFAULT));
        temp_float_buf_samples = num_samples;
    }
    float* temp_buf = temp_float_buf;
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
        
        if (pcm5122_initialized) {
            set_pcm5122_mute(true);
        }
        
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
    
    if (buffer && sound->play_callback) {
        memset(buffer, 0, sound->play_chunk_samples * 2 * sizeof(int16_t));
        for (int i = 0; i < 8; i++) {
            sound->play_callback(buffer, sound->play_chunk_samples);
        }
        
        if (sound->pcm5122_initialized) {
            sound->set_pcm5122_mute(false);
        }
    }
    
    while (sound->playing) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!sound->playing) break;

        if (sound->xm_ctx) {
            sound->generate16(buffer, sound->play_chunk_samples);
            if (sound->play_callback) {
                sound->play_callback(buffer, sound->play_chunk_samples);
            }
        }
        vTaskDelay(0); // Yield to reset task watchdog if needed
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
    cont_cfg.freq_hz = dac_oversampling ? kOutputSampleRate * 2 : kOutputSampleRate;
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
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kOutputSampleRate),
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

inline uint32_t IRAM_ATTR xorshift32() {
    static uint32_t x = 2463534242; // Seed
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

inline uint8_t IRAM_ATTR processSampleWithDitherInt(int16_t sampleIn) {
    int32_t scaled = (sampleIn * 127) / 128 + 32768;
    int32_t noise = (int32_t)(xorshift32() % 256) - 128;
    scaled += noise;
    if (scaled > 65535) scaled = 65535;
    if (scaled < 0) scaled = 0;
    return (uint8_t)(scaled >> 8);
}

void IRAM_ATTR Sound::output_internal_dac(int16_t* buffer, uint16_t samples) {
    if (!dac_handle) return;
    
    if (dac_oversampling) {
        if (!dac_buf || dac_buf_size < samples * 4) {
            if (dac_buf) heap_caps_free(dac_buf);
            dac_buf = (uint8_t*)heap_caps_malloc(samples * 4, MALLOC_CAP_DEFAULT);
            dac_buf_size = samples * 4;
        }
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
    } else {
        if (!dac_buf || dac_buf_size < samples * 2) {
            if (dac_buf) heap_caps_free(dac_buf);
            dac_buf = (uint8_t*)heap_caps_malloc(samples * 2, MALLOC_CAP_DEFAULT);
            dac_buf_size = samples * 2;
        }
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
    }
}

void IRAM_ATTR Sound::output_external_i2s(int16_t* buffer, uint16_t samples) {
    if (!i2s_tx_handle) return;
    
    size_t bytes_loaded = 0;
    i2s_channel_write(i2s_tx_handle, buffer, samples * 2 * sizeof(int16_t), &bytes_loaded, portMAX_DELAY);
}

void Sound::test_i2c() {
    if (!i2c_bus_handle) return;
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t i = 1; i < 127; i++) {
        esp_err_t ret = i2c_master_probe(i2c_bus_handle, i, -1);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02x", i);
        }
    }
    ESP_LOGI(TAG, "I2C scan complete.");
}

bool Sound::init_pcm5122(int sda_io_num, int scl_io_num) {
    i2c_master_bus_config_t i2c_mst_config = {};
    i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_mst_config.i2c_port = -1;
    i2c_mst_config.scl_io_num = static_cast<gpio_num_t>(scl_io_num);
    i2c_mst_config.sda_io_num = static_cast<gpio_num_t>(sda_io_num);
    i2c_mst_config.glitch_ignore_cnt = 7;
    i2c_mst_config.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&i2c_mst_config, &i2c_bus_handle) != ESP_OK) return false;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kPcm5122Addr;
    dev_cfg.scl_speed_hz = 100000;
    
    if (i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &pcm5122_handle) != ESP_OK) return false;

    test_i2c();

    // Initialize PCM5122
    if (pcm5122_write_reg(0, 0x00) != ESP_OK) return false;   // Page 0
    pcm5122_write_reg(2, 0x11);   // Standby
    pcm5122_write_reg(13, 0x10);  // PLL Reference = BCK
    pcm5122_write_reg(37, 0x08);  // Ignore SCK Missing error
    pcm5122_write_reg(2, 0x00);   // Exit Standby
    
    pcm5122_initialized = true;

    // Apply initial volume
    set_volume(master_volume);
    
    return true;
}

uint8_t Sound::get_loop_count() const {
    if (!xm_ctx) return 0;
    return xm_get_loop_count(xm_ctx);
}

void Sound::set_max_loop_count(uint8_t loopcnt) {
    if (!xm_ctx) return;
    xm_set_max_loop_count(xm_ctx, loopcnt);
}

float Sound::get_current_time() const {
    if (!xm_ctx) return 0.0f;
    uint32_t samples = 0;
    xm_get_position(xm_ctx, nullptr, nullptr, nullptr, &samples);
    return static_cast<float>(samples) / xm_get_sample_rate(xm_ctx);
}

void Sound::skip(float seconds) {
    if (!xm_ctx) return;
    uint8_t bpm = 125, tempo = 6;
    xm_get_playing_speed(xm_ctx, &bpm, &tempo);
    
    if (tempo == 0) tempo = 1; // Prevent division by zero
    
    // Ticks per second = BPM * 0.4
    // Rows per second = (BPM * 0.4) / tempo
    float rows_per_sec = (bpm * 0.4f) / tempo;
    uint16_t rows_to_skip = static_cast<uint16_t>(seconds * rows_per_sec);
    
    if (rows_to_skip > 0) {
        xm_skip_rows(xm_ctx, rows_to_skip);
    }
}

void Sound::set_volume(float vol) {
    master_volume = vol;
#ifndef ENABLE_SOFTWARE_VOLUME
    uint8_t hw_vol = 255;
    if (vol > 0.00001f) {
        float db = 20.0f * std::log10(vol);
        float reg_val = 48.0f - (db * 2.0f);
        if (reg_val < 0.0f) reg_val = 0.0f;
        if (reg_val > 255.0f) reg_val = 255.0f;
        hw_vol = static_cast<uint8_t>(reg_val);
    }
    ESP_LOGI(TAG, "Setting hardware volume to %d (from float %f, %.1fdB)", hw_vol, vol, 20.0f * std::log10(vol > 0.00001f ? vol : 0.00001f));
    set_pcm5122_volume(hw_vol, hw_vol);
#endif
}

void Sound::set_pcm5122_volume(uint8_t left, uint8_t right) {
    if (!pcm5122_initialized) return;
    pcm5122_write_reg(0, 0x00);
    pcm5122_write_reg(61, left);
    pcm5122_write_reg(62, right);
}

void Sound::set_pcm5122_mute(bool mute) {
    if (!pcm5122_initialized) return;
    pcm5122_write_reg(0, 0x00);
    if (mute) {
        pcm5122_write_reg(3, 0x11); // Mute left and right
    } else {
        pcm5122_write_reg(3, 0x00); // Unmute left and right
    }
}

void Sound::set_pcm5122_dsp_program(Pcm5122DspProgram dsp_program) {
    if (!pcm5122_initialized) return;
    pcm5122_write_reg(0, 0x00);
    pcm5122_write_reg(43, static_cast<uint8_t>(dsp_program));
}
