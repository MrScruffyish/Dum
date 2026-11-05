// Dum - procedural art: wall/floor textures, monster/item sprites and first-person weapons.
// Everything here is generated from code so the game needs no external art.
// Any picture can be replaced by a PNG in data/gfx/ (see Assets::loadOverrides).
#include "client/textures.h"

#include <SDL.h>
#include <SDL_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "game/data.h"
#include "game/map.h"

namespace {

// ------------------------------------------------------------------ noise --
uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float h2(int x, int y, int seed) {
    return (hash32((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)seed * 83492791u) & 0xFFFF) / 65535.0f;
}
float vnoise(float x, float y, int period, int seed) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    auto c = [&](int a, int b) { return h2(((a % period) + period) % period, ((b % period) + period) % period, seed); };
    float a = c(xi, yi), b = c(xi + 1, yi), cc = c(xi, yi + 1), d = c(xi + 1, yi + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (cc * (1 - fx) + d * fx) * fy;
}
// Tileable fractal noise over a 64 pixel texture.
float fbm(int x, int y, int basePeriod, int seed, int oct = 4, int size = 64) {
    float sum = 0, amp = 1, norm = 0;
    int period = basePeriod;
    for (int o = 0; o < oct; o++) {
        sum += vnoise(x * (float)period / size, y * (float)period / size, period, seed + o * 31) * amp;
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / norm;
}

uint32_t vary(uint32_t c, float f) { return colMul(c, f); }

// -------------------------------------------------------------- drawing --
void fillRect(Texture& t, int x0, int y0, int x1, int y1, uint32_t c) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) t.set(x, y, c);
}
void fillEllipse(Texture& t, float cx, float cy, float rx, float ry, uint32_t c) {
    for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++)
        for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) t.set(x, y, c);
        }
}
void fillCircle(Texture& t, float cx, float cy, float r, uint32_t c) { fillEllipse(t, cx, cy, r, r, c); }
void line(Texture& t, float x0, float y0, float x1, float y1, float w, uint32_t c) {
    float len = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
    int steps = (int)len + 1;
    for (int i = 0; i <= steps; i++) {
        float k = (float)i / steps;
        fillCircle(t, x0 + (x1 - x0) * k, y0 + (y1 - y0) * k, w * 0.5f, c);
    }
}
void fillTri(Texture& t, Vec2 a, Vec2 b, Vec2 c, uint32_t col) {
    int minx = (int)std::min({a.x, b.x, c.x}), maxx = (int)std::max({a.x, b.x, c.x}) + 1;
    int miny = (int)std::min({a.y, b.y, c.y}), maxy = (int)std::max({a.y, b.y, c.y}) + 1;
    auto edge = [](Vec2 p, Vec2 q, Vec2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
    float area = edge(a, b, c);
    if (std::fabs(area) < 1e-3f) return;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            Vec2 p(x + 0.5f, y + 0.5f);
            float w0 = edge(b, c, p) / area, w1 = edge(c, a, p) / area, w2 = edge(a, b, p) / area;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) t.set(x, y, col);
        }
}
void fillQuad(Texture& t, Vec2 a, Vec2 b, Vec2 c, Vec2 d, uint32_t col) {
    fillTri(t, a, b, c, col);
    fillTri(t, a, c, d, col);
}
// Dark outline around opaque pixels gives a crisp pixel-art look.
void outline(Texture& t, uint32_t col = 0xFF100808) {
    Texture o = t;
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            if (t.at(x, y) >> 24) continue;
            if ((t.at(x - 1, y) >> 24) || (t.at(x + 1, y) >> 24) || (t.at(x, y - 1) >> 24) || (t.at(x, y + 1) >> 24)) o.set(x, y, col);
        }
    t = o;
}
// Light from the top-left, plus grain.
void shade(Texture& t, int seed) {
    int top = t.h, bot = 0;
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++)
            if (t.at(x, y) >> 24) { top = std::min(top, y); bot = std::max(bot, y); }
    float span = std::max(1, bot - top);
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            uint32_t c = t.at(x, y);
            if (!(c >> 24)) continue;
            float f = 1.18f - 0.38f * (y - top) / span;
            // edge darkening (pixels next to transparency)
            bool edgeR = !(t.at(x + 1, y) >> 24), edgeB = !(t.at(x, y + 1) >> 24);
            bool edgeL = !(t.at(x - 1, y) >> 24), edgeT = !(t.at(x, y - 1) >> 24);
            if (edgeR || edgeB) f *= 0.72f;
            else if (edgeL || edgeT) f *= 1.12f;
            f *= 0.93f + 0.14f * h2(x, y, seed);
            t.set(x, y, colMul(c, f));
        }
}

// ---------------------------------------------------------------- walls ---
Texture brickWall(uint32_t brick, uint32_t mortar, int seed, int bw = 16, int bh = 8) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int row = y / bh;
            int off = (row & 1) ? bw / 2 : 0;
            int bx = (x + off) / bw;
            bool m = (y % bh == 0) || ((x + off) % bw == 0);
            float n = fbm(x, y, 8, seed, 3);
            uint32_t c;
            if (m) {
                c = vary(mortar, 0.8f + 0.4f * n);
            } else {
                float v = 0.82f + 0.3f * h2(bx, row, seed);
                c = vary(brick, v * (0.85f + 0.3f * n));
                if (y % bh == 1 || (x + off) % bw == 1) c = colMul(c, 1.18f);
                if (y % bh == bh - 1 || (x + off) % bw == bw - 1) c = colMul(c, 0.75f);
            }
            t.set(x, y, c);
        }
    return t;
}

Texture caveWall(uint32_t base, int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float n = fbm(x, y, 4, seed, 5);
            float cr = fbm(x, y, 8, seed + 99, 2);
            float f = 0.55f + 0.8f * n;
            if (std::fabs(cr - 0.5f) < 0.025f) f *= 0.45f;  // cracks
            t.set(x, y, vary(base, f));
        }
    return t;
}

Texture woodWall(uint32_t base, int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int plank = x / 16;
            float grain = std::sin((y * 0.35f + fbm(x, y, 4, seed + plank, 2) * 8.0f + plank * 2.1f)) * 0.5f + 0.5f;
            float f = 0.75f + 0.25f * grain + 0.15f * (h2(plank, 0, seed) - 0.5f);
            if (x % 16 == 0) f *= 0.45f;
            if (x % 16 == 15) f *= 0.75f;
            if ((y == 6 || y == 57) && (x % 16 == 4 || x % 16 == 11)) f = 0.3f;  // nails
            t.set(x, y, vary(base, f));
        }
    return t;
}

Texture darkBlocks(uint32_t base, int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int row = y / 16, off = (row & 1) ? 16 : 0;
            int bx = (x + off) / 32;
            int lx = (x + off) % 32, ly = y % 16;
            float n = fbm(x, y, 8, seed, 4);
            float f = (0.75f + 0.3f * h2(bx, row, seed)) * (0.8f + 0.4f * n);
            if (lx == 0 || ly == 0) f *= 0.35f;
            else if (lx == 1 || ly == 1) f *= 1.3f;
            else if (lx == 31 || ly == 15) f *= 0.6f;
            t.set(x, y, vary(base, f));
        }
    return t;
}

void addMoss(Texture& t, int seed) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float n = fbm(x, y, 4, seed, 4);
            float drip = fbm(x, y / 4, 8, seed + 5, 2);
            if (n + (1.0f - y / 64.0f) * 0.25f - drip * 0.1f > 0.68f)
                t.set(x, y, colMix(t.at(x, y), vary(0xFF3A6A28, 0.7f + 0.6f * h2(x, y, seed)), 0.75f));
        }
}

void addBlood(Texture& t, int seed) {
    for (int x = 0; x < 64; x++) {
        if (h2(x / 3, 0, seed) < 0.75f) continue;
        int len = (int)(10 + 40 * h2(x / 3, 1, seed));
        for (int y = 0; y < len; y++)
            t.set(x, y, colMix(t.at(x, y), 0xFF500808, 0.75f * (1.0f - (float)y / len)));
    }
}

Texture doorTex(bool locked) {
    Texture t = woodWall(0xFF7A5030, 77);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            // frame
            if (x < 3 || x > 60 || y < 3) { t.set(x, y, vary(0xFF404040, 0.8f + 0.3f * h2(x, y, 3))); continue; }
            // iron bands
            if ((y >= 12 && y <= 16) || (y >= 46 && y <= 50)) {
                float f = 0.8f + 0.3f * h2(x, y, 4);
                if (y == 12 || y == 46) f *= 1.3f;
                t.set(x, y, vary(locked ? 0xFF505868 : 0xFF484848, f));
                if (x % 10 == 5 && (y == 14 || y == 48)) t.set(x, y, 0xFFB0B0B0);  // rivets
            }
        }
    // handle / lock
    if (locked) {
        fillRect(t, 44, 28, 54, 40, 0xFF8A8A30);
        fillRect(t, 45, 29, 53, 39, 0xFFC8B040);
        fillRect(t, 48, 31, 50, 36, 0xFF101010);
        fillCircle(t, 49, 32, 1.6f, 0xFF101010);
    } else {
        fillCircle(t, 50, 33, 3, 0xFF202020);
        fillCircle(t, 50, 33, 2, 0xFF909090);
    }
    return t;
}

