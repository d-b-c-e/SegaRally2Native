// Music for the menus: your own third_party/menu/music.mp3 (or .wav) if there
// is one, else a tune of the game (g_menu_music_path, decoded from the music
// board's ROM at launch: Dsb2::extract_track), looped while a menu is up over the backdrop (not over a paused
// game) and faded out when the game starts.
// The file is decoded into memory once and mixed into the game's own audio
// output (AudioOut calls mix()), so it plays wherever the game has sound and
// needs nothing from Windows. Portable C++: tests/menu_music_test.cpp.
#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

extern std::string g_menu_music_path;   // set by app.cpp before the menu opens

class MenuMusic {
public:
    bool open();    // false: there is no music file
    // One file: 16-bit PCM WAV or MP3, by its content. False if it is neither
    bool load(const char* path);
    void close();
    // Every frame: whether the music should be heard, at what volume (0..10, the
    // game's), and the seconds since the last call
    void update(bool want, int volume, double dt);

    // Audio thread: adds the music to `frames` frames of `channels` floats at
    // `rate` Hz (the first two channels are the front pair). Nothing is added
    // until a file is loaded, or while the music is faded out
    void mix(float* out, size_t frames, int channels, int rate);

    int rate() const { return rate_; }
    size_t length() const { return pcm_.size() / 2; }   // frames

private:
    // Written before ready_ is set, only read afterwards
    std::vector<float> pcm_;   // stereo, full scale = 1
    int rate_ = 0;
    std::atomic<bool> ready_{false};

    // Main thread to audio thread
    std::atomic<bool> want_{false};
    std::atomic<float> volume_{0};   // 0..1, the gain for the game's volume

    // Audio thread
    double pos_ = 0;    // frames into pcm_
    float level_ = 0;   // the fade, 0..1
};
