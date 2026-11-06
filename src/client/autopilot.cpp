// Dum - helpers that make the game playable with less precise or less frequent input:
// auto-explore, travel (stairs, coloured doors, chests), facing the nearest monster, and auto-fight.
#include <cstdio>
#include <cstdlib>
#include <queue>

#include "client/client.h"
#include "game/rules.h"

namespace {
constexpr uint32_t COL_HINT = 0xFFFFE080, COL_DIM = 0xFF908070;
}

uint32_t Client::nearestAwakeVisible(float maxDist) const {
    uint32_t best = 0;
    float bd = maxDist;
    for (uint32_t id : visibleMonsters_) {
        auto it = ents_.find(id);
        if (it == ents_.end() || (it->second.s.flags & EF_ASLEEP)) continue;
        float d = dist(it->second.rpos, pos_);
        if (d < bd && map_.los(pos_, it->second.rpos)) { bd = d; best = id; }
    }
    return best;
}

void Client::startAuto(Auto a) {
    if (me_.dead) return;
    if (a == Auto::Explore && map_.depth == 0) { addLog("The town is all explored. (G walks to the stairs down)", COL_DIM); return; }
    if (a == Auto::Travel && (travel_.kind == Dest::StairsDown || travel_.kind == Dest::StairsUp) &&
        (int)pos_.x == travel_.x && (int)pos_.y == travel_.y) {
        addLog("You are standing on the stairs. Press " + keys_->label(K_USE) + (travel_.kind == Dest::StairsDown ? " to go down." : " to go up."), COL_DIM);
        return;
    }
    if (nearestAwakeVisible(12.0f)) { addLog("Not with an enemy in sight!", COL_HINT); return; }
    auto_ = a;
    autoWalk_ = false;
    path_.clear();
    repathT_ = 0;
    stuckT_ = 0;
    hpAtStart_ = (float)me_.hp;
    seenMonsters_ = visibleMonsters_;   // monsters already in view (asleep) don't stop us
    addLog(a == Auto::Explore ? "Exploring... (any movement key stops)" : "Travelling to the " + travel_.label + "... (any movement key stops)", COL_DIM);
}

void Client::stopAuto(const std::string& why) {
    if (auto_ == Auto::None) return;
    auto_ = Auto::None;
    path_.clear();
    if (!why.empty()) addLog(why, COL_HINT);
}