// A heap of loose rocks with a dark gap above: "the passage continues, dig through".
Texture rubbleTex() {
    Texture t(64, 64);
    // Dark opening behind the heap, with a hint of a timber frame.
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float n = fbm(x, y, 4, 5, 3);
            uint32_t c = vary(0xFF141010, 0.6f + 0.6f * n);
            if (y < 5 || x < 4 || x > 59) c = vary(0xFF6A4A2A, 0.6f + 0.5f * n);  // beams
            t.set(x, y, c);
        }
    Rng r(42);
    // The heap: rocks below a mound-shaped curve, bigger ones at the bottom.
    for (int i = 0; i < 140; i++) {
        float cx = 2 + r.frand() * 60;
        float top = 22 + 14 * std::fabs(cx - 32) / 32.0f;      // mound, highest in the middle
        float cy = top + r.frand() * (64 - top);
        float rad = 2.0f + r.frand() * (2.5f + (cy - top) / 9.0f);
        uint32_t c = vary(i % 5 == 0 ? 0xFF9A8060 : 0xFF8A7A68, 0.65f + 0.55f * r.frand());
        fillEllipse(t, cx, cy, rad, rad * 0.8f, colMul(c, 0.55f));
        fillEllipse(t, cx - 0.5f, cy - 0.5f, rad * 0.85f, rad * 0.68f, c);
        fillEllipse(t, cx - rad * 0.3f, cy - rad * 0.35f, rad * 0.35f, rad * 0.25f, colMul(c, 1.3f));
    }
    // A broken plank sticking out.
    line(t, 8, 30, 26, 20, 3, 0xFF7A5230);
    return t;
}

// --------------------------------------------------------------- floors ---
Texture flagstones(uint32_t base, int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int bx = x / 16, by = y / 16;
            int ox = (by & 1) ? 8 : 0;
            bx = ((x + ox) / 16);
            bool gap = (y % 16 == 0) || ((x + ox) % 16 == 0);
            float n = fbm(x, y, 8, seed, 3);
            float f = gap ? 0.35f : (0.75f + 0.25f * h2(bx, by, seed)) * (0.85f + 0.3f * n);
            t.set(x, y, vary(base, f));
        }
    return t;
}
Texture dirtFloor(uint32_t base, int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float n = fbm(x, y, 4, seed, 5);
            float f = 0.6f + 0.7f * n;
            if (h2(x, y, seed + 9) > 0.97f) f *= 1.4f;  // pebbles
            t.set(x, y, vary(base, f));
        }
    return t;
}
Texture checkerFloor(uint32_t a, uint32_t b, int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            bool which = ((x / 16) + (y / 16)) & 1;
            float n = fbm(x, y, 8, seed, 3);
            float f = 0.85f + 0.3f * n;
            if (x % 16 == 0 || y % 16 == 0) f *= 0.6f;
            t.set(x, y, vary(which ? a : b, f));
        }
    return t;
}
Texture plankFloor(uint32_t base, int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int plank = y / 8;
            int seg = (x + plank * 23) / 32;
            float grain = std::sin(x * 0.3f + fbm(x, y, 4, seed + plank, 2) * 6.0f) * 0.5f + 0.5f;
            float f = (0.72f + 0.2f * grain) * (0.85f + 0.3f * h2(seg, plank, seed));
            if (y % 8 == 0 || (x + plank * 23) % 32 == 0) f *= 0.45f;
            t.set(x, y, vary(base, f));
        }
    return t;
}
Texture grassFloor(int seed) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float n = fbm(x, y, 4, seed, 4);
            float blade = h2(x, y, seed + 3);
            uint32_t c = vary(0xFF4A7A30, 0.65f + 0.55f * n);
            if (blade > 0.9f) c = colMul(c, 1.35f);
            if (blade < 0.06f) c = colMul(c, 0.6f);
            t.set(x, y, c);
        }
    return t;
}
Texture cobbleFloor(int seed) {
    Texture t(64, 64, 0xFF3A3632);
    Rng r((uint32_t)seed);
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++) {
            float cx = col * 8 + 4 + (row & 1) * 4 + r.frand() * 1.5f, cy = row * 8 + 4 + r.frand() * 1.5f;
            uint32_t c = vary(0xFF8A847A, 0.75f + 0.4f * r.frand());
            for (int y = -4; y <= 4; y++)
                for (int x = -4; x <= 4; x++) {
                    float dx = x / 3.6f, dy = y / 3.3f;
                    if (dx * dx + dy * dy > 1) continue;
                    int px = ((int)cx + x) & 63, py = ((int)cy + y) & 63;
                    float sh = 1.15f - 0.3f * (dx + dy + 1) * 0.5f;
                    t.set(px, py, colMul(c, sh));
                }
        }
    return t;
}

Texture stairsTex(bool down) {
    Texture t(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int step = y / 8;
            float f = down ? (1.0f - step / 8.0f) * 0.9f : 0.4f + step / 8.0f * 0.7f;
            if (y % 8 == 0) f *= 0.5f;
            if (x < 6 || x > 57) f = 0.35f;
            f *= 0.85f + 0.3f * fbm(x, y, 8, 21, 2);
            t.set(x, y, vary(down ? 0xFF8A8070 : 0xFFA09888, f));
        }
    return t;
}

// -------------------------------------------------------------- sprites ---
Texture humanoid(uint32_t c1, uint32_t c2, bool attack, int seed, uint32_t skin = 0, int hat = 0) {
    Texture t(64, 64);
    uint32_t dark = colMul(c1, 0.65f), head = skin ? skin : colMul(c1, 1.1f);
    // legs
    fillRect(t, 24, 46, 29, 62, dark);
    fillRect(t, 35, 46, 40, 62, dark);
    fillRect(t, 22, 60, 29, 63, colMul(dark, 0.6f));
    fillRect(t, 35, 60, 42, 63, colMul(dark, 0.6f));
    // torso
    fillEllipse(t, 32, 37, 11, 12, c1);
    fillRect(t, 23, 44, 41, 48, colMul(c1, 0.5f));  // belt
    // arms
    line(t, 22, 29, 16, 47, 5, c1);
    if (attack) {
        line(t, 42, 29, 51, 14, 5, c1);
        line(t, 51, 14, 58, 2, 3, 0xFFB0B0B8);  // weapon raised
    } else {
        line(t, 42, 29, 48, 47, 5, c1);
        line(t, 48, 47, 56, 34, 3, 0xFF8A8A90);  // weapon
    }
    // head
    fillCircle(t, 32, 19, 7, head);
    t.set(29, 19, c2); t.set(30, 19, c2); t.set(34, 19, c2); t.set(35, 19, c2);
    t.set(29, 18, colMul(c2, 0.7f)); t.set(35, 18, colMul(c2, 0.7f));
    if (hat == 1) {  // helmet
        fillEllipse(t, 32, 15, 8, 5, 0xFF8A8A92);
        fillRect(t, 31, 14, 33, 23, 0xFF8A8A92);
    } else if (hat == 2) {  // hood
        fillEllipse(t, 32, 16, 9, 7, colMul(c1, 0.8f));
        fillRect(t, 28, 17, 36, 22, head);
        t.set(29, 19, c2); t.set(30, 19, c2); t.set(34, 19, c2); t.set(35, 19, c2);
    }
    shade(t, seed);
    outline(t);
    return t;
}

Texture caster(uint32_t c1, uint32_t c2, bool attack, int seed, uint32_t skin = 0, bool hat = false) {
    Texture t(64, 64);
    // staff
    line(t, 50, attack ? 6 : 12, 50, 63, 3, 0xFF6A4A2A);
    // robe
    fillQuad(t, Vec2(22, 28), Vec2(42, 28), Vec2(48, 63), Vec2(16, 63), c1);
    fillRect(t, 30, 30, 34, 62, colMul(c1, 0.8f));
    // arms
    line(t, 24, 30, 18, 46, 5, c1);
    line(t, 40, 30, 49, attack ? 20 : 40, 5, c1);
    // hood + face
    fillCircle(t, 32, 20, 8, colMul(c1, 0.85f));
    fillEllipse(t, 32, 22, 5, 5, skin ? skin : 0xFF181018);
    t.set(30, 21, c2); t.set(34, 21, c2);
    if (!skin) { t.set(29, 21, c2); t.set(35, 21, c2); }
    if (hat) fillTri(t, Vec2(22, 16), Vec2(42, 16), Vec2(34, 0), colMul(c1, 0.9f));
    shade(t, seed);
    // glowing orb (unshaded)
    fillCircle(t, 50, attack ? 5 : 10, attack ? 5.0f : 3.5f, c2);
    fillCircle(t, 49, attack ? 4 : 9, 1.5f, 0xFFFFFFFF);
    outline(t);
    return t;
}

