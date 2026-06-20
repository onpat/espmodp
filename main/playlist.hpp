#pragma once

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
    void stop();
    void resume();

    void set_loop(bool loop);
    bool get_loop() const { return loop_; }
    
    int get_current_index() const { return current_index_; }
    bool is_playing() const { return is_playing_; }

    const std::vector<PlaylistItem>& get_items() const { return items_; }

private:
    Sound& sound_;
    std::vector<PlaylistItem> items_;
    int current_index_;
    bool is_playing_;
    bool loop_ = false;
    uint8_t last_loop_count_ = 0;
};
