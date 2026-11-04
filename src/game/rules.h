// Dum - small rules shared by server and client, so the client's on-screen
// prompts ("[E] Pick up ...") always match what the server will actually do.
#pragma once

#include <vector>

#include "game/map.h"

constexpr float USE_RANGE = 1.3f;

// Index of the item E would pick up, or -1. Items at your feet win, then the
// closest one roughly in front of you within reach.
inline int pickupCandidate(const Map& m, Vec2 pos, float angle, const std::vector<Vec2>& items) {
    int best = -1;
    float bestScore = 1e9f;
    for (size_t i = 0; i < items.size(); i++) {
        Vec2 to = items[i] - pos;
        float d = to.len();
        float score;
        if (d <= 0.9f) score = d;
        else if (d <= 1.7f && std::fabs(wrapAngle(std::atan2(to.y, to.x) - angle)) < 0.8f && m.los(pos, items[i])) score = d + 1.0f;
        else continue;
        if (score < bestScore) { bestScore = score; best = (int)i; }
    }
    return best;
}

// The door or rubble tile E would act on, if any.
inline bool useTileInFront(const Map& m, Vec2 pos, float angle, int& ox, int& oy) {
    Vec2 dir = dirFromAngle(angle);
    for (float d = 0.4f; d <= USE_RANGE; d += 0.3f) {
        Vec2 c = pos + dir * d;
        int x = (int)std::floor(c.x), y = (int)std::floor(c.y);
        uint8_t t = m.at(x, y);
        if (t == T_DOOR || t == T_DOOR_OPEN || t == T_DOOR_LOCKED || t == T_RUBBLE || t == T_SHOP) { ox = x; oy = y; return true; }
        if (Map::tileSolid(t)) return false;
    }
    return false;
}

// Movement with "corridor assist": if you walk into a wall right next to an opening
// (doorway, corridor), you are nudged sideways into the gap instead of stopping dead.
// Returns the new position.
inline Vec2 assistedMove(const Map& m, Vec2 pos, Vec2 step, float radius) {
    Vec2 start = pos;
    m.moveCircle(pos, step, radius);
    float want = step.len();
    if (want < 1e-5f || dist(start, pos) > want * 0.6f) return pos;  // moved fine (or slid along a wall)

    // Blocked: look for a free lane to either side, closest first.
    Vec2 dir = step * (1.0f / want);
    Vec2 side(-dir.y, dir.x);
    for (float s = 0.05f; s <= 0.75f; s += 0.05f) {
        for (int sign = -1; sign <= 1; sign += 2) {
            Vec2 lane = start + side * (s * sign);
            if (!m.circleFree(lane, radius) || !m.circleFree(lane + dir * 0.45f, radius)) continue;
            if (!m.los(start, lane)) continue;
            // Slide toward the lane (a bit faster than walking) while still moving forward a little.
            float lat = std::min(s, want * 1.3f) * sign;
            Vec2 p = start;
            m.moveCircle(p, side * lat + dir * (want * 0.35f), radius);
            return p;
        }
    }
    return pos;
}
