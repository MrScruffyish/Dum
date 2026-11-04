// Dum - tile map and procedural dungeon generator.
#pragma once

#include <cstdint>
#include <vector>

#include "common/common.h"
#include "common/serialize.h"

enum Tile : uint8_t {
    T_ROCK = 0,
    T_WALL,
    T_FLOOR,
    T_DOOR,          // closed door
    T_DOOR_OPEN,
    T_DOOR_LOCKED,
    T_DOOR_SECRET,   // looks like a wall until found
    T_STAIRS_DOWN,
    T_STAIRS_UP,
    T_RUBBLE,        // blocks movement and sight until cleared
    T_SHOP,          // town shop entrance (store index in floorTex)
};

enum TileFlag : uint8_t {
    F_LIT = 1,       // room is lit by itself
    F_ROOM = 2,
    F_CORRIDOR = 4,
    F_SEEN = 8,      // client side: player has seen this tile (automap)
    F_OUTDOOR = 16,  // open sky above (town)
};

struct Room {
    int x, y, w, h;
    bool lit;
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }
};

struct Map {
    int w = 0, h = 0;
    int depth = 1;
    uint32_t seed = 0;
    std::vector<uint8_t> tiles, flags, wallTex, floorTex;
    std::vector<Room> rooms;
    int startX = 1, startY = 1;   // up staircase
    int downX = 1, downY = 1;     // down staircase

    void resize(int w_, int h_);
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    uint8_t at(int x, int y) const { return inside(x, y) ? tiles[y * w + x] : (uint8_t)T_ROCK; }
    void set(int x, int y, uint8_t t) { if (inside(x, y)) tiles[y * w + x] = t; }
    uint8_t flag(int x, int y) const { return inside(x, y) ? flags[y * w + x] : 0; }

    static bool tileSolid(uint8_t t) {
        return t == T_ROCK || t == T_WALL || t == T_DOOR || t == T_DOOR_LOCKED || t == T_DOOR_SECRET || t == T_RUBBLE || t == T_SHOP;
    }
    static bool tileIsDoor(uint8_t t) { return t == T_DOOR || t == T_DOOR_OPEN || t == T_DOOR_LOCKED; }
    bool solid(int x, int y) const { return tileSolid(at(x, y)); }
    bool opaque(int x, int y) const { return tileSolid(at(x, y)); }
    bool solidAt(Vec2 p) const { return solid((int)std::floor(p.x), (int)std::floor(p.y)); }

    // Line of sight between two points (sampled; endpoints' own tiles are ignored).
    bool los(Vec2 a, Vec2 b) const;
    // Move a circle with wall sliding. Returns true if not blocked at all.
    bool moveCircle(Vec2& pos, Vec2 delta, float radius) const;
    bool circleFree(Vec2 p, float radius) const;

    void serialize(Writer& w, bool hideSecrets) const;
    bool deserialize(Reader& r);
};

// Build a level. Deterministic for a given seed and depth. Depth 0 is the town.
void generateLevel(Map& m, uint32_t seed, int depth);
void generateTown(Map& m, uint32_t seed);
extern int gTownStores;   // number of shops in town (set from the data files)

// Theme texture ids (kept in sync with the client's texture set).
enum WallTexId : uint8_t { WT_BRICK = 0, WT_CAVE, WT_MOSS, WT_DARK, WT_WOOD, WT_BLOOD, WT_COUNT };
enum FloorTexId : uint8_t { FT_STONE = 0, FT_DIRT, FT_TILES, FT_WOOD, FT_GRASS, FT_COBBLE, FT_COUNT };
