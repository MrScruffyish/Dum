// Dum - procedural sound effects.
#include "client/sound.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "common/common.h"

namespace {
const char* kSoundNames[SND_COUNT] = {"swing", "hit", "miss", "shoot", "cast", "explode", "hurt", "door",
                                      "pickup", "die", "levelup", "growl", "step", "stairs", "crit", "heal"};
}

bool Sound::init() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { std::printf("audio disabled: %s\n", SDL_GetError()); return false; }
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &Sound::callback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) { std::printf("audio disabled: %s\n", SDL_GetError()); return false; }
    rate_ = have.freq;
    synth();
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void Sound::shutdown() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

void Sound::play(SoundId id, float volume, float pan) {
    if (!dev_ || id < 0 || id >= SND_COUNT || samples_[id].empty()) return;
    volume *= master_;
    if (volume < 0.01f) return;
    pan = clampf(pan, -1, 1);
    SDL_LockAudioDevice(dev_);
    if (voices_.size() < 24) voices_.push_back({id, 0, volume * (1 - std::max(0.0f, pan) * 0.7f), volume * (1 + std::min(0.0f, pan) * 0.7f)});
    SDL_UnlockAudioDevice(dev_);
}

void Sound::callback(void* ud, Uint8* stream, int len) {
    Sound* s = (Sound*)ud;
    float* out = (float*)stream;
    int frames = len / (int)(sizeof(float) * 2);
    std::memset(stream, 0, len);
    for (auto& v : s->voices_) {
        const auto& smp = s->samples_[v.id];
        for (int i = 0; i < frames && v.pos < smp.size(); i++, v.pos++) {
            out[i * 2] += smp[v.pos] * v.l;
            out[i * 2 + 1] += smp[v.pos] * v.r;
        }
    }
    for (int i = 0; i < frames * 2; i++) out[i] = std::tanh(out[i]);  // soft clip
    s->voices_.erase(std::remove_if(s->voices_.begin(), s->voices_.end(), [s](const Voice& v) { return v.pos >= s->samples_[v.id].size(); }), s->voices_.end());
}