Texture skeleton(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    uint32_t bone = c1, dark = colMul(c1, 0.6f);
    line(t, 32, 25, 32, 46, 3, bone);  // spine
    for (int i = 0; i < 4; i++) line(t, 25 + i, 29 + i * 4, 39 - i, 29 + i * 4, 2, bone);
    fillRect(t, 26, 44, 38, 48, bone);  // pelvis
    line(t, 28, 48, 26, 63, 3, bone);
    line(t, 36, 48, 38, 63, 3, bone);
    line(t, 24, 28, 18, 46, 3, bone);
    if (attack) { line(t, 40, 28, 50, 14, 3, bone); line(t, 50, 14, 56, 0, 3, 0xFF9A8A7A); }
    else { line(t, 40, 28, 46, 46, 3, bone); line(t, 46, 46, 52, 30, 3, 0xFF9A8A7A); }
    fillCircle(t, 32, 17, 7, bone);
    fillRect(t, 28, 22, 36, 26, bone);
    fillCircle(t, 29, 17, 2, 0xFF080808);
    fillCircle(t, 35, 17, 2, 0xFF080808);
    for (int x = 29; x <= 35; x += 2) t.set(x, 24, dark);
    shade(t, seed);
    t.set(29, 17, c2); t.set(35, 17, c2);
    outline(t);
    return t;
}

Texture bigBrute(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    uint32_t dark = colMul(c1, 0.65f);
    fillRect(t, 20, 46, 29, 63, dark);
    fillRect(t, 35, 46, 44, 63, dark);
    fillEllipse(t, 32, 36, 17, 14, c1);
    fillRect(t, 18, 42, 46, 48, colMul(0xFF5A4030, 1.0f));  // loincloth/belt
    line(t, 16, 28, 8, 52, 8, c1);
    if (attack) { line(t, 48, 28, 56, 10, 8, c1); line(t, 56, 10, 60, 0, 6, 0xFF6A4A2A); }
    else { line(t, 48, 28, 56, 50, 8, c1); line(t, 56, 50, 62, 30, 6, 0xFF6A4A2A); }
    fillCircle(t, 32, 18, 8, colMul(c1, 1.05f));
    fillRect(t, 27, 16, 37, 18, colMul(c1, 0.7f));  // brow
    t.set(29, 19, c2); t.set(30, 19, c2); t.set(34, 19, c2); t.set(35, 19, c2);
    t.set(28, 24, 0xFFF0F0E0); t.set(36, 24, 0xFFF0F0E0); t.set(28, 23, 0xFFF0F0E0); t.set(36, 23, 0xFFF0F0E0);
    shade(t, seed);
    outline(t);
    return t;
}

Texture rat(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    line(t, 14, 56, 2, attack ? 50 : 60, 2, 0xFFC09090);
    fillEllipse(t, 30, 54, 16, 8, c1);
    fillEllipse(t, 47, attack ? 50 : 53, 8, 6, colMul(c1, 1.1f));
    fillCircle(t, 44, attack ? 44 : 47, 3, colMul(c1, 0.8f));
    for (int x : {20, 26, 34, 40}) fillRect(t, x, 59, x + 2, 63, colMul(c1, 0.6f));
    shade(t, seed);
    t.set(51, attack ? 49 : 52, c2); t.set(52, attack ? 49 : 52, c2);
    t.set(55, attack ? 51 : 54, 0xFFE0A0A0);
    outline(t);
    return t;
}

Texture bat(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    float wy = attack ? 14 : 22;
    fillTri(t, Vec2(30, 26), Vec2(4, wy), Vec2(14, 38), c1);
    fillTri(t, Vec2(34, 26), Vec2(60, wy), Vec2(50, 38), c1);
    fillTri(t, Vec2(30, 28), Vec2(14, 38), Vec2(22, 36), colMul(c1, 0.8f));
    fillTri(t, Vec2(34, 28), Vec2(50, 38), Vec2(42, 36), colMul(c1, 0.8f));
    fillEllipse(t, 32, 30, 6, 9, colMul(c1, 1.2f));
    fillTri(t, Vec2(27, 22), Vec2(30, 16), Vec2(31, 23), c1);
    fillTri(t, Vec2(37, 22), Vec2(34, 16), Vec2(33, 23), c1);
    shade(t, seed);
    t.set(30, 26, c2); t.set(34, 26, c2);
    outline(t);
    return t;
}

Texture spider(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    for (int s = -1; s <= 1; s += 2)
        for (int i = 0; i < 4; i++) {
            float bx = 32 + s * 5, by = 40 + i * 2;
            float kx = 32 + s * (14 + i * 3), ky = 30 + i * 3 - (attack && i == 0 ? 10 : 0);
            float fx = 32 + s * (20 + i * 3), fy = 63;
            if (attack && i == 0) { fx = 32 + s * 12; fy = 24; }
            line(t, bx, by, kx, ky, 2, colMul(c1, 0.8f));
            line(t, kx, ky, fx, fy, 2, colMul(c1, 0.8f));
        }
    fillEllipse(t, 32, 50, 13, 10, c1);
    fillCircle(t, 32, 39, 7, colMul(c1, 1.15f));
    for (int i = 0; i < 6; i++) fillRect(t, 27 + i * 2, 51 - (i % 2) * 3, 27 + i * 2, 51 - (i % 2) * 3, 0xFFA02020);
    shade(t, seed);
    t.set(29, 37, c2); t.set(31, 36, c2); t.set(33, 36, c2); t.set(35, 37, c2);
    t.set(30, 39, c2); t.set(34, 39, c2);
    outline(t);
    return t;
}

Texture beast(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    line(t, 12, 44, 3, attack ? 34 : 40, 4, c1);
    for (int x : {16, 22, 38, 44}) fillRect(t, x, 48, x + 3, 63, colMul(c1, 0.8f));
    fillEllipse(t, 30, 45, 18, 8, c1);
    float hy = attack ? 38 : 40;
    fillEllipse(t, 51, hy, 8, 6, colMul(c1, 1.1f));
    fillEllipse(t, 59, hy + 3, 5, attack ? 4 : 3, colMul(c1, 1.0f));
    fillTri(t, Vec2(46, hy - 4), Vec2(48, hy - 12), Vec2(51, hy - 4), c1);
    fillTri(t, Vec2(52, hy - 4), Vec2(54, hy - 12), Vec2(56, hy - 4), c1);
    shade(t, seed);
    t.set(53, (int)hy - 1, c2); t.set(54, (int)hy - 1, c2);
    if (attack) for (int x = 56; x < 63; x += 2) t.set(x, (int)hy + 5, 0xFFFFFFFF);
    outline(t);
    return t;
}

Texture ghost(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    fillEllipse(t, 32, 22, 13, 15, c1);
    fillRect(t, 19, 22, 45, 52, c1);
    for (int x = 19; x <= 45; x++) {
        int wave = (int)(3 * std::sin(x * 0.7f + seed));
        for (int y = 52; y < 58 + wave; y++) t.set(x, y, c1);
    }
    line(t, 20, 28, attack ? 6 : 10, attack ? 14 : 42, 5, c1);
    line(t, 44, 28, attack ? 58 : 54, attack ? 14 : 42, 5, c1);
    shade(t, seed);
    fillEllipse(t, 27, 21, 3, 4, 0xFF000000 | 0x080810);
    fillEllipse(t, 37, 21, 3, 4, 0xFF000000 | 0x080810);
    t.set(27, 21, c2); t.set(37, 21, c2);
    fillEllipse(t, 32, 31, 3, attack ? 4 : 2, 0xFF080810);
    return t;
}

Texture blob(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    float ry = attack ? 20 : 16;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = (x - 32) / 26.0f, dy = (y - 63) / ry;
            if (dx * dx + dy * dy <= 1.0f && y <= 63) t.set(x, y, c1);
        }
    fillEllipse(t, 24, 63 - ry * 0.6f, 6, 3, colMul(c1, 1.5f));
    shade(t, seed);
    fillCircle(t, 27, 63 - ry * 0.45f, 2.5f, c2);
    fillCircle(t, 38, 63 - ry * 0.5f, 2.0f, c2);
    outline(t);
    return t;
}

