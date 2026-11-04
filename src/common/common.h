// Dum - common math, RNG and small helpers.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

constexpr float PI = 3.14159265358979f;

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    float len() const { return std::sqrt(x * x + y * y); }
    float len2() const { return x * x + y * y; }
    Vec2 norm() const { float l = len(); return l > 1e-6f ? Vec2{x / l, y / l} : Vec2{0, 0}; }
};

inline float dist(Vec2 a, Vec2 b) { return (a - b).len(); }
inline Vec2 dirFromAngle(float a) { return {std::cos(a), std::sin(a)}; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Wrap angle to (-PI, PI].
inline float wrapAngle(float a) {
    while (a > PI) a -= 2 * PI;
    while (a <= -PI) a += 2 * PI;
    return a;
}

// Deterministic RNG (splitmix64 seeded xorshift64*). Same seed -> same sequence on all platforms.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 1) { reseed(seed); }
    void reseed(uint64_t seed) {
        uint64_t z = seed + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        s = z ^ (z >> 31);
        if (s == 0) s = 0x1234567ull;
    }
    uint32_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    // Inclusive range.
    int range(int lo, int hi) { return hi <= lo ? lo : lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
    float frand() { return (next() >> 8) * (1.0f / 16777216.0f); }
    bool chance(int pct) { return range(1, 100) <= pct; }
    int dice(int n, int sides) { int t = 0; for (int i = 0; i < n; i++) t += range(1, std::max(1, sides)); return t; }
};

struct Dice {
    int n = 0, s = 0;
    int roll(Rng& r) const { return r.dice(n, s); }
    int maxv() const { return n * s; }
    std::string str() const { return std::to_string(n) + "d" + std::to_string(s); }
};

// D&D style ability modifier.
inline int statMod(int s) { return (int)std::floor((s - 10) / 2.0f); }

// Pack 8-bit RGB into 0xAARRGGBB with full alpha.
inline uint32_t rgb(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)clampi(r, 0, 255) << 16) | ((uint32_t)clampi(g, 0, 255) << 8) | (uint32_t)clampi(b, 0, 255);
}
