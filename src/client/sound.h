// Dum - tiny procedural sound effects (synthesized at startup, mixed with SDL audio).
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

enum SoundId {
    SND_SWING, SND_HIT, SND_MISS, SND_SHOOT, SND_CAST, SND_EXPLODE, SND_HURT, SND_DOOR,
    SND_PICKUP, SND_DIE, SND_LEVELUP, SND_GROWL, SND_STEP, SND_STAIRS, SND_CRIT, SND_HEAL,
    SND_COUNT
};

class Sound {
public:
    bool init();
    void shutdown();
    // Play with volume 0..1 and pan -1 (left) .. 1 (right).
    void play(SoundId id, float volume = 1.0f, float pan = 0.0f);
    void setMasterVolume(float v) { master_ = v; }
    float masterVolume() const { return master_; }
    // Load data/sfx/<name>.wav overrides (16-bit or float, any rate is converted).
    void loadOverrides(const std::string& dataDir);

private:
    struct Voice { int id; size_t pos; float l, r; };
    SDL_AudioDeviceID dev_ = 0;
    int rate_ = 44100;
    std::vector<float> samples_[SND_COUNT];
    std::vector<Voice> voices_;
    float master_ = 0.7f;
    static void callback(void* ud, Uint8* stream, int len);
    void synth();
};