// Breadth-first search over the tiles we have seen. Explore: the nearest seen tile next to
// unseen open ground (or with an item on it). Travel: the nearest known stairs down.
bool Client::findPath(bool explore) {
    int sx = (int)pos_.x, sy = (int)pos_.y;
    if (!map_.inside(sx, sy)) return false;
    auto walkable = [&](int x, int y) {
        if (!map_.inside(x, y) || !(map_.flag(x, y) & F_SEEN)) return false;
        uint8_t t = map_.at(x, y);
        return !Map::tileSolid(t) || t == T_DOOR || t == T_DOOR_LOCKED || t == T_RUBBLE;
    };
    std::vector<std::pair<int, int>> itemTiles;
    if (explore && autoPickup)
        for (auto& [id, e] : ents_)
            if (e.s.type == ET_ITEM && std::find(visitedItems_.begin(), visitedItems_.end(), id) == visitedItems_.end())
                itemTiles.push_back({(int)e.s.pos.x, (int)e.s.pos.y});
    auto isGoal = [&](int x, int y) {
        if (x == sx && y == sy) return false;
        if (!explore) {
            // A coloured door: stand right in front of it. Everything else: on it.
            if (travel_.kind == Dest::KeyDoor) return std::abs(x - travel_.x) + std::abs(y - travel_.y) == 1;
            return x == travel_.x && y == travel_.y;
        }
        for (auto& it : itemTiles)
            if (it.first == x && it.second == y) return true;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (!map_.inside(nx, ny) || (map_.flag(nx, ny) & F_SEEN)) continue;
                uint8_t nt = map_.at(nx, ny);
                if (!Map::tileSolid(nt) || nt == T_DOOR || nt == T_DOOR_LOCKED) return true;
            }
        return false;
    };
    std::vector<int> parent(map_.w * map_.h, -1);
    std::queue<int> q;
    int start = sy * map_.w + sx;
    parent[start] = start;
    q.push(start);
    int goal = -1;
    while (!q.empty() && goal < 0) {
        int c = q.front();
        q.pop();
        int x = c % map_.w, y = c / map_.w;
        for (int dy = -1; dy <= 1 && goal < 0; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int nx = x + dx, ny = y + dy;
                if (!walkable(nx, ny) || parent[ny * map_.w + nx] >= 0) continue;
                if (dx && dy && (!walkable(x + dx, y) || !walkable(x, y + dy))) continue;  // no corner cutting
                // Doors and rubble only straight on (diagonal bumps don't open them).
                uint8_t t = map_.at(nx, ny);
                if (dx && dy && (t == T_DOOR || t == T_DOOR_LOCKED || t == T_RUBBLE)) continue;
                parent[ny * map_.w + nx] = c;
                if (isGoal(nx, ny)) { goal = ny * map_.w + nx; break; }
                q.push(ny * map_.w + nx);
            }
    }
    if (goal < 0) {
        if (std::getenv("DUM_DEBUG_PATH")) {
            int visited = 0;
            for (int v : parent) visited += v >= 0;
            std::printf("findPath(%d) failed from %d,%d tile %d seen=%d visited=%d\n", explore, sx, sy, map_.at(sx, sy), (map_.flag(sx, sy) & F_SEEN) != 0, visited);
        }
        return false;
    }
    path_.clear();
    for (int c = goal; c != start; c = parent[c]) path_.push_back({c % map_.w, c / map_.w});
    std::reverse(path_.begin(), path_.end());
    return true;
}

// Returns the direction to walk this frame (zero = stand still).
Vec2 Client::autopilot(float dt) {
    // Stop for anything new and dangerous.
    for (uint32_t id : visibleMonsters_) {
        if (std::find(seenMonsters_.begin(), seenMonsters_.end(), id) != seenMonsters_.end()) continue;
        seenMonsters_.push_back(id);
        auto it = ents_.find(id);
        if (it == ents_.end()) continue;
        std::string n = gData.monsters[it->second.s.def].name;
        stopAuto("You see a " + n + ((it->second.s.flags & EF_ASLEEP) ? " (asleep)." : "!") +
                 (autoFight_ ? "" : "  (" + keys_->label(K_AUTOFIGHT) + ": auto-fight)"));
        return {};
    }
    if (me_.hp < hpAtStart_) { stopAuto("You are hurt - stopping."); return {}; }
    hpAtStart_ = std::max(hpAtStart_, (float)me_.hp);

    repathT_ -= dt;
    if (path_.empty() || repathT_ <= 0) {
        repathT_ = 0.5f;
        if (!findPath(auto_ == Auto::Explore)) {
            if (auto_ == Auto::Explore) stopAuto("Nothing more to explore here. (" + keys_->label(K_TRAVEL) + ": travel)");
            else stopAuto("There is no known way to the " + travel_.label + ". (" + keys_->label(K_EXPLORE) + ": explore)");
            return {};
        }
    }
    // Waypoints reached?
    while (!path_.empty()) {
        Vec2 c(path_.front().first + 0.5f, path_.front().second + 0.5f);
        if (dist(c, pos_) > 0.35f) break;
        // Items on reached tiles are remembered so we don't keep coming back to them.
        for (auto& [id, e] : ents_)
            if (e.s.type == ET_ITEM && (int)e.s.pos.x == path_.front().first && (int)e.s.pos.y == path_.front().second)
                visitedItems_.push_back(id);
        path_.erase(path_.begin());
    }
    if (path_.empty()) {
        if (auto_ == Auto::Travel) {
            std::string e = keys_->label(K_USE);
            switch (travel_.kind) {
                case Dest::StairsDown: stopAuto("You reach the stairs. Press " + e + " to go down."); break;
                case Dest::StairsUp: stopAuto("You reach the stairs. Press " + e + " to go up."); break;
                case Dest::KeyDoor:
                    angle_ = std::atan2(travel_.y + 0.5f - pos_.y, travel_.x + 0.5f - pos_.x);   // face the door
                    stopAuto(me_.keys[map_.keyDoorColor(travel_.x, travel_.y)] ? "Here is the door. Press " + e + " to unlock it."
                                                                               : "Here is the door - but you don't have its key yet.");
                    break;
                case Dest::Chest: stopAuto("You reach the chest. Press " + e + " to open it."); break;
            }
        }
        return {};
    }
    Vec2 target(path_.front().first + 0.5f, path_.front().second + 0.5f);
    Vec2 dir = (target - pos_).norm();
    // Look where we walk, turning smoothly.
    float diff = wrapAngle(std::atan2(dir.y, dir.x) - angle_);
    float maxTurn = 3.5f * turnSpeed * dt;
    angle_ = wrapAngle(angle_ + clampf(diff, -maxTurn, maxTurn));
    return dir;
}

