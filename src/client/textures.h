// Dum - textures and procedurally generated art.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "common/common.h"

struct Texture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // 0xAARRGGBB, alpha 0 = transparent
    int top = 0;                // first non-empty row (for sprites)
    Texture() = default;
    Texture(int w_, int h_, uint32_t fill = 0) : w(w_), h(h_), px(w_ * h_, fill) {}
    uint32_t get(int x, int y) const { return px[(y & (h - 1)) * w + (x & (w - 1))]; }  // power-of-two wrap
    uint32_t at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : px[y * w + x]; }
    void set(int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = c; }
    void computeTop();
};

// Monster/player movement: a list of walk frames per creature. Generated ones (WALK_FRAMES)
// are made from the normal picture by bending it (legs swing, wings flap, bodies slither...),
// see walkFrame(); data/gfx/monster_<id>_walk1.png, _walk2.png... replace them (any number).
// Monsters also get death frames (_die1.png...; generated: the body collapses).
constexpr int WALK_FRAMES = 4, DIE_FRAMES = 4, MAX_ANIM_FRAMES = 16;
// PNG art bigger than this (either side) is scaled down when loaded.
constexpr int MAX_ART_SIZE = 256;
enum class AnimStyle { Legs, Skitter, Flap, Float, Slither, Squash };
AnimStyle animStyleFor(const std::string& sprite);
// True for creatures that move all the time (wings, hovering), not only while walking.
inline bool animAlways(AnimStyle s) { return s == AnimStyle::Flap || s == AnimStyle::Float; }
struct Texture;
Texture walkFrame(const Texture& base, AnimStyle style, int frame);
Texture dieFrame(const Texture& base, int frame);

enum ViewModel { VM_FIST, VM_DAGGER, VM_SWORD, VM_MACE, VM_AXE, VM_STAFF, VM_BOW, VM_SPEAR, VM_COUNT };
int viewModelFromName(const std::string& n);

struct Assets {
    Texture walls[8];
    Texture door, doorLocked, rubble;
    Texture floors[8];
    Texture shopDoors[8];
    Texture ceiling, ceilingCave, stairsDown, stairsUp, stairsUpCeil;
    std::vector<Texture> monsters, monstersAttack;  // per monster def
    std::vector<std::vector<Texture>> monstersWalk, monstersDie;   // per monster def: frames
    std::vector<std::vector<Texture>> playersWalk;                 // per class: frames
    std::vector<Texture> items;                     // per item def
    std::vector<Texture> players, playersAttack;    // per class
    std::vector<Texture> spellOrbs;                 // per spell def
    Texture arrow, corpse, blood;
    Texture viewmodels[VM_COUNT];
    Texture title;
    void build();
    // (Re)make the walk and death frames of every monster and player from their normal
    // pictures. keep: animations ("monster_orc_walk", ...) loaded from PNGs, left alone.
    void buildWalkFrames(const std::vector<std::string>& keep = {});
    // Replace generated art with PNGs from data/gfx/<name>.png if present.
    void loadOverrides(const std::string& dataDir);
    // Write all current art as PNGs (dum --export-art) so it can be edited.
    int exportPngs(const std::string& dir);
    std::vector<std::pair<std::string, Texture*>> namedTextures();
};

// Color helpers
inline uint32_t colMul(uint32_t c, float f) {
    int r = (int)(((c >> 16) & 255) * f), g = (int)(((c >> 8) & 255) * f), b = (int)((c & 255) * f);
    return (c & 0xFF000000u) | ((uint32_t)clampi(r, 0, 255) << 16) | ((uint32_t)clampi(g, 0, 255) << 8) | (uint32_t)clampi(b, 0, 255);
}
inline uint32_t colMix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int s) { return (int)(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t); };
    return 0xFF000000u | ((uint32_t)clampi(ch(16), 0, 255) << 16) | ((uint32_t)clampi(ch(8), 0, 255) << 8) | (uint32_t)clampi(ch(0), 0, 255);
}
