#include "playlist.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include "led.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef CONFIG_STORAGE_SDCARD
#define MOUNT_POINT "/sdcard"
#define MOUNT_POINT_PREFIX "/sdcard/"
#else
#define MOUNT_POINT "/lfs"
#define MOUNT_POINT_PREFIX "/lfs/"
#endif

static const char* TAG = "Playlist";

namespace {
constexpr size_t kMaxPlaylistItems = 32;

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool has_extension(const std::string& filename, const char* extension) {
    const size_t ext_len = std::strlen(extension);
    if (filename.length() < ext_len) {
        return false;
    }

    std::string suffix = filename.substr(filename.length() - ext_len);
    return to_lower(suffix) == extension;
}

bool is_supported_module_file(const std::string& filename) {
    return has_extension(filename, ".xm") ||
           has_extension(filename, ".mod") ||
           has_extension(filename, ".s3m");
}

bool is_m3u_file(const std::string& filename) {
    return has_extension(filename, ".m3u");
}

void sort_items_by_filename(std::vector<PlaylistItem>& items) {
    std::sort(items.begin(), items.end(), [](const PlaylistItem& a, const PlaylistItem& b) {
        return a.filename < b.filename;
    });
}

std::string trim(std::string value) {
    const char* whitespace = " \t\r\n";
    size_t start = value.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }

    size_t end = value.find_last_not_of(whitespace);
    return value.substr(start, end - start + 1);
}

bool starts_with_case_insensitive(const std::string& value, const char* prefix) {
    const size_t prefix_len = std::strlen(prefix);
    if (value.length() < prefix_len) {
        return false;
    }

    return to_lower(value.substr(0, prefix_len)) == to_lower(prefix);
}

bool parse_vlc_gain(const std::string& line, float& gain) {
    constexpr const char* kPrefix = "#EXTVLCOPT:";
    if (!starts_with_case_insensitive(line, kPrefix)) {
        return false;
    }

    std::string option = trim(line.substr(std::strlen(kPrefix)));
    size_t separator = option.find('=');
    if (separator == std::string::npos) {
        return false;
    }

    std::string key = to_lower(trim(option.substr(0, separator)));
    if (key != "gain") {
        return false;
    }

    std::string value = trim(option.substr(separator + 1));
    errno = 0;
    char* end = nullptr;
    float parsed = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }

    if (parsed < 0.0f) {
        parsed = 0.0f;
    } else if (parsed > 1.0f) {
        parsed = 1.0f;
    }

    gain = parsed;
    return true;
}

std::string normalize_relative_path(std::string path) {
    path = trim(path);
    std::replace(path.begin(), path.end(), '\\', '/');

    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }

    return path;
}

bool is_safe_relative_path(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find(':') != std::string::npos) {
        return false;
    }

    size_t start = 0;
    while (start <= path.length()) {
        size_t end = path.find('/', start);
        std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "..") {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return true;
}
} // namespace

Playlist::Playlist(Sound& sound) : sound_(sound), current_index_(-1), is_playing_(false) {
}

void Playlist::rescan() {
    const bool was_playing = is_playing_;
    const bool was_m3u_mode = m3u_mode_;

    DIR* dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s", MOUNT_POINT);
        return;
    }

    items_.clear();
    m3u_items_.clear();
    current_m3u_entries_.clear();
    item_m3u_entry_indices_.clear();

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string filename = entry->d_name;
        if (is_m3u_file(filename)) {
            PlaylistItem item;
            item.filename = filename;
            item.playback_time = 0;
            m3u_items_.push_back(item);

            if (m3u_items_.size() >= kMaxPlaylistItems) {
                break;
            }
        }
    }
    closedir(dir);

    sort_items_by_filename(m3u_items_);

    if (!m3u_items_.empty()) {
        m3u_mode_ = true;
        current_index_ = -1;
        current_m3u_index_ = -1;
        current_m3u_entry_index_ = -1;
        ESP_LOGI(TAG, "Scanned %d m3u files", (int)m3u_items_.size());

        if (was_playing) {
            sound_.stop_playing();
            is_playing_ = false;
            play_next();
        }
        return;
    }

    m3u_mode_ = false;
    reset_m3u_playlist_state();

    dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to reopen directory %s", MOUNT_POINT);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        std::string filename = entry->d_name;
        if (is_supported_module_file(filename)) {
            PlaylistItem item;
            item.filename = filename;
            item.playback_time = 0; // Not easily computable without parsing the entire file
            items_.push_back(item);

            if (items_.size() >= kMaxPlaylistItems) {
                break;
            }
        }
    }
    closedir(dir);

    sort_items_by_filename(items_);

    ESP_LOGI(TAG, "Scanned %d files", (int)items_.size());

    // If currently playing file was removed, or if we were playing and list is empty
    if (was_playing) {
        // We could try to keep playing the same file if it's still there
        // For simplicity, if we rescan, we just keep current_index_ valid if possible
        if (items_.empty()) {
            stop();
        } else if (was_m3u_mode || current_index_ < 0 || current_index_ >= (int)items_.size()) {
            play(0);
        }
    }
}