Texture snake(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    // Coiled body made of overlapping segments, head raised.
    for (int i = 0; i < 26; i++) {
        float a = i * 0.42f;
        float r = 16 - i * 0.35f;
        float x = 32 + std::cos(a) * r, y = 56 + std::sin(a) * r * 0.35f;
        fillCircle(t, x, y, 5.5f - i * 0.08f, (i % 4 < 2) ? c1 : colMul(c1, 0.8f));
    }
    float hy = attack ? 22 : 30;
    line(t, 32, 50, 36, hy + 6, 7, c1);
    fillEllipse(t, 37, hy, 7, 5, colMul(c1, 1.1f));
    shade(t, seed);
    t.set(39, (int)hy - 1, c2); t.set(35, (int)hy - 1, c2);
    if (attack) { line(t, 40, hy + 3, 46, hy + 6, 1, 0xFFFF4040); }
    outline(t);
    return t;
}

Texture worm(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    for (int w = 0; w < 3; w++) {
        float bx = 14 + w * 17, phase = w * 1.7f;
        for (int i = 0; i < 9; i++) {
            float x = bx + i * 1.6f - 6, y = 60 - std::fabs(std::sin(i * 0.5f + phase)) * (attack ? 12 : 7) - i * 0.6f;
            fillCircle(t, x, y, 3.6f, i % 2 ? c1 : colMul(c1, 0.88f));
        }
    }
    shade(t, seed);
    outline(t);
    return t;
}

Texture eyeMonster(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    fillCircle(t, 32, 30, 18, c1);
    for (int i = 0; i < 6; i++) line(t, 32 + std::cos(i * 1.05f) * 17, 30 + std::sin(i * 1.05f) * 17, 32 + std::cos(i * 1.05f + 0.3f) * 24,
                                     30 + std::sin(i * 1.05f + 0.3f) * 24, 2, colMul(c1, 0.7f));
    shade(t, seed);
    fillEllipse(t, 32, 30, 12, attack ? 10 : 8, 0xFFF8F8F0);
    fillCircle(t, 32, 30, attack ? 6 : 5, c2);
    fillCircle(t, 32, 30, 2.5f, 0xFF000000);
    t.set(30, 28, 0xFFFFFFFF);
    outline(t);
    return t;
}

Texture imp(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    float wy = attack ? 8 : 16;
    fillTri(t, Vec2(28, 28), Vec2(6, wy), Vec2(14, 40), colMul(c1, 0.7f));
    fillTri(t, Vec2(36, 28), Vec2(58, wy), Vec2(50, 40), colMul(c1, 0.7f));
    fillEllipse(t, 32, 36, 9, 11, c1);
    line(t, 28, 45, 24, 58, 3, c1);
    line(t, 36, 45, 40, 58, 3, c1);
    line(t, 38, 44, 50, 54, 2, c1);  // tail
    fillTri(t, Vec2(48, 52), Vec2(54, 52), Vec2(52, 57), c1);
    fillCircle(t, 32, 22, 7, colMul(c1, 1.1f));
    fillTri(t, Vec2(26, 18), Vec2(28, 9), Vec2(30, 17), 0xFFE0D0B0);  // horns
    fillTri(t, Vec2(38, 18), Vec2(36, 9), Vec2(34, 17), 0xFFE0D0B0);
    shade(t, seed);
    t.set(29, 22, c2); t.set(35, 22, c2); t.set(30, 22, c2); t.set(34, 22, c2);
    outline(t);
    return t;
}

Texture elemental(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    // A swirling column that is brightest at the core.
    for (int y = 4; y < 64; y++) {
        float w = 6 + 12 * std::sin((y - 4) / 60.0f * PI) + 3 * std::sin(y * 0.5f + seed);
        if (attack) w += 3;
        for (int x = (int)(32 - w); x <= (int)(32 + w); x++) {
            float d = std::fabs(x - 32) / std::max(1.0f, w);
            float n = fbm(x, y, 8, seed, 3);
            if (n < 0.3f && d > 0.7f) continue;  // ragged edges
            t.set(x, y, colMix(c1, c2, std::max(0.0f, 1.0f - d * 1.4f) * (0.6f + 0.6f * n)));
        }
    }
    fillCircle(t, 27, 22, 2, 0xFF101010);
    fillCircle(t, 37, 22, 2, 0xFF101010);
    return t;
}

Texture golem(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    uint32_t dark = colMul(c1, 0.7f);
    fillRect(t, 18, 44, 28, 63, dark);
    fillRect(t, 36, 44, 46, 63, dark);
    fillRect(t, 14, 20, 50, 46, c1);
    fillRect(t, 22, 6, 42, 20, colMul(c1, 1.05f));
    if (attack) { fillRect(t, 50, 4, 60, 30, c1); fillRect(t, 4, 22, 14, 48, c1); }
    else { fillRect(t, 4, 22, 14, 50, c1); fillRect(t, 50, 22, 60, 50, c1); }
    // cracks
    line(t, 20, 26, 30, 38, 1, colMul(c1, 0.5f));
    line(t, 40, 24, 46, 34, 1, colMul(c1, 0.5f));
    shade(t, seed);
    fillRect(t, 26, 11, 29, 13, c2);
    fillRect(t, 35, 11, 38, 13, c2);
    outline(t);
    return t;
}

Texture dragon(uint32_t c1, uint32_t c2, bool attack, int seed) {
    Texture t(64, 64);
    uint32_t belly = colMix(c1, 0xFFE0C080, 0.5f);
    fillTri(t, Vec2(24, 30), Vec2(2, 6), Vec2(14, 40), colMul(c1, 0.65f));   // wings
    fillTri(t, Vec2(40, 30), Vec2(62, 6), Vec2(50, 40), colMul(c1, 0.65f));
    fillEllipse(t, 32, 44, 16, 13, c1);
    fillEllipse(t, 32, 47, 9, 9, belly);
    fillRect(t, 20, 54, 26, 63, colMul(c1, 0.8f));
    fillRect(t, 38, 54, 44, 63, colMul(c1, 0.8f));
    line(t, 32, 34, 32, attack ? 12 : 18, 7, c1);                         // neck
    fillEllipse(t, 32, attack ? 10 : 16, 8, 6, colMul(c1, 1.1f));
    fillTri(t, Vec2(26, attack ? 7.0f : 13.0f), Vec2(24, attack ? 0.0f : 5.0f), Vec2(29, attack ? 6.0f : 12.0f), 0xFFE0D0B0);
    fillTri(t, Vec2(38, attack ? 7.0f : 13.0f), Vec2(40, attack ? 0.0f : 5.0f), Vec2(35, attack ? 6.0f : 12.0f), 0xFFE0D0B0);
    shade(t, seed);
    int hy = attack ? 9 : 15;
    t.set(29, hy, c2); t.set(35, hy, c2); t.set(28, hy, c2); t.set(36, hy, c2);
    if (attack) fillEllipse(t, 32, 16, 3, 2, 0xFFFFA030);
    outline(t);
    return t;
}

void addCrown(Texture& t) {
    t.computeTop();
    int y = std::max(4, t.top - 1);
    int minx = 64, maxx = 0;
    for (int x = 0; x < 64; x++)
        if (t.at(x, t.top + 1) >> 24) { minx = std::min(minx, x); maxx = std::max(maxx, x); }
    int cx = (minx + maxx) / 2;
    fillRect(t, cx - 6, y - 3, cx + 6, y, 0xFFE0B020);
    for (int i = -6; i <= 6; i += 4) fillTri(t, Vec2(cx + i - 2, y - 2), Vec2(cx + i + 2, y - 2), Vec2(cx + i, y - 7), 0xFFF0C830);
    t.set(cx, y - 2, 0xFFFF2020);
}

