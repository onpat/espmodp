#include "led.hpp"
#include "esp_log.h"

static const char* TAG = "LED";

Led& Led::get_instance() {
    static Led instance;
    return instance;
}

Led::Led() : gpio_num_((gpio_num_t)CONFIG_LED_GPIO), state_(State::OFF), 
             blink_once_requested_(false), blink_once_duration_ms_(0), task_handle_(NULL) {
}

Led::~Led() {
    if (task_handle_) {
        vTaskDelete(task_handle_);
    }
}

void Led::init() {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << gpio_num_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level(gpio_num_, 0);

    xTaskCreate(task_func, "led_task", 2048, this, 5, &task_handle_);
}

void Led::set_state(State state) {
    state_ = state;
    blink_once_requested_ = false; // Reset blink once if state is explicitly set
}

void Led::blink_once(uint32_t duration_ms) {
    blink_once_duration_ms_ = duration_ms;
    blink_once_requested_ = true;
}

void Led::task_func(void* param) {
    Led* self = (Led*)param;
    bool toggle = false;

    while (true) {
        if (self->blink_once_requested_) {
            uint32_t level = gpio_get_level(self->gpio_num_);
            gpio_set_level(self->gpio_num_, !level);
            vTaskDelay(pdMS_TO_TICKS(self->blink_once_duration_ms_));
            gpio_set_level(self->gpio_num_, level);
            self->blink_once_requested_ = false;
        } else {
            switch (self->state_) {
                case State::OFF:
                    gpio_set_level(self->gpio_num_, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    break;
                case State::ON:
                    gpio_set_level(self->gpio_num_, 1);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    break;
                case State::BLINK:
                    toggle = !toggle;
                    gpio_set_level(self->gpio_num_, toggle ? 1 : 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    break;
            }
        }
    }
}