void Playlist::update() {
    if (is_playing_ && (m3u_mode_ || !items_.empty())) {
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
    if (m3u_mode_) {
        if (m3u_items_.empty()) return;

        Led::get_instance().blink_once(500);
        size_t next_entry_index = 0;
        if (current_m3u_entry_index_ >= 0) {
            next_entry_index = static_cast<size_t>(current_m3u_entry_index_) + 1;
        }
        play_next_m3u_track_from(next_entry_index);
        return;
    }

    if (items_.empty()) return;

    Led::get_instance().blink_once(500);

    current_index_++;
    if (current_index_ >= (int)items_.size()) {
        current_index_ = 0; // Play first song if reached end
    }
    play(current_index_);
}

void Playlist::play(size_t index) {
    if (index >= items_.size()) return;

    if (m3u_mode_) {
        if (index >= item_m3u_entry_indices_.size()) return;

        size_t entry_index = item_m3u_entry_indices_[index];
        apply_m3u_gains_until(entry_index);
        if (!play_m3u_entry(entry_index)) {
            play_next_m3u_track_from(entry_index + 1);
        }
        return;
    }

    current_index_ = index;
    std::string path = std::string(MOUNT_POINT_PREFIX) + items_[current_index_].filename;

    if (!start_playing_path(path)) {
        ESP_LOGE(TAG, "Failed to play %s", path.c_str());
        Led::get_instance().set_blink();
        // Try next song to avoid getting stuck?
        // play_next();
    }
}

void Playlist::skip_current_m3u() {
    if (!m3u_mode_) {
        play_next();
        return;
    }

    if (m3u_items_.empty()) {
        return;
    }

    Led::get_instance().blink_once(500);
    size_t next_m3u_index = 0;
    if (current_m3u_index_ >= 0) {
        next_m3u_index = (static_cast<size_t>(current_m3u_index_) + 1) % m3u_items_.size();
    }

    load_m3u(next_m3u_index);
    play_next_m3u_track_from(0);
}

void Playlist::stop() {
    ESP_LOGI(TAG, "Playlist::stop() (Pause)");
    sound_.stop_playing();
    is_playing_ = false;
    Led::get_instance().set_off();
}

void Playlist::resume() {
    ESP_LOGI(TAG, "Playlist::resume()");
    if (!is_playing_ && current_index_ >= 0 && current_index_ < (int)items_.size()) {
        const uint16_t chunk_samples = 1024;
        sound_.start_playing(chunk_samples, [this](int16_t* buffer, uint16_t samples) {
            sound_.output_external_i2s(buffer, samples);
        });
        is_playing_ = true;
        Led::get_instance().set_on();
    }
}

void Playlist::set_loop(bool loop) {
    ESP_LOGI(TAG, "Playlist::set_loop(%d)", loop ? 1 : 0);
    loop_ = loop;
}

bool Playlist::load_m3u(size_t index) {
    if (index >= m3u_items_.size()) {
        return false;
    }

    items_.clear();
    current_m3u_entries_.clear();
    item_m3u_entry_indices_.clear();
    current_index_ = -1;
    current_m3u_index_ = static_cast<int>(index);
    current_m3u_entry_index_ = -1;

    std::string path = std::string(MOUNT_POINT_PREFIX) + m3u_items_[index].filename;
    ESP_LOGI(TAG, "Loading m3u: %s", path.c_str());

    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open m3u: %s", path.c_str());
        return false;
    }

    char line[512];
    int line_number = 0;
    while (std::fgets(line, sizeof(line), file)) {
        line_number++;
        std::string text = trim(line);
        if (text.empty()) {
            continue;
        }

        if (text[0] == '#') {
            float gain = 1.0f;
            if (parse_vlc_gain(text, gain)) {
                M3uEntry entry;
                entry.kind = M3uEntryKind::Gain;
                entry.gain = gain;
                current_m3u_entries_.push_back(entry);
                ESP_LOGI(TAG, "m3u gain %.3f at %s:%d", gain, m3u_items_[index].filename.c_str(), line_number);
            } else if (starts_with_case_insensitive(text, "#EXTVLCOPT:")) {
                ESP_LOGW(TAG, "Ignoring unsupported VLC option at %s:%d: %s",
                         m3u_items_[index].filename.c_str(), line_number, text.c_str());
            }
            continue;
        }

        std::string filename = normalize_relative_path(text);
        if (!is_safe_relative_path(filename)) {
            ESP_LOGW(TAG, "Ignoring unsafe m3u path at %s:%d: %s",
                     m3u_items_[index].filename.c_str(), line_number, text.c_str());
            continue;
        }

        if (!is_supported_module_file(filename)) {
            ESP_LOGW(TAG, "Ignoring unsupported m3u track at %s:%d: %s",
                     m3u_items_[index].filename.c_str(), line_number, filename.c_str());
            continue;
        }

        M3uEntry entry;
        entry.kind = M3uEntryKind::Track;
        entry.filename = filename;
        entry.item_index = static_cast<int>(items_.size());

        size_t entry_index = current_m3u_entries_.size();
        current_m3u_entries_.push_back(entry);
        item_m3u_entry_indices_.push_back(entry_index);

        PlaylistItem item;
        item.filename = filename;
        item.playback_time = 0;
        items_.push_back(item);
    }

    std::fclose(file);

    ESP_LOGI(TAG, "Loaded m3u %s: %d entries, %d tracks",
             m3u_items_[index].filename.c_str(),
             (int)current_m3u_entries_.size(),
             (int)items_.size());
    return true;
}

