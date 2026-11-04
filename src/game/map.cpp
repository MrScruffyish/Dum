// Dum - map utilities and dungeon generator (rooms + corridors + caves, Angband style).
#include "game/map.h"

#include <queue>

void Map::resize(int w_, int h_) {
    w = w_;
    h = h_;
    tiles.assign(w * h, T_ROCK);
    flags.assign(w * h, 0);
    wallTex.assign(w * h, WT_BRICK);
    floorTex.assign(w * h, FT_STONE);
    rooms.clear();
}

bool Map::los(Vec2 a, Vec2 b) const {
    Vec2 d = b - a;
    float L = d.len();
    if (L < 1e-4f) return true;
    int steps = (int)(L / 0.15f) + 1;
    int ax = (int)std::floor(a.x), ay = (int)std::floor(a.y);
    int bx = (int)std::floor(b.x), by = (int)std::floor(b.y);
    for (int i = 1; i < steps; i++) {
        Vec2 p = a + d * ((float)i / steps);
        int x = (int)std::floor(p.x), y = (int)std::floor(p.y);
        if ((x == ax && y == ay) || (x == bx && y == by)) continue;
        if (opaque(x, y)) return false;
    }
    return true;
}

bool Map::circleFree(Vec2 p, float r) const {
    int x0 = (int)std::floor(p.x - r), x1 = (int)std::floor(p.x + r);
    int y0 = (int)std::floor(p.y - r), y1 = (int)std::floor(p.y + r);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (solid(x, y)) return false;
    return true;
}

bool Map::moveCircle(Vec2& pos, Vec2 delta, float r) const {
    bool free = true;
    // Sub-step so fast movement can't tunnel through walls.
    int steps = (int)(delta.len() / 0.25f) + 1;
    Vec2 step = delta * (1.0f / steps);
    for (int i = 0; i < steps; i++) {
        Vec2 nx{pos.x + step.x, pos.y};
        if (circleFree(nx, r)) pos.x = nx.x; else free = false;
        Vec2 ny{pos.x, pos.y + step.y};
        if (circleFree(ny, r)) pos.y = ny.y; else free = false;
    }
    return free;
}

void Map::serialize(Writer& wr, bool hideSecrets) const {
    wr.u16((uint16_t)w);
    wr.u16((uint16_t)h);
    wr.u8((uint8_t)depth);
    wr.u32(seed);
    wr.u16((uint16_t)startX); wr.u16((uint16_t)startY);
    for (int i = 0; i < w * h; i++) {
        uint8_t t = tiles[i];
        if (hideSecrets && t == T_DOOR_SECRET) t = T_WALL;
        wr.u8(t);
    }
    for (int i = 0; i < w * h; i++) wr.u8(flags[i] & (F_LIT | F_ROOM | F_CORRIDOR | F_OUTDOOR));
    for (int i = 0; i < w * h; i++) wr.u8((uint8_t)(wallTex[i] | (floorTex[i] << 4)));
}