// ---------------------------------------------------------------- items ---
Texture itemSprite(const std::string& s, uint32_t col, int seed) {
    Texture t(32, 32);
    uint32_t steel = 0xFFC8CCD8, wood = 0xFF7A5230, gold = 0xFFE8C040;
    if (s == "sword" || s == "dagger") {
        float len = s == "sword" ? 22 : 13;
        line(t, 6, 26, 6 + len * 0.7f, 26 - len * 0.7f, 3, col == 0xFFC0C0C0 ? steel : col);
        line(t, 3, 23, 10, 30, 2, gold);
        line(t, 2, 30, 5, 27, 3, wood);
    } else if (s == "mace") {
        line(t, 6, 28, 18, 14, 3, wood);
        fillCircle(t, 21, 11, 5, 0xFF8A8A90);
        for (int a = 0; a < 6; a++) fillCircle(t, 21 + std::cos(a * 1.05f) * 6, 11 + std::sin(a * 1.05f) * 6, 1.2f, 0xFFB0B0B0);
    } else if (s == "axe") {
        line(t, 6, 29, 20, 8, 3, wood);
        fillTri(t, Vec2(16, 8), Vec2(28, 4), Vec2(26, 18), steel);
    } else if (s == "staff") {
        line(t, 5, 30, 24, 6, 3, wood);
        fillCircle(t, 25, 5, 3.5f, col == 0xFFC0C0C0 ? 0xFF80C0FF : col);
    } else if (s == "bow") {
        for (int i = 0; i < 20; i++) {
            float a = -1.2f + i * 0.12f;
            fillCircle(t, 10 + std::cos(a) * 12, 16 + std::sin(a) * 13, 1.3f, col == 0xFFC0C0C0 ? wood : col);
        }
        line(t, 14, 4, 14, 28, 1, 0xFFE0E0E0);
    } else if (s == "armor" || s == "robe") {
        uint32_t c = s == "robe" ? (col == 0xFFC0C0C0 ? 0xFF5050A0 : col) : col;
        fillQuad(t, Vec2(8, 8), Vec2(24, 8), Vec2(26, 28), Vec2(6, 28), c);
        fillRect(t, 3, 8, 8, 16, c);
        fillRect(t, 24, 8, 29, 16, c);
        fillEllipse(t, 16, 8, 4, 2, 0x00000000);
    } else if (s == "spear") {
        line(t, 3, 30, 24, 9, 2, wood);
        fillTri(t, Vec2(22, 6), Vec2(30, 2), Vec2(26, 12), col == 0xFFC0C0C0 ? steel : col);
    } else if (s == "flail") {
        line(t, 4, 30, 14, 18, 3, wood);
        line(t, 14, 18, 20, 12, 1, 0xFF909090);
        fillCircle(t, 22, 9, 5, 0xFF8A8A90);
        for (int a = 0; a < 6; a++) fillCircle(t, 22 + std::cos(a * 1.05f) * 6, 9 + std::sin(a * 1.05f) * 6, 1.2f, 0xFFB0B0B0);
    } else if (s == "helm") {
        uint32_t c = col == 0xFFC0C0C0 ? 0xFF9A9AA8 : col;
        fillEllipse(t, 16, 18, 11, 10, c);
        fillRect(t, 4, 18, 28, 28, 0x00000000);
        fillRect(t, 5, 17, 27, 20, colMul(c, 0.8f));
        fillRect(t, 15, 17, 17, 26, colMul(c, 0.9f));
    } else if (s == "crown") {
        fillRect(t, 6, 16, 26, 24, gold);
        for (int i = 6; i <= 26; i += 5) fillTri(t, Vec2(i - 3.0f, 16), Vec2(i + 2.0f, 16), Vec2((float)i, 8), gold);
        fillCircle(t, 16, 20, 2, col);
    } else if (s == "ring") {
        for (int i = 0; i < 24; i++) {
            float a = i / 24.0f * 2 * PI;
            fillCircle(t, 16 + std::cos(a) * 7, 20 + std::sin(a) * 5, 1.6f, gold);
        }
        fillCircle(t, 16, 14, 3.5f, col);
        t.set(15, 13, 0xFFFFFFFF);
    } else if (s == "torch") {
        line(t, 12, 30, 18, 12, 3, wood);
        fillEllipse(t, 19, 9, 3, 5, 0xFFFF9020);
        fillEllipse(t, 19, 10, 1.5f, 3, 0xFFFFF080);
    } else if (s == "lantern") {
        fillRect(t, 10, 10, 22, 28, 0xFF505050);
        fillRect(t, 12, 12, 20, 26, 0xFFFFD060);
        fillRect(t, 15, 4, 17, 10, 0xFF505050);
    } else if (s == "ration") {
        fillRect(t, 6, 16, 26, 28, 0xFFB09060);
        line(t, 6, 22, 26, 22, 1, 0xFF705030);
        line(t, 16, 16, 16, 28, 1, 0xFF705030);
    } else if (s == "apple") {
        fillCircle(t, 16, 20, 8, 0xFFD02020);
        line(t, 16, 12, 18, 7, 2, 0xFF603010);
        fillEllipse(t, 21, 9, 3, 2, 0xFF30A030);
    } else if (s == "meat") {
        fillEllipse(t, 16, 22, 11, 6, 0xFF8A3020);
        fillEllipse(t, 14, 21, 6, 3, 0xFFB05040);
    } else if (s == "wand") {
        uint32_t c = col == 0xFFC0C0C0 ? 0xFF8060C0 : col;
        line(t, 7, 27, 24, 8, 2.2f, 0xFF3A2A20);
        line(t, 8, 26, 23, 9, 1.2f, colMix(0xFF3A2A20, c, 0.5f));
        fillCircle(t, 24.5f, 7.5f, 2.6f, c);
        t.set(24, 6, 0xFFFFFFFF);
        for (int i = 0; i < 4; i++) t.set(27 + (i % 2) * 2 - 2, 3 + i * 2, colMix(c, 0xFFFFFFFF, 0.6f));
    } else if (s == "rod") {
        uint32_t c = col == 0xFFC0C0C0 ? 0xFF60C0E0 : col;
        line(t, 5, 29, 25, 7, 3.2f, 0xFF707884);
        line(t, 6, 28, 24, 8, 1.4f, 0xFFB0B8C8);
        for (int i = 0; i < 3; i++) line(t, 8 + i * 6, 26 - i * 6.6f, 10 + i * 6, 24 - i * 6.6f, 2.5f, gold);
        fillCircle(t, 26, 6, 3.4f, c);
        fillCircle(t, 25, 5, 1.2f, colMix(c, 0xFFFFFFFF, 0.7f));
    } else if (s == "potion") {
        fillCircle(t, 16, 22, 8, col);
        fillRect(t, 13, 8, 19, 15, 0xFFB0C0D0);
        fillRect(t, 12, 6, 20, 8, 0xFF7A5230);
        fillCircle(t, 13, 19, 2, 0xFFFFFFFF);
    } else if (s == "scroll") {
        uint32_t paper = col == 0xFFC0C0C0 ? 0xFFE8DCB0 : colMix(0xFFE8DCB0, col, 0.4f);
        fillRect(t, 6, 10, 26, 24, paper);
        fillCircle(t, 6, 17, 4, colMul(paper, 0.8f));
        fillCircle(t, 26, 17, 4, colMul(paper, 0.8f));
        for (int y = 13; y < 22; y += 3) line(t, 10, y, 22, y, 1, 0xFF605040);
    } else if (s == "flask") {
        fillEllipse(t, 16, 22, 7, 7, 0xFF907040);
        fillRect(t, 14, 9, 18, 15, 0xFF907040);
        fillRect(t, 13, 7, 19, 9, 0xFF503010);
    } else if (s == "gold") {
        for (int i = 0; i < 9; i++) {
            float x = 8 + (i % 3) * 7 + (i / 3) * 2, y = 26 - (i / 3) * 4;
            fillEllipse(t, x, y, 4, 2.5f, gold);
            t.set((int)x - 1, (int)y - 1, 0xFFFFF0A0);
        }
    } else if (s == "amulet") {
        for (int i = 0; i < 16; i++) {
            float a = i / 16.0f * PI;
            t.set((int)(16 + std::cos(a + PI) * 10), (int)(8 + std::sin(a) * 10), gold);
        }
        fillCircle(t, 16, 21, 5, col);
        fillCircle(t, 15, 20, 1.5f, 0xFFFFFFFF);
    } else if (s == "tome") {
        fillRect(t, 7, 9, 25, 28, col);
        fillRect(t, 9, 11, 23, 26, colMul(col, 0.8f));
        fillRect(t, 24, 10, 26, 27, 0xFFE0D8C0);
        fillCircle(t, 16, 18, 3, gold);
    } else if (s == "chalice") {
        fillEllipse(t, 16, 10, 8, 6, col);
        fillRect(t, 15, 14, 17, 24, col);
        fillEllipse(t, 16, 26, 7, 2.5f, col);
    } else {
        fillCircle(t, 16, 20, 7, col);
    }
    shade(t, seed);
    outline(t);
    return t;
}

Texture orb(uint32_t col, int size) {
    Texture t(size, size);
    float c = size / 2.0f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float d = std::sqrt((x + 0.5f - c) * (x + 0.5f - c) + (y + 0.5f - c) * (y + 0.5f - c)) / c;
            if (d > 1) continue;
            float core = std::max(0.0f, 1.0f - d * 2.2f);
            float glow = (1.0f - d) * (1.0f - d);
            uint32_t cc = colMix(colMul(col, glow * 1.4f), 0xFFFFFFFF, core);
            t.set(x, y, cc);
        }
    return t;
}

