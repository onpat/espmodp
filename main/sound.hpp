#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
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
    void generate(float* output, uint16_t num_samples);
    void generate16(int16_t* output, uint16_t num_samples);
    void generate8(int8_t* output, uint16_t num_samples);

    void set_volume(float vol) { master_volume = vol; }
    float get_volume() const { return master_volume; }

    void start_playing(uint16_t chunk_samples, std::function<void(int16_t*, uint16_t)> callback);
    void stop_playing();

    bool init_internal_dac();
    bool init_external_i2s(int bck_io_num, int ws_io_num, int data_out_num);
    void output_internal_dac(int16_t* buffer, uint16_t samples);
    void output_external_i2s(int16_t* buffer, uint16_t samples);

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

    float master_volume;

    static void play_task(void* arg);
    static bool timer_isr_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx);
};