void Sound::synth() {
    Rng rng(1234);
    const float R = (float)rate_;
    auto make = [&](float secs) { return std::vector<float>((size_t)(secs * R)); };
    auto noise = [&]() { return rng.frand() * 2 - 1; };
    // Simple one-pole low-pass filtered noise helper.
    auto env = [](float t, float a, float d) { return t < a ? t / a : std::exp(-(t - a) / d); };

    {   // swing: filtered noise whoosh
        auto v = make(0.22f);
        float lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            float k = 0.05f + 0.25f * std::sin(t / 0.22f * PI);
            lp += (noise() - lp) * k;
            v[i] = lp * env(t, 0.05f, 0.06f) * 0.9f;
        }
        samples_[SND_SWING] = v;
    }
    {   // hit: thump + crunch
        auto v = make(0.25f);
        float lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            lp += (noise() - lp) * 0.3f;
            v[i] = (std::sin(2 * PI * (110 - 200 * t) * t) * 0.8f + lp * 0.6f) * env(t, 0.003f, 0.05f);
        }
        samples_[SND_HIT] = v;
    }
    {   // crit: brighter hit
        auto v = make(0.35f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            v[i] = (std::sin(2 * PI * (160 - 200 * t) * t) * 0.7f + noise() * 0.4f + std::sin(2 * PI * 880 * t) * 0.2f) * env(t, 0.002f, 0.08f);
        }
        samples_[SND_CRIT] = v;
    }
    {   // miss: short soft whoosh
        auto v = make(0.15f);
        float lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            lp += (noise() - lp) * 0.08f;
            v[i] = lp * env(t, 0.02f, 0.04f) * 0.6f;
        }
        samples_[SND_MISS] = v;
    }
    {   // shoot: bow string twang
        auto v = make(0.3f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            float f = 180 + 60 * std::exp(-t * 20);
            v[i] = (std::sin(2 * PI * f * t) > 0 ? 0.5f : -0.5f) * env(t, 0.001f, 0.06f) * 0.6f;
        }
        samples_[SND_SHOOT] = v;
    }
    {   // cast: shimmering rising tone
        auto v = make(0.5f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            float f = 400 + 900 * t;
            v[i] = (std::sin(2 * PI * f * t) * 0.5f + std::sin(2 * PI * f * 1.5f * t + std::sin(t * 60)) * 0.3f) * env(t, 0.05f, 0.15f) * 0.5f;
        }
        samples_[SND_CAST] = v;
    }
    {   // explode
        auto v = make(0.9f);
        float lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            lp += (noise() - lp) * (0.25f * std::exp(-t * 3) + 0.02f);
            v[i] = (lp * 1.6f + std::sin(2 * PI * 50 * t) * 0.5f) * env(t, 0.005f, 0.25f);
        }
        samples_[SND_EXPLODE] = v;
    }
    {   // hurt: low grunt
        auto v = make(0.25f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            float f = 140 - 60 * t;
            float saw = std::fmod(f * t, 1.0f) * 2 - 1;
            v[i] = saw * env(t, 0.01f, 0.07f) * 0.5f;
        }
        samples_[SND_HURT] = v;
    }
    {   // door: wooden creak + thud
        auto v = make(0.6f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            float f = 70 + 30 * std::sin(t * 9);
            float saw = std::fmod(f * t, 1.0f) * 2 - 1;
            v[i] = saw * 0.25f * (t < 0.45f ? 1.0f : 0.0f) * env(t, 0.05f, 0.3f) + (t > 0.45f ? std::sin(2 * PI * 60 * t) * env(t - 0.45f, 0.002f, 0.05f) * 0.6f : 0);
        }
        samples_[SND_DOOR] = v;
    }
    {   // pickup: two blips
        auto v = make(0.18f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            float f = t < 0.07f ? 660 : 990;
            v[i] = std::sin(2 * PI * f * t) * 0.35f * env(std::fmod(t, 0.07f), 0.003f, 0.04f);
        }
        samples_[SND_PICKUP] = v;
    }
    {   // monster dies: descending groan
        auto v = make(0.7f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            float f = 220 * std::exp(-t * 2.5f);
            float saw = std::fmod(f * t, 1.0f) * 2 - 1;
            v[i] = (saw * 0.4f + noise() * 0.15f) * env(t, 0.02f, 0.25f);
        }
        samples_[SND_DIE] = v;
    }
    {   // level up: arpeggio
        auto v = make(0.8f);
        const float notes[4] = {523, 659, 784, 1046};
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            int n = std::min(3, (int)(t / 0.12f));
            float lt = t - n * 0.12f;
            v[i] = (std::sin(2 * PI * notes[n] * t) * 0.3f + std::sin(2 * PI * notes[n] * 2 * t) * 0.1f) * env(lt, 0.005f, n == 3 ? 0.3f : 0.1f);
        }
        samples_[SND_LEVELUP] = v;
    }
    {   // growl: modulated low noise
        auto v = make(0.8f);
        float lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            lp += (noise() - lp) * 0.04f;
            float am = 0.6f + 0.4f * std::sin(2 * PI * 18 * t);
            v[i] = (lp * 2.5f * am + std::sin(2 * PI * 75 * t) * 0.2f * am) * env(t, 0.1f, 0.25f);
        }
        samples_[SND_GROWL] = v;
    }
    {   // footstep
        auto v = make(0.08f);
        float lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            lp += (noise() - lp) * 0.15f;
            v[i] = lp * env(t, 0.002f, 0.02f) * 0.35f;
        }
        samples_[SND_STEP] = v;
    }
    {   // stairs: descending steps
        auto v = make(1.0f);
        float lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            lp += (noise() - lp) * 0.12f;
            float lt = std::fmod(t, 0.2f);
            v[i] = lp * env(lt, 0.002f, 0.03f) * 0.6f * (1 - t * 0.6f);
        }
        samples_[SND_STAIRS] = v;
    }
    {   // heal: soft chime
        auto v = make(0.6f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = i / R;
            v[i] = (std::sin(2 * PI * 880 * t) + std::sin(2 * PI * 1320 * t) * 0.5f) * 0.2f * env(t, 0.02f, 0.2f);
        }
        samples_[SND_HEAL] = v;
    }
}

void Sound::loadOverrides(const std::string& dataDir) {
    if (!dev_) return;
    for (int i = 0; i < SND_COUNT; i++) {
        std::string path = dataDir + "/sfx/" + kSoundNames[i] + ".wav";
        SDL_AudioSpec spec;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV(path.c_str(), &spec, &buf, &len)) continue;
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_F32SYS, 1, rate_) >= 0) {
            cvt.len = (int)len;
            std::vector<Uint8> work(len * cvt.len_mult);
            std::memcpy(work.data(), buf, len);
            cvt.buf = work.data();
            if (SDL_ConvertAudio(&cvt) == 0) {
                size_t n = cvt.len_cvt / sizeof(float);
                SDL_LockAudioDevice(dev_);
                samples_[i].assign((float*)work.data(), (float*)work.data() + n);
                SDL_UnlockAudioDevice(dev_);
            }
        }
        SDL_FreeWAV(buf);
    }
}