// ----------------------------------------------------------- viewmodels ---
// 128x128, bottom aligned, held in the right hand.
void hand(Texture& t, float x, float y, uint32_t skin = 0xFFD09878) {
    fillEllipse(t, x, y, 13, 10, skin);
    fillEllipse(t, x - 3, y - 6, 9, 5, colMul(skin, 1.08f));
    fillRect(t, (int)x - 10, (int)y + 4, (int)x + 12, 127, 0xFF6A4A3A);  // sleeve
}
Texture viewModel(int kind) {
    Texture t(128, 128);
    uint32_t steel = 0xFFC8CCD8, wood = 0xFF7A5230;
    switch (kind) {
        case VM_FIST:
            hand(t, 80, 108);
            break;
        case VM_DAGGER:
            fillQuad(t, Vec2(68, 96), Vec2(80, 96), Vec2(68, 46), Vec2(62, 48), steel);
            line(t, 72, 94, 65, 48, 1, 0xFFFFFFFF);
            line(t, 64, 98, 86, 98, 4, 0xFFC0A040);
            hand(t, 76, 112);
            break;
        case VM_SWORD:
            fillQuad(t, Vec2(64, 92), Vec2(80, 92), Vec2(62, 10), Vec2(52, 14), steel);
            fillTri(t, Vec2(52, 14), Vec2(62, 10), Vec2(54, 2), steel);
            line(t, 70, 90, 57, 10, 2, 0xFFFFFFFF);
            line(t, 76, 90, 61, 12, 1, colMul(steel, 0.7f));
            line(t, 56, 94, 92, 94, 6, 0xFFC0A040);
            hand(t, 76, 110);
            break;
        case VM_MACE:
            line(t, 76, 110, 60, 40, 6, wood);
            fillCircle(t, 58, 32, 14, 0xFF8A8A90);
            for (int a = 0; a < 8; a++) fillCircle(t, 58 + std::cos(a * 0.785f) * 15, 32 + std::sin(a * 0.785f) * 15, 3, 0xFFB8B8C0);
            hand(t, 78, 112);
            break;
        case VM_AXE:
            line(t, 78, 115, 58, 26, 6, wood);
            fillTri(t, Vec2(52, 20), Vec2(30, 6), Vec2(34, 52), steel);
            fillTri(t, Vec2(60, 24), Vec2(52, 20), Vec2(34, 52), colMul(steel, 0.8f));
            hand(t, 80, 112);
            break;
        case VM_STAFF:
            line(t, 84, 127, 64, 18, 7, wood);
            fillCircle(t, 63, 14, 9, 0xFF80A0FF);
            fillCircle(t, 60, 11, 3, 0xFFFFFFFF);
            hand(t, 80, 100);
            break;
        case VM_SPEAR:
            line(t, 90, 127, 58, 10, 6, wood);
            fillTri(t, Vec2(52, 18), Vec2(62, 16), Vec2(54, 0), steel);
            hand(t, 82, 104);
            break;
        case VM_BOW:
            for (int i = 0; i < 40; i++) {
                float a = -1.25f + i * 0.0625f;
                fillCircle(t, 34 + std::cos(a) * 40, 70 + std::sin(a) * 52, 3, wood);
            }
            line(t, 48, 22, 48, 118, 1, 0xFFE0E0E0);
            line(t, 50, 70, 92, 70, 2, 0xFF9A7A50);
            fillTri(t, Vec2(40, 70), Vec2(50, 66), Vec2(50, 74), steel);
            hand(t, 96, 76);
            break;
    }
    shade(t, 5 + kind);
    outline(t);
    return t;
}

Texture monsterSprite(const MonsterDef& d, bool attack, int seed) {
    const std::string& s = d.sprite;
    Texture t;
    if (s == "rat") t = rat(d.color, d.color2, attack, seed);
    else if (s == "bat") t = bat(d.color, d.color2, attack, seed);
    else if (s == "caster") t = caster(d.color, d.color2, attack, seed);
    else if (s == "skeleton") t = skeleton(d.color, d.color2, attack, seed);
    else if (s == "spider") t = spider(d.color, d.color2, attack, seed);
    else if (s == "beast") t = beast(d.color, d.color2, attack, seed);
    else if (s == "big") t = bigBrute(d.color, d.color2, attack, seed);
    else if (s == "ghost") t = ghost(d.color, d.color2, attack, seed);
    else if (s == "blob") t = blob(d.color, d.color2, attack, seed);
    else if (s == "snake") t = snake(d.color, d.color2, attack, seed);
    else if (s == "worm") t = worm(d.color, d.color2, attack, seed);
    else if (s == "eye") t = eyeMonster(d.color, d.color2, attack, seed);
    else if (s == "imp") t = imp(d.color, d.color2, attack, seed);
    else if (s == "elemental") t = elemental(d.color, d.color2, attack, seed);
    else if (s == "golem") t = golem(d.color, d.color2, attack, seed);
    else if (s == "dragon") t = dragon(d.color, d.color2, attack, seed);
    else t = humanoid(d.color, d.color2, attack, seed);
    if (d.boss) addCrown(t);
    return t;
}

Texture playerSprite(const ClassDef& c, bool attack, int seed) {
    uint32_t skin = 0xFFD09878;
    if (c.id == "wizard") return caster(c.color, 0xFF80C0FF, attack, seed, skin, true);
    if (c.id == "priest") return caster(c.color, 0xFFFFF0A0, attack, seed, skin, false);
    int hat = c.id == "warrior" ? 1 : (c.id == "rogue" || c.id == "ranger") ? 2 : 0;
    return humanoid(c.color, 0xFF202020, attack, seed, skin, hat);
}

}  // namespace

AnimStyle animStyleFor(const std::string& s) {
    if (s == "rat" || s == "spider" || s == "beast") return AnimStyle::Skitter;
    if (s == "bat" || s == "imp") return AnimStyle::Flap;
    if (s == "ghost" || s == "eye" || s == "elemental") return AnimStyle::Float;
    if (s == "snake" || s == "worm") return AnimStyle::Slither;
    if (s == "blob") return AnimStyle::Squash;
    return AnimStyle::Legs;   // humanoids, casters, skeletons, brutes, imps, golems, dragons, players
}

// One frame of movement, made by moving the pixels of the normal picture around.
// Frames 0 and 2 are the extremes (left leg forward / right leg forward, wings up / down),
// 1 and 3 the in-between poses.
Texture walkFrame(const Texture& base, AnimStyle style, int frame) {
    const int w = base.w, h = base.h;
    Texture t(w, h);
    if (w == 0 || h == 0) return t;
    float k = w / 64.0f;                                    // art may be bigger than 64x64
    float ph = frame * PI / 2;                              // 0, 90, 180, 270 degrees
    float swing = std::cos(ph);                             // 1, 0, -1, 0
    int bob = (frame % 2 == 1) ? (int)std::lround(k) : 0;   // up a pixel between steps
    // Where the creature is: its bounding box (pictures have empty space around them).
    int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (base.px[y * w + x] >> 24) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
    if (bx1 < 0) return base;
    float cx = (bx0 + bx1) / 2.0f, bw = std::max(1, bx1 - bx0), bh = std::max(1, by1 - by0);
    // For every target pixel, find the source pixel it shows.
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float sx = x, sy = y;
            float ry = (y - by0) / bh;                      // 0 = top of the creature, 1 = feet
            switch (style) {
                case AnimStyle::Legs:
                    // Lower third: the two halves swing in opposite directions (a scissor).
                    sy += -bob;
                    if (ry > 0.7f) {
                        float a = (ry - 0.7f) / 0.3f * 4.0f * k * swing;
                        sx += x < cx ? -a : a;
                    } else if (ry > 0.35f && ry < 0.7f) {
                        // Arms/torso sway a little the other way.
                        sx += (x < cx ? 1 : -1) * 0.8f * k * swing * ((x < cx - bw * 0.3f || x > cx + bw * 0.3f) ? 1.0f : 0.0f);
                    }
                    break;
                case AnimStyle::Skitter:
                    // Legs along the bottom shuffle back and forth in alternating groups.
                    sy += -bob;
                    if (ry > 0.75f) sx += ((((int)((x - bx0) / (4 * k))) & 1) ? 1.5f : -1.5f) * k * swing;
                    break;
                case AnimStyle::Flap: {
                    // Wings: the further from the middle, the more they move up and down.
                    float out = std::fabs(x - cx) / (bw / 2);
                    sy += out * out * 6.0f * k * swing;
                    break;
                }
                case AnimStyle::Float:
                    // Hovering: a gentle wave through the body, stronger at the bottom.
                    sx += std::sin(y * 0.35f / k + ph) * (0.6f + ry * 1.4f) * k;
                    sy += std::sin(ph) * k;
                    break;
                case AnimStyle::Slither:
                    // A wave runs along the body.
                    sy += std::sin((x - bx0) * 0.25f / k + ph) * 2.0f * k * std::min(1.0f, ry * 2);
                    break;
                case AnimStyle::Squash: {
                    // Squash and stretch, standing on the floor.
                    float f = 1.0f + 0.08f * swing;               // taller / flatter
                    sy = by1 - (by1 - y) / f;
                    sx = cx + (x - cx) * f;
                    break;
                }
            }
            int ix = (int)std::floor(sx + 0.5f), iy = (int)std::floor(sy + 0.5f);
            t.px[y * w + x] = base.at(ix, iy);
        }
    t.computeTop();
    return t;
}

