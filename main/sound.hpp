#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>
#include <xm.h>
#include "driver/gptimer.h"
#include "driver/dac_continuous.h"
#include "driver/i2s_std.h"

class Sound {
public:
    Sound();
    ~Sound();

    // Disable copy and move
    Sound(const Sound&) = delete;
    Sound& operator=(const Sound&) = delete;

    bool load(const uint8_t* data, size_t size);
    bool load_from_file(const char* filepath);
    void generate(float* output, uint16_t num_samples);
    void generate16(int16_t* output, uint16_t num_samples);
    void generate8(int8_t* output, uint16_t num_samples);

    void set_volume(float vol);
    float get_volume() const { return master_volume; }

    uint8_t get_loop_count() const;
    void set_max_loop_count(uint8_t loopcnt);

    float get_current_time() const;
    void skip(float seconds);

    void start_playing(uint16_t chunk_samples, std::function<void(int16_t*, uint16_t)> callback);
    void stop_playing();

    bool init_internal_dac();
    bool init_external_i2s(int bck_io_num, int ws_io_num, int data_out_num);
    void output_internal_dac(int16_t* buffer, uint16_t samples);
    void output_external_i2s(int16_t* buffer, uint16_t samples);

    bool init_pcm5122(int sda_io_num, int scl_io_num);
    void test_i2c();
    void set_pcm5122_volume(uint8_t left, uint8_t right);
    void set_pcm5122_mute(bool mute);

    enum class Pcm5122DspProgram : uint8_t {
        FirInterpolation = 1,
        LowLatencyIir = 2,
        HighAttenuationFir = 3,
        FixedProcessFlow = 5,
        RingingLessLowLatencyFir = 7
    };
    void set_pcm5122_dsp_program(Pcm5122DspProgram dsp_program);

    bool dac_oversampling = true;
    bool dac_dither = true;

private:
    xm_context_t* xm_ctx;
    char* xm_pool;

    std::function<void(int16_t*, uint16_t)> play_callback;
    uint16_t play_chunk_samples;
    void* play_task_handle; // Store as void* to avoid freertos headers in hpp if possible
    bool playing;
    gptimer_handle_t gptimer;

    dac_continuous_handle_t dac_handle;
    i2s_chan_handle_t i2s_tx_handle;
    bool pcm5122_initialized = false;

    float master_volume;

    float* temp_float_buf = nullptr;
    size_t temp_float_buf_samples = 0;
    
    uint8_t* dac_buf = nullptr;
    size_t dac_buf_size = 0;

    static void play_task(void* arg);
    static bool timer_isr_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx);
};
