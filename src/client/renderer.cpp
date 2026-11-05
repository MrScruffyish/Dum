// Dum - software renderer implementation.
#include "client/renderer.h"

#include <cstring>
#include <thread>

namespace {

// Classic 5x7 font, ASCII 32..126, column-major, bit 0 = top row.
const uint8_t kFont[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x08, 0x2A, 0x1C, 0x2A, 0x08}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08},
};

inline uint32_t shadePx(uint32_t c, int l) {
    uint32_t r = (((c >> 16) & 255) * l) >> 8, g = (((c >> 8) & 255) * l) >> 8, b = ((c & 255) * l) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
inline uint32_t addPx(uint32_t a, uint32_t b) {
    uint32_t r = std::min(255u, ((a >> 16) & 255) + ((b >> 16) & 255));
    uint32_t g = std::min(255u, ((a >> 8) & 255) + ((b >> 8) & 255));
    uint32_t bl = std::min(255u, (a & 255) + (b & 255));
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}
inline uint32_t blendPx(uint32_t d, uint32_t s, int a) {
    uint32_t rb = (((s & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8) & 0xFF00FF;
    uint32_t g = (((s & 0x00FF00) * a + (d & 0x00FF00) * (256 - a)) >> 8) & 0x00FF00;
    return 0xFF000000u | rb | g;
}

// Straight-alpha "over" blend for the transparent UI layer.
inline uint32_t overPx(uint32_t d, uint32_t s, int a) {
    if (a >= 255) return s | 0xFF000000u;
    int da = (int)(d >> 24);
    int oa = a + da * (255 - a) / 255;
    if (oa <= 0) return 0;
    auto ch = [&](int sh) {
        int sc = (int)((s >> sh) & 255), dc = (int)((d >> sh) & 255);
        return (uint32_t)((sc * a + dc * da * (255 - a) / 255) / oa);
    };
    return ((uint32_t)oa << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

}  // namespace

Renderer::Renderer(int w, int h) { resize(w, h); }

void Renderer::resize(int w, int h) {
    if (inUi_) endUi();
    W = worldW_ = w;
    H = worldH_ = h;
    fb.assign(W * H, gpu_ ? 0u : 0xFF000000u);
    dirty_[0] = dirty_[1] = 0; dirty_[2] = W; dirty_[3] = H;
    zbuf.assign(W, 1e9f);
    layered_ = w != UI_W || h != UI_H;
    uiFb_.assign(layered_ ? UI_W * UI_H : 0, 0);
}

void Renderer::setGpu(bool on) {
    if (inUi_) endUi();
    gpu_ = on;
    std::fill(fb.begin(), fb.end(), on ? 0u : 0xFF000000u);
    dirty_[0] = dirty_[1] = 0; dirty_[2] = W; dirty_[3] = H;
    scene_ = WorldScene();
}

void Renderer::beginFrame() {
    uiUsed_ = false;
    if (!gpu_) return;
    if (inUi_) endUi();
    scene_.valid = false;
    scene_.sprites.clear();
    // Clear what was drawn last frame; it must be sent again (now empty) as well.
    for (int y = dirty_[1]; y < dirty_[3]; y++) std::fill(&fb[y * W + dirty_[0]], &fb[y * W + dirty_[2]], 0u);
    std::copy(dirty_, dirty_ + 4, shown_);
    dirty_[0] = W; dirty_[1] = H; dirty_[2] = 0; dirty_[3] = 0;
}

void Renderer::markDirty(int x0, int y0, int x1, int y1) {
    if (!gpu_ || inUi_) return;
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(W, x1); y1 = std::min(H, y1);
    if (x0 >= x1 || y0 >= y1) return;
    dirty_[0] = std::min(dirty_[0], x0); dirty_[1] = std::min(dirty_[1], y0);
    dirty_[2] = std::max(dirty_[2], x1); dirty_[3] = std::max(dirty_[3], y1);
}

void Renderer::takeWorldDirty(int& x0, int& y0, int& x1, int& y1) {
    // Union of this frame's drawing and last frame's (which was cleared).
    x0 = std::min(dirty_[0], shown_[0]); y0 = std::min(dirty_[1], shown_[1]);
    x1 = std::max(dirty_[2], shown_[2]); y1 = std::max(dirty_[3], shown_[3]);
    std::copy(dirty_, dirty_ + 4, shown_);   // a second present in the same frame sends nothing new
    if (x0 >= x1 || y0 >= y1) { x0 = y0 = x1 = y1 = 0; }
}

void Renderer::beginUi() {
    if (!layered_ || inUi_) return;
    fb.swap(uiFb_);
    W = UI_W;
    H = UI_H;
    inUi_ = true;
    if (!uiUsed_) std::fill(fb.begin(), fb.end(), 0u);
    uiUsed_ = true;
}

void Renderer::endUi() {
    if (!inUi_) return;
    fb.swap(uiFb_);
    W = worldW_;
    H = worldH_;
    inUi_ = false;
}

std::vector<uint32_t> Renderer::composite() const {
    const std::vector<uint32_t>& world = inUi_ ? uiFb_ : fb;
    const std::vector<uint32_t>& ui = inUi_ ? fb : uiFb_;
    std::vector<uint32_t> out = world;
    if (!layered_ || !uiUsed_) return out;
    for (int y = 0; y < worldH_; y++)
        for (int x = 0; x < worldW_; x++) {
            uint32_t u = ui[(y * UI_H / worldH_) * UI_W + x * UI_W / worldW_];
            uint32_t& d = out[y * worldW_ + x];
            d = blendPx(d, u, (int)(u >> 24));
        }
    return out;
}

// ------------------------------------------------------------- lighting ---

void Renderer::setupLights(const Map& m, const std::vector<LightSource>& lights) {
    mapW_ = m.w;
    mapH_ = m.h;
    ambient_.assign(m.w * m.h, 0.0f);
    for (int i = 0; i < m.w * m.h; i++)
        if (m.flags[i] & F_OUTDOOR) ambient_[i] = 1.0f;   // daylight
        else if (m.flags[i] & F_LIT) ambient_[i] = 0.55f;
    lights_.clear();
    for (auto& s : lights) {
        if (s.radius <= 0.1f) continue;
        LightInfo li;
        li.s = s;
        li.invR2 = 1.0f / (s.radius * s.radius);
        li.vis.assign(m.w * m.h, 0);
        int r = (int)std::ceil(s.radius) + 1;
        int cx = (int)s.pos.x, cy = (int)s.pos.y;
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (!m.inside(x, y)) continue;
                Vec2 c(x + 0.5f, y + 0.5f);
                if (dist(c, s.pos) > s.radius + 0.8f) continue;
                if (m.los(s.pos, c)) li.vis[y * m.w + x] = 1;
            }
        lights_.push_back(std::move(li));
    }
    tileLight_.assign(m.w * m.h, 0.0f);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) tileLight_[y * m.w + x] = lightAt(Vec2(x + 0.5f, y + 0.5f), y * m.w + x);
}

float Renderer::lightAt(Vec2 p, int tile) const {
    if (tile < 0 || tile >= (int)ambient_.size()) return 0;
    float l = ambient_[tile];
    for (auto& li : lights_) {
        if (!li.vis[tile]) continue;
        float dx = p.x - li.s.pos.x, dy = p.y - li.s.pos.y;
        float f = 1.0f - (dx * dx + dy * dy) * li.invR2;
        if (f > 0) l += f * li.s.intensity;
    }
    return l > 1.4f ? 1.4f : l;
}

// ---------------------------------------------------------------- world ---

bool Renderer::project(Vec2 p, float& sx, float& depth) const {
    Vec2 d = p - cam_;
    float invDet = 1.0f / (plane_.x * dir_.y - dir_.x * plane_.y);
    float tx = invDet * (dir_.y * d.x - dir_.x * d.y);
    float ty = invDet * (-plane_.y * d.x + plane_.x * d.y);
    if (ty <= 0.05f) return false;
    sx = (W / 2.0f) * (1 + tx / ty);
    depth = ty;
    return true;
}

void Renderer::drawWorld(Map& m, const Assets& a, Vec2 cam, float angle, float horizon, std::vector<SpriteDraw>& sprites) {
    cam_ = cam;
    dir_ = dirFromAngle(angle);
    float planeLen = W / (2.0f * H);
    plane_ = Vec2(-dir_.y, dir_.x) * planeLen;
    horizon_ = horizon;
    const float posZ = 0.5f * H;
    if (gpu_) {
        scene_.valid = true;
        scene_.map = &m;
        scene_.assets = &a;
        scene_.cam = cam; scene_.dir = dir_; scene_.plane = plane_;
        scene_.horizon = horizon;
    }

    // Floor and ceiling (row by row), split across CPU cores.
    Vec2 r0 = dir_ - plane_, r1 = dir_ + plane_;
    if (!gpu_) {
    auto rows = [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            bool isFloor = y > horizon;
            float p = isFloor ? (y - horizon) : (horizon - y);
            uint32_t* row = &fb[y * W];
            if (p < 0.5f) { for (int x = 0; x < W; x++) row[x] = 0xFF000000; continue; }
            float rowDist = posZ / p;
            float stepX = rowDist * (r1.x - r0.x) / W, stepY = rowDist * (r1.y - r0.y) / W;
            float fx = cam.x + rowDist * r0.x, fy = cam.y + rowDist * r0.y;
            for (int x = 0; x < W; x++, fx += stepX, fy += stepY) {
                int cx = (int)std::floor(fx), cy = (int)std::floor(fy);
                if (!m.inside(cx, cy)) { row[x] = 0xFF000000; continue; }
                int idx = cy * m.w + cx;
                float l = lightAt(Vec2(fx, fy), idx);
                if (l < 0.01f) { row[x] = 0xFF000000; continue; }
                const Texture* t;
                uint8_t tile = m.tiles[idx];
                if (isFloor) {
                    t = tile == T_STAIRS_DOWN ? &a.stairsDown : tile == T_STAIRS_UP ? &a.stairsUp : &a.floors[m.floorTex[idx] & 7];
                } else if (m.flags[idx] & F_OUTDOOR) {
                    // Open sky: a vertical gradient, lighter toward the horizon.
                    float k = clampf((horizon - y) / (H * 0.5f), 0.0f, 1.0f);
                    row[x] = colMix(0xFF9AB8D8, 0xFF2A4A8A, k);
                    continue;
                } else {
                    t = tile == T_STAIRS_UP ? &a.stairsUpCeil : (m.floorTex[idx] == FT_DIRT ? &a.ceilingCave : &a.ceiling);
                    l *= 0.8f;
                }
                int tx = (int)((fx - cx) * t->w), ty = (int)((fy - cy) * t->h);
                row[x] = shadePx(t->get(tx, ty), (int)(l * 256));
            }
        }
    };
    int nThreads = (int)std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    if (nThreads <= 1) {
        rows(0, H);
    } else {
        std::vector<std::thread> pool;
        int chunk = (H + nThreads - 1) / nThreads;
        for (int i = 1; i < nThreads; i++) pool.emplace_back(rows, std::min(H, i * chunk), std::min(H, (i + 1) * chunk));
        rows(0, std::min(H, chunk));
        for (auto& t : pool) t.join();
    }
    }   // !gpu_

    // Walls (one ray per column).
    for (int x = 0; x < W; x++) {
        float camX = 2.0f * x / W - 1.0f;
        Vec2 rd = dir_ + plane_ * camX;
        int mx = (int)std::floor(cam.x), my = (int)std::floor(cam.y);
        float ddx = rd.x == 0 ? 1e30f : std::fabs(1.0f / rd.x);
        float ddy = rd.y == 0 ? 1e30f : std::fabs(1.0f / rd.y);
        int stepX, stepY;
        float sdx, sdy;
        if (rd.x < 0) { stepX = -1; sdx = (cam.x - mx) * ddx; } else { stepX = 1; sdx = (mx + 1.0f - cam.x) * ddx; }
        if (rd.y < 0) { stepY = -1; sdy = (cam.y - my) * ddy; } else { stepY = 1; sdy = (my + 1.0f - cam.y) * ddy; }
        int side = 0, px = mx, py = my;
        bool hit = false;
        for (int guard = 0; guard < 128; guard++) {
            if (m.inside(mx, my) && tileLight_[my * m.w + mx] > 0.08f) m.flags[my * m.w + mx] |= F_SEEN;
            px = mx; py = my;
            if (sdx < sdy) { sdx += ddx; mx += stepX; side = 0; }
            else { sdy += ddy; my += stepY; side = 1; }
            if (!m.inside(mx, my)) break;
            if (Map::tileSolid(m.tiles[my * m.w + mx])) { hit = true; break; }
        }
        if (!hit) { zbuf[x] = 1e9f; continue; }
        float perp = side == 0 ? (sdx - ddx) : (sdy - ddy);
        if (perp < 0.01f) perp = 0.01f;
        zbuf[x] = perp;
        int idx = my * m.w + mx;
        uint8_t tile = m.tiles[idx];
        const Texture* t = tile == T_DOOR ? &a.door : tile == T_DOOR_LOCKED ? &a.doorLocked : tile == T_RUBBLE ? &a.rubble
                         : tile == T_DOOR_KEY ? &a.keyDoors[m.keyDoorColor(mx, my)]
                         : tile == T_SHOP ? &a.shopDoors[m.floorTex[idx] & 7] : &a.walls[m.wallTex[idx] & 7];
        float wallX = side == 0 ? cam.y + perp * rd.y : cam.x + perp * rd.x;
        wallX -= std::floor(wallX);
        int texX = (int)(wallX * t->w);
        if ((side == 0 && rd.x > 0) || (side == 1 && rd.y < 0)) texX = t->w - texX - 1;
        Vec2 hp = cam + rd * perp;
        int lightTile = m.inside(px, py) ? py * m.w + px : idx;
        float l = lightAt(hp, lightTile) * (side == 1 ? 0.78f : 1.0f);
        if (l > 0.06f) m.flags[idx] |= F_SEEN;
        if (gpu_) continue;   // the GPU draws the wall; we only needed the depth and what was seen
        int li = (int)(l * 256);
        float lineH = H / perp;
        float top = horizon - lineH / 2;
        int y0 = std::max(0, (int)std::ceil(top)), y1 = std::min(H, (int)std::ceil(top + lineH));
        float texStep = t->h / lineH;
        float texPos = (y0 - top) * texStep;
        if (li <= 2) {
            for (int y = y0; y < y1; y++) fb[y * W + x] = 0xFF000000;
            continue;
        }
        for (int y = y0; y < y1; y++, texPos += texStep) fb[y * W + x] = shadePx(t->get(texX, (int)texPos), li);
    }

    if (gpu_) spriteVisibility(sprites);
    else drawSprites(sprites);
}

void Renderer::spriteVisibility(std::vector<SpriteDraw>& sprites) {
    for (auto& s : sprites) s.dist2 = (s.pos - cam_).len2();
    std::sort(sprites.begin(), sprites.end(), [](const SpriteDraw& a, const SpriteDraw& b) { return a.dist2 > b.dist2; });
    for (auto& s : sprites) {
        s.visible = false;
        if (!s.tex || s.tex->w == 0) continue;
        float sx, depth;
        if (!project(s.pos, sx, depth)) continue;
        float unit = H / depth;
        float sh = unit * s.height;
        float sw = sh * s.tex->w / s.tex->h;
        float bottom = horizon_ + unit * 0.5f - unit * s.zoff;
        float top = bottom - sh;
        int x0 = (int)(sx - sw / 2), x1 = (int)(sx + sw / 2);
        int y0 = (int)top, y1 = (int)bottom;
        if (x1 < 0 || x0 >= W || y1 < 0 || y0 >= H) continue;
        float light = 1.0f;
        if (!s.fullbright) {
            int tx = (int)s.pos.x, ty = (int)s.pos.y;
            light = lightAt(s.pos, (ty >= 0 && ty < mapH_ && tx >= 0 && tx < mapW_) ? ty * mapW_ + tx : -1);
            if (light < 4 / 256.0f) continue;
        }
        s.light = light * s.brightness;
        bool any = false;
        for (int x = std::max(0, x0); x < std::min(W, x1) && !any; x++) any = depth < zbuf[x];
        if (!any) continue;
        s.visible = true;
        int texTop = s.tex->top;
        s.screenX0 = std::max(0, x0); s.screenX1 = std::min(W - 1, x1);
        s.screenY0 = std::max(0, y0 + (int)(texTop * sh / s.tex->h)); s.screenY1 = std::min(H - 1, y1);
        scene_.sprites.push_back(s);
    }
}

void Renderer::drawSprites(std::vector<SpriteDraw>& sprites) {
    for (auto& s : sprites) s.dist2 = (s.pos - cam_).len2();
    std::sort(sprites.begin(), sprites.end(), [](const SpriteDraw& a, const SpriteDraw& b) { return a.dist2 > b.dist2; });
    for (auto& s : sprites) {
        s.visible = false;
        if (!s.tex || s.tex->w == 0) continue;
        float sx, depth;
        if (!project(s.pos, sx, depth)) continue;
        float unit = H / depth;
        float sh = unit * s.height;
        float sw = sh * s.tex->w / s.tex->h;
        float bottom = horizon_ + unit * 0.5f - unit * s.zoff;
        float top = bottom - sh;
        int x0 = (int)(sx - sw / 2), x1 = (int)(sx + sw / 2);
        int y0 = (int)top, y1 = (int)bottom;
        if (x1 < 0 || x0 >= W || y1 < 0 || y0 >= H) continue;
        int light = 256;
        if (!s.fullbright) {
            int tx = (int)s.pos.x, ty = (int)s.pos.y;
            light = (int)(lightAt(s.pos, (ty >= 0 && ty < mapH_ && tx >= 0 && tx < mapW_) ? ty * mapW_ + tx : -1) * 256);
            if (light < 4) continue;
        }
        light = (int)(light * s.brightness);
        int tintA = (int)(s.tintAmt * 256);
        bool any = false;
        int texTop = s.tex->top;
        for (int x = std::max(0, x0); x < std::min(W, x1); x++) {
            if (depth >= zbuf[x]) continue;
            int texX = (int)((x - (sx - sw / 2)) * s.tex->w / sw);
            if (texX < 0 || texX >= s.tex->w) continue;
            for (int y = std::max(0, y0); y < std::min(H, y1); y++) {
                int texY = (int)((y - top) * s.tex->h / sh);
                if (texY < texTop || texY >= s.tex->h) continue;
                uint32_t c = s.tex->px[texY * s.tex->w + texX];
                if (!(c >> 24)) continue;
                if (s.dither && ((x + y) & 1)) continue;
                any = true;
                uint32_t& d = fb[y * W + x];
                if (s.additive) { d = addPx(d, shadePx(c, light)); continue; }
                c = shadePx(c, light);
                if (tintA) c = blendPx(c, s.tint, tintA);
                d = c;
            }
        }
        if (any) {
            s.visible = true;
            s.screenX0 = std::max(0, x0); s.screenX1 = std::min(W - 1, x1);
            s.screenY0 = std::max(0, y0 + (int)(texTop * sh / s.tex->h)); s.screenY1 = std::min(H - 1, y1);
        }
    }
}

// ------------------------------------------------------------------- 2D ---

void Renderer::fill(int x, int y, int w, int h, uint32_t c, int alpha) {
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(W, x + w), y1 = std::min(H, y + h);
    markDirty(x0, y0, x1, y1);
    bool ov = overlay();
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            uint32_t& d = fb[j * W + i];
            d = alpha >= 255 ? (c | 0xFF000000u) : ov ? overPx(d, c, alpha) : blendPx(d, c, alpha);
        }
}

