#pragma once

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

class Led {
public:
    enum class State {
        OFF,
        ON,
        BLINK,
    };

    static Led& get_instance();

    void init();
    void set_state(State state);
    void set_on() { set_state(State::ON); }
    void set_off() { set_state(State::OFF); }
    void set_blink() { set_state(State::BLINK); }
    
    // Briefly blink (off-on or on-off depending on current state)
    void blink_once(uint32_t duration_ms = 500);

private:
    Led();
    ~Led();
    Led(const Led&) = delete;
    Led& operator=(const Led&) = delete;

    static void task_func(void* param);
    
    gpio_num_t gpio_num_;
    State state_;
    bool blink_once_requested_;
    uint32_t blink_once_duration_ms_;
    TaskHandle_t task_handle_;
};
