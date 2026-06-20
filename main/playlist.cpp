#include "playlist.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include <algorithm>

static const char* TAG = "Playlist";

Playlist::Playlist(Sound& sound) : sound_(sound), current_index_(-1), is_playing_(false) {
}

void Playlist::rescan() {
    DIR* dir = opendir("/lfs");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory /lfs");
        return;
    }

    items_.clear();
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string filename = entry->d_name;
        
        bool is_supported = false;
        if (filename.length() >= 3) {
            std::string ext3 = filename.substr(filename.length() - 3);
            std::transform(ext3.begin(), ext3.end(), ext3.begin(), ::tolower);
            if (ext3 == ".xm") is_supported = true;
        }
        if (filename.length() >= 4) {
            std::string ext4 = filename.substr(filename.length() - 4);
            std::transform(ext4.begin(), ext4.end(), ext4.begin(), ::tolower);
            if (ext4 == ".mod" || ext4 == ".s3m") is_supported = true;
        }

        if (is_supported) {
            PlaylistItem item;
            item.filename = filename;
            item.playback_time = 0; // Not easily computable without parsing the entire file
            items_.push_back(item);

            if (items_.size() >= 32) {
                break;
            }
        }
    }
    closedir(dir);

    // Sort by filename
    std::sort(items_.begin(), items_.end(), [](const PlaylistItem& a, const PlaylistItem& b) {
        return a.filename < b.filename;
    });

    ESP_LOGI(TAG, "Scanned %d files", (int)items_.size());

    // If currently playing file was removed, or if we were playing and list is empty
    if (is_playing_) {
        // We could try to keep playing the same file if it's still there
        // For simplicity, if we rescan, we just keep current_index_ valid if possible
        if (items_.empty()) {
            stop();
        } else if (current_index_ >= (int)items_.size()) {
            current_index_ = 0;
            play(current_index_);
        }
    }
}

void Playlist::update() {
    if (is_playing_ && !items_.empty()) {
        uint8_t current_loops = sound_.get_loop_count();
        if (current_loops > last_loop_count_) {
            last_loop_count_ = current_loops;
            if (!loop_) {
                ESP_LOGI(TAG, "Track looped, playing next song.");
                play_next();
            } else {
                ESP_LOGI(TAG, "Track looped, loop is enabled.");
            }
        }
    }
}

void Playlist::play_next() {
    if (items_.empty()) return;

    current_index_++;
    if (current_index_ >= (int)items_.size()) {
        current_index_ = 0; // Play first song if reached end
    }
    play(current_index_);
}

void Playlist::play(size_t index) {
    if (index >= items_.size()) return;

    is_playing_ = false; // Prevent update() from accessing sound_ while loading
    current_index_ = index;
    std::string path = "/lfs/" + items_[current_index_].filename;
    
    ESP_LOGI(TAG, "Playlist::play(%zu) - File: %s", index, path.c_str());

    sound_.stop_playing();
    if (sound_.load_from_file(path.c_str())) {
        sound_.set_max_loop_count(0); // 0 means loop indefinitely, which allows get_loop_count() to increment
        last_loop_count_ = 0;
        const uint16_t chunk_samples = 1024;
        sound_.start_playing(chunk_samples, [this](int16_t* buffer, uint16_t samples) {
            sound_.output_external_i2s(buffer, samples);
        });
        is_playing_ = true;
        ESP_LOGI(TAG, "Playing %s", path.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to play %s", path.c_str());
        // Try next song to avoid getting stuck?
        // play_next();
    }
}

void Playlist::stop() {
    ESP_LOGI(TAG, "Playlist::stop() (Pause)");
    sound_.stop_playing();
    is_playing_ = false;
}

void Playlist::resume() {
    ESP_LOGI(TAG, "Playlist::resume()");
    if (!is_playing_ && current_index_ >= 0 && current_index_ < (int)items_.size()) {
        const uint16_t chunk_samples = 1024;
        sound_.start_playing(chunk_samples, [this](int16_t* buffer, uint16_t samples) {
            sound_.output_external_i2s(buffer, samples);
        });
        is_playing_ = true;
    }
}

void Playlist::set_loop(bool loop) {
    ESP_LOGI(TAG, "Playlist::set_loop(%d)", loop ? 1 : 0);
    loop_ = loop;
}
