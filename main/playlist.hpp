#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "sound.hpp"

struct PlaylistItem {
    std::string filename;
    uint32_t playback_time;
};

class Playlist {
public:
    Playlist(Sound& sound);

    void rescan();
    void update();
    void play_next();
    void play(size_t index);
    void skip_current_m3u();
    void stop();
    void resume();

    void set_loop(bool loop);
    bool get_loop() const { return loop_; }
    
    int get_current_index() const { return current_index_; }
    int get_current_m3u_index() const { return current_m3u_index_; }
    bool is_playing() const { return is_playing_; }
    bool is_m3u_mode() const { return m3u_mode_; }

    const std::vector<PlaylistItem>& get_items() const { return items_; }
    const std::vector<PlaylistItem>& get_m3u_items() const { return m3u_items_; }

private:
    enum class M3uEntryKind {
        Track,
        Gain
    };

    struct M3uEntry {
        M3uEntryKind kind = M3uEntryKind::Track;
        std::string filename;
        float gain = 1.0f;
        int item_index = -1;
    };

    bool load_m3u(size_t index);
    bool play_next_m3u_track_from(size_t start_entry_index);
    bool play_m3u_entry(size_t entry_index);
    bool start_playing_path(const std::string& path);
    void apply_m3u_gains_until(size_t entry_index);
    void reset_m3u_playlist_state();

    Sound& sound_;
    std::vector<PlaylistItem> items_;
    std::vector<PlaylistItem> m3u_items_;
    std::vector<M3uEntry> current_m3u_entries_;
    std::vector<size_t> item_m3u_entry_indices_;
    int current_index_;
    int current_m3u_index_ = -1;
    int current_m3u_entry_index_ = -1;
    bool is_playing_;
    bool m3u_mode_ = false;
    bool loop_ = false;
    uint8_t last_loop_count_ = 0;
};