// Death: the body sinks into a heap and darkens.
Texture dieFrame(const Texture& base, int frame) {
    const int w = base.w, h = base.h;
    Texture t(w, h);
    int by1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (base.px[y * w + x] >> 24) by1 = y;
    if (by1 < 0) return base;
    float sy = 0.75f - 0.2f * frame;          // 0.75, 0.55, 0.35, 0.15 of the height
    float sx = 1.0f + 0.08f * (frame + 1);    // spreading out a little
    float dark = 1.0f - 0.12f * (frame + 1);
    float cx = w / 2.0f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int ix = (int)std::floor(cx + (x - cx) / sx), iy = (int)std::floor(by1 - (by1 - y) / sy);
            uint32_t c = base.at(ix, iy);
            if (c >> 24) t.px[y * w + x] = colMul(c, dark) | 0xFF000000u;
        }
    t.computeTop();
    return t;
}

void Assets::buildWalkFrames(const std::vector<std::string>& keep) {
    auto kept = [&](const std::string& n) { return std::find(keep.begin(), keep.end(), n) != keep.end(); };
    monstersWalk.resize(monsters.size());
    monstersDie.resize(monsters.size());
    for (size_t i = 0; i < monsters.size(); i++) {
        const std::string n = "monster_" + gData.monsters[i].id;
        AnimStyle st = animStyleFor(gData.monsters[i].sprite);
        if (!kept(n + "_walk")) {
            monstersWalk[i].clear();
            for (int f = 0; f < WALK_FRAMES; f++) monstersWalk[i].push_back(walkFrame(monsters[i], st, f));
        }
        if (!kept(n + "_die")) {
            monstersDie[i].clear();
            for (int f = 0; f < DIE_FRAMES; f++) monstersDie[i].push_back(dieFrame(monsters[i], f));
        }
    }
    playersWalk.resize(players.size());
    for (size_t i = 0; i < players.size(); i++) {
        if (kept("player_" + gData.classes[i].id + "_walk")) continue;
        playersWalk[i].clear();
        for (int f = 0; f < WALK_FRAMES; f++) playersWalk[i].push_back(walkFrame(players[i], AnimStyle::Legs, f));
    }
}

void Texture::computeTop() {
    top = h;
    for (int y = 0; y < h && top == h; y++)
        for (int x = 0; x < w; x++)
            if (px[y * w + x] >> 24) { top = y; break; }
}

int viewModelFromName(const std::string& n) {
    static const char* names[VM_COUNT] = {"fist", "dagger", "sword", "mace", "axe", "staff", "bow", "spear"};
    for (int i = 0; i < VM_COUNT; i++)
        if (n == names[i]) return i;
    return VM_FIST;
}

void Assets::build() {
    walls[WT_BRICK] = brickWall(0xFF8A7A6A, 0xFF4A4440, 1);
    walls[WT_CAVE] = caveWall(0xFF7A6A58, 2);
    walls[WT_MOSS] = brickWall(0xFF6A7A6A, 0xFF3A4038, 3);
    addMoss(walls[WT_MOSS], 3);
    walls[WT_DARK] = darkBlocks(0xFF5A5A66, 4);
    walls[WT_WOOD] = woodWall(0xFF8A6038, 5);
    walls[WT_BLOOD] = brickWall(0xFF6A4A44, 0xFF2A2020, 6);
    addBlood(walls[WT_BLOOD], 6);
    walls[6] = walls[WT_BRICK];
    walls[7] = walls[WT_BRICK];
    door = doorTex(false);
    doorLocked = doorTex(true);
    rubble = rubbleTex();
    floors[FT_STONE] = flagstones(0xFF6A6460, 11);
    floors[FT_DIRT] = dirtFloor(0xFF5A4A3A, 12);
    floors[FT_TILES] = checkerFloor(0xFF7A7068, 0xFF4A4440, 13);
    floors[FT_WOOD] = plankFloor(0xFF7A5434, 14);
    floors[FT_GRASS] = grassFloor(17);
    floors[FT_COBBLE] = cobbleFloor(18);
    for (int i = 6; i < 8; i++) floors[i] = floors[FT_STONE];
    ceiling = darkBlocks(0xFF4A4848, 15);
    ceilingCave = caveWall(0xFF4A4038, 16);
    stairsDown = stairsTex(true);
    stairsUp = stairsTex(false);
    stairsUpCeil = Texture(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float d = std::sqrt((x - 32.0f) * (x - 32.0f) + (y - 32.0f) * (y - 32.0f)) / 32.0f;
            stairsUpCeil.set(x, y, d < 0.8f ? colMix(0xFFFFF0C0, 0xFF6A6A70, d / 0.8f) : 0xFF3A3A40);
        }

    monsters.clear(); monstersAttack.clear();
    for (size_t i = 0; i < gData.monsters.size(); i++) {
        monsters.push_back(monsterSprite(gData.monsters[i], false, (int)i * 17 + 3));
        monstersAttack.push_back(monsterSprite(gData.monsters[i], true, (int)i * 17 + 3));
    }
    players.clear(); playersAttack.clear();
    for (size_t i = 0; i < gData.classes.size(); i++) {
        players.push_back(playerSprite(gData.classes[i], false, (int)i * 7 + 1));
        playersAttack.push_back(playerSprite(gData.classes[i], true, (int)i * 7 + 1));
    }
    items.clear();
    for (size_t i = 0; i < gData.items.size(); i++) items.push_back(itemSprite(gData.items[i].sprite, gData.items[i].color, (int)i * 13 + 5));
    spellOrbs.clear();
    for (auto& s : gData.spells) spellOrbs.push_back(orb(s.color, 32));
    arrow = Texture(32, 32);
    line(arrow, 4, 16, 26, 16, 2, 0xFF9A7A50);
    fillTri(arrow, Vec2(26, 12), Vec2(31, 16), Vec2(26, 20), 0xFFC8CCD8);
    fillTri(arrow, Vec2(2, 12), Vec2(7, 16), Vec2(2, 20), 0xFFE0E0E0);
    blood = Texture(32, 32);
    Rng r(9);
    for (int i = 0; i < 12; i++) fillEllipse(blood, 6 + r.frand() * 20, 26 + r.frand() * 5, 2 + r.frand() * 5, 1 + r.frand() * 2, colMul(0xFF801010, 0.6f + r.frand() * 0.5f));
    for (int i = 0; i < VM_COUNT; i++) viewmodels[i] = viewModel(i);

    // Shop doors: a door with a sign showing what the shop sells.
    for (int i = 0; i < 8; i++) {
        shopDoors[i] = door;
        if (i >= (int)gData.stores.size()) continue;
        Texture& t = shopDoors[i];
        fillRect(t, 12, 4, 51, 30, 0xFF3A2A1A);
        fillRect(t, 14, 6, 49, 28, 0xFFE8D8B0);
        int sign = gData.stores[i].sign;
        if (sign >= 0) {
            const Texture& ic = items[sign];
            for (int y = 0; y < 22; y++)
                for (int x = 0; x < 22; x++) {
                    uint32_t c = ic.at(x * ic.w / 22, y * ic.h / 22);
                    if (c >> 24) t.set(21 + x, 6 + y, c);
                }
        }
    }
    auto tops = [](std::vector<Texture>& v) { for (auto& t : v) t.computeTop(); };
    tops(monsters); tops(monstersAttack); tops(players); tops(playersAttack); tops(items);
    buildWalkFrames();
}