void Renderer::frame(int x, int y, int w, int h, uint32_t c) {
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

void Renderer::text(int x, int y, const std::string& s, uint32_t c, int scale, bool shadow) {
    if (shadow) text(x + scale, y + scale, s, 0xFF000000, scale, false);
    markDirty(x, y, x + textW(s, scale), y + 8 * scale);
    int cx = x;
    for (char ch : s) {
        int i = (unsigned char)ch - 32;
        if (i < 0 || i >= 95) i = '?' - 32;
        for (int col = 0; col < 5; col++) {
            uint8_t bits = kFont[i][col];
            for (int row = 0; row < 7; row++)
                if (bits & (1 << row)) {
                    int px = cx + col * scale, py = y + row * scale;
                    for (int a = 0; a < scale; a++)
                        for (int b = 0; b < scale; b++) {
                            int X = px + a, Y = py + b;
                            if (X >= 0 && Y >= 0 && X < W && Y < H) fb[Y * W + X] = c | 0xFF000000u;
                        }
                }
        }
        cx += 6 * scale;
    }
}

void Renderer::blit(const Texture& t, int x, int y, int w, int h, float light, bool additive) {
    int li = (int)(light * 256);
    markDirty(x, y, x + w, y + h);
    bool ov = overlay();
    for (int j = std::max(0, y); j < std::min(H, y + h); j++) {
        int ty = (j - y) * t.h / h;
        for (int i = std::max(0, x); i < std::min(W, x + w); i++) {
            int tx = (i - x) * t.w / w;
            uint32_t c = t.px[ty * t.w + tx];
            if (!(c >> 24)) continue;
            uint32_t& d = fb[j * W + i];
            if (additive && ov) {
                uint32_t sc = shadePx(c, li);
                uint32_t m = std::max({(sc >> 16) & 255, (sc >> 8) & 255, sc & 255});
                d = overPx(d, sc, (int)m);
            } else {
                d = additive ? addPx(d, shadePx(c, li)) : shadePx(c, li);
            }
        }
    }
}

void Renderer::darken(float f) {
    int l = (int)(f * 256);
    markDirty(0, 0, W, H);
    if (overlay()) { for (auto& p : fb) p = overPx(p, 0xFF000000, clampi(256 - l, 0, 255)); return; }
    for (auto& p : fb) p = shadePx(p, l);
}

void Renderer::tintScreen(uint32_t c, int alpha) {
    markDirty(0, 0, W, H);
    if (overlay()) { for (auto& p : fb) p = overPx(p, c, alpha); return; }
    for (auto& p : fb) p = blendPx(p, c, alpha);
}
