#pragma once

#include "sound.hpp"
#include "playlist.hpp"
#include "driver/gpio.h"
#include "sdkconfig.h"

class Input {
public:
    Input(Sound& sound, Playlist& playlist);
    void update();

private:
    Sound& sound_;
    Playlist& playlist_;

    static constexpr gpio_num_t kRedButtonGpio = (gpio_num_t)CONFIG_BUTTON_RED_GPIO;
    static constexpr gpio_num_t kBlackButtonGpio = (gpio_num_t)CONFIG_BUTTON_BLACK_GPIO;

    static constexpr bool kActiveLow = CONFIG_BUTTON_ACTIVE_LOW;
    static constexpr int64_t kHoldTimeUs = (int64_t)CONFIG_BUTTON_HOLD_TIME_MS * 1000;
    static constexpr int64_t kClickWindowUs = (int64_t)CONFIG_BUTTON_CLICK_WINDOW_MS * 1000;

    struct ButtonState {
        bool is_pressed = false;
        int64_t press_time = 0;
        int64_t last_click_time = 0;
        int click_count = 0;
        bool hold_triggered = false;
    };

    ButtonState red_state_;
    ButtonState black_state_;

    void handle_red_button();
    void handle_black_button();
    void cycle_volume();
};