bool Map::deserialize(Reader& r) {
    int nw = r.u16(), nh = r.u16();
    if (!r.ok || nw <= 0 || nh <= 0 || nw > 512 || nh > 512) return false;
    resize(nw, nh);
    depth = r.u8();
    seed = r.u32();
    startX = r.u16(); startY = r.u16();
    for (int i = 0; i < w * h; i++) tiles[i] = r.u8();
    for (int i = 0; i < w * h; i++) flags[i] = r.u8();
    for (int i = 0; i < w * h; i++) { uint8_t t = r.u8(); wallTex[i] = t & 15; floorTex[i] = t >> 4; }
    return r.ok;
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------
namespace {

struct Gen {
    Map& m;
    Rng rng;
    int depth;
    std::vector<int> roomId;   // -1 = none

    Gen(Map& map, uint32_t seed, int d) : m(map), rng(seed), depth(d) {}

    bool isFloorish(int x, int y) const {
        uint8_t t = m.at(x, y);
        return !Map::tileSolid(t) || Map::tileIsDoor(t) || t == T_DOOR_SECRET;
    }

    bool areaFree(int x, int y, int w, int h) const {
        for (int j = y - 2; j < y + h + 2; j++)
            for (int i = x - 2; i < x + w + 2; i++) {
                if (i < 1 || j < 1 || i >= m.w - 1 || j >= m.h - 1) return false;
                if (m.at(i, j) != T_ROCK) return false;
            }
        return true;
    }

    void carveFloor(int x, int y, int room, uint8_t extraFlags, uint8_t ftex) {
        if (x < 1 || y < 1 || x >= m.w - 1 || y >= m.h - 1) return;
        m.set(x, y, T_FLOOR);
        m.flags[y * m.w + x] |= extraFlags;
        m.floorTex[y * m.w + x] = ftex;
        if (room >= 0) roomId[y * m.w + x] = room;
    }

    uint8_t themeWall() {
        if (depth <= 2) return rng.chance(70) ? WT_BRICK : (rng.chance(50) ? WT_WOOD : WT_MOSS);
        if (depth <= 5) return rng.chance(40) ? WT_BRICK : (rng.chance(50) ? WT_MOSS : WT_DARK);
        return rng.chance(50) ? WT_DARK : (rng.chance(60) ? WT_BLOOD : WT_BRICK);
    }

    bool placeRoom() {
        // Room shapes: mostly round (ovals, rounded rectangles, octagons), some plain
        // rectangles and crosses, and caves.
        int kind = rng.range(0, 99);
        enum { OVAL, ROUNDED, OCTAGON, RECT, CROSS, CAVE } shape =
            kind < 28 ? OVAL : kind < 56 ? ROUNDED : kind < 70 ? OCTAGON : kind < 80 ? RECT : kind < 86 ? CROSS : CAVE;
        int w = rng.range(5, 11), h = rng.range(5, 9);
        if (shape == OVAL || shape == CAVE) { w = rng.range(7, 13); h = rng.range(6, 11); }
        int x = rng.range(2, m.w - w - 3), y = rng.range(2, m.h - h - 3);
        if (!areaFree(x, y, w, h)) return false;

        Room r{x, y, w, h, rng.chance(std::max(10, 85 - depth * 12))};
        int id = (int)m.rooms.size();
        uint8_t fl = F_ROOM | (r.lit ? F_LIT : 0);
        uint8_t wt = themeWall();
        uint8_t ft = (uint8_t)(wt == WT_WOOD ? FT_WOOD : (rng.chance(30) ? FT_TILES : FT_STONE));

        auto mark = [&](int i, int j) { carveFloor(i, j, id, fl, ft); };
        if (shape == RECT) {  // rectangle, maybe with pillars
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++) mark(i, j);
            if (w >= 7 && h >= 7 && rng.chance(35)) {
                for (int j = y + 2; j < y + h - 2; j += 3)
                    for (int i = x + 2; i < x + w - 2; i += 3) m.set(i, j, T_WALL);
            }
        } else if (shape == ROUNDED) {  // rectangle with round corners
            float rad = std::max(1.5f, std::min(w, h) / 2.6f);
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++) {
                    float px = i + 0.5f, py = j + 0.5f;
                    float cx = clampf(px, rad, w - rad), cy = clampf(py, rad, h - rad);
                    if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= rad * rad + 0.1f) mark(x + i, y + j);
                }
            if (w >= 9 && h >= 7 && rng.chance(25)) m.set(x + w / 2, y + h / 2, T_WALL);  // central pillar
        } else if (shape == OCTAGON) {  // corners cut diagonally
            int cut = std::max(1, std::min(w, h) / 3);
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    if (std::min(i, w - 1 - i) + std::min(j, h - 1 - j) >= cut) mark(x + i, y + j);
        } else if (shape == CROSS) {
            int hw = std::max(1, w / 4), hh = std::max(1, h / 4);
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++)
                    if ((i >= x + hw && i < x + w - hw) || (j >= y + hh && j < y + h - hh)) mark(i, j);
        } else if (shape == OVAL) {
            float cx = x + w / 2.0f, cy = y + h / 2.0f, rx = w / 2.0f, ry = h / 2.0f;
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++) {
                    float dx = (i + 0.5f - cx) / rx, dy = (j + 0.5f - cy) / ry;
                    if (dx * dx + dy * dy <= 1.08f) mark(i, j);
                }
        } else {  // cave blob via cellular automaton
            std::vector<uint8_t> c(w * h);
            for (auto& v : c) v = rng.chance(58);
            for (int it = 0; it < 4; it++) {
                std::vector<uint8_t> n(w * h);
                for (int j = 0; j < h; j++)
                    for (int i = 0; i < w; i++) {
                        int cnt = 0;
                        for (int dj = -1; dj <= 1; dj++)
                            for (int di = -1; di <= 1; di++) {
                                int a = i + di, b = j + dj;
                                if (a < 0 || b < 0 || a >= w || b >= h) continue;
                                cnt += c[b * w + a];
                            }
                        n[j * w + i] = cnt >= 5;
                    }
                c.swap(n);
            }
            // Always keep the center open so corridors can connect.
            for (int j = h / 2 - 1; j <= h / 2 + 1; j++)
                for (int i = w / 2 - 1; i <= w / 2 + 1; i++) c[j * w + i] = 1;
            // Keep only the part connected to the center.
            std::vector<uint8_t> keep(w * h);
            std::queue<std::pair<int, int>> q;
            q.push({w / 2, h / 2});
            keep[(h / 2) * w + w / 2] = 1;
            while (!q.empty()) {
                auto [i, j] = q.front();
                q.pop();
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; k++) {
                    int a = i + dx[k], b = j + dy[k];
                    if (a < 0 || b < 0 || a >= w || b >= h || keep[b * w + a] || !c[b * w + a]) continue;
                    keep[b * w + a] = 1;
                    q.push({a, b});
                }
            }
            wt = WT_CAVE;
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    if (keep[j * w + i]) carveFloor(x + i, y + j, id, fl, FT_DIRT);
            r.lit = false;
        }
        // Theme the walls around this room.
        for (int j = y - 1; j <= y + h; j++)
            for (int i = x - 1; i <= x + w; i++)
                if (m.inside(i, j)) m.wallTex[j * m.w + i] = wt;
        m.rooms.push_back(r);
        return true;
    }

    // Find a floor tile of a room close to its center.
    void roomAnchor(const Room& r, int& ox, int& oy) {
        ox = r.cx(); oy = r.cy();
        for (int rad = 0; rad < 8; rad++)
            for (int j = r.cy() - rad; j <= r.cy() + rad; j++)
                for (int i = r.cx() - rad; i <= r.cx() + rad; i++)
                    if (m.at(i, j) == T_FLOOR && (m.flag(i, j) & F_ROOM)) { ox = i; oy = j; return; }
    }

    void tunnel(int x0, int y0, int x1, int y1) {
        int x = x0, y = y0;
        bool horizFirst = rng.chance(50);
        int guard = 0;
        auto step = [&](int nx, int ny) {
            x = nx; y = ny;
            if (m.at(x, y) == T_ROCK) carveFloor(x, y, -1, F_CORRIDOR, FT_STONE);
            else if (m.at(x, y) == T_WALL && !(m.flag(x, y) & F_ROOM)) carveFloor(x, y, -1, F_CORRIDOR, FT_STONE);
        };
        // Occasionally wiggle to make corridors less straight.
        while ((x != x1 || y != y1) && guard++ < 1000) {
            bool moveH = horizFirst ? (x != x1) : (y == y1);
            if (rng.chance(12)) moveH = !moveH;
            if (moveH && x != x1) step(x + (x1 > x ? 1 : -1), y);
            else if (y != y1) step(x, y + (y1 > y ? 1 : -1));
            else step(x + (x1 > x ? 1 : -1), y);
        }
    }

    void placeDoors() {
        std::vector<std::pair<int, int>> placed;
        for (int y = 1; y < m.h - 1; y++)
            for (int x = 1; x < m.w - 1; x++) {
                if (m.at(x, y) != T_FLOOR || !(m.flag(x, y) & F_CORRIDOR)) continue;
                bool horiz = Map::tileSolid(m.at(x, y - 1)) && Map::tileSolid(m.at(x, y + 1)) &&
                             !Map::tileSolid(m.at(x - 1, y)) && !Map::tileSolid(m.at(x + 1, y));
                bool vert = Map::tileSolid(m.at(x - 1, y)) && Map::tileSolid(m.at(x + 1, y)) &&
                            !Map::tileSolid(m.at(x, y - 1)) && !Map::tileSolid(m.at(x, y + 1));
                if (!horiz && !vert) continue;
                // One side must be a room.
                bool roomSide = horiz ? ((m.flag(x - 1, y) | m.flag(x + 1, y)) & F_ROOM)
                                      : ((m.flag(x, y - 1) | m.flag(x, y + 1)) & F_ROOM);
                if (!roomSide) continue;
                bool near = false;
                for (auto& p : placed)
                    if (std::abs(p.first - x) <= 1 && std::abs(p.second - y) <= 1) near = true;
                if (near || !rng.chance(70)) continue;
                int r = rng.range(0, 99);
                int lockedPct = std::min(25, 6 + depth * 2);
                uint8_t t = r < lockedPct ? T_DOOR_LOCKED : (r < lockedPct + 12 ? T_DOOR_SECRET : (r < 85 ? T_DOOR : T_DOOR_OPEN));
                m.set(x, y, t);
                m.wallTex[y * m.w + x] = m.wallTex[(y + (horiz ? 1 : 0)) * m.w + x + (horiz ? 0 : 1)];
                placed.push_back({x, y});
            }
    }

    void placeRubble() {
        for (int y = 1; y < m.h - 1; y++)
            for (int x = 1; x < m.w - 1; x++)
                if (m.at(x, y) == T_FLOOR && (m.flag(x, y) & F_CORRIDOR) && rng.chance(1))
                    m.set(x, y, T_RUBBLE);
    }

    void buildWalls() {
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                if (m.at(x, y) != T_ROCK) continue;
                bool adj = false;
                for (int dy = -1; dy <= 1 && !adj; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (isFloorish(x + dx, y + dy) && m.at(x + dx, y + dy) != T_ROCK && m.at(x + dx, y + dy) != T_WALL) { adj = true; break; }
                if (adj) {
                    m.set(x, y, T_WALL);
                    // Corridors get cave-like walls deeper down.
                    if (!(m.flag(x, y) & F_ROOM) && depth >= 3 && m.wallTex[y * m.w + x] == WT_BRICK && rng.chance(40))
                        m.wallTex[y * m.w + x] = WT_CAVE;
                }
            }
    }

    // BFS distance from (sx,sy); doors/rubble count as passable.
    std::vector<int> distances(int sx, int sy) {
        std::vector<int> d(m.w * m.h, -1);
        std::queue<int> q;
        d[sy * m.w + sx] = 0;
        q.push(sy * m.w + sx);
        while (!q.empty()) {
            int c = q.front();
            q.pop();
            int x = c % m.w, y = c / m.w;
            const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int nx = x + dx[k], ny = y + dy[k];
                if (!m.inside(nx, ny) || d[ny * m.w + nx] >= 0) continue;
                uint8_t t = m.at(nx, ny);
                if (t == T_ROCK || t == T_WALL) continue;
                d[ny * m.w + nx] = d[c] + 1;
                q.push(ny * m.w + nx);
            }
        }
        return d;
    }

    bool run() {
        m.resize(72, 56);
        roomId.assign(m.w * m.h, -1);
        int target = rng.range(9, 14);
        for (int tries = 0; tries < 600 && (int)m.rooms.size() < target; tries++) placeRoom();
        if (m.rooms.size() < 4) return false;

        // Connect rooms: chain sorted by x, plus a few loops.
        std::vector<int> order(m.rooms.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return m.rooms[a].cx() < m.rooms[b].cx(); });
        for (size_t i = 0; i + 1 < order.size(); i++) {
            int ax, ay, bx, by;
            roomAnchor(m.rooms[order[i]], ax, ay);
            roomAnchor(m.rooms[order[i + 1]], bx, by);
            tunnel(ax, ay, bx, by);
        }
        int extra = rng.range(2, 4);
        for (int i = 0; i < extra; i++) {
            int a = rng.range(0, (int)m.rooms.size() - 1), b = rng.range(0, (int)m.rooms.size() - 1);
            if (a == b) continue;
            int ax, ay, bx, by;
            roomAnchor(m.rooms[a], ax, ay);
            roomAnchor(m.rooms[b], bx, by);
            tunnel(ax, ay, bx, by);
        }
        buildWalls();
        placeDoors();
        placeRubble();

        // Stairs: up in first room, down in the farthest reachable room.
        int sx, sy;
        roomAnchor(m.rooms[0], sx, sy);
        auto d = distances(sx, sy);
        int best = -1, bx = sx, by = sy;
        for (auto& r : m.rooms) {
            int x, y;
            roomAnchor(r, x, y);
            int dd = d[y * m.w + x];
            if (dd > best) { best = dd; bx = x; by = y; }
        }
        if (best < 10) return false;
        for (auto& r : m.rooms) {  // every room must be reachable
            int x, y;
            roomAnchor(r, x, y);
            if (d[y * m.w + x] < 0) return false;
        }
        m.set(sx, sy, T_STAIRS_UP);
        m.set(bx, by, T_STAIRS_DOWN);
        m.startX = sx; m.startY = sy;
        m.downX = bx; m.downY = by;
        return true;
    }
};

}  // namespace