void Client::updateFacing(float dt) {
    uint32_t target = 0;
    if (faceT_ > 0) {
        faceT_ -= dt;
        target = nearestAwakeVisible(20.0f);
        if (!target && !visibleMonsters_.empty()) target = visibleMonsters_.front();
        if (!target) { faceT_ = 0; addLog("No monster in sight.", COL_DIM); return; }
    } else if (autoFight_ && autoFace && auto_ == Auto::None) {
        target = fightTarget_;
    }
    if (!target) return;
    auto it = ents_.find(target);
    if (it == ents_.end()) return;
    Vec2 to = it->second.rpos - pos_;
    float diff = wrapAngle(std::atan2(to.y, to.x) - angle_);
    float maxTurn = 5.0f * turnSpeed * dt;
    angle_ = wrapAngle(angle_ + clampf(diff, -maxTurn, maxTurn));
    if (faceT_ > 0 && std::fabs(diff) < 0.03f) faceT_ = 0;
}

void Client::autoFightStep() {
    if (me_.dead) return;
    const ItemDef* w = me_.weapon();
    bool ranged = w && w->ranged;
    float reach = ranged ? w->range : (w ? w->range : 1.3f) + 2.5f;
    // A selected attack spell with enough mana: casters fight with magic.
    const SpellDef* spell = nullptr;
    int qv = me_.quick[selSpell_];
    if (const SpellDef* s = quickSpell(selSpell_)) {
        bool attack = s->type == SpellType::Bolt || s->type == SpellType::Ball;
        if (attack && qv >= 0 && qv < QUICK_ITEM && me_.knowsSpell(qv) && me_.mp >= s->mana && me_.c().magic >= 3) spell = s;
    }
    if (spell) reach = std::max(reach, spell->range);
    fightTarget_ = nearestAwakeVisible(reach);
    // Something attacking from behind (not on screen) still counts when it is close.
    float bestBehind = 2.3f;
    for (auto& [id, e] : ents_) {
        if (e.s.type != ET_MONSTER || (e.s.flags & EF_ASLEEP)) continue;
        float d = dist(e.rpos, pos_);
        if (d < bestBehind && map_.los(pos_, e.rpos)) {
            if (!fightTarget_ || d < dist(ents_.at(fightTarget_).rpos, pos_) - 0.5f) { bestBehind = d; fightTarget_ = id; }
        }
    }
    if (!fightTarget_) return;
    auto it = ents_.find(fightTarget_);
    if (it == ents_.end()) return;
    float d = dist(it->second.rpos, pos_);
    Vec2 to = it->second.rpos - pos_;
    bool facing = std::fabs(wrapAngle(std::atan2(to.y, to.x) - angle_)) < 0.35f;
    if (!facing) return;
    if (spell && d <= spell->range) { tryCast(); return; }
    bool weaponTarget = targetId_ == fightTarget_ && !targetIsSpell_;
    if (weaponTarget) tryAttack();
}

// ---- Travel menu (G): every known place worth walking to ----

