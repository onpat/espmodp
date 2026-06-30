#include "input.hpp"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "Input";

Input::Input(Sound& sound, Playlist& playlist) : sound_(sound), playlist_(playlist) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << kRedButtonGpio) | (1ULL << kBlackButtonGpio);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
}

void Input::update() {
    handle_red_button();
    handle_black_button();
}

void Input::handle_red_button() {
    int level = gpio_get_level(kRedButtonGpio);
    int64_t now = esp_timer_get_time();

    bool pressed = kActiveLow ? (level == 0) : (level == 1);

    if (pressed) {
        if (!red_state_.is_pressed) {
            red_state_.is_pressed = true;
            red_state_.press_time = now;
            red_state_.hold_triggered = false;
        } else if (!red_state_.hold_triggered && (now - red_state_.press_time) > kHoldTimeUs) {
            // Hold detected
            cycle_volume();
            red_state_.hold_triggered = true;
            red_state_.click_count = 0; // Cancel any pending clicks
        }
    } else {
        if (red_state_.is_pressed) {
            int64_t duration = now - red_state_.press_time;
            if (!red_state_.hold_triggered && duration < kHoldTimeUs) {
                red_state_.click_count++;
                red_state_.last_click_time = now;
                
                if (red_state_.click_count == 2) {
                    // Double click action
                    sound_.skip(3.0f);
                    ESP_LOGI(TAG, "Red Button: Double Click (Skip 3s)");
                    red_state_.click_count = 0;
                }
            }
            red_state_.is_pressed = false;
        }

        // Check for single/double click timeout
        if (red_state_.click_count == 1 && (now - red_state_.last_click_time) > kClickWindowUs) {
            if (playlist_.is_playing()) {
                playlist_.stop();
                ESP_LOGI(TAG, "Red Button: Single Click (Pause)");
            } else {
                playlist_.resume();
                ESP_LOGI(TAG, "Red Button: Single Click (Resume)");
            }
            red_state_.click_count = 0;
        }
    }
}

void Input::handle_black_button() {
    int level = gpio_get_level(kBlackButtonGpio);
    int64_t now = esp_timer_get_time();

    bool pressed = kActiveLow ? (level == 0) : (level == 1);

    if (pressed) {
        if (!black_state_.is_pressed) {
            black_state_.is_pressed = true;
            black_state_.press_time = now;
            black_state_.hold_triggered = false;
        } else if (!black_state_.hold_triggered && (now - black_state_.press_time) > kHoldTimeUs) {
            bool loop = !playlist_.get_loop();
            playlist_.set_loop(loop);
            ESP_LOGI(TAG, "Black Button: Long Press (Loop %s)", loop ? "enabled" : "disabled");
            black_state_.hold_triggered = true;
            black_state_.click_count = 0; // Cancel any pending clicks
        }
    } else {
        if (black_state_.is_pressed) {
            if (!black_state_.hold_triggered) {
                if (playlist_.is_m3u_mode()) {
                    black_state_.click_count++;
                    black_state_.last_click_time = now;

                    if (black_state_.click_count == 2) {
                        playlist_.skip_current_m3u();
                        ESP_LOGI(TAG, "Black Button: Double Click (Next m3u)");
                        black_state_.click_count = 0;
                    }
                } else {
                    playlist_.play_next();
                    ESP_LOGI(TAG, "Black Button: Click (Next song)");
                    black_state_.click_count = 0;
                }
            }
            black_state_.is_pressed = false;
        }

        if (playlist_.is_m3u_mode() &&
            black_state_.click_count == 1 &&
            (now - black_state_.last_click_time) > kClickWindowUs) {
            playlist_.play_next();
            ESP_LOGI(TAG, "Black Button: Single Click (Next song)");
            black_state_.click_count = 0;
        }
    }
}

void Input::cycle_volume() {
    float current = sound_.get_volume();
    float next;
    
    // Cycle: 0.1 -> 0.3 -> 0.6 -> 1.0 -> 0.1
    if (current < 0.2f) {
        next = 0.3f;
    } else if (current < 0.45f) { // midpoint between 0.3 and 0.6
        next = 0.6f;
    } else if (current < 0.8f) { // midpoint between 0.6 and 1.0
        next = 1.0f;
    } else {
        next = 0.1f;
    }
    
    sound_.set_volume(next);
    ESP_LOGI(TAG, "Volume changed to %.1f", next);
}