std::vector<std::pair<std::string, Texture*>> Assets::namedTextures() {
    std::vector<std::pair<std::string, Texture*>> v;
    static const char* wallNames[WT_COUNT] = {"wall_brick", "wall_cave", "wall_moss", "wall_dark", "wall_wood", "wall_blood"};
    for (int i = 0; i < WT_COUNT; i++) v.push_back({wallNames[i], &walls[i]});
    static const char* floorNames[FT_COUNT] = {"floor_stone", "floor_dirt", "floor_tiles", "floor_wood", "floor_grass", "floor_cobble"};
    for (int i = 0; i < FT_COUNT; i++) v.push_back({floorNames[i], &floors[i]});
    v.push_back({"door", &door});
    v.push_back({"door_locked", &doorLocked});
    for (size_t i = 0; i < gData.stores.size() && i < 8; i++) v.push_back({"shop_" + gData.stores[i].id, &shopDoors[i]});
    v.push_back({"rubble", &rubble});
    v.push_back({"ceiling", &ceiling});
    v.push_back({"ceiling_cave", &ceilingCave});
    v.push_back({"stairs_down", &stairsDown});
    v.push_back({"stairs_up", &stairsUp});
    v.push_back({"stairs_up_ceiling", &stairsUpCeil});
    v.push_back({"arrow", &arrow});
    static const char* vmNames[VM_COUNT] = {"fist", "dagger", "sword", "mace", "axe", "staff", "bow", "spear"};
    for (int i = 0; i < VM_COUNT; i++) v.push_back({std::string("hand_") + vmNames[i], &viewmodels[i]});
    for (size_t i = 0; i < gData.monsters.size(); i++) {
        v.push_back({"monster_" + gData.monsters[i].id, &monsters[i]});
        v.push_back({"monster_" + gData.monsters[i].id + "_attack", &monstersAttack[i]});
        if (i < monstersWalk.size())
            for (size_t f = 0; f < monstersWalk[i].size(); f++)
                v.push_back({"monster_" + gData.monsters[i].id + "_walk" + std::to_string(f + 1), &monstersWalk[i][f]});
        if (i < monstersDie.size())
            for (size_t f = 0; f < monstersDie[i].size(); f++)
                v.push_back({"monster_" + gData.monsters[i].id + "_die" + std::to_string(f + 1), &monstersDie[i][f]});
    }
    for (size_t i = 0; i < gData.classes.size(); i++) {
        v.push_back({"player_" + gData.classes[i].id, &players[i]});
        v.push_back({"player_" + gData.classes[i].id + "_attack", &playersAttack[i]});
        if (i < playersWalk.size())
            for (size_t f = 0; f < playersWalk[i].size(); f++)
                v.push_back({"player_" + gData.classes[i].id + "_walk" + std::to_string(f + 1), &playersWalk[i][f]});
    }
    for (size_t i = 0; i < gData.items.size(); i++) v.push_back({"item_" + gData.items[i].id, &items[i]});
    for (size_t i = 0; i < gData.spells.size(); i++) v.push_back({"spell_" + gData.spells[i].id, &spellOrbs[i]});
    return v;
}

static bool isPow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

namespace {
// Shrink a picture so neither side is over maxSide, averaging the pixels it covers (alpha-
// weighted). A pixel ends up see-through when less than half of what it covers was opaque,
// so edges stay crisp. Power-of-two sizes stay powers of two.
void shrinkToFit(Texture& t, int maxSide) {
    if (t.w <= maxSide && t.h <= maxSide) return;
    float f = (float)maxSide / std::max(t.w, t.h);
    int nw = std::max(1, (int)std::lround(t.w * f)), nh = std::max(1, (int)std::lround(t.h * f));
    if (isPow2(t.w) && isPow2(t.h)) { nw = t.w; nh = t.h; while (nw > maxSide || nh > maxSide) { nw /= 2; nh /= 2; } }
    Texture n(nw, nh);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            int x0 = x * t.w / nw, x1 = std::max(x0 + 1, (x + 1) * t.w / nw);
            int y0 = y * t.h / nh, y1 = std::max(y0 + 1, (y + 1) * t.h / nh);
            double r = 0, g = 0, b = 0, a = 0;
            int cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, cnt++) {
                    uint32_t c = t.px[yy * t.w + xx];
                    double al = (c >> 24) / 255.0;
                    r += ((c >> 16) & 255) * al; g += ((c >> 8) & 255) * al; b += (c & 255) * al; a += al;
                }
            if (a < cnt * 0.5) continue;   // mostly see-through
            n.px[y * nw + x] = 0xFF000000u | ((uint32_t)(r / a) << 16) | ((uint32_t)(g / a) << 8) | (uint32_t)(b / a);
        }
    n.computeTop();
    t = std::move(n);
}

bool loadPng(const std::string& path, Texture& out) {
    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s) return false;
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!c) return false;
    Texture n(c->w, c->h);
    for (int y = 0; y < c->h; y++)
        std::memcpy(&n.px[y * c->w], (uint8_t*)c->pixels + y * c->pitch, c->w * 4);
    SDL_FreeSurface(c);
    n.computeTop();
    shrinkToFit(n, MAX_ART_SIZE);   // draw big, the game scales it down
    out = std::move(n);
    return true;
}
bool isAnimFrame(const std::string& n) { return n.find("_walk") != std::string::npos || n.find("_die") != std::string::npos; }
}  // namespace

void Assets::loadOverrides(const std::string& dataDir) {
    // data/gfx/<name>.png replaces the generated picture with the same name.
    // Wall/floor/ceiling textures must have power-of-two sizes (e.g. 64x64, 128x128).
    int loadedAny = 0;
    for (auto& [name, tex] : namedTextures()) {
        if (isAnimFrame(name)) continue;   // animations are loaded as a series below
        std::string path = dataDir + "/gfx/" + name + ".png";
        Texture n;
        if (!loadPng(path, n)) continue;
        bool tiled = name.rfind("wall_", 0) == 0 || name.rfind("floor_", 0) == 0 || name.rfind("ceiling", 0) == 0 ||
                     name.rfind("stairs", 0) == 0 || name.rfind("door", 0) == 0 || name == "rubble";
        if (tiled && (!isPow2(n.w) || !isPow2(n.h))) {
            std::printf("art override %s ignored: size must be a power of two (64x64, 128x128...)\n", path.c_str());
            continue;
        }
        *tex = std::move(n);
        loadedAny++;
    }
    // Animations: <name>_walk1.png, _walk2.png, ... (and _die1.png ... for monsters), as many as
    // there are (up to 16). A series given as PNGs replaces the generated frames completely.
    std::vector<std::string> series;
    auto loadSeries = [&](const std::string& prefix, std::vector<Texture>& frames) {
        std::vector<Texture> got;
        for (int f = 1; f <= MAX_ANIM_FRAMES; f++) {
            Texture t;
            if (!loadPng(dataDir + "/gfx/" + prefix + std::to_string(f) + ".png", t)) break;
            got.push_back(std::move(t));
        }
        if (got.empty()) return;
        frames = std::move(got);
        series.push_back(prefix.substr(0, prefix.size() - 0));
    };
    for (size_t i = 0; i < monsters.size(); i++) {
        loadSeries("monster_" + gData.monsters[i].id + "_walk", monstersWalk[i]);
        loadSeries("monster_" + gData.monsters[i].id + "_die", monstersDie[i]);
    }
    for (size_t i = 0; i < players.size(); i++) loadSeries("player_" + gData.classes[i].id + "_walk", playersWalk[i]);
    // Frames that weren't given are remade from the (possibly replaced) normal pictures.
    if (loadedAny) buildWalkFrames(series);
}

namespace {
// Simple wildcard match: * matches any text (also none), everything else matches itself.
bool wildcard(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return wildcard(p + 1, s) || (*s && wildcard(p, s + 1));
    return *s && (*p == *s) && wildcard(p + 1, s + 1);
}
bool fileExists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}
}  // namespace

Assets::ExportResult Assets::exportPngs(const std::string& dir, const ExportOptions& o) {
    ExportResult r;
    int k = std::max(1, std::min(16, o.scale));
    for (auto& [name, tex] : namedTextures()) {
        if (!wildcard(o.pattern.c_str(), name.c_str())) continue;
        if (!o.anim && isAnimFrame(name)) continue;
        std::string path = dir + "/" + name + ".png";
        if (!o.force && fileExists(path)) { r.kept++; continue; }
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, tex->w * k, tex->h * k, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) { r.failed++; continue; }
        for (int y = 0; y < tex->h * k; y++) {
            uint32_t* row = (uint32_t*)((uint8_t*)s->pixels + y * s->pitch);
            for (int x = 0; x < tex->w * k; x++) row[x] = tex->px[(y / k) * tex->w + x / k];
        }
        if (IMG_SavePNG(s, path.c_str()) == 0) r.written++;
        else { r.failed++; std::printf("could not write %s: %s\n", path.c_str(), IMG_GetError()); }
        SDL_FreeSurface(s);
    }
    return r;
}