// The town: an open-air walled square with shops around a central staircase.
void generateTown(Map& m, uint32_t seed) {
    Rng rng(seed ^ 0x70E4u);
    m.resize(48, 36);
    m.depth = 0;
    m.seed = seed;
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) {
            int i = y * m.w + x;
            bool edge = x == 0 || y == 0 || x == m.w - 1 || y == m.h - 1;
            m.tiles[i] = edge ? T_WALL : T_FLOOR;
            m.flags[i] = F_LIT | F_OUTDOOR;
            m.wallTex[i] = WT_BRICK;
            m.floorTex[i] = FT_GRASS;
        }
    int cx = m.w / 2, cy = m.h / 2;
    // Cobbled square in the middle.
    for (int y = 1; y < m.h - 1; y++)
        for (int x = 1; x < m.w - 1; x++) {
            float dx = (x + 0.5f - cx) / 9.0f, dy = (y + 0.5f - cy) / 6.0f;
            if (dx * dx + dy * dy <= 1.0f) m.floorTex[y * m.w + x] = FT_COBBLE;
        }
    // Shops: three along the top, up to five along the bottom.
    struct Slot { int x, y; bool top; };
    const Slot slots[8] = {{5, 4, true}, {20, 4, true}, {35, 4, true}, {11, 26, false}, {29, 26, false},
                           {3, 15, true}, {38, 15, true}, {20, 28, false}};
    int stores = std::min(8, gTownStores);
    for (int k = 0; k < stores; k++) {
        const Slot& sl = slots[k];
        int hw = 7, hh = 5;
        for (int y = sl.y; y < sl.y + hh; y++)
            for (int x = sl.x; x < sl.x + hw; x++) {
                m.set(x, y, T_WALL);
                m.wallTex[y * m.w + x] = (k % 2) ? WT_WOOD : WT_BRICK;
            }
        int ex = sl.x + hw / 2, ey = sl.top ? sl.y + hh - 1 : sl.y;
        m.set(ex, ey, T_SHOP);
        m.floorTex[ey * m.w + ex] = (uint8_t)k;  // which store
        // Cobbled path from the door toward the square.
        int py = sl.top ? ey + 1 : ey - 1, step = sl.top ? 1 : -1;
        int px = ex;
        for (int g = 0; g < 40 && m.floorTex[py * m.w + px] != FT_COBBLE; g++) {
            m.floorTex[py * m.w + px] = FT_COBBLE;
            if (std::abs(px - cx) > std::abs(py - cy) * 2) px += px < cx ? 1 : -1;
            else py += step;
            if (!m.inside(px, py) || m.at(px, py) != T_FLOOR) break;
        }
    }
    // A few trees (pillars of moss-covered stone) on the grass.
    for (int t = 0; t < 10; t++) {
        int x = rng.range(2, m.w - 3), y = rng.range(2, m.h - 3);
        bool ok = m.at(x, y) == T_FLOOR && m.floorTex[y * m.w + x] == FT_GRASS;
        for (int dy = -1; dy <= 1 && ok; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (m.at(x + dx, y + dy) != T_FLOOR) ok = false;
        if (ok) { m.set(x, y, T_WALL); m.wallTex[y * m.w + x] = WT_MOSS; }
    }
    m.set(cx, cy, T_STAIRS_DOWN);
    m.downX = cx; m.downY = cy;
    m.startX = cx; m.startY = cy + 2;
    m.rooms.push_back(Room{cx - 8, cy - 5, 16, 10, true});
}

int gTownStores = 5;

void generateLevel(Map& m, uint32_t seed, int depth) {
    if (depth <= 0) { generateTown(m, seed); return; }
    for (uint32_t attempt = 0;; attempt++) {
        Gen g(m, seed * 7919u + attempt * 104729u + (uint32_t)depth, depth);
        if (g.run()) break;
    }
    m.depth = depth;
    m.seed = seed;
}