void Client::openTravelMenu() {
    travelOpts_.clear();
    auto seen = [&](int x, int y) { return map_.inside(x, y) && (map_.flag(x, y) & F_SEEN); };
    for (int y = 0; y < map_.h; y++)
        for (int x = 0; x < map_.w; x++) {
            if (!seen(x, y)) continue;
            uint8_t t = map_.at(x, y);
            if (t == T_STAIRS_DOWN) travelOpts_.insert(travelOpts_.begin(), {Dest::StairsDown, x, y, "stairs down"});
            else if (t == T_STAIRS_UP && map_.depth > 0) travelOpts_.push_back({Dest::StairsUp, x, y, "stairs up"});
        }
    for (int y = 0; y < map_.h; y++)
        for (int x = 0; x < map_.w; x++)
            if (seen(x, y) && map_.at(x, y) == T_DOOR_KEY) {
                int c = map_.keyDoorColor(x, y);
                travelOpts_.push_back({Dest::KeyDoor, x, y, std::string(keyColorName(c)) + " door" + (me_.keys[c] ? " (you have the key)" : " (no key yet)")});
            }
    // The nearest closed chest you have seen (or detected).
    float best = 1e9f;
    TravelDest chest{Dest::Chest, -1, -1, "nearest unopened chest"};
    for (auto& [id, e] : ents_) {
        if (e.s.type != ET_ITEM || gData.items[e.s.def].id != "chest") continue;
        int x = (int)e.s.pos.x, y = (int)e.s.pos.y;
        if (!seen(x, y) && !(e.s.flags & EF_CHEST_KNOWN)) continue;
        float d = dist(e.s.pos, pos_);
        if (d < best) { best = d; chest.x = x; chest.y = y; }
    }
    if (chest.x >= 0) travelOpts_.push_back(chest);
    if (travelOpts_.empty()) { addLog("You haven't found anywhere to travel to yet. (" + keys_->label(K_EXPLORE) + ": explore)", COL_DIM); return; }
    travelSel_ = 0;
    if (travelOpts_.size() == 1) { travel_ = travelOpts_[0]; startAuto(Auto::Travel); return; }   // nothing to choose
    ui_ = Ui::Travel;
}

void Client::travelKey(SDL_Keycode k) {
    int n = (int)travelOpts_.size();
    int pick = -1;
    if (k == SDLK_ESCAPE) { ui_ = Ui::None; return; }
    if (k == SDLK_UP || k == SDLK_w) travelSel_ = (travelSel_ + n - 1) % n;
    else if (k == SDLK_DOWN || k == SDLK_s) travelSel_ = (travelSel_ + 1) % n;
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_e) pick = travelSel_;
    else if (k >= SDLK_1 && k <= SDLK_9 && k - SDLK_1 < n) pick = k - SDLK_1;
    if (pick < 0) return;
    ui_ = Ui::None;
    travel_ = travelOpts_[pick];
    startAuto(Auto::Travel);
}

void Client::drawTravel(Renderer& r) {
    int w = 260, h = 40 + (int)travelOpts_.size() * 12;
    int x = (r.W - w) / 2, y = (r.H - 44 - h) / 2;
    r.fill(x, y, w, h, 0xFF100C08, 230);
    r.frame(x, y, w, h, 0xFF6A5030);
    r.text(x + 8, y + 6, "Travel to:", 0xFFE0D0A0);
    for (size_t i = 0; i < travelOpts_.size(); i++) {
        int ry = y + 20 + (int)i * 12;
        bool sel = (int)i == travelSel_;
        if (sel) r.fill(x + 4, ry - 2, w - 8, 11, 0xFF4A3A20);
        std::string t = std::to_string(i + 1) + "  " + travelOpts_[i].label;
        if (!t.empty()) t[3] = (char)std::toupper((unsigned char)t[3]);
        r.text(x + 10, ry, t, sel ? 0xFFFFFFFF : 0xFFD0C8B8);
    }
    std::string hint = keys_->label(K_TRAVEL) + "/Enter: go   1-9: pick   Esc: cancel";
    r.text(x + (w - r.textW(hint)) / 2, y + h - 12, hint, 0xFF908070);
}