bool Playlist::play_next_m3u_track_from(size_t start_entry_index) {
    if (m3u_items_.empty()) {
        return false;
    }

    if (current_m3u_index_ < 0 || current_m3u_index_ >= (int)m3u_items_.size()) {
        load_m3u(0);
        start_entry_index = 0;
    }

    size_t checked_m3u_count = 0;
    size_t entry_index = start_entry_index;
    while (checked_m3u_count < m3u_items_.size()) {
        while (entry_index < current_m3u_entries_.size()) {
            M3uEntry& entry = current_m3u_entries_[entry_index];
            if (entry.kind == M3uEntryKind::Gain) {
                sound_.set_volume(entry.gain);
                current_m3u_entry_index_ = static_cast<int>(entry_index);
                ESP_LOGI(TAG, "Applied m3u gain %.3f", entry.gain);
                entry_index++;
                continue;
            }

            if (entry.kind == M3uEntryKind::Track) {
                if (play_m3u_entry(entry_index)) {
                    return true;
                }

                entry_index++;
                continue;
            }

            entry_index++;
        }

        checked_m3u_count++;
        size_t next_m3u_index = 0;
        if (current_m3u_index_ >= 0) {
            next_m3u_index = (static_cast<size_t>(current_m3u_index_) + 1) % m3u_items_.size();
        }
        load_m3u(next_m3u_index);
        entry_index = 0;
    }

    ESP_LOGE(TAG, "No playable tracks found in m3u list");
    stop();
    Led::get_instance().set_blink();
    return false;
}

bool Playlist::play_m3u_entry(size_t entry_index) {
    if (entry_index >= current_m3u_entries_.size()) {
        return false;
    }

    M3uEntry& entry = current_m3u_entries_[entry_index];
    if (entry.kind != M3uEntryKind::Track || entry.item_index < 0 || entry.item_index >= (int)items_.size()) {
        return false;
    }

    current_m3u_entry_index_ = static_cast<int>(entry_index);
    current_index_ = entry.item_index;

    std::string path = std::string(MOUNT_POINT_PREFIX) + entry.filename;
    if (start_playing_path(path)) {
        return true;
    }

    ESP_LOGE(TAG, "Failed to play m3u track %s", path.c_str());
    return false;
}

bool Playlist::start_playing_path(const std::string& path) {
    is_playing_ = false; // Prevent update() from accessing sound_ while loading
    ESP_LOGI(TAG, "Playlist::play - File: %s", path.c_str());

    Led::get_instance().set_off();
    sound_.stop_playing();
    if (!sound_.load_from_file(path.c_str())) {
        return false;
    }

    Led::get_instance().set_on();
    sound_.set_max_loop_count(0); // 0 means loop indefinitely, which allows get_loop_count() to increment
    last_loop_count_ = 0;
    const uint16_t chunk_samples = 1024;
    sound_.start_playing(chunk_samples, [this](int16_t* buffer, uint16_t samples) {
        sound_.output_external_i2s(buffer, samples);
    });
    is_playing_ = true;
    ESP_LOGI(TAG, "Playing %s", path.c_str());
    return true;
}

void Playlist::apply_m3u_gains_until(size_t entry_index) {
    for (size_t i = 0; i < entry_index && i < current_m3u_entries_.size(); ++i) {
        if (current_m3u_entries_[i].kind == M3uEntryKind::Gain) {
            sound_.set_volume(current_m3u_entries_[i].gain);
            ESP_LOGI(TAG, "Applied m3u gain %.3f", current_m3u_entries_[i].gain);
        }
    }
}

void Playlist::reset_m3u_playlist_state() {
    m3u_items_.clear();
    current_m3u_entries_.clear();
    item_m3u_entry_indices_.clear();
    current_m3u_index_ = -1;
    current_m3u_entry_index_ = -1;
}
