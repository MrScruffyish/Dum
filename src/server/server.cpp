// Dum - game server implementation.
#include "server/server.h"
#include "game/scores.h"

#include "game/rules.h"

#include <cstdio>
#include <queue>

namespace {
constexpr uint32_t COL_WHITE = 0xFFFFFFFF, COL_YELLOW = 0xFFFFE060, COL_RED = 0xFFFF5050,
                   COL_GREEN = 0xFF70FF70, COL_CYAN = 0xFF60E0FF, COL_GREY = 0xFFA0A0A0,
                   COL_ORANGE = 0xFFFFA040, COL_PURPLE = 0xFFE080FF;
constexpr float SNAPSHOT_RANGE = 28.0f;

std::string capitalize(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
    return s;
}
std::string theName(const MonsterDef& d) { return d.boss ? d.name : "the " + d.name; }
}  // namespace

const MonsterDef& Monster::d() const { return gData.monsters[def]; }

Server::Server(std::shared_ptr<ServerNet> net, uint32_t seed) : net_(net), rng_(seed), baseSeed_(seed) {
    difficulty_ = defaultDifficulty();
    newGame();
}

Server::~Server() {
    if (!savePath_.empty()) saveGame();
}

int Server::playerCount() const {
    int n = 0;
    for (auto& [id, p] : players_)
        if (p.joined) n++;
    return n;
}

// ============================================================ networking ====

void Server::send(int playerId, const Writer& w, bool reliable) {
    auto it = players_.find(playerId);
    if (it == players_.end() || it->second.peer < 0) return;
    net_->send(it->second.peer, w.buf, reliable);
}

void Server::broadcast(const Writer& w, bool reliable) {
    for (auto& [id, p] : players_)
        if (p.joined) net_->send(p.peer, w.buf, reliable);
}

void Server::text(int playerId, uint32_t color, const std::string& s) {
    Writer w;
    w.u8(S_TEXT);
    w.u32(color);
    w.str(s);
    if (playerId < 0) broadcast(w, true); else send(playerId, w, true);
}

void Server::event(EventKind k, Vec2 pos, uint32_t ent) {
    Writer w;
    w.u8(S_EVENT);
    w.u8(k);
    w.f32(pos.x);
    w.f32(pos.y);
    w.u32(ent);
    for (auto& [id, p] : players_)
        if (p.joined && dist(p.pos, pos) < SNAPSHOT_RANGE) net_->send(p.peer, w.buf, false);
}

void Server::sendLevel(int playerId) {
    Writer w;
    w.u8(S_LEVEL);
    map_.serialize(w, true);
    send(playerId, w, true);
}

void Server::sendTile(int x, int y) {
    Writer w;
    w.u8(S_TILE);
    w.u16((uint16_t)x);
    w.u16((uint16_t)y);
    uint8_t t = map_.at(x, y);
    w.u8(t == T_DOOR_SECRET ? (uint8_t)T_WALL : t);
    broadcast(w, true);
}

void Server::sendQuests() {
    Writer w;
    w.u8(S_QUESTS);
    w.u8((uint8_t)quests_.size());
    for (auto& q : quests_) q.write(w);
    broadcast(w, true);
}

void Server::sendRoster() {
    Writer w;
    w.u8(S_ROSTER);
    int n = playerCount();
    w.u8((uint8_t)n);
    for (auto& [id, p] : players_) {
        if (!p.joined) continue;
        w.u16((uint16_t)id);
        w.str(p.st.name);
        w.u8(p.st.cls);
        w.u8((uint8_t)p.st.level);
    }
    broadcast(w, true);
}

void Server::sendTeleport(SPlayer& p) {
    Writer w;
    w.u8(S_TELEPORT);
    w.f32(p.pos.x);
    w.f32(p.pos.y);
    w.f32(p.angle);
    send(p.st.id, w, true);
}

void Server::handleNet() {
    std::vector<NetEvent> evs;
    net_->poll(evs);
    for (auto& e : evs) {
        if (e.type == NetEvent::Connect) {
            std::printf("[server] peer %d connected\n", e.peer);
        } else if (e.type == NetEvent::Disconnect) {
            onDisconnect(e.peer);
        } else {
            onData(e.peer, e.data);
        }
    }
}

static std::string lowerName(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void Server::onDisconnect(int peer) {
    auto it = peerToPlayer_.find(peer);
    if (it == peerToPlayer_.end()) return;
    int pid = it->second;
    peerToPlayer_.erase(it);
    auto pit = players_.find(pid);
    if (pit != players_.end()) {
        SPlayer& p = pit->second;
        std::string name = p.st.name;
        // Keep the character (with all belongings) for when they come back.
        saved_[lowerName(name)] = SavedChar{p.st, p.pos, p.angle, map_.depth};
        players_.erase(pit);
        text(-1, COL_YELLOW, name + " has left the game. Their character will be here when they return.");
        sendRoster();
    }
    std::printf("[server] peer %d left\n", peer);
}

// Where a joining player appears: next to a living party member, else at the up stairs.
Vec2 Server::spawnPoint(int exceptPlayer) {
    Vec2 start(map_.startX + 0.5f, map_.startY + 0.5f);
    Vec2 anchor = start;
    for (auto& [id, o] : players_)
        if (id != exceptPlayer && o.joined && !o.st.dead) { anchor = o.pos; break; }
    for (int t = 0; t < 40; t++) {
        Vec2 c = anchor + dirFromAngle(rng_.frand() * 2 * PI) * (0.7f + rng_.frand() * 1.2f);
        if (map_.circleFree(c, PLAYER_RADIUS) && map_.los(anchor, c)) return c;
    }
    return anchor;
}

void Server::onData(int peer, const std::vector<uint8_t>& data) {
    Reader r(data);
    uint8_t type = r.u8();
    if (type == C_HELLO) { onHello(peer, r); return; }
    auto it = peerToPlayer_.find(peer);
    if (it == peerToPlayer_.end()) return;
    SPlayer* p = playerById(it->second);
    if (!p) return;

    if (type == C_INPUT) {
        Vec2 np{r.f32(), r.f32()};
        float ang = r.f32();
        if (!r.ok) return;
        p->angle = ang;
        if (p->st.dead) return;
        // Client moves itself (co-op, trusted), but sanity check against walls & teleports.
        if (dist(np, p->pos) < 2.5f && map_.circleFree(np, PLAYER_RADIUS * 0.9f)) {
            p->pos = np;
        } else if (dist(np, p->pos) > 0.05f) {
            sendTeleport(*p);
        }
    } else if (type == C_ACTION) {
        onAction(*p, r);
    } else if (type == C_CHAT) {
        std::string msg = r.str();
        if (!msg.empty()) text(-1, COL_CYAN, p->st.name + ": " + msg);
    }
}

void Server::onHello(int peer, Reader& r) {
    uint16_t ver = r.u16();
    std::string name = r.str();
    int cls = r.u8();
    if (ver != PROTOCOL_VERSION) {
        Writer w;
        w.u8(S_REJECT);
        w.str("Version mismatch: server " + std::to_string(PROTOCOL_VERSION) + ", client " + std::to_string(ver));
        net_->send(peer, w.buf, true);
        net_->disconnect(peer);
        return;
    }
    if (peerToPlayer_.count(peer)) return;
    // Everyone must have the same data files (monsters, items... are sent by number).
    // Only checksums are compared; no files are ever sent. Different files: refuse, and say
    // which, so the players can sort it out.
    std::map<std::string, uint32_t> theirs;
    int nf = r.u8();
    for (int i = 0; i < nf && r.ok; i++) { std::string f = r.str(); theirs[f] = r.u32(); }
    std::string differ;
    for (auto& sf : gData.shared)
        if (!theirs.count(sf.name) || theirs[sf.name] != sf.hash) differ += (differ.empty() ? "" : ", ") + sf.name;
    if (!differ.empty()) {
        Writer w;
        w.u8(S_REJECT);
        w.str("Your data files differ from the host's: " + differ + ". Everyone needs the same files in data/.");
        net_->send(peer, w.buf, true);
        net_->disconnect(peer);
        std::string who = name.empty() ? "A player" : name.substr(0, 16);
        text(-1, COL_ORANGE, who + " can't join: different data files (" + differ + ").");
        std::printf("[server] %s refused: data files differ (%s)\n", who.c_str(), differ.c_str());
        return;
    }
    if (name.empty()) name = "Hero";
    if (name.size() > 16) name.resize(16);
    if (cls < 0 || cls >= (int)gData.classes.size()) cls = 0;

    // Someone with this name already playing? Don't hand out the same character twice.
    for (auto& [id, o] : players_)
        if (o.joined && lowerName(o.st.name) == lowerName(name)) name += "_";

    int pid = nextPlayerId_++;
    SPlayer& p = players_[pid];
    p.peer = peer;
    p.st.id = (uint16_t)pid;
    p.st.name = name;
    auto sit = saved_.find(lowerName(name));
    if (sit == saved_.end() && adoptAny_ && saved_.size() == 1) sit = saved_.begin();
    bool returning = sit != saved_.end();
    if (returning) {
        SavedChar sc = sit->second;
        saved_.erase(sit);
        p.st = sc.st;
        p.st.id = (uint16_t)pid;
        p.st.name = name;
        fixupChar(p.st);
        p.angle = sc.angle;
        p.pos = (sc.depth == map_.depth && map_.circleFree(sc.pos, PLAYER_RADIUS)) ? sc.pos : spawnPoint(pid);
        p.dirty = true;
    } else {
        rollCharacter(p, cls);
        p.pos = spawnPoint(pid);
        p.angle = rng_.frand() * 2 * PI;
    }
    p.joined = true;
    peerToPlayer_[peer] = pid;

    Writer w;
    w.u8(S_WELCOME);
    w.u16((uint16_t)pid);
    w.u8((uint8_t)difficulty_);
    w.u8(ammoRule_ ? 1 : 0);
    send(pid, w, true);
    sendLevel(pid);
    sendTeleport(p);
    sendRoster();
    sendQuests();
    if (returning) { fillQuiver(p.st); p.dirty = true; }   // e.g. the Ammo option was turned on since
    if (returning)
        text(-1, COL_YELLOW, name + " the " + p.st.c().name + " (level " + std::to_string(p.st.level) + ") returns to the dungeon.");
    else
        text(-1, COL_YELLOW, name + " the " + p.st.c().name + (map_.depth ? " has entered the dungeon." : " has arrived in town."));
    text(pid, COL_GREY, "Press F1 for help.");
}

// ================================================================ world =====

void Server::rollCharacter(SPlayer& p, int cls) {
    PlayerState& s = p.st;
    std::string name = s.name;
    uint16_t id = s.id;
    s = PlayerState();
    s.id = id;
    s.name = name;
    s.cls = (uint8_t)cls;
    const ClassDef& c = gData.classes[cls];
    for (int i = 0; i < NUM_STATS; i++) s.stats[i] = clampi(c.stats[i] + rng_.range(-1, 1), 3, 18);
    s.maxHp = s.hp = std::max(4, c.hitDie + 4 + s.mod(CON));
    s.maxMp = s.mp = c.manaBase > 0 ? (float)std::max(1, c.manaBase + s.mod(c.castStat)) : 0.0f;
    s.food = 800;
    s.gold = rng_.range(10, 40);
    int slot = 0;
    for (int it : c.startItems) {
        ItemStack st = makeItem(it);
        const ItemDef& d = gData.items[it];
        if (d.kind == ItemKind::Ammo) {   // into the quiver (only when ammo is used up at all)
            if (!ammoRule_) continue;
            ItemStack& q = s.equip[EQ_QUIVER];
            if (q.empty()) { q = st; continue; }
            if (q.def == st.def) { q.count = std::min(d.maxStack, q.count + st.count); continue; }
            if (slot < INV_SIZE) s.inv[slot++] = st;
            continue;
        }
        int eq = equipSlotFor(d);
        if (eq >= 0 && s.equip[eq].empty()) { s.equip[eq] = st; continue; }
        // stack with existing
        bool merged = false;
        for (int i = 0; i < slot; i++)
            if (s.inv[i].def == st.def && s.inv[i].count < d.maxStack) { s.inv[i].count++; merged = true; break; }
        if (!merged && slot < INV_SIZE) s.inv[slot++] = st;
    }
    fixupChar(s);
    p.dirty = true;
}

// Make sure a character's quick slots and recharge timers fit its class (new or old saves).
void Server::fixupChar(PlayerState& s) {
    s.recharge.resize(s.c().spells.size(), 0.0f);
    for (auto& it : s.inv)   // keys carried in the pack (older saves) go on the key ring
        if (!it.empty() && it.d().kind == ItemKind::Key && it.d().keyColor >= 0) {
            s.keys[it.d().keyColor] = (uint8_t)std::min(99, s.keys[it.d().keyColor] + it.count);
            it = ItemStack();
        }
    for (auto& q : s.quick) {
        bool ok = (q >= 0 && q < (int)s.c().spells.size()) || (q >= QUICK_ITEM && q - QUICK_ITEM < (int)gData.items.size());
        if (!ok) q = -1;
    }
    for (size_t i = 0; i < s.c().spells.size(); i++)
        if (s.knowsSpell((int)i)) s.autoQuick((int)i);
    for (auto& it : s.inv)
        if (!it.empty() && (it.d().kind == ItemKind::Wand || it.d().kind == ItemKind::Rod)) s.autoQuick(QUICK_ITEM + it.def);
}

void Server::newGame() {
    quests_.clear();
    mainQuestStage_ = 0;
    campaignDone_.clear();
    campaignWon_ = false;
    if (campaign_ < 0) setupMainQuest();
    artifactsInPlay_.clear();
    maxDepth_ = 0;
    recallT_ = 0;
    descentT_ = 0;
    bool town = campaign_ < 0 || gData.campaigns[campaign_].town;
    newLevel(town ? 0 : 1, true);  // adventures start in town (unless a campaign says otherwise)
    if (campaign_ >= 0) {
        const Campaign& c = gData.campaigns[campaign_];
        text(-1, COL_YELLOW, "Campaign: " + c.name);
        if (!c.desc.empty()) text(-1, COL_WHITE, c.desc);
    }
}

ItemStack Server::makeItem(int def, int count) {
    ItemStack s;
    s.def = (int16_t)def;
    s.count = count;
    const ItemDef& d = gData.items[def];
    s.fuel = d.kind == ItemKind::Light ? (d.everburning ? 1 : d.fuel) : 0;
    if (d.kind == ItemKind::Ammo && count == 1) s.count = d.bundle;   // a bundle (a single shot is made by hand)
    if (d.kind == ItemKind::Wand) s.fuel = rng_.range(d.chargesMin, std::max(d.chargesMin, d.chargesMax));
    if (d.artifact) artifactsInPlay_.insert(def);
    return s;
}

bool Server::randomFloor(Vec2& out, bool awayFromStart, int room) {
    for (int tries = 0; tries < 500; tries++) {
        int x, y;
        if (room >= 0) {
            const Room& r = map_.rooms[room];
            x = rng_.range(r.x, r.x + r.w - 1);
            y = rng_.range(r.y, r.y + r.h - 1);
        } else {
            x = rng_.range(1, map_.w - 2);
            y = rng_.range(1, map_.h - 2);
        }
        if (map_.at(x, y) != T_FLOOR) continue;
        if (awayFromStart && std::abs(x - map_.startX) + std::abs(y - map_.startY) < 10) continue;
        out = Vec2(x + 0.5f, y + 0.5f);
        return true;
    }
    return false;
}

int Server::pickMonster(int depth, const std::vector<int>* pool) {
    if (pool && !pool->empty()) return (*pool)[rng_.range(0, (int)pool->size() - 1)];
    // Weighted by rarity; occasionally out-of-depth monsters appear.
    int maxDepth = depth + (rng_.chance(10) ? 2 : 0);
    int total = 0;
    for (auto& m : gData.monsters)
        if (!m.boss && m.rarity > 0 && m.depth <= maxDepth) total += m.rarity * (m.depth >= depth - 2 ? 3 : 1);
    if (total == 0) return 0;
    int r = rng_.range(1, total);
    for (size_t i = 0; i < gData.monsters.size(); i++) {
        auto& m = gData.monsters[i];
        if (m.boss || m.rarity <= 0 || m.depth > maxDepth) continue;
        r -= m.rarity * (m.depth >= depth - 2 ? 3 : 1);
        if (r <= 0) return (int)i;
    }
    return 0;
}

// A random artifact that isn't in play yet (or -1).
int Server::pickArtifact(int depth) {
    std::vector<int> cands;
    for (size_t i = 0; i < gData.items.size(); i++) {
        const ItemDef& d = gData.items[i];
        if (d.artifact && d.depth <= depth && !artifactsInPlay_.count((int)i)) cands.push_back((int)i);
    }
    return cands.empty() ? -1 : cands[rng_.range(0, (int)cands.size() - 1)];
}

int Server::pickItem(int depth) {
    // Now and then, something legendary.
    if (rng_.range(1, 1000) <= 15 + depth * 2) {
        int a = pickArtifact(depth + 2);
        if (a >= 0) return a;
    }
    int total = 0;
    for (auto& it : gData.items)
        if (it.rarity > 0 && it.depth <= depth + 1 && it.kind != ItemKind::Gold && !it.artifact && (ammoRule_ || it.kind != ItemKind::Ammo)) total += it.rarity;
    int r = rng_.range(1, std::max(1, total));
    for (size_t i = 0; i < gData.items.size(); i++) {
        auto& it = gData.items[i];
        if (it.rarity <= 0 || it.depth > depth + 1 || it.kind == ItemKind::Gold || it.artifact || (!ammoRule_ && it.kind == ItemKind::Ammo)) continue;
        r -= it.rarity;
        if (r <= 0) return (int)i;
    }
    return gData.findItem("ration");
}

void Server::spawnMonster(int def, Vec2 pos, bool forceAwake) {
    Monster m;
    m.id = newId();
    m.def = def;
    m.pos = pos;
    m.angle = rng_.frand() * 2 * PI;
    const MonsterDef& d = gData.monsters[def];
    m.maxHp = m.hp = std::max(1, (int)std::lround(d.hp.roll(rng_) * rules().monsterHp));
    m.asleep = !forceAwake && rng_.chance(d.sleep);
    m.boss = d.boss;
    monsters_.push_back(m);
}

void Server::dropItem(const ItemStack& it, Vec2 pos, int droppedBy) {
    if (it.empty()) return;
    FloorItem f;
    f.id = newId();
    f.item = it;
    f.droppedBy = droppedBy;
    // Scatter a little so piles don't overlap exactly.
    for (int i = 0; i < 8; i++) {
        Vec2 p = pos + Vec2(rng_.frand() * 0.6f - 0.3f, rng_.frand() * 0.6f - 0.3f);
        if (!map_.solidAt(p)) { pos = p; break; }
    }
    f.pos = pos;
    items_.push_back(f);
}

void Server::populate(const std::vector<int>* pool) {
    int depth = power_;
    // Monsters
    int groups = 7 + depth + rng_.range(0, 3) + rules().extraGroups;
    groups = clampi((int)std::lround(groups * rules().monsterCount), 3, 45);   // The Abyss gets crowded
    for (int g = 0; g < groups; g++) {
        int def = pickMonster(depth, pool);
        const MonsterDef& d = gData.monsters[def];
        Vec2 anchor;
        if (!randomFloor(anchor, true)) continue;
        int n = rng_.range(d.groupMin, d.groupMax);
        for (int i = 0; i < n; i++) {
            Vec2 p = anchor;
            for (int t = 0; t < 10; t++) {
                Vec2 c = anchor + Vec2(rng_.frand() * 3 - 1.5f, rng_.frand() * 3 - 1.5f);
                if (map_.circleFree(c, MONSTER_RADIUS)) { p = c; break; }
            }
            spawnMonster(def, p);
        }
    }
    // Items
    int nItems = 9 + rng_.range(0, 5);
    for (int i = 0; i < nItems; i++) {
        Vec2 p;
        if (randomFloor(p, false)) dropItem(makeItem(pickItem(depth)), p);
    }
    placeChestsAndKeys();
    // Guaranteed supplies: food and light.
    for (const char* id : {"ration", "ration", "torch", "flask_oil"}) {
        Vec2 p;
        int def = gData.findItem(id);
        if (def >= 0 && randomFloor(p, false)) dropItem(makeItem(def), p);
    }
    int gold = gData.findItem("gold");
    for (int i = 0; i < 4 + depth / 2; i++) {
        Vec2 p;
        if (randomFloor(p, false)) dropItem(makeItem(gold, std::max(1, (int)(rng_.range(5, 20) * depth * rules().gold))), p);
    }

    // Boss for the main quest.
    for (auto& q : quests_) {
        bool present = false;
        for (auto& m : monsters_) if (m.def == q.target) present = true;
        if (q.main && !q.done && q.type == Q_KILL_BOSS && q.depth == map_.depth && !present) {
            Vec2 bp(map_.downX + 0.5f, map_.downY + 0.5f);
            spawnMonster(q.target, bp, false);
            monsters_.back().asleep = true;  // sleeping on its hoard
            int escorts = rng_.range(2, 4);
            for (int i = 0; i < escorts; i++) {
                Vec2 c = bp + Vec2(rng_.frand() * 4 - 2, rng_.frand() * 4 - 2);
                if (map_.circleFree(c, MONSTER_RADIUS)) spawnMonster(pickMonster(depth), c);
            }
            text(-1, COL_ORANGE, "You sense a powerful presence on this level...");
        }
    }
}

// ------------------------------------------------------------------ ammo --

bool Server::reloadQuiver(SPlayer& p, int kind) {
    PlayerState& s = p.st;
    ItemStack& q = s.equip[EQ_QUIVER];
    if (!q.empty() && q.d().ammo == kind) return true;
    int best = -1;   // the kind shot last, else the first that fits
    for (int i = 0; i < INV_SIZE; i++) {
        const ItemStack& it = s.inv[i];
        if (it.empty() || it.d().kind != ItemKind::Ammo || it.d().ammo != kind) continue;
        if (best < 0 || it.def == p.lastAmmo) best = i;
    }
    if (best < 0) return false;
    std::swap(q, s.inv[best]);   // whatever was in the quiver goes back into the pack
    p.dirty = true;
    text(s.id, COL_WHITE, "You load " + itemName(q) + " into your quiver.");
    return true;
}

int Server::ammoItem(int kind) const {
    for (size_t i = 0; i < gData.items.size(); i++)
        if (gData.items[i].kind == ItemKind::Ammo && gData.items[i].ammo == kind) return (int)i;
    return -1;
}

void Server::dropAmmo(int def, Vec2 pos) {
    if (def < 0 || map_.solidAt(pos)) return;
    for (auto& it : items_)   // join a pile lying right there
        if (it.item.def == def && dist(it.pos, pos) < 0.6f) { it.item.count++; return; }
    ItemStack one;
    one.def = (int16_t)def;
    one.count = 1;
    dropItem(one, pos);
}

void Server::fillQuiver(PlayerState& s) {
    const ItemDef* w = s.weapon();
    if (!ammoRule_ || !w || !w->ranged || w->ammo < 0 || !s.equip[EQ_QUIVER].empty()) return;
    for (auto& it : s.inv)
        if (!it.empty() && it.d().kind == ItemKind::Ammo && it.d().ammo == w->ammo) return;   // has some
    int def = ammoItem(w->ammo);
    if (def < 0) return;
    s.equip[EQ_QUIVER] = makeItem(def);
    text(s.id, COL_WHITE, "You find " + itemName(s.equip[EQ_QUIVER]) + " at the bottom of your pack.");
}

// ------------------------------------------------------------ chests, keys --

static const char* kTrapNames[CH_COUNT] = {"", "", "poison needle", "gas trap", "fire trap", "summoning rune", "alarm", "teleport rune"};

ItemStack Server::makeChest(bool rich) {
    ItemStack c = makeItem(gData.findItem("chest"));
    int r = rng_.range(0, 99), code;
    if (rich) code = r < 75 ? CH_TREASURE : rng_.range(CH_TRAP_NEEDLE, CH_TRAP_SUMMON);
    else code = r < 45 ? CH_TREASURE : r < 70 ? CH_EMPTY : rng_.range(CH_TRAP_NEEDLE, CH_TRAP_TELEPORT);
    c.fuel = code | (rich ? CHEST_RICH : 0);
    return c;
}

// A key for every coloured door (somewhere you can get to without it), rich chests in the
// vault behind it, and a few ordinary chests around the level.
void Server::placeChestsAndKeys() {
    if (gData.findItem("chest") < 0) return;
    // What can be reached from the stairs without any key?
    std::vector<uint8_t> reach(map_.w * map_.h, 0);
    std::vector<int> stack = {map_.startY * map_.w + map_.startX};
    reach[stack[0]] = 1;
    while (!stack.empty()) {
        int c = stack.back();
        stack.pop_back();
        int x = c % map_.w, y = c / map_.w;
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k];
            if (!map_.inside(nx, ny) || reach[ny * map_.w + nx]) continue;
            uint8_t t = map_.at(nx, ny);
            if (t == T_ROCK || t == T_WALL || t == T_DOOR_KEY) continue;
            reach[ny * map_.w + nx] = 1;
            stack.push_back(ny * map_.w + nx);
        }
    }
    auto spotIn = [&](bool inReach, Vec2& out) {
        for (int t = 0; t < 300; t++) {
            int x = rng_.range(1, map_.w - 2), y = rng_.range(1, map_.h - 2);
            if (map_.at(x, y) != T_FLOOR || !(map_.flag(x, y) & F_ROOM) || (bool)reach[y * map_.w + x] != inReach) continue;
            if (dist(Vec2(x + 0.5f, y + 0.5f), Vec2(map_.startX + 0.5f, map_.startY + 0.5f)) < 5) continue;
            out = Vec2(x + 0.5f, y + 0.5f);
            return true;
        }
        return false;
    };
    bool vault = false;
    for (int y = 0; y < map_.h; y++)
        for (int x = 0; x < map_.w; x++) {
            if (map_.at(x, y) != T_DOOR_KEY) continue;
            vault = true;
            int color = map_.keyDoorColor(x, y), keyDef = -1;
            for (size_t i = 0; i < gData.items.size(); i++)
                if (gData.items[i].kind == ItemKind::Key && gData.items[i].keyColor == color) keyDef = (int)i;
            Vec2 p;
            if (keyDef >= 0 && spotIn(true, p)) dropItem(makeItem(keyDef), p);
        }
    if (vault) {
        int n = rng_.range(1, 2);
        for (int i = 0; i < n; i++) {
            Vec2 p;
            if (spotIn(false, p)) dropItem(makeChest(true), p);
        }
        text(-1, COL_YELLOW, "Somewhere on this level, a door with a coloured lock guards a treasure.");
    }
    int n = rng_.range(0, 2) + (power_ >= 5 ? 1 : 0);
    for (int i = 0; i < n; i++) {
        Vec2 p;
        if (spotIn(true, p)) dropItem(makeChest(false), p);
    }
}

void Server::openChest(SPlayer& p, size_t idx) {
    PlayerState& s = p.st;
    int code = items_[idx].item.fuel & 15;
    bool known = items_[idx].item.fuel & CHEST_KNOWN, rich = items_[idx].item.fuel & CHEST_RICH;
    Vec2 pos = items_[idx].pos;
    int openDef = gData.findItem("chest_open");
    if (openDef >= 0) items_[idx].item = makeItem(openDef);   // same entity: its picture just changes
    else items_.erase(items_.begin() + idx);
    p.useCd = 0.6f;
    event(EV_DOOR, pos);
    if (chestTrapped(code)) {
        // Notice and disarm it: skill, DEX and experience; much easier when it was detected.
        int chance = 10 + s.c().disarm * 8 + s.mod(DEX) * 4 + s.level * 2 + (known ? 35 : 0) - power_;
        if (rng_.chance(clampi(chance, 5, 95))) {
            text(s.id, COL_GREEN, std::string("You find a ") + kTrapNames[code] + " on the chest and disarm it.");
        } else {
            springTrap(p, code, pos);
            if (s.dead) return;
        }
    }
    if (code == CH_EMPTY) { text(s.id, COL_GREY, "The chest is empty."); return; }
    // Treasure: gold and items (vault chests: more and better).
    int gold = gData.findItem("gold");
    std::vector<ItemStack> loot;
    if (gold >= 0) loot.push_back(makeItem(gold, std::max(1, (int)(rng_.range(8, 25) * std::max(1, power_) * (rich ? 3 : 1) * rules().gold))));
    int n = rich ? rng_.range(2, 3) : rng_.range(1, 2);
    for (int i = 0; i < n; i++) loot.push_back(makeItem(pickItem(power_ + (rich ? 3 : 1))));
    if (rich && rng_.chance(12)) { int a = pickArtifact(power_ + 4); if (a >= 0) loot.push_back(makeItem(a)); }
    for (size_t i = 0; i < loot.size(); i++) {
        float a = (float)i / loot.size() * 2 * PI;
        dropItem(loot[i], pos + dirFromAngle(a) * 0.35f);
    }
    text(s.id, COL_YELLOW, rich ? "The chest is full of treasure!" : "There is something in the chest.");
}

void Server::springTrap(SPlayer& p, int trap, Vec2 pos) {
    PlayerState& s = p.st;
    int dmg = rng_.range(2, 8) + power_;
    text(s.id, COL_RED, std::string("It's a trap! A ") + kTrapNames[trap] + "!");
    switch (trap) {
        case CH_TRAP_NEEDLE:
            s.poisonT = std::max(s.poisonT, 8.0f + power_);
            damagePlayer(p, std::max(1, dmg / 2), "a poison needle", Element::Poison);
            break;
        case CH_TRAP_GAS:
            explode(pos, 2.5f, dmg, Element::Poison, gData.findSpell("stinking_cloud"), true, nullptr);
            break;
        case CH_TRAP_FIRE:
            explode(pos, 2.2f, dmg + rng_.range(1, 6), Element::Fire, gData.findSpell("fireball"), true, nullptr);
            break;
        case CH_TRAP_SUMMON: {
            int n = rng_.range(2, 3) + power_ / 6;
            for (int i = 0; i < n; i++) {
                Vec2 c = pos + dirFromAngle(rng_.frand() * 2 * PI) * (1.0f + rng_.frand());
                if (map_.circleFree(c, MONSTER_RADIUS)) spawnMonster(pickMonster(power_ + 1), c, true);
            }
            text(-1, COL_ORANGE, "Monsters appear around the chest!");
            break;
        }
        case CH_TRAP_ALARM:
            for (auto& m : monsters_) wakeMonster(m, false);
            text(-1, COL_ORANGE, "A loud bell rings! Everything on this level heard it.");
            break;
        case CH_TRAP_TELEPORT: {
            Vec2 best = p.pos;
            float bestD = 0;
            for (int t = 0; t < 30; t++) {
                Vec2 c;
                if (randomFloor(c, false) && dist(c, p.pos) > bestD) { best = c; bestD = dist(c, p.pos); }
            }
            p.pos = best;
            sendTeleport(p);
            text(s.id, COL_PURPLE, "The world twists around you.");
            break;
        }
        default: break;
    }
    p.dirty = true;
}

void Server::placePlayers() {
    Vec2 start(map_.startX + 0.5f, map_.startY + 0.5f);
    int i = 0;
    for (auto& [id, p] : players_) {
        if (!p.joined) continue;
        Vec2 pos = start;
        if (i > 0) {
            for (int t = 0; t < 30; t++) {
                float a = rng_.frand() * 2 * PI;
                Vec2 c = start + dirFromAngle(a) * (0.8f + rng_.frand() * 1.5f);
                if (map_.circleFree(c, PLAYER_RADIUS) && map_.los(start, c)) { pos = c; break; }
            }
        }
        p.pos = pos;
        // Face the most open direction.
        float best = 0, bestA = 0;
        for (int k = 0; k < 8; k++) {
            float a = k * PI / 4;
            float d = 0;
            while (d < 10 && !map_.solidAt(pos + dirFromAngle(a) * d)) d += 0.25f;
            if (d > best) { best = d; bestA = a; }
        }
        p.angle = bestA;
        i++;
    }
}

void Server::newLevel(int depth, bool down) {
    // Artifacts left lying on the floor seep back into the dungeon (they can be found again).
    for (auto& it : items_)
        if (!it.item.empty() && it.item.d().artifact) artifactsInPlay_.erase(it.item.def);
    monsters_.clear();
    items_.clear();
    projectiles_.clear();
    effects_.clear();
    rubbleDig_.clear();
    // Side quests belong to a single level.
    bool abandoned = false;
    for (auto& q : quests_)
        if (!q.main && !q.done) abandoned = true;
    quests_.erase(std::remove_if(quests_.begin(), quests_.end(), [](const Quest& q) { return !q.main; }), quests_.end());
    if (abandoned) text(-1, COL_GREY, "You abandon the unfinished tasks of the level above.");

    uint32_t seed = baseSeed_ + (uint32_t)depth * 1337u + rng_.next() % 100000u;
    bool campaignLevel = campaign_ >= 0 && depth > 0;
    if (campaignLevel) depth = std::min(depth, (int)gData.campaigns[campaign_].levels.size());
    bool deeper = depth > maxDepth_;
    maxDepth_ = std::max(maxDepth_, depth);
    recallT_ = 0;
    descentT_ = 0;
    if (campaignLevel) {
        addCampaignGoal(depth);
        map_.seed = seed;
        if (!buildCampaignLevel(depth)) { generateLevel(map_, seed, depth); map_.depth = depth; power_ = depth; populate(); }
    } else {
        generateLevel(map_, seed, depth);
        power_ = std::max(1, depth);
        collapseQuests();
        if (depth > 0) {
            populate();
            if (rng_.chance(65)) makeSideQuest();
        }
    }
    for (auto& it : items_)
        if (it.item.d().artifact) { text(-1, COL_YELLOW, "You feel there is something special on this level..."); break; }
    if (abyss() && depth > 0) {
        char buf[120];
        const DifficultyRules& base = difficultyRules(difficulty_);
        std::snprintf(buf, sizeof buf, "The Abyss, level %d: monsters %.1fx as strong and %.1fx as many as on level 1.", depth,
                      rules().monsterHp / base.monsterHp, rules().monsterCount / base.monsterCount);
        text(-1, COL_ORANGE, buf);
        if (deeper) recordScore();
    }
    placePlayers();
    for (auto& [id, p] : players_) {
        if (!p.joined) continue;
        sendLevel(id);
        sendTeleport(p);
    }
    sendQuests();
    if (!pendingIntro_.empty()) { text(-1, COL_CYAN, pendingIntro_); pendingIntro_.clear(); }
    flowT_ = 0;
    if (!savePath_.empty()) saveGame();
    std::printf("[server] level %d generated (seed %u), %zu monsters, %zu items\n", depth, map_.seed, monsters_.size(), items_.size());
    (void)down;
}

// ============================================================ simulation ====

void Server::tick() {
    tickNo_++;
    handleNet();
    float dt = TICK_DT;
    updatePlayers(dt);
    flowT_ -= dt;
    if (flowT_ <= 0) { computeFlow(); flowT_ = 0.4f; }
    updateMonsters(dt);
    updateProjectiles(dt);
    for (auto& e : effects_) e.t += dt;
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(), [](const Effect& e) { return e.t >= e.life; }), effects_.end());
    checkPartyWipe(dt);
    if (recallT_ > 0) {
        recallT_ -= dt;
        if (recallT_ <= 0) {
            int target = map_.depth > 0 ? 0 : std::max(1, maxDepth_);
            text(-1, COL_PURPLE, target == 0 ? "You feel yourselves yanked upwards!" : "You feel yourselves yanked downwards!");
            event(EV_STAIRS, Vec2(map_.startX + 0.5f, map_.startY + 0.5f));
            newLevel(target, target > 0);
        }
    }
    if (descentT_ > 0) {
        descentT_ -= dt;
        if (descentT_ <= 0) {
            text(-1, COL_PURPLE, "The floor opens beneath you, and you fall deeper into the dungeon!");
            event(EV_STAIRS, Vec2(map_.startX + 0.5f, map_.startY + 0.5f));
            newLevel(descentTo_, true);
        }
    }
    for (auto& [id, p] : players_) p.st.recallT = recallT_;
    if (!savePath_.empty() && tickNo_ % (30 * 60) == 0) saveGame();
    if (tickNo_ % 2 == 0) sendSnapshots();
    sendPlayerStates();
    net_->flush();
}

void Server::pausedTick() {
    handleNet();
    for (auto& [pid, p] : players_) {
        if (!p.joined || !p.dirty) continue;
        Writer w;
        w.u8(S_PSTATE);
        w.u32(tickNo_);
        p.st.write(w);
        send(pid, w, true);
        p.dirty = false;
    }
    net_->flush();
}

void Server::checkPartyWipe(float dt) {
    int alive = 0, total = 0;
    for (auto& [id, p] : players_) {
        if (!p.joined) continue;
        total++;
        if (!p.st.dead) alive++;
    }
    if (total == 0) { partyWipeT_ = -1; return; }
    if (alive > 0) { partyWipeT_ = -1; return; }
    if (partyWipeT_ < 0) {
        partyWipeT_ = 6.0f;
        if (abyss()) {
            abyssDeaths_++;
            if (!abyssFirstDeath_) abyssFirstDeath_ = map_.depth;
            text(-1, COL_RED, "The Abyss claims the party on level " + std::to_string(map_.depth) + ". Deepest level reached: " +
                                  std::to_string(maxDepth_) + ".");
            recordScore();
        } else {
            text(-1, COL_RED, rules().wipeRestarts ? "The whole party has perished. A new party will enter the dungeon..."
                                                   : "The whole party has fallen... but the dungeon is not done with you yet.");
        }
    }
    partyWipeT_ -= dt;
    if (partyWipeT_ > 0) return;
    partyWipeT_ = -1;
    if (abyss()) {
        // The Abyss: everyone wakes up halfway back up, keeping what they carry.
        int back = std::max(1, (map_.depth + 1) / 2);
        newLevel(back, false);
        for (auto& [id, p] : players_)
            if (p.joined && p.st.dead) p.st.respawnT = 0.01f;
        text(-1, COL_YELLOW, "You wake up on level " + std::to_string(back) + ", halfway back up. Deaths in this run: " +
                                 std::to_string(abyssDeaths_) + ".");
        return;
    }
    if (!rules().wipeRestarts) {
        // Easy: everyone gets back up at the stairs of this level.
        for (auto& [id, p] : players_)
            if (p.joined) p.st.respawnT = 0.01f;
        return;
    }
    for (auto& [id, p] : players_) rollCharacter(p, p.st.cls);
    newGame();
    sendRoster();
    text(-1, COL_YELLOW, "A new party descends into Dum.");
}

// The Abyss: write this run's line on the high score list.
void Server::recordScore() {
    if (!abyss() || scorePath_.empty() || maxDepth_ <= 0) return;
    AbyssScore sc;
    sc.run = baseSeed_;
    sc.deepest = maxDepth_;
    sc.deaths = abyssDeaths_;
    sc.firstDeath = abyssFirstDeath_;
    std::vector<const PlayerState*> party;
    for (auto& [id, p] : players_) if (p.joined) party.push_back(&p.st);
    for (auto& [k, c] : saved_) party.push_back(&c.st);
    for (auto* st : party) {
        sc.level = std::max(sc.level, st->level);
        sc.party += (sc.party.empty() ? "" : ", ") + st->name + " the " + st->c().name;
    }
    int place = saveAbyssScore(scorePath_, sc);
    if (place > 0 && place <= 10) text(-1, COL_CYAN, "High score list: place " + std::to_string(place) + ".");
}

void Server::updatePlayers(float dt) {
    for (auto& [id, p] : players_) {
        if (!p.joined) continue;
        PlayerState& s = p.st;
        s.attackCd = std::max(0.0f, s.attackCd - dt);
        s.castCd = std::max(0.0f, s.castCd - dt);
        for (float& t : s.recharge) t = std::max(0.0f, t - dt);
        // Rods recharge in your pack (counted in tenths of seconds).
        p.rodAcc += dt;
        if (p.rodAcc >= 0.1f) {
            int steps = (int)(p.rodAcc / 0.1f);
            p.rodAcc -= steps * 0.1f;
            for (auto& it : s.inv)
                if (!it.empty() && it.d().kind == ItemKind::Rod && it.fuel > 0) {
                    it.fuel = std::max(0, it.fuel - steps);
                    if (it.fuel == 0) p.dirty = true;
                }
        }
        p.useCd = std::max(0.0f, p.useCd - dt);
        p.stairsConfirmT = std::max(0.0f, p.stairsConfirmT - dt);
        p.hurtT = std::max(0.0f, p.hurtT - dt);
        p.attackT = std::max(0.0f, p.attackT - dt);
        p.noise = std::max(0.0f, p.noise - dt * 0.5f);

        if (s.dead) {
            s.respawnT -= dt;
            if (s.respawnT <= 0) {
                // Respawn at the up staircase (only happens while someone else lives).
                s.dead = false;
                s.hp = std::max(1, s.maxHp / 2);
                s.food = std::max(s.food, 300.0f);
                p.pos = Vec2(map_.startX + 0.5f, map_.startY + 0.5f);
                sendTeleport(p);
                text(id, COL_YELLOW, "You awaken at the stairs, weakened but alive.");
                text(-1, COL_GREY, s.name + " has returned.");
                p.dirty = true;
            }
            continue;
        }

        auto tickDown = [&](float& t, const char* msg) {
            if (t > 0) { t -= dt; if (t <= 0) { t = 0; text(id, COL_GREY, msg); } }
        };
        tickDown(s.blessT, "You feel less righteous.");
        tickDown(s.lightT, "The magical light fades.");
        tickDown(s.detectT, "Your awareness of monsters fades.");
        tickDown(s.slowT, "You feel yourself speed up.");
        tickDown(s.hasteT, "You feel yourself slow down.");
        tickDown(s.shieldT, "Your skin returns to normal.");
        tickDown(s.resistT, "You feel less protected from the elements.");
        tickDown(s.heroT, "The battle fury leaves you.");
        if (s.poisonT > 0) {
            float before = s.poisonT;
            s.poisonT = std::max(0.0f, s.poisonT - dt);
            if ((int)before != (int)s.poisonT) damagePlayer(p, 1 + power_ / 8, "poison", Element::Poison);
            if (s.poisonT <= 0 && !s.dead) text(id, COL_GREY, "You are no longer poisoned.");
            if (s.dead) continue;
        }

        // Food
        s.food -= dt * rules().hunger * (1.0f - s.slowDigest() / 100.0f);
        if (s.food < 150 && !p.warnedFood) { text(id, COL_ORANGE, "You are getting hungry."); p.warnedFood = true; }
        if (s.food > 200) p.warnedFood = false;
        if (s.food <= 0) {
            s.food = 0;
            p.starve += dt;
            if (p.starve >= 3.0f) { p.starve = 0; text(id, COL_RED, "You are starving!"); damagePlayer(p, 1, "starvation"); }
        }

        // Light fuel
        ItemStack& light = s.equip[EQ_LIGHT];
        if (!light.empty() && light.fuel > 0 && map_.depth > 0 && !light.d().everburning) {  // no need to burn it in daylight
            // Fuel counts whole seconds.
            p.fuelAcc += dt * rules().fuel;
            if (p.fuelAcc >= 1.0f) {
                p.fuelAcc -= 1.0f;
                light.fuel--;
                if (light.fuel == 60 && !p.warnedLight) { text(id, COL_ORANGE, "Your light is growing faint."); p.warnedLight = true; }
                if (light.fuel <= 0) {
                    if (light.d().id == "torch") {
                        text(id, COL_ORANGE, "Your torch burns out.");
                        light = ItemStack();
                    } else {
                        text(id, COL_ORANGE, "Your " + light.d().name + " has run out of fuel.");
                    }
                    p.dirty = true;
                }
            }
        }
        if (!light.empty() && light.fuel > 60) p.warnedLight = false;

        // Regeneration (only when fed)
        if (s.food > 50) {
            p.regenHp += dt * (1.0f + 0.25f * std::max(0, s.mod(CON))) * (0.05f + s.maxHp / 400.0f) * (1.0f + s.regenBonus() / 100.0f);
            if (p.regenHp >= 1) { int n = (int)p.regenHp; p.regenHp -= n; s.hp = std::min(s.maxHp, s.hp + n); }
            if (s.maxMp > 0) {
                s.mp = std::min(s.maxMp, s.mp + dt * (0.12f + 0.03f * std::max(0, s.mod(s.c().castStat)) + s.maxMp / 300.0f));
            }
        }

        // Searching for secret doors nearby.
        p.search += dt;
        if (p.search >= 1.0f) {
            p.search = 0;
            int px = (int)p.pos.x, py = (int)p.pos.y;
            for (int y = py - 3; y <= py + 3; y++)
                for (int x = px - 3; x <= px + 3; x++) {
                    if (map_.at(x, y) != T_DOOR_SECRET) continue;
                    int chance = 4 + (s.c().search + s.searchBonus()) * 4 + s.mod(WIS) * 2 + (std::abs(x - px) + std::abs(y - py) <= 1 ? 10 : 0);
                    if (rng_.chance(clampi(chance, 1, 90))) {
                        map_.set(x, y, T_DOOR);
                        sendTile(x, y);
                        text(id, COL_YELLOW, "You have found a secret door!");
                        event(EV_DOOR, Vec2(x + 0.5f, y + 0.5f));
                    }
                }
        }

        // Searching: a good eye (class 'search' skill) may notice what is odd about a chest
        // standing right next to it - trapped, empty, or full.
        if ((tickNo_ + id) % 15 == 0 && s.c().search > 0) {
            for (auto& it : items_) {
                if (!isClosedChest(it.item) || (it.item.fuel & CHEST_KNOWN) || dist(it.pos, p.pos) > 2.0f) continue;
                if (!rng_.chance(s.c().search * 4 + s.mod(WIS) * 2)) continue;
                it.item.fuel |= CHEST_KNOWN;
                int code = it.item.fuel & 15;
                text(id, COL_CYAN, chestTrapped(code) ? std::string("You notice a ") + kTrapNames[code] + " on the chest!"
                                   : code == CH_EMPTY ? "The chest seems light - probably empty." : "The chest is heavy. Treasure!");
            }
        }

        // Auto-pickup: gold always, other items when enabled and there is room in the pack.
        // Items you dropped yourself stay on the floor (press E to take them back).
        for (size_t i = 0; i < items_.size(); i++) {
            const FloorItem& f = items_[i];
            if (dist(f.pos, p.pos) > 0.6f) continue;
            ItemKind fk = f.item.d().kind;
            bool gold = fk == ItemKind::Gold || fk == ItemKind::Key || fk == ItemKind::Ammo;   // purse, key ring, quiver
            if (f.item.d().kind == ItemKind::Chest) continue;   // opened with E, never by walking over
            if (!gold && (!p.autoPickup || f.droppedBy == id || !canCarry(p, f.item))) continue;
            pickupItem(p, i, false);
            break;  // at most one item per tick
        }
    }
}

void Server::computeFlow() {
    // Multi-source BFS from all living players. Two variants: through doors and not.
    for (int pass = 0; pass < 2; pass++) {
        std::vector<int>& f = pass == 0 ? flowDoors_ : flowNoDoors_;
        f.assign(map_.w * map_.h, 9999);
        std::queue<int> q;
        for (auto& [id, p] : players_) {
            if (!p.joined || p.st.dead) continue;
            int x = (int)p.pos.x, y = (int)p.pos.y;
            if (!map_.inside(x, y)) continue;
            f[y * map_.w + x] = 0;
            q.push(y * map_.w + x);
        }
        while (!q.empty()) {
            int c = q.front();
            q.pop();
            int x = c % map_.w, y = c / map_.w;
            if (f[c] > 60) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int nx = x + dx, ny = y + dy;
                    if (!map_.inside(nx, ny)) continue;
                    uint8_t t = map_.at(nx, ny);
                    bool pass_ = !Map::tileSolid(t) || (pass == 0 && (t == T_DOOR || t == T_DOOR_LOCKED));
                    if (!pass_) continue;
                    if (dx && dy && (map_.solid(x + dx, y) || map_.solid(x, y + dy))) continue;  // no corner cutting
                    int cost = f[c] + 1;
                    if (cost < f[ny * map_.w + nx]) { f[ny * map_.w + nx] = cost; q.push(ny * map_.w + nx); }
                }
        }
    }
}

void Server::wakeMonster(Monster& m, bool shout) {
    if (!m.asleep) return;
    m.asleep = false;
    if (m.boss) text(-1, COL_RED, capitalize(theName(m.d())) + " awakens!");
    event(EV_GROWL, m.pos, m.id);
    if (shout) {
        // Nearby sleepers of the same kind wake up too.
        for (auto& o : monsters_)
            if (o.asleep && o.def == m.def && dist(o.pos, m.pos) < 4.0f && rng_.chance(60)) o.asleep = false;
    }
}

void Server::updateMonsters(float dt) {
    std::vector<size_t> breeders;
    for (size_t i = 0; i < monsters_.size(); i++) {
        Monster& m = monsters_[i];
        const MonsterDef& d = m.d();
        m.attackCd = std::max(0.0f, m.attackCd - dt);
        m.spellCd = std::max(0.0f, m.spellCd - dt);
        m.hurtT = std::max(0.0f, m.hurtT - dt);
        m.attackT = std::max(0.0f, m.attackT - dt);
        m.slowT = std::max(0.0f, m.slowT - dt);
        m.sleepT = std::max(0.0f, m.sleepT - dt);
        m.fearT = std::max(0.0f, m.fearT - dt);

        // Find the closest visible living player.
        SPlayer* best = nullptr;
        float bestD = 1e9f;
        for (auto& [id, p] : players_) {
            if (!p.joined || p.st.dead) continue;
            float dd = dist(p.pos, m.pos);
            if (dd > d.sight * 1.5f) continue;
            // Players are easier to spot when carrying a bright light.
            float seeRange = d.sight + p.st.lightRadius() * 0.5f;
            if (dd < seeRange && map_.los(m.pos, p.pos) && dd < bestD) { best = &p; bestD = dd; }
        }

        if (d.regen > 0 && m.hp < m.maxHp) {
            m.regenAcc += d.regen * dt;
            int n = (int)m.regenAcc;
            if (n > 0) { m.hp = std::min(m.maxHp, m.hp + n); m.regenAcc -= n; }
        }
        if (d.breedTime > 0 && !m.asleep) {
            m.breedT += dt;
            if (m.breedT >= d.breedTime) { m.breedT = -rng_.frand() * d.breedTime * 0.5f; breeders.push_back(i); }
        }
        if (m.asleep && m.sleepT > 0) continue;   // magical sleep
        if (m.asleep) {
            // Chance to notice players each tick: distance, stealth, noise and light matter.
            for (auto& [id, p] : players_) {
                if (!p.joined || p.st.dead) continue;
                float dd = dist(p.pos, m.pos);
                if (dd > d.sight * 1.5f) continue;
                float base = (1.0f - dd / (d.sight * 1.5f));
                float stealth = 1.0f - 0.12f * (p.st.c().stealth + p.st.stealthBonus()) - 0.03f * std::max(0, p.st.mod(DEX));
                float perSec = (base * base * 0.35f * std::max(0.1f, stealth) + p.noise * base * 0.8f) * rules().alertness;
                if (map_.los(m.pos, p.pos) && dd <= p.st.lightRadius() + 1) perSec *= 2.0f;
                if (rng_.frand() < perSec * dt) { wakeMonster(m, true); break; }
            }
            continue;
        }

        float speed = d.speed * rules().monsterSpeed * (m.slowT > 0 ? 0.5f : 1.0f);
        Vec2 move;
        if (best && d.summon != -1 && m.summons < 3 && m.fearT <= 0 && monsters_.size() < 100 && rng_.frand() < d.summonRate * dt) {
            // Calls for help: one or two more appear nearby (at most three times).
            m.summons++;
            int kind = d.summon == -2 ? m.def : d.summon;
            int n = rng_.range(1, 2), made = 0;
            Vec2 at = m.pos;
            int who = best->st.id;
            std::string name = capitalize(theName(d));
            for (int t = 0; t < 20 && made < n; t++) {
                Vec2 c = at + dirFromAngle(rng_.frand() * 2 * PI) * (0.8f + rng_.frand() * 1.5f);
                if (map_.circleFree(c, MONSTER_RADIUS) && map_.los(at, c)) { spawnMonster(kind, c, true); made++; }
            }
            if (made) {
                text(who, COL_ORANGE, name + " calls for help!");
                event(EV_GROWL, at, 0);
            }
            break;   // the monster list changed: the rest moves next tick
        }
        if (best && d.blink && m.fearT <= 0) {
            m.blinkT -= dt;
            if (m.blinkT <= 0) {
                m.blinkT = 2.5f + rng_.frand() * 3.0f;
                for (int t = 0; t < 15; t++) {
                    Vec2 c = best->pos + dirFromAngle(rng_.frand() * 2 * PI) * (1.5f + rng_.frand() * 3.0f);
                    if (map_.circleFree(c, MONSTER_RADIUS) && map_.los(best->pos, c)) { m.pos = c; event(EV_CAST, c, m.id); break; }
                }
            }
        }
        if (best) {
            m.target = best->st.id;
            m.hasSeen = true;
            Vec2 to = best->pos - m.pos;
            m.angle = std::atan2(to.y, to.x);
            float reach = d.reach + PLAYER_RADIUS + MONSTER_RADIUS * d.size;
            if (m.fearT > 0) {
                // Frightened: run away from the player.
                move = (m.pos - best->pos).norm();
                m.angle = std::atan2(move.y, move.x);
            } else if (bestD <= reach) {
                if (m.attackCd <= 0) {
                    m.attackCd = d.attackCd;
                    m.attackT = 0.3f;
                    int roll = rng_.range(1, 20);
                    int toHit = roll + d.toHit + power_ / 2 + rules().monsterToHit;
                    if (roll == 20 || (roll != 1 && toHit >= best->st.armorClass())) {
                        int dmg = std::max(1, (int)std::lround(d.dmg.roll(rng_) * (roll == 20 ? 2 : 1) * rules().monsterDamage));
                        damagePlayer(*best, dmg, theName(d));
                        PlayerState& bs = best->st;
                        if (d.poison > 0 && !bs.dead) {
                            float t = (float)d.poison * (bs.resists(Element::Poison) ? 0.5f : 1.0f);
                            if (bs.poisonT <= 0) text(bs.id, COL_GREEN, "You are poisoned!");
                            bs.poisonT = std::min(30.0f, bs.poisonT + t);
                            best->dirty = true;
                        }
                        if (d.drainStat >= 0 && !bs.dead && rng_.chance(30) && bs.stats[d.drainStat] - bs.drained[d.drainStat] > 3) {
                            bs.drained[d.drainStat]++;
                            static const char* feel[NUM_STATS] = {"weaker", "clumsier", "sickly", "stupider", "naive"};
                            text(bs.id, COL_ORANGE, std::string("You feel ") + feel[d.drainStat] + "! (" + kStatNames[d.drainStat] + " drained)");
                            best->dirty = true;
                        }
                        if (d.drainsLight) {
                            ItemStack& L = best->st.equip[EQ_LIGHT];
                            if (!L.empty() && L.fuel > 1 && L.d().fuel > 0) {
                                L.fuel = std::max(1, L.fuel - rng_.range(30, 90));
                                best->dirty = true;
                                text(best->st.id, COL_ORANGE, capitalize(theName(d)) + " drains the fire from your light!");
                            }
                        }
                        if (d.steals && best->st.gold > 0 && rng_.chance(60)) {
                            int amt = std::min(best->st.gold, 5 + best->st.gold * rng_.range(10, 30) / 100);
                            best->st.gold -= amt;
                            best->dirty = true;
                            text(best->st.id, COL_ORANGE, capitalize(theName(d)) + " touches you... your purse feels " + std::to_string(amt) + " gold lighter!");
                            Vec2 away;
                            if (randomFloor(away, false)) { m.pos = away; m.hasSeen = false; m.target = -1; }
                            text(best->st.id, COL_GREY, "The thief vanishes!");
                            continue;
                        }
                    } else {
                        text(best->st.id, COL_GREY, capitalize(theName(d)) + " misses you.");
                        event(EV_MISS, best->pos);
                    }
                }
            } else {
                // Ranged spell?
                if (d.spell >= 0 && m.spellCd <= 0 && bestD < 11.0f && rng_.frand() < d.spellRate * dt) {
                    const SpellDef& sp = gData.spells[d.spell];
                    Projectile pr;
                    pr.id = newId();
                    pr.spell = d.spell;
                    pr.pos = m.pos;
                    pr.vel = to.norm() * sp.speed;
                    pr.fromMonster = true;
                    pr.target = best->st.id;
                    pr.dmg = std::max(1, (int)std::lround((sp.dmg.roll(rng_) + power_ / 2) * rules().monsterDamage));
                    pr.radius = sp.radius;
                    pr.elem = sp.elem;
                    pr.ttl = 4.0f;
                    projectiles_.push_back(pr);
                    m.spellCd = 2.0f;
                    m.attackT = 0.4f;
                    event(EV_CAST, m.pos, m.id);
                    text(best->st.id, COL_ORANGE, capitalize(theName(d)) + " casts a " + sp.name + ".");
                } else if (bestD > 1.4f || !map_.los(m.pos, best->pos)) {
                    move = to.norm();
                } else {
                    move = to.norm();
                }
            }
        }
        if ((!best || move.len2() > 0) && m.fearT <= 0) {
            // Follow the flow field toward players (they can smell/hear you).
            const std::vector<int>& flow = d.opensDoors ? flowDoors_ : flowNoDoors_;
            int x = (int)m.pos.x, y = (int)m.pos.y;
            int here = flow.empty() ? 9999 : flow[y * map_.w + x];
            bool direct = best && map_.los(m.pos, best->pos) && bestD < 4.0f;
            if (!direct && here < (m.hasSeen ? 40 : 18)) {
                int bx = x, by = y, bv = here;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (!map_.inside(nx, ny)) continue;
                        int v = flow[ny * map_.w + nx];
                        if (v < bv) { bv = v; bx = nx; by = ny; }
                    }
                if (bx != x || by != y) {
                    uint8_t t = map_.at(bx, by);
                    if (t == T_DOOR || t == T_DOOR_LOCKED) {
                        m.doorT += dt;
                        if (m.doorT > 0.8f) {
                            m.doorT = 0;
                            if (t == T_DOOR || rng_.chance(15)) {
                                map_.set(bx, by, T_DOOR_OPEN);
                                sendTile(bx, by);
                                event(EV_DOOR, Vec2(bx + 0.5f, by + 0.5f));
                                if (t == T_DOOR_LOCKED) text(-1, COL_ORANGE, "You hear a door burst open!");
                            }
                        }
                        move = Vec2();
                    } else {
                        move = (Vec2(bx + 0.5f, by + 0.5f) - m.pos).norm();
                        m.angle = std::atan2(move.y, move.x);
                    }
                }
                m.hasSeen = true;
            } else if (!best) {
                // Wander aimlessly.
                m.wanderT -= dt;
                if (m.wanderT <= 0) {
                    m.wanderT = 1.0f + rng_.frand() * 2.0f;
                    m.wanderDir = rng_.chance(40) ? Vec2() : dirFromAngle(rng_.frand() * 2 * PI);
                }
                move = m.wanderDir * 0.4f;
                if (move.len2() > 0) m.angle = std::atan2(move.y, move.x);
            }
        }

        // Erratic fliers flutter about.
        if (d.erratic > 0) {
            m.erraticT -= dt;
            if (m.erraticT <= 0 && rng_.frand() * 100.0f < d.erratic * dt * 3.0f) {
                m.erraticT = 0.4f + rng_.frand() * 0.4f;
                m.erraticDir = dirFromAngle(rng_.frand() * 2 * PI);
            }
            if (m.erraticT > 0) move = m.erraticDir;
        }
        // Separation from other monsters.
        for (auto& o : monsters_) {
            if (&o == &m) continue;
            Vec2 diff = m.pos - o.pos;
            float dd = diff.len();
            if (dd < 0.55f && dd > 0.001f) move += diff.norm() * (0.55f - dd) * 3.0f;
        }
        // Don't walk into players.
        for (auto& [id, p] : players_) {
            if (!p.joined || p.st.dead) continue;
            Vec2 diff = m.pos - p.pos;
            float dd = diff.len();
            float minD = PLAYER_RADIUS + MONSTER_RADIUS * d.size * 0.8f + 0.15f;
            if (dd < minD && dd > 0.001f) move += diff.norm() * 2.0f;
        }
        if (move.len2() > 0) {
            Vec2 step = move * (speed * dt);
            if (step.len() > speed * dt) step = step.norm() * (speed * dt);
            if (!map_.moveCircle(m.pos, step, MONSTER_RADIUS)) m.wanderT = 0;
        }
    }
    // Slow breeders: a copy now and then, but never more than 6 close together,
    // 20 of a kind on the level, or 100 monsters in all - you can always clear them out.
    for (size_t bi : breeders) {
        if (bi >= monsters_.size() || monsters_.size() >= 100) break;
        Monster src = monsters_[bi];
        int near = 0, all = 0;
        for (auto& o : monsters_)
            if (o.def == src.def) { all++; if (dist(o.pos, src.pos) < 6.0f) near++; }
        if (near >= 6 || all >= 20) continue;
        for (int t = 0; t < 8; t++) {
            Vec2 c = src.pos + dirFromAngle(rng_.frand() * 2 * PI) * 0.8f;
            if (map_.circleFree(c, MONSTER_RADIUS)) { spawnMonster(src.def, c, true); break; }
        }
    }
}

void Server::updateProjectiles(float dt) {
    for (size_t i = 0; i < projectiles_.size();) {
        Projectile& pr = projectiles_[i];
        pr.ttl -= dt;
        bool remove = pr.ttl <= 0;
        // Homing toward the auto-aimed target.
        Vec2 tpos;
        bool haveTarget = false;
        if (pr.fromMonster) {
            if (SPlayer* p = playerById((int)pr.target)) if (!p->st.dead) { tpos = p->pos; haveTarget = true; }
        } else if (pr.target) {
            if (Monster* m = monsterById(pr.target)) { tpos = m->pos; haveTarget = true; }
        }
        if (remove && pr.recover) dropAmmo(pr.ammoDef, pr.pos);   // flew its full range: it lies there
        float spd = pr.vel.len();
        if (haveTarget) {
            Vec2 want = (tpos - pr.pos).norm() * spd;
            float k = pr.fromMonster ? 2.0f : 8.0f;   // monster missiles are easier to dodge
            pr.vel = (pr.vel + (want - pr.vel) * clampf(k * dt, 0, 1)).norm() * spd;
        }
        int sub = 3;
        for (int s = 0; s < sub && !remove; s++) {
            pr.pos += pr.vel * (dt / sub);
            if (map_.solidAt(pr.pos)) {
                if (pr.radius > 0) explode(pr.pos - pr.vel.norm() * 0.3f, pr.radius, pr.dmg, pr.elem, pr.spell, pr.fromMonster, playerById(pr.ownerPlayer), pr.power);
                if (pr.recover && rng_.chance(60)) dropAmmo(pr.ammoDef, pr.pos - pr.vel.norm() * 0.4f);   // bounced off the wall
                remove = true;
                break;
            }
            if (pr.fromMonster) {
                for (auto& [id, p] : players_) {
                    if (!p.joined || p.st.dead || dist(p.pos, pr.pos) > 0.4f) continue;
                    if (pr.radius > 0) explode(pr.pos, pr.radius, pr.dmg, pr.elem, pr.spell, true, nullptr);
                    else {
                        int dmg = pr.dmg;
                        // DEX gives a chance to partially dodge.
                        if (rng_.range(1, 20) + p.st.mod(DEX) >= 15) { dmg /= 2; text(id, COL_GREY, "You partially dodge the attack."); }
                        damagePlayer(p, dmg, gData.spells[pr.spell].name, pr.elem);
                        if (pr.elem == Element::Cold && !p.st.resists(Element::Cold)) { p.st.slowT = 3.0f; text(id, COL_CYAN, "You are slowed by the cold!"); }
                    }
                    remove = true;
                    break;
                }
            } else {
                for (size_t mi = 0; mi < monsters_.size(); mi++) {
                    Monster& m = monsters_[mi];
                    if (dist(m.pos, pr.pos) > 0.35f + 0.15f * m.d().size) continue;
                    SPlayer* owner = playerById(pr.ownerPlayer);
                    if (pr.radius > 0) {
                        explode(pr.pos, pr.radius, pr.dmg, pr.elem, pr.spell, false, owner, pr.power);
                    } else if (pr.spell < 0) {
                        // Arrow: roll to hit now.
                        int roll = rng_.range(1, 20);
                        int total = roll + pr.toHit + (m.asleep ? 4 : 0);
                        if (roll == 20 || (roll != 1 && total >= m.d().ac)) {
                            int dmg = std::max(1, pr.dice.roll(rng_) + pr.dmgBonus);
                            if (roll == 20) dmg += pr.dice.roll(rng_);
                            if (pr.sneak && m.asleep) dmg *= 2;
                            if (pr.recover && rng_.chance(25)) dropAmmo(pr.ammoDef, m.pos);   // pulled out afterwards
                            damageMonster(m, dmg, pr.elem, owner, roll == 20);
                        } else {
                            if (owner) text(owner->st.id, COL_GREY, "Your missile misses " + theName(m.d()) + ".");
                            if (pr.recover && rng_.chance(50)) dropAmmo(pr.ammoDef, m.pos + pr.vel.norm() * 0.6f);
                            event(EV_MISS, m.pos);
                            wakeMonster(m, false);
                        }
                    } else {
                        if (pr.dmg > 0) damageMonster(m, pr.dmg, pr.elem, owner, false);
                        if (m.hp > 0) applyEffect(m, gData.spells[pr.spell].effect, owner, pr.power);
                    }
                    remove = true;
                    break;
                }
            }
        }
        if (remove) projectiles_.erase(projectiles_.begin() + i);
        else i++;
    }
    // Remove dead monsters (hp <= 0) created by explosions etc.
    for (size_t i = 0; i < monsters_.size();) {
        if (monsters_[i].hp <= 0) killMonster(i, nullptr);
        else i++;
    }
}

// =============================================================== combat =====

Monster* Server::monsterById(uint32_t id) {
    for (auto& m : monsters_)
        if (m.id == id && m.hp > 0) return &m;
    return nullptr;
}

SPlayer* Server::playerById(int id) {
    auto it = players_.find(id);
    return it == players_.end() ? nullptr : &it->second;
}

// The auto-aim: best monster inside a cone in front of the player, with line of sight.
Monster* Server::findTarget(Vec2 from, float angle, float range, float arcDeg, bool seeInvisible) {
    Monster* best = nullptr;
    float bestScore = 1e9f;
    float half = arcDeg * 0.5f * PI / 180.0f;
    for (auto& m : monsters_) {
        if (m.hp <= 0) continue;
        Vec2 to = m.pos - from;
        float d = to.len();
        float r = range + MONSTER_RADIUS * m.d().size;
        if (d > r) continue;
        if (m.d().invisible && !seeInvisible && d > 3.0f) continue;   // too faint to aim at from afar
        float da = std::fabs(wrapAngle(std::atan2(to.y, to.x) - angle));
        // Very close monsters count even if slightly outside the cone.
        float effHalf = half + (d < 1.2f ? 0.35f : 0.0f);
        if (da > effHalf) continue;
        if (!map_.los(from, m.pos)) continue;
        float score = da / std::max(0.01f, effHalf) + d / r * 0.7f;
        if (score < bestScore) { bestScore = score; best = &m; }
    }
    return best;
}

void Server::damageMonster(Monster& m, int dmg, Element e, SPlayer* by, bool crit) {
    const MonsterDef& d = m.d();
    if (e == Element::Holy && d.undead) dmg *= 2;
    bool resisted = false, weak = false;
    for (Element r : d.resist) if (r == e) resisted = true;
    for (Element v : d.vulnerable) if (v == e) weak = true;
    if (resisted) dmg = std::max(1, dmg / 2);
    if (weak) dmg *= 2;
    if (e == Element::Cold && !resisted) m.slowT = 4.0f;
    if (by && resisted) text(by->st.id, COL_GREY, capitalize(theName(d)) + " resists.");
    if (by && weak) text(by->st.id, COL_YELLOW, capitalize(theName(d)) + " is hit hard!");
    m.hp -= dmg;
    m.hurtT = 0.2f;
    m.sleepT = 0;
    bool wasAsleep = m.asleep;
    wakeMonster(m, true);
    if (by) {
        m.target = by->st.id;
        m.hasSeen = true;
        std::string verb = crit ? "You critically hit " : (wasAsleep ? "You strike the sleeping " : "You hit ");
        std::string tgt = wasAsleep && !crit ? d.name : theName(d);
        text(by->st.id, crit ? COL_YELLOW : COL_WHITE, verb + tgt + " for " + std::to_string(dmg) + ".");
    }
    event(crit ? EV_CRIT : EV_HIT, m.pos, m.id);
}

void Server::killMonster(size_t idx, SPlayer* by) {
    Monster m = monsters_[idx];
    monsters_.erase(monsters_.begin() + idx);
    const MonsterDef& d = m.d();
    text(-1, d.boss ? COL_YELLOW : COL_WHITE, capitalize(theName(d)) + (d.undead ? " is destroyed." : " dies."));
    event(EV_MONSTER_DIE, m.pos, m.id);
    (void)by;
    for (auto& [id, p] : players_)
        if (p.joined) p.st.kills++;
    giveXp(std::max(1, (int)std::lround(d.xp * rules().xp)));
    // Loot
    int drops = d.boss ? 3 : (rng_.chance(d.drop) ? 1 : 0);
    if (d.boss) {
        int a = pickArtifact(power_ + 5);
        if (a >= 0) { dropItem(makeItem(a), m.pos); drops--; }
    }
    for (int i = 0; i < drops; i++) dropItem(makeItem(pickItem(power_ + (d.boss ? 3 : 0))), m.pos);
    if (rng_.chance(d.boss ? 100 : d.drop / 2)) dropItem(makeItem(gData.findItem("gold"), rng_.range(3, 12) * power_ * (d.boss ? 10 : 1)), m.pos);
    questKill(m.def, d.boss);
    if (d.deathSpell >= 0) {
        const SpellDef& sp = gData.spells[d.deathSpell];
        text(-1, COL_ORANGE, capitalize(theName(d)) + " explodes!");
        explode(m.pos, std::max(1.0f, sp.radius), std::max(1, (int)std::lround((sp.dmg.roll(rng_) + power_ / 2) * rules().monsterDamage)),
                sp.elem, d.deathSpell, true, nullptr);
    }
}

void Server::giveXp(int amount) {
    for (auto& [id, p] : players_) {
        if (!p.joined || p.st.dead) continue;
        PlayerState& s = p.st;
        s.xp += amount;
        while (s.xp >= s.xpForNext() && s.level < 50) {
            s.level++;
            int hpGain = std::max(1, rng_.range(1, s.c().hitDie) + s.mod(CON));
            s.maxHp += hpGain;
            s.hp += hpGain;
            if (s.c().manaBase > 0) {
                float mg = (float)std::max(1, s.c().manaPerLevel + s.mod(s.c().castStat) / 2);
                s.maxMp += mg;
                s.mp += mg;
            }
            text(id, COL_YELLOW, "Welcome to level " + std::to_string(s.level) + "!");
            for (int& dr : s.drained) dr = 0;   // leveling up restores drained stats
            text(-1, COL_GREY, s.name + " reached level " + std::to_string(s.level) + ".");
            for (size_t k = 0; k < s.c().spells.size(); k++)
                if (s.c().spellLevels[k] == s.level) {
                    const SpellDef& sd = gData.spells[s.c().spells[k]];
                    text(id, COL_PURPLE, std::string(sd.ability ? "You master a new technique: " : "You learn the spell ") + sd.name + ".");
                    s.autoQuick((int)k);
                }
            event(EV_LEVELUP, p.pos, id);
            sendRoster();
        }
        p.dirty = true;
    }
}

void Server::damagePlayer(SPlayer& p, int dmg, const std::string& source, Element e) {
    if (p.st.dead || dmg <= 0) return;
    if (e != Element::Physical && p.st.resists(e)) { dmg = std::max(1, dmg / 2); text(p.st.id, COL_GREY, "You resist."); }
    p.st.hp -= dmg;
    p.hurtT = 0.25f;
    event(EV_HURT, p.pos, p.st.id);
    if (source != "starvation") text(p.st.id, COL_RED, capitalize(source) + " hits you for " + std::to_string(dmg) + ".");
    if (p.st.hp <= 0) {
        p.st.hp = 0;
        p.st.dead = true;
        p.st.respawnT = rules().respawnTime;
        text(-1, COL_RED, p.st.name + " was killed by " + source + ".");
        event(EV_PLAYER_DIE, p.pos, p.st.id);
        // Gold spills on the floor.
        if (p.st.gold > 0) {
            dropItem(makeItem(gData.findItem("gold"), p.st.gold / 2), p.pos);
            p.st.gold -= p.st.gold / 2;
        }
    }
    p.dirty = true;
}

void Server::explode(Vec2 pos, float radius, int dmg, Element e, int spell, bool fromMonster, SPlayer* by, int power) {
    Effect ef;
    ef.id = newId();
    ef.spell = spell;
    ef.pos = pos;
    ef.radius = radius;
    ef.life = 0.45f;
    effects_.push_back(ef);
    event(EV_EXPLODE, pos);
    if (fromMonster) {
        for (auto& [id, p] : players_) {
            if (!p.joined || p.st.dead) continue;
            float d = dist(p.pos, pos);
            if (d <= radius && map_.los(pos, p.pos)) damagePlayer(p, d < radius * 0.5f ? dmg : dmg / 2, spell >= 0 ? gData.spells[spell].name : "explosion", e);
        }
    } else {
        for (auto& m : monsters_) {
            float d = dist(m.pos, pos);
            if (d > radius + 0.2f || m.hp <= 0 || !map_.los(pos, m.pos)) continue;
            if (dmg > 0) damageMonster(m, d < radius * 0.5f ? dmg : dmg * 2 / 3, e, by, false);
            if (spell >= 0 && m.hp > 0) applyEffect(m, gData.spells[spell].effect, by, power);
        }
    }
}

// ============================================================== actions =====

void Server::onAction(SPlayer& p, Reader& r) {
    uint8_t a = r.u8();
    int32_t x = r.i32(), y = r.i32();
    if (!r.ok) return;
    if (p.st.dead) return;
    switch (a) {
        case A_ATTACK: doAttack(p); break;
        case A_CAST: doCast(p, x); break;
        case A_USE: {
            // x = 1: use in direction y/1000 radians (bump-to-open while strafing)
            float saved = p.angle;
            if (x == 1) p.angle = y / 1000.0f;
            doUse(p, x == 1);
            p.angle = saved;
            break;
        }
        case A_INV_USE: useItem(p, x, y - 1); break;   // y = equipment slot + 1 (0 = where it goes)
        case A_INV_DROP: dropFromInv(p, x, y); break;
        case A_INV_GIVE: giveItem(p, x, y); break;
        case A_UNEQUIP: unequip(p, x); break;
        case A_SET_AUTOPICKUP: p.autoPickup = x != 0; break;
        case A_SHOP_BUY: shopBuy(p, x, y); break;
        case A_SET_AIM: p.strongAim = x != 0; break;
        case A_SET_QUICK:
            if (x >= 0 && x < NUM_QUICK) {
                bool ok = y == -1 || (y >= 0 && y < (int)p.st.c().spells.size()) ||
                          (y >= QUICK_ITEM && y - QUICK_ITEM < (int)gData.items.size());
                if (ok) {
                    for (auto& q : p.st.quick) if (q == y && y != -1) q = -1;   // one slot per spell
                    p.st.quick[x] = (int16_t)y;
                    p.dirty = true;
                }
            }
            break;
        case A_SHOP_SELL: shopSell(p, x, y); break;
        default: break;
    }
}

// Melee to-hit and damage bonuses of a character with its current weapon.
static void meleeBonuses(const PlayerState& s, bool ranged, bool finesse, int& toHit, int& dmgBonus) {
    int skill = ranged ? s.c().ranged : s.c().melee;
    int statBonus = (ranged || finesse) ? std::max(s.mod(DEX), ranged ? s.mod(DEX) : s.mod(STR)) : s.mod(STR);
    toHit = skill + s.level / 2 + statBonus + s.toHitBonus() + (s.blessT > 0 ? 2 : 0) + (s.heroT > 0 ? 4 : 0);
    dmgBonus = (ranged ? 0 : s.mod(STR)) + (skill * s.level) / 10 + s.toDamBonus() + (s.heroT > 0 ? 3 : 0);
}

// One melee blow against a monster: roll to hit, then damage. Returns true on a hit.
bool Server::meleeHit(SPlayer& p, Monster& t, int extraHit, int extraDmg) {
    PlayerState& s = p.st;
    const ItemDef* w = s.weapon();
    Dice dice = w ? w->dmg : Dice{1, 2};
    int toHit, dmgBonus;
    meleeBonuses(s, false, w && w->finesse, toHit, dmgBonus);
    bool sneak = s.c().stealth >= 5;  // rogues double damage vs sleeping monsters
    int roll = rng_.range(1, 20);
    bool asleep = t.asleep;
    int total = roll + toHit + extraHit + (asleep ? 4 : 0);
    const MonsterDef& d = t.d();
    if (roll == 20 || (roll != 1 && total >= d.ac)) {
        int dmg = std::max(1, dice.roll(rng_) + dmgBonus + extraDmg);
        if (roll == 20) dmg += dice.roll(rng_);
        if (sneak && asleep) { dmg *= 2; text(s.id, COL_YELLOW, "Sneak attack!"); }
        damageMonster(t, dmg, s.weaponBrand(), &p, roll == 20);
        return true;
    }
    text(s.id, COL_GREY, "You miss " + theName(d) + ".");
    event(EV_MISS, t.pos, t.id);
    wakeMonster(t, false);
    return false;
}

void Server::doAttack(SPlayer& p) {
    PlayerState& s = p.st;
    if (s.attackCd > 0) return;
    const ItemDef* w = s.weapon();
    Dice dice = w ? w->dmg : Dice{1, 2};
    float range = w ? w->range : 1.3f;
    float arc = w ? w->arc : 70.0f;
    float cd = w ? w->cooldown : 0.6f;
    bool ranged = w && w->ranged;
    int toHit, dmgBonus;
    meleeBonuses(s, ranged, w && w->finesse, toHit, dmgBonus);
    s.attackCd = cd * s.cdMult();
    p.attackT = 0.3f;
    p.noise = std::min(1.0f, p.noise + 0.5f);

    int ammo = ranged ? w->ammo : -1, ammoDef = -1;
    Element brand = s.weaponBrand();
    if (ranged && ammoRule_ && ammo >= 0) {
        if (!reloadQuiver(p, ammo)) {
            s.attackCd = 1.0f;   // (and the message doesn't repeat every frame)
            text(s.id, COL_ORANGE, std::string("You are out of ") + ammoName(ammo) + "! Find or buy more, or wield a melee weapon.");
            return;
        }
        // The loaded ammo's own bonuses count for this shot.
        ItemStack& q = s.equip[EQ_QUIVER];
        const ItemDef& ad = q.d();
        ammoDef = q.def;
        p.lastAmmo = q.def;
        toHit += ad.toHit;
        dmgBonus += ad.toDam;
        if (ad.brand != Element::Physical) brand = ad.brand;
        if (--q.count <= 0) q = ItemStack();
        else if (q.count == 5) text(s.id, COL_ORANGE, "Only 5 " + ad.name + " left in your quiver.");
        p.dirty = true;
    }
    if (p.strongAim) { arc = std::min(170.0f, arc * 1.6f); range += 0.3f; }
    Monster* t = findTarget(p.pos, p.angle, range, arc, s.detectT > 0);
    if (ranged) {
        Projectile pr;
        pr.id = newId();
        pr.spell = -1;
        pr.pos = p.pos + dirFromAngle(p.angle) * 0.3f;
        Vec2 dir = t ? (t->pos - p.pos).norm() : dirFromAngle(p.angle);
        pr.vel = dir * 16.0f;
        pr.ownerPlayer = s.id;
        pr.target = t ? t->id : 0;
        pr.toHit = toHit;
        pr.dice = dice;
        pr.dmgBonus = dmgBonus;
        pr.ttl = range / 16.0f + 0.2f;
        pr.sneak = s.c().stealth >= 5;
        pr.elem = brand;
        pr.ammo = ammo;
        pr.ammoDef = ammoDef;
        pr.recover = ammoDef >= 0;
        projectiles_.push_back(pr);
        event(EV_SHOOT, p.pos, s.id);
        return;
    }
    if (!t) { event(EV_SWING, p.pos, s.id); return; }
    meleeHit(p, *t, 0, 0);
}

void Server::doCast(SPlayer& p, int classSpell) {
    PlayerState& s = p.st;
    if (!s.knowsSpell(classSpell)) { text(s.id, COL_GREY, "You don't know that spell yet."); return; }
    int spell = s.c().spells[classSpell];
    const SpellDef& sp = gData.spells[spell];
    if (s.castCd > 0) return;
    if (s.rechargeLeft(classSpell) > 0) {
        text(s.id, COL_GREY, sp.name + " is not ready yet (" + std::to_string((int)std::ceil(s.rechargeLeft(classSpell))) + "s).");
        return;
    }
    if (s.mp < sp.mana) { text(s.id, COL_GREY, "You don't have enough mana to cast " + sp.name + "."); return; }
    if (castSpell(p, spell, s.spellPower())) {
        s.mp -= sp.mana;
        if (sp.recharge > 0 && classSpell < (int)s.recharge.size()) s.recharge[classSpell] = sp.recharge;
        p.dirty = true;
    }
}

// Aim and use a wand or rod from the pack.
void Server::useDevice(SPlayer& p, int slot) {
    PlayerState& s = p.st;
    ItemStack& it = s.inv[slot];
    const ItemDef& d = it.d();
    if (s.castCd > 0) return;
    if (d.kind == ItemKind::Wand && it.fuel <= 0) { text(s.id, COL_GREY, "The " + d.name + " has no charges left."); return; }
    if (d.kind == ItemKind::Rod && it.fuel > 0) {
        text(s.id, COL_GREY, "The " + d.name + " is still charging (" + std::to_string((it.fuel + 9) / 10) + "s).");
        return;
    }
    if (rng_.range(1, 100) <= s.deviceFail(d)) {
        s.castCd = 0.6f;
        text(s.id, COL_GREY, std::string("You failed to use the ") + d.name + " properly.");
        return;
    }
    if (!castSpell(p, d.spell, s.devicePower())) return;
    if (d.kind == ItemKind::Wand) {
        it.fuel--;
        if (it.fuel == 0) text(s.id, COL_GREY, "The " + d.name + " has no charges left.");
    } else {
        it.fuel = d.recharge * 10;
    }
    p.dirty = true;
}

bool Server::castSpell(SPlayer& p, int spell, int power) {
    PlayerState& s = p.st;
    const SpellDef& sp = gData.spells[spell];
    bool damaging = sp.dmg.n > 0;
    int amount = damaging ? std::max(1, sp.dmg.roll(rng_) + (int)(sp.scale * s.level) + power) : 0;
    s.castCd = sp.cooldown * s.cdMult();
    p.attackT = 0.35f;
    p.noise = std::min(1.0f, p.noise + 0.4f);
    event(EV_CAST, p.pos, s.id);

    switch (sp.type) {
        case SpellType::Bolt:
        case SpellType::Ball: {
            Monster* t = findTarget(p.pos, p.angle, sp.range, p.strongAim ? std::min(170.0f, sp.arc * 1.6f) : sp.arc, s.detectT > 0);
            Projectile pr;
            pr.id = newId();
            pr.spell = spell;
            pr.pos = p.pos + dirFromAngle(p.angle) * 0.3f;
            pr.vel = (t ? (t->pos - p.pos).norm() : dirFromAngle(p.angle)) * sp.speed;
            pr.ownerPlayer = s.id;
            pr.target = t ? t->id : 0;
            pr.dmg = amount;
            pr.radius = sp.type == SpellType::Ball ? sp.radius : 0;
            pr.elem = sp.elem;
            pr.ttl = sp.range / sp.speed + 0.5f;
            pr.power = power + s.level;
            projectiles_.push_back(pr);
            return true;
        }
        case SpellType::Nova:
            explode(p.pos, sp.radius, amount, sp.elem, spell, false, &p, power + s.level);
            if (sp.effect == SpellEffect::Fear) {
                text(s.id, COL_YELLOW, "You let out a terrible battle cry!");
                p.noise = 1.0f;
            }
            return true;
        case SpellType::Heal: {
            // Heal the most wounded ally in front of you, else yourself.
            SPlayer* target = &p;
            float bestNeed = 0;
            for (auto& [id, o] : players_) {
                if (sp.selfOnly) break;
                if (&o == &p || !o.joined || o.st.dead) continue;
                Vec2 to = o.pos - p.pos;
                float da = std::fabs(wrapAngle(std::atan2(to.y, to.x) - p.angle));
                if (to.len() > 6 || da > 0.5f || !map_.los(p.pos, o.pos)) continue;
                float need = 1.0f - (float)o.st.hp / o.st.maxHp;
                if (need > bestNeed) { bestNeed = need; target = &o; }
            }
            target->st.hp = std::min(target->st.maxHp, target->st.hp + amount);
            target->st.poisonT = 0;
            target->dirty = true;
            event(EV_HEAL, target->pos, target->st.id);
            if (target == &p) text(s.id, COL_GREEN, sp.selfOnly ? "You catch your breath and feel your strength return." : "You feel better.");
            else {
                text(s.id, COL_GREEN, "You heal " + target->st.name + ".");
                text(target->st.id, COL_GREEN, s.name + " heals you.");
            }
            return true;
        }
        case SpellType::Bless:
            for (auto& [id, o] : players_) {
                if (!o.joined || o.st.dead || dist(o.pos, p.pos) > std::max(1.0f, sp.radius)) continue;
                o.st.blessT = sp.duration;
                text(id, COL_GREEN, "You feel righteous!");
            }
            return true;
        case SpellType::Light:
            s.lightT = sp.duration;
            text(s.id, COL_YELLOW, "A warm light surrounds you.");
            return true;
        case SpellType::Detect: {
            s.detectT = sp.duration;
            int n = 0;
            for (auto& m : monsters_)
                if (dist(m.pos, p.pos) <= sp.radius) n++;
            text(s.id, COL_PURPLE, n ? "You sense the presence of " + std::to_string(n) + " creatures. (see map)" : "You sense no creatures nearby.");
            return true;
        }
        case SpellType::Treasure: {
            int found = 0, trapped = 0, empty = 0;
            for (auto& it : items_) {
                if (!isClosedChest(it.item) || dist(it.pos, p.pos) > sp.radius) continue;
                it.item.fuel |= CHEST_KNOWN;
                found++;
                if (chestTrapped(it.item.fuel & 15)) trapped++;
                else if ((it.item.fuel & 15) == CH_EMPTY) empty++;
            }
            if (!found) text(s.id, COL_PURPLE, "You sense no chests nearby.");
            else text(s.id, COL_PURPLE, "You sense " + std::to_string(found) + (found == 1 ? " chest" : " chests") + ": " +
                                            std::to_string(found - trapped - empty) + " with treasure, " + std::to_string(trapped) +
                                            " trapped, " + std::to_string(empty) + " empty. (see map)");
            return true;
        }
        case SpellType::Blink: {
            Vec2 dir = dirFromAngle(p.angle);
            Vec2 best = p.pos;
            for (float d = 0.25f; d <= sp.range; d += 0.25f) {
                Vec2 c = p.pos + dir * d;
                if (map_.solidAt(c)) break;
                if (map_.circleFree(c, PLAYER_RADIUS)) best = c;
            }
            if (dist(best, p.pos) < 0.5f) {
                // Random direction instead.
                for (int t = 0; t < 30; t++) {
                    Vec2 c = p.pos + dirFromAngle(rng_.frand() * 2 * PI) * (1.0f + rng_.frand() * sp.range);
                    if (map_.circleFree(c, PLAYER_RADIUS) && map_.los(p.pos, c)) { best = c; break; }
                }
            }
            p.pos = best;
            sendTeleport(p);
            text(s.id, COL_PURPLE, "You blink.");
            return true;
        }
        case SpellType::Map: {
            Writer w;
            w.u8(S_REVEAL);
            w.u16((uint16_t)p.pos.x);
            w.u16((uint16_t)p.pos.y);
            w.u8((uint8_t)sp.radius);
            send(s.id, w, true);
            text(s.id, COL_GREEN, "You sense the dungeon around you.");
            return true;
        }
        case SpellType::Teleport: {
            Vec2 best = p.pos;
            float bestD = 0;
            for (int t = 0; t < 40; t++) {
                Vec2 c;
                if (!randomFloor(c, false)) continue;
                float d = dist(c, p.pos);
                if (d > bestD) { best = c; bestD = d; }
                if (d >= sp.range) break;
            }
            p.pos = best;
            sendTeleport(p);
            text(s.id, COL_PURPLE, "You teleport away.");
            return true;
        }
        case SpellType::Haste:
            s.hasteT = sp.duration;
            text(s.id, COL_GREEN, "You feel yourself moving faster!");
            return true;
        case SpellType::Shield:
            s.shieldT = sp.duration;
            text(s.id, COL_GREEN, "Your skin turns hard as stone.");
            return true;
        case SpellType::Resist:
            for (auto& [id, o] : players_) {
                if (!o.joined || o.st.dead || dist(o.pos, p.pos) > std::max(1.0f, sp.radius)) continue;
                o.st.resistT = sp.duration;
                o.dirty = true;
                text(id, COL_GREEN, "You feel protected from fire, cold and poison.");
            }
            return true;
        case SpellType::Feed:
            s.food = std::max(s.food, FOOD_MAX - 100);
            text(s.id, COL_GREEN, "You feel full.");
            return true;
        case SpellType::Berserk:
            s.heroT = sp.duration;
            s.hp = std::min(s.maxHp, s.hp + amount);
            text(s.id, COL_YELLOW, "You feel a berserk fury!");
            return true;
        case SpellType::Whirlwind: {
            const ItemDef* w = s.weapon();
            if (w && w->ranged) { text(s.id, COL_GREY, "You need a melee weapon for that."); return false; }
            float reach = (w ? w->range : 1.3f) + 0.4f;
            std::vector<uint32_t> ids;
            for (auto& m : monsters_)
                if (m.hp > 0 && dist(m.pos, p.pos) <= reach + MONSTER_RADIUS * m.d().size && map_.los(p.pos, m.pos)) ids.push_back(m.id);
            event(EV_SWING, p.pos, s.id);
            if (ids.empty()) { text(s.id, COL_GREY, "You spin around, but nothing is close enough."); return true; }
            text(s.id, COL_YELLOW, "You spin in a whirlwind of steel!");
            for (uint32_t id : ids)
                if (Monster* m = monsterById(id)) meleeHit(p, *m, 2, 0);
            return true;
        }
        case SpellType::Dig: {
            if (map_.depth <= 0) { text(s.id, COL_GREY, "The town's walls resist your magic."); return false; }
            Vec2 dir = dirFromAngle(p.angle);
            for (float d = 0.3f; d <= sp.range; d += 0.1f) {
                Vec2 c = p.pos + dir * d;
                int x = (int)c.x, y = (int)c.y;
                if (!map_.solidAt(c)) continue;
                uint8_t t = map_.at(x, y);
                bool edge = x <= 0 || y <= 0 || x >= map_.w - 1 || y >= map_.h - 1;
                if (edge || (t != T_WALL && t != T_ROCK && t != T_RUBBLE && t != T_DOOR_SECRET && t != T_DOOR && t != T_DOOR_LOCKED)) {
                    text(s.id, COL_GREY, "The wall here is too strong.");
                    return true;
                }
                map_.set(x, y, T_FLOOR);
                rubbleDig_.erase(y * map_.w + x);
                sendTile(x, y);
                computeFlow();
                event(EV_DIG, Vec2(x + 0.5f, y + 0.5f));
                text(s.id, COL_WHITE, t == T_RUBBLE ? "The rubble turns to mud and is gone." : "The wall turns into mud!");
                return true;
            }
            text(s.id, COL_GREY, "There is no wall close enough in front of you.");
            return true;
        }
        case SpellType::Descent: {
            if (campaign_ >= 0 || map_.depth <= 0) { text(s.id, COL_GREY, "The floor feels solid. (Deep Descent only works in the endless dungeon.)"); return true; }
            if (descentT_ > 0) { text(s.id, COL_GREY, "The floor is already trembling."); return true; }
            descentT_ = sp.duration;
            descentTo_ = map_.depth + std::max(1, (int)sp.range);
            text(-1, COL_PURPLE, s.name + " reads a Deep Descent. The floor begins to tremble...");
            return true;
        }
        case SpellType::Recall: {
            if (recallT_ > 0) {
                recallT_ = 0;
                text(-1, COL_GREY, s.name + " cancels the recall. A tension leaves the air.");
                return true;
            }
            if (map_.depth == 0 && maxDepth_ < 1) maxDepth_ = 1;
            recallT_ = sp.duration + rng_.frand() * sp.range;
            text(-1, COL_PURPLE, s.name + " reads a Word of Recall. The air about the party becomes charged...");
            text(-1, COL_GREY, map_.depth > 0 ? std::string("(In a moment you will all be taken back to town.)")
                                              : "(In a moment you will all be taken down to level " + std::to_string(maxDepth_) + ".)");
            return true;
        }
    }
    return false;
}

// Sleep, fear and slow from spells. Bosses shrug them off; deep monsters often resist.
void Server::applyEffect(Monster& m, SpellEffect e, SPlayer* by, int power) {
    if (e == SpellEffect::None) return;
    const MonsterDef& d = m.d();
    int chance = clampi(55 + 3 * (power - d.depth), 10, 95);
    bool fail = m.boss || !rng_.chance(chance);
    auto say = [&](const std::string& t) { if (by) text(by->st.id, COL_PURPLE, capitalize(theName(d)) + t); };
    if (fail) { say(" is unaffected."); if (e != SpellEffect::Sleep) wakeMonster(m, false); return; }
    switch (e) {
        case SpellEffect::Sleep:
            m.asleep = true;
            m.sleepT = 12.0f + power * 0.5f;
            m.hasSeen = false;
            say(" falls asleep.");
            break;
        case SpellEffect::Fear:
            wakeMonster(m, false);
            m.fearT = 6.0f + power * 0.3f;
            say(" flees in terror!");
            break;
        case SpellEffect::Slow:
            wakeMonster(m, false);
            m.slowT = 8.0f;
            say(" starts moving slower.");
            break;
        case SpellEffect::TeleportAway: {
            Vec2 best = m.pos;
            float bestD = 0;
            for (int t = 0; t < 30; t++) {
                Vec2 c;
                if (randomFloor(c, false) && dist(c, m.pos) > bestD) { best = c; bestD = dist(c, m.pos); }
            }
            say(" disappears!");
            m.pos = best;
            m.target = -1;
            m.hasSeen = false;
            break;
        }
        default: break;
    }
}

void Server::doUse(SPlayer& p, bool doorsOnly) {
    if (p.useCd > 0) return;
    p.useCd = 0.25f;
    PlayerState& s = p.st;
    int tx = (int)p.pos.x, ty = (int)p.pos.y;
    uint8_t under = map_.at(tx, ty);

    // 1. Pick up an item at your feet or right in front of you.
    std::vector<Vec2> itemPos;
    for (auto& it : items_)   // (open chests are scenery)
        itemPos.push_back(it.item.d().kind == ItemKind::Chest && !isClosedChest(it.item) ? Vec2(-1000, -1000) : it.pos);
    int bestI = doorsOnly ? -1 : pickupCandidate(map_, p.pos, p.angle, itemPos);
    if (bestI >= 0) { pickupItem(p, (size_t)bestI); return; }

    // 2. Stairs.
    if (doorsOnly && under != T_DOOR_OPEN) under = T_FLOOR;
    if (under == T_STAIRS_DOWN) {
        if (campaign_ >= 0 && map_.depth >= (int)gData.campaigns[campaign_].levels.size()) {
            text(s.id, COL_GREY, "These stairs lead nowhere - this is the deepest level of the campaign.");
            return;
        }
        for (size_t qi = 0; qi < quests_.size(); qi++)
            if (!quests_[qi].done && quests_[qi].type == Q_STAIRS && quests_[qi].depth == map_.depth) completeQuest(quests_[qi]);
        int target = map_.depth + 1;
        if (map_.depth == 0 && collapsedDepth() > 0) {
            target = collapsedDepth() + 1;
            text(-1, COL_GREY, "The upper levels have collapsed; the stairs lead straight down to level " + std::to_string(target) + ".");
        }
        text(-1, COL_YELLOW, s.name + " descends the stairs. The party follows.");
        event(EV_STAIRS, p.pos);
        newLevel(target, true);
        return;
    }
    if (under == T_STAIRS_UP) {
        if (map_.depth <= 0) return;
        if (p.stairsConfirmT <= 0) {
            p.stairsConfirmT = 3.0f;
            text(s.id, COL_YELLOW, map_.depth == 1 ? "Press E again to climb up to town (the whole party comes along)."
                                                   : "Press E again to climb back up (the whole party comes along).");
            return;
        }
        p.stairsConfirmT = 0;
        int target = map_.depth - 1;
        if (target > 0 && target <= collapsedDepth()) {
            target = 0;
            text(-1, COL_GREY, "The levels above have collapsed; the passage leads straight up to town.");
        }
        text(-1, COL_YELLOW, s.name + " climbs the stairs. The party follows.");
        newLevel(target, false);
        return;
    }

    // 3. Doors / rubble in front of you.
    int x, y;
    if (!useTileInFront(map_, p.pos, p.angle, x, y)) return;
    {
        uint8_t t = map_.at(x, y);
        if (t == T_DOOR) {
            map_.set(x, y, T_DOOR_OPEN);
            sendTile(x, y);
            event(EV_DOOR, Vec2(x + 0.5f, y + 0.5f));
            return;
        }
        if (t == T_DOOR_OPEN) {
            // Close it (if nobody is standing in the doorway).
            bool blocked = false;
            for (auto& [id, o] : players_)
                if ((int)o.pos.x == x && (int)o.pos.y == y) blocked = true;
            for (auto& m : monsters_)
                if ((int)m.pos.x == x && (int)m.pos.y == y) blocked = true;
            for (auto& it : items_)
                if ((int)it.pos.x == x && (int)it.pos.y == y) blocked = true;
            if (blocked) { text(s.id, COL_GREY, "Something is blocking the doorway."); return; }
            map_.set(x, y, T_DOOR);
            sendTile(x, y);
            event(EV_DOOR, Vec2(x + 0.5f, y + 0.5f));
            return;
        }
        if (t == T_DOOR_KEY) {
            p.useCd = 0.6f;
            int color = map_.keyDoorColor(x, y);
            if (s.keys[color] > 0) {
                s.keys[color]--;
                p.dirty = true;
                map_.set(x, y, T_DOOR_OPEN);
                sendTile(x, y);
                event(EV_DOOR, Vec2(x + 0.5f, y + 0.5f));
                text(-1, COL_GREEN, s.name + " unlocks the " + keyColorName(color) + " door.");
                return;
            }
            text(s.id, COL_GREY, std::string("This door has a ") + keyColorName(color) + " lock that can't be picked. The " +
                                     keyColorName(color) + " key is somewhere on this level.");
            return;
        }
        if (t == T_DOOR_LOCKED) {
            p.useCd = 1.0f;
            int chance = 15 + s.c().disarm * 8 + s.mod(DEX) * 4 + s.level * 2;
            int bash = 10 + s.mod(STR) * 6 + s.level;
            if (rng_.chance(clampi(chance, 5, 95))) {
                map_.set(x, y, T_DOOR_OPEN);
                sendTile(x, y);
                text(s.id, COL_GREEN, "You have picked the lock.");
                event(EV_DOOR, Vec2(x + 0.5f, y + 0.5f));
            } else if (rng_.chance(clampi(bash, 2, 50))) {
                map_.set(x, y, T_DOOR_OPEN);
                sendTile(x, y);
                text(s.id, COL_GREEN, "You bash the door open!");
                p.noise = 1.0f;
                event(EV_DOOR, Vec2(x + 0.5f, y + 0.5f));
            } else {
                text(s.id, COL_GREY, "The door is locked. You failed to pick the lock.");
            }
            return;
        }
        if (t == T_RUBBLE) {
            // Digging makes steady progress: 5 points needed, strong characters dig faster.
            p.useCd = 0.5f;
            p.noise = std::min(1.0f, p.noise + 0.3f);
            int& prog = rubbleDig_[y * map_.w + x];
            prog += 1 + std::max(0, s.mod(STR) / 2) + (rng_.chance(30) ? 1 : 0);
            event(EV_DIG, Vec2(x + 0.5f, y + 0.5f), s.id);
            if (prog >= 5) {
                rubbleDig_.erase(y * map_.w + x);
                map_.set(x, y, T_FLOOR);
                sendTile(x, y);
                text(s.id, COL_GREEN, "You have dug through the rubble.");
                if (rng_.chance(15)) dropItem(makeItem(pickItem(power_)), Vec2(x + 0.5f, y + 0.5f));
            } else {
                text(s.id, COL_GREY, "Digging through the rubble... (" + std::to_string(std::min(prog, 4)) + "/5)");
            }
            return;
        }
    }
}

// Same item, stackable, and (lights) the same fuel left: two fresh torches make "2 x Torch".
static bool stacksWith(const ItemStack& a, const ItemStack& b) {
    if (a.def != b.def || a.empty() || b.empty() || a.d().maxStack <= 1) return false;
    return a.d().kind != ItemKind::Light || a.fuel == b.fuel;
}

bool Server::canCarry(const SPlayer& p, const ItemStack& it) const {
    const ItemDef& d = it.d();
    if (d.kind == ItemKind::Gold || d.kind == ItemKind::Key) return true;   // purse and key ring
    int left = it.count;
    const ItemStack& q = p.st.equip[EQ_QUIVER];
    if (d.kind == ItemKind::Ammo && stacksWith(q, it)) left -= d.maxStack - q.count;   // the loaded kind
    if (left <= 0) return true;
    for (auto& slot : p.st.inv) {
        if (slot.empty()) left -= d.maxStack;
        else if (stacksWith(slot, it)) left -= d.maxStack - slot.count;
        if (left <= 0) return true;
    }
    return left <= 0;
}

bool Server::addToInventory(SPlayer& p, const ItemStack& it) {
    if (!canCarry(p, it)) return false;  // never add partially
    PlayerState& s = p.st;
    const ItemDef& d = it.d();
    if (d.kind == ItemKind::Gold) { s.gold += it.count; p.dirty = true; return true; }
    int left = it.count;
    ItemStack& q = s.equip[EQ_QUIVER];
    if (d.kind == ItemKind::Ammo && stacksWith(q, it)) {   // more of what is loaded: into the quiver
        int add = std::min(left, d.maxStack - q.count);
        q.count += add;
        left -= add;
        p.dirty = true;
        if (!left) return true;
    }
    if (d.kind == ItemKind::Key && d.keyColor >= 0) {   // keys go on the key ring
        s.keys[d.keyColor] = (uint8_t)std::min(99, s.keys[d.keyColor] + it.count);
        p.dirty = true;
        return true;
    }
    if (d.maxStack > 1)
        for (auto& slot : s.inv)
            if (stacksWith(slot, it) && slot.count < d.maxStack) {
                int add = std::min(left, d.maxStack - slot.count);
                slot.count += add;
                left -= add;
                if (!left) break;
            }
    while (left > 0) {
        int free = -1;
        for (int i = 0; i < INV_SIZE; i++)
            if (s.inv[i].empty()) { free = i; break; }
        if (free < 0) return false;
        s.inv[free] = it;
        s.inv[free].count = std::min(left, d.maxStack);
        left -= s.inv[free].count;
    }
    if (d.kind == ItemKind::Wand || d.kind == ItemKind::Rod) s.autoQuick(QUICK_ITEM + it.def);
    p.dirty = true;
    return true;
}

void Server::pickupItem(SPlayer& p, size_t idx, bool quiet) {
    if (items_[idx].item.d().kind == ItemKind::Chest) {   // chests are opened, not carried
        if (isClosedChest(items_[idx].item)) openChest(p, idx);
        return;
    }
    FloorItem f = items_[idx];
    if (!addToInventory(p, f.item)) {
        ItemKind k = f.item.d().kind;
        if (!quiet && (k == ItemKind::Food || k == ItemKind::Potion || k == ItemKind::Scroll)) {
            // Pack full: eat, drink or read it right where it lies.
            ItemStack here = f.item;
            useStack(p, here, -1, -1);
            for (size_t i = 0; i < items_.size(); i++)   // (using it may have changed the floor)
                if (items_[i].id == f.id) {
                    if (here.empty()) items_.erase(items_.begin() + i);
                    else items_[i].item = here;
                    break;
                }
            return;
        }
        if (!quiet) text(p.st.id, COL_GREY, "Your pack is full.");
        return;
    }
    items_.erase(items_.begin() + idx);
    if (f.item.d().artifact) text(-1, COL_YELLOW, p.st.name + " has found " + f.item.d().name + "!");
    else text(p.st.id, COL_WHITE, "You pick up " + itemName(f.item) + ".");
    event(EV_PICKUP, f.pos, p.st.id);
    if (f.item.d().kind == ItemKind::Quest) questPickup(f.item.def);
}

void Server::useItem(SPlayer& p, int slot, int target) {
    if (slot < 0 || slot >= INV_SIZE) return;
    useStack(p, p.st.inv[slot], slot, target);
}

// Use one of the items in 'it': from the pack (invSlot >= 0) or lying on the floor
// (invSlot = -1: food, potions and scrolls only). target: equipment slot to put it in, or -1.
void Server::useStack(SPlayer& p, ItemStack& it, int slot, int target) {
    PlayerState& s = p.st;
    if (it.empty()) return;
    const ItemDef& d = it.d();
    if (slot < 0 && d.kind != ItemKind::Food && d.kind != ItemKind::Potion && d.kind != ItemKind::Scroll) return;
    auto consume = [&]() { if (--it.count <= 0) it = ItemStack(); };
    p.dirty = true;
    switch (d.kind) {
        case ItemKind::Light:
            // Combine torches: a torch used while holding a torch adds its fuel (max 2.5 torches).
            if (!s.equip[EQ_LIGHT].empty() && s.equip[EQ_LIGHT].def == it.def && d.id == "torch") {
                ItemStack& held = s.equip[EQ_LIGHT];
                int cap = d.fuel * 5 / 2;
                if (held.fuel >= cap) { text(s.id, COL_GREY, "Your torch can't burn any longer than it already does."); break; }
                held.fuel = std::min(cap, held.fuel + it.fuel);
                if (--it.count <= 0) it = ItemStack();   // one torch of a stack
                text(s.id, COL_WHITE, "You combine the torches. " + itemName(held) + " - it burns brighter for longer.");
                break;
            }
            [[fallthrough]];
        case ItemKind::Weapon:
        case ItemKind::Armor:
        case ItemKind::Helm:
        case ItemKind::Ring:
        case ItemKind::Ammo:
        case ItemKind::Amulet: {
            if (d.kind == ItemKind::Ammo) {   // the whole stack goes into the quiver
                ItemStack& q = s.equip[EQ_QUIVER];
                if (stacksWith(q, it)) {
                    int add = std::min(it.count, d.maxStack - q.count);
                    q.count += add;
                    if ((it.count -= add) <= 0) it = ItemStack();
                } else {
                    std::swap(q, it);
                }
                text(s.id, COL_WHITE, "You load " + itemName(q) + " into your quiver.");
                break;
            }
            int eq = (target >= 0 && target < EQ_COUNT && fitsSlot(d, target)) ? target : equipSlotFor(d);
            // A second ring goes on the other hand.
            if (target < 0 && eq == EQ_RING && !s.equip[EQ_RING].empty() && s.equip[EQ_RING2].empty()) eq = EQ_RING2;
            if (it.count > 1) {
                // From a stack (torches): take one, and the old one goes into the pack.
                ItemStack one = it, old = s.equip[eq];
                one.count = 1;
                it.count--;
                s.equip[eq] = one;
                if (!old.empty() && !addToInventory(p, old)) {
                    dropItem(old, p.pos);
                    text(s.id, COL_GREY, "Your pack is full; you drop " + itemName(old) + ".");
                }
            } else {
                std::swap(s.equip[eq], it);
            }
            text(s.id, d.artifact ? COL_YELLOW : COL_WHITE, "You are now using " + itemName(s.equip[eq]) + ".");
            break;
        }
        case ItemKind::Food:
            if (s.food >= FOOD_MAX - 50) { text(s.id, COL_GREY, "You are too full to eat."); break; }
            s.food = std::min(FOOD_MAX, s.food + d.food);
            if (d.heal) s.hp = std::min(s.maxHp, s.hp + d.heal);
            text(s.id, COL_GREEN, "You eat the " + d.name + ". " + (s.food > 900 ? "You are full." : "That tastes good."));
            consume();
            break;
        case ItemKind::Potion:
            if (d.heal) { s.hp = std::min(s.maxHp, s.hp + d.heal); event(EV_HEAL, p.pos, s.id); }
            if (d.mana) s.mp = std::min(s.maxMp, s.mp + d.mana);
            if ((d.heal || d.curePoison) && s.poisonT > 0) { s.poisonT = 0; text(s.id, COL_GREEN, "The poison leaves your body."); }
            if (d.restore) {
                bool any = false;
                for (int& dr : s.drained) { any |= dr > 0; dr = 0; }
                text(s.id, COL_GREEN, any ? "You feel your strength returning to you." : "You feel healthy.");
            }
            if (d.spell >= 0) castSpell(p, d.spell, std::max(s.spellPower(), s.devicePower()));
            if (d.gainStat >= 0) {
                if (s.stats[d.gainStat] >= 20) text(s.id, COL_GREY, "You feel nothing more - you are as good as it gets.");
                else { s.stats[d.gainStat]++; text(s.id, COL_YELLOW, std::string("You feel your ") + kStatNames[d.gainStat] + " increase!"); }
            }
            text(s.id, COL_GREEN, "You quaff the " + d.name + ".");
            consume();
            break;
        case ItemKind::Scroll:
            if (d.spell >= 0) {
                text(s.id, COL_WHITE, "You read the " + d.name + ".");
                castSpell(p, d.spell, std::max(s.spellPower(), s.devicePower()));
                consume();
            }
            break;
        case ItemKind::Fuel: {
            ItemStack& light = s.equip[EQ_LIGHT];
            if (light.empty() || light.d().id == "torch") { text(s.id, COL_GREY, "You need a lantern to use oil."); break; }
            light.fuel = std::min(light.d().fuel, light.fuel + d.fuel);
            text(s.id, COL_WHITE, "You refuel your " + light.d().name + ".");
            consume();
            break;
        }
        case ItemKind::Wand:
        case ItemKind::Rod:
            useDevice(p, slot);
            break;
        case ItemKind::Gold:
            break;
        case ItemKind::Quest:
            text(s.id, COL_YELLOW, "The " + d.name + " glimmers. Someone would pay well for this.");
            break;
        case ItemKind::Key:
            text(s.id, COL_WHITE, std::string("It opens a door with a ") + keyColorName(d.keyColor) + " lock: walk up to it and press E.");
            break;
        case ItemKind::Chest:
            break;
    }
}

void Server::dropFromInv(SPlayer& p, int slot, int count) {
    if (slot < 0 || slot >= INV_SIZE) return;
    ItemStack& it = p.st.inv[slot];
    if (it.empty()) return;
    ItemStack drop = it;
    if (count > 0 && count < it.count) drop.count = count;
    it.count -= drop.count;
    if (it.count <= 0) it = ItemStack();
    dropItem(drop, p.pos + dirFromAngle(p.angle) * 0.4f, p.st.id);
    text(p.st.id, COL_WHITE, "You drop " + itemName(drop) + ".");
    p.dirty = true;
}

void Server::giveItem(SPlayer& p, int slot, int toPlayer) {
    if (slot < 0 || slot >= INV_SIZE || p.st.inv[slot].empty()) return;
    SPlayer* o = playerById(toPlayer);
    if (!o || o == &p || o->st.dead) { text(p.st.id, COL_GREY, "There is nobody to give it to."); return; }
    if (dist(o->pos, p.pos) > 3.0f) { text(p.st.id, COL_GREY, o->st.name + " is too far away."); return; }
    ItemStack it = p.st.inv[slot];
    if (it.d().maxStack > 1 && it.count > 1) it.count = 1;  // give one at a time from stacks
    if (!addToInventory(*o, it)) { text(p.st.id, COL_GREY, o->st.name + "'s pack is full."); return; }
    p.st.inv[slot].count -= it.count;
    if (p.st.inv[slot].count <= 0) p.st.inv[slot] = ItemStack();
    text(p.st.id, COL_WHITE, "You give " + itemName(it) + " to " + o->st.name + ".");
    text(o->st.id, COL_GREEN, p.st.name + " gives you " + itemName(it) + ".");
    p.dirty = true;
    o->dirty = true;
}

void Server::unequip(SPlayer& p, int slot) {
    if (slot < 0 || slot >= EQ_COUNT || p.st.equip[slot].empty()) return;
    ItemStack it = p.st.equip[slot];
    p.st.equip[slot] = ItemStack();
    if (!addToInventory(p, it)) {
        dropItem(it, p.pos);
        text(p.st.id, COL_GREY, "Your pack is full; you drop " + itemName(it) + ".");
    } else {
        text(p.st.id, COL_WHITE, "You take off " + itemName(it) + ".");
    }
    p.dirty = true;
}

// =============================================================== quests =====

void Server::setupMainQuest() {
    std::vector<int> bosses;
    for (size_t i = 0; i < gData.monsters.size(); i++)
        if (gData.monsters[i].boss) bosses.push_back((int)i);
    std::sort(bosses.begin(), bosses.end(), [](int a, int b) { return gData.monsters[a].depth < gData.monsters[b].depth; });
    Quest q;
    q.main = true;
    q.type = Q_KILL_BOSS;
    if (mainQuestStage_ < (int)bosses.size()) {
        q.target = bosses[mainQuestStage_];
        q.depth = gData.monsters[q.target].depth;
    } else {
        // Endless: reuse the last boss deeper and deeper.
        q.target = bosses.empty() ? 0 : bosses.back();
        q.depth = gData.monsters[q.target].depth + 3 * (mainQuestStage_ - (int)bosses.size() + 1);
    }
    q.rewardXp = gData.monsters[q.target].xp;
    q.text = "Slay " + gData.monsters[q.target].name + " on dungeon level " + std::to_string(q.depth) + ".";
    quests_.push_back(q);
}

void Server::makeSideQuest() {
    Quest q;
    q.depth = map_.depth;
    if (rng_.chance(50)) {
        // Find an item.
        std::vector<int> qitems;
        for (size_t i = 0; i < gData.items.size(); i++)
            if (gData.items[i].kind == ItemKind::Quest) qitems.push_back((int)i);
        if (qitems.empty()) return;
        q.type = Q_FIND_ITEM;
        q.target = qitems[rng_.range(0, (int)qitems.size() - 1)];
        Vec2 pos;
        if (!randomFloor(pos, true)) return;
        dropItem(makeItem(q.target), pos);
        q.rewardXp = 15 * power_;
        q.rewardGold = 50 * power_;
        q.text = "Find the " + gData.items[q.target].name + " on this level.";
    } else {
        // Slay a number of a monster type present on the level.
        std::map<int, int> counts;
        for (auto& m : monsters_)
            if (!m.boss) counts[m.def]++;
        std::vector<int> cands;
        for (auto& [def, n] : counts)
            if (n >= 3) cands.push_back(def);
        if (cands.empty()) return;
        q.type = Q_SLAY;
        q.target = cands[rng_.range(0, (int)cands.size() - 1)];
        q.needed = std::min(counts[q.target], rng_.range(3, 6));
        q.rewardXp = gData.monsters[q.target].xp * q.needed;
        q.rewardGold = 20 * power_;
        q.text = "Slay " + std::to_string(q.needed) + " " + gData.monsters[q.target].plural + " on this level.";
    }
    quests_.push_back(q);
    text(-1, COL_CYAN, "New task: " + q.text);
}

void Server::completeQuest(Quest& q) {
    q.done = true;
    text(-1, COL_YELLOW, "Quest complete: " + q.text);
    if (q.rewardGold > 0) {
        for (auto& [id, p] : players_)
            if (p.joined) { p.st.gold += q.rewardGold; p.dirty = true; }
        text(-1, COL_YELLOW, "Each of you receives " + std::to_string(q.rewardGold) + " gold.");
    }
    if (q.rewardXp > 0) giveXp(q.rewardXp);
    if (q.main && campaign_ >= 0) {
        campaignDone_.insert(q.depth);
        const Campaign& c = gData.campaigns[campaign_];
        if ((int)campaignDone_.size() >= (int)c.levels.size() && !campaignWon_) {
            campaignWon_ = true;
            text(-1, COL_YELLOW, "*** " + c.name + " - campaign complete! ***");
            if (!c.victory.empty()) text(-1, COL_WHITE, c.victory);
            text(-1, COL_GREY, "You may keep exploring, or start a new adventure from the menu.");
        }
    } else if (q.main) {
        mainQuestStage_++;
        text(-1, COL_YELLOW, "A new challenge awaits deeper in the dungeon...");
        setupMainQuest();
    }
    sendQuests();
}

void Server::questKill(int monsterDef, bool boss) {
    for (size_t i = 0; i < quests_.size(); i++) {
        Quest& q = quests_[i];
        if (!q.done && q.type == Q_CLEAR && q.depth == map_.depth) {
            q.count++;
            if (q.count >= q.needed) completeQuest(quests_[i]);
            else sendQuests();
            continue;
        }
        if (q.done || q.target != monsterDef) continue;
        if (q.type == Q_KILL_BOSS || q.type == Q_SLAY) {
            q.count++;
            if (q.count >= q.needed) completeQuest(quests_[i]);
            else sendQuests();
            return;
        }
    }
}

void Server::questPickup(int itemDef) {
    for (size_t i = 0; i < quests_.size(); i++)
        if (!quests_[i].done && quests_[i].type == Q_FIND_ITEM && quests_[i].target == itemDef) {
            completeQuest(quests_[i]);
            return;
        }
}

// ============================================================ snapshots =====

void Server::sendSnapshots() {
    for (auto& [pid, viewer] : players_) {
        if (!viewer.joined) continue;
        std::vector<EntSnap> ents;
        auto near = [&](Vec2 p) { return dist(p, viewer.pos) <= SNAPSHOT_RANGE; };
        for (auto& [id, p] : players_) {
            if (!p.joined || id == pid) continue;
            EntSnap e;
            e.id = (uint32_t)id;
            e.type = ET_PLAYER;
            e.def = p.st.cls;
            e.pos = p.pos;
            e.angle = p.angle;
            e.flags = (p.st.dead ? EF_DEAD : 0) | (p.hurtT > 0 ? EF_HURT : 0) | (p.attackT > 0 ? EF_ATTACK : 0) | (p.st.blessT > 0 ? EF_BLESS : 0);
            e.hp = (uint8_t)clampi(p.st.hp * 255 / std::max(1, p.st.maxHp), 0, 255);
            e.extra = (uint8_t)(p.st.dead ? 0 : p.st.lightRadius() * 10);
            ents.push_back(e);
        }
        for (auto& m : monsters_) {
            if (!near(m.pos)) continue;
            EntSnap e;
            e.id = m.id;
            e.type = ET_MONSTER;
            e.def = (uint16_t)m.def;
            e.pos = m.pos;
            e.angle = m.angle;
            e.flags = (m.asleep ? EF_ASLEEP : 0) | (m.hurtT > 0 ? EF_HURT : 0) | (m.attackT > 0 ? EF_ATTACK : 0) | (m.slowT > 0 ? EF_SLOW : 0) | (m.boss ? EF_BOSS : 0);
            e.hp = (uint8_t)clampi(m.hp * 255 / std::max(1, m.maxHp), 0, 255);
            ents.push_back(e);
        }
        for (auto& it : items_) {
            if (!near(it.pos)) continue;
            EntSnap e;
            e.id = it.id;
            e.type = ET_ITEM;
            e.def = (uint16_t)it.item.def;
            if (isClosedChest(it.item) && (it.item.fuel & CHEST_KNOWN)) {
                int code = it.item.fuel & 15;
                e.flags = EF_CHEST_KNOWN | (chestTrapped(code) ? EF_CHEST_TRAP : 0) | (code == CH_EMPTY ? EF_CHEST_EMPTY : 0);
            }
            e.pos = it.pos;
            e.extra = (uint8_t)std::min(255, it.item.count);
            ents.push_back(e);
        }
        for (auto& pr : projectiles_) {
            if (!near(pr.pos)) continue;
            EntSnap e;
            e.id = pr.id;
            e.type = ET_PROJECTILE;
            e.def = pr.spell < 0 ? 0xFFFF : (uint16_t)pr.spell;
            if (pr.spell < 0) e.extra = (uint8_t)(pr.ammo + 1);   // 0 = arrow, else AmmoKind + 1
            e.pos = pr.pos;
            e.angle = std::atan2(pr.vel.y, pr.vel.x);
            ents.push_back(e);
        }
        for (auto& ef : effects_) {
            if (!near(ef.pos)) continue;
            EntSnap e;
            e.id = ef.id;
            e.type = ET_EFFECT;
            e.def = ef.spell < 0 ? 0xFFFF : (uint16_t)ef.spell;
            e.pos = ef.pos;
            e.hp = (uint8_t)clampi((int)(ef.t / ef.life * 255), 0, 255);
            e.extra = (uint8_t)std::min(255.0f, ef.radius * 10);
            ents.push_back(e);
        }
        Writer w;
        w.u8(S_SNAPSHOT);
        w.u32(tickNo_);
        w.u16((uint16_t)ents.size());
        for (auto& e : ents) e.write(w);
        send(pid, w, false);
    }
}

void Server::sendPlayerStates() {
    for (auto& [pid, p] : players_) {
        if (!p.joined) continue;
        bool periodic = (tickNo_ + pid) % 3 == 0;
        if (!p.dirty && !periodic) continue;
        Writer w;
        w.u8(S_PSTATE);
        w.u32(tickNo_);   // lets the client drop stale (reordered) updates
        p.st.write(w);
        send(pid, w, p.dirty);
        p.dirty = false;
    }
}

void Server::debugSpawn(const std::string& ids, bool awake) {
    if (players_.empty()) return;
    SPlayer& p = players_.begin()->second;
    std::string rest = ids;
    int i = 0;
    while (!rest.empty()) {
        size_t c = rest.find(',');
        std::string id = rest.substr(0, c);
        rest = c == std::string::npos ? "" : rest.substr(c + 1);
        int def = gData.findMonster(id);
        if (def < 0) { std::printf("[server] unknown monster %s\n", id.c_str()); continue; }
        Vec2 pos = p.pos;
        for (float d = 3.5f + i * 0.8f; d > 1.0f; d -= 0.25f) {
            Vec2 c2 = p.pos + dirFromAngle(p.angle + (i % 3 - 1) * 0.35f) * d;
            if (map_.circleFree(c2, MONSTER_RADIUS) && map_.los(p.pos, c2)) { pos = c2; break; }
        }
        spawnMonster(def, pos, awake);
        monsters_.back().asleep = !awake;
        i++;
    }
}

// =============================================================== shops ======

bool Server::nearShop(const SPlayer& p, int store) const {
    if (map_.depth != 0 || store < 0 || store >= (int)gData.stores.size()) return false;
    int px = (int)p.pos.x, py = (int)p.pos.y;
    for (int y = py - 2; y <= py + 2; y++)
        for (int x = px - 2; x <= px + 2; x++)
            if (map_.at(x, y) == T_SHOP && map_.floorTex[y * map_.w + x] == store) return true;
    return false;
}

void Server::shopBuy(SPlayer& p, int store, int ware) {
    if (!nearShop(p, store)) return;
    const StoreDef& st = gData.stores[store];
    if (ware < 0 || ware >= (int)st.items.size()) return;
    const ItemDef& d = gData.items[st.items[ware]];
    if (d.kind == ItemKind::Ammo && !ammoRule_) { text(p.st.id, COL_GREY, "No need: ammunition isn't used up in this game (Options: Ammo)."); return; }
    int price = buyPrice(d);
    if (p.st.gold < price) { text(p.st.id, COL_GREY, "You can't afford the " + d.name + " (" + std::to_string(price) + " gold)."); return; }
    ItemStack it = makeItem(st.items[ware]);
    if (!addToInventory(p, it)) { text(p.st.id, COL_GREY, "Your pack is full."); return; }
    p.st.gold -= price;
    p.dirty = true;
    text(p.st.id, COL_WHITE, "You buy " + itemName(it) + " for " + std::to_string(price) + " gold.");
    event(EV_PICKUP, p.pos, p.st.id);
}

void Server::shopSell(SPlayer& p, int store, int slot) {
    if (!nearShop(p, store) || slot < 0 || slot >= INV_SIZE) return;
    ItemStack& it = p.st.inv[slot];
    if (it.empty()) return;
    const ItemDef& d = it.d();
    if (!gData.storeBuys(store, d)) { text(p.st.id, COL_GREY, gData.stores[store].name + " doesn't buy that."); return; }
    int price = sellPriceEach(d, it.fuel);
    if (price <= 0) { text(p.st.id, COL_GREY, "The " + d.name + " is worthless here."); return; }
    ItemStack one = it;
    one.count = 1;
    it.count--;
    if (it.count <= 0) it = ItemStack();
 if (d.artifact) artifactsInPlay_.erase(one.def);
    p.st.gold += price;
    p.dirty = true;
    text(p.st.id, COL_YELLOW, "You sell " + itemName(one) + " for " + std::to_string(price) + " gold.");
    event(EV_PICKUP, p.pos, p.st.id);
}

// Anything that lived on a collapsed level seeps down to the first open level.
void Server::collapseQuests() {
    int c = collapsedDepth();
    if (c <= 0) return;
    bool changed = false;
    for (auto& q : quests_) {
        if (!q.main || q.done || q.depth > c) continue;
        q.depth = c + 1;
        const MonsterDef& d = gData.monsters[q.target];
        q.text = "Slay " + d.name + " on dungeon level " + std::to_string(q.depth) + ".";
        text(-1, COL_ORANGE, d.name + " has fled deeper as the upper levels collapsed - now on level " + std::to_string(q.depth) + "!");
        changed = true;
    }
    if (changed) sendQuests();
}

void Server::debugGive(int playerId, const std::string& ids) {
    SPlayer* p = playerId ? playerById(playerId) : (players_.empty() ? nullptr : &players_.begin()->second);
    if (!p) return;
    std::string rest = ids;
    while (!rest.empty()) {
        size_t c = rest.find(',');
        std::string id = rest.substr(0, c);
        rest = c == std::string::npos ? "" : rest.substr(c + 1);
        int def = gData.findItem(id);
        if (def < 0) { std::printf("[server] unknown item %s\n", id.c_str()); continue; }
        if (gData.items[def].artifact)  // keep artifacts unique: remove a copy lying around
            items_.erase(std::remove_if(items_.begin(), items_.end(), [def](const FloorItem& f) { return f.item.def == def; }), items_.end());
        ItemStack it = makeItem(def);
        if (!addToInventory(*p, it)) dropItem(it, p->pos);
    }
}

void Server::debugDrop(int playerId, const std::string& id) {
    SPlayer* p = playerById(playerId);
    int def = gData.findItem(id);
    if (p && def >= 0) dropItem(makeItem(def), p->pos);
}

// =========================================================== campaigns ======

// The goal of a campaign level becomes a main quest (once, until done).
void Server::addCampaignGoal(int level) {
    if (campaign_ < 0 || campaignDone_.count(level)) return;
    for (auto& q : quests_)
        if (q.main && q.depth == level) return;
    const CampaignLevel& L = gData.campaigns[campaign_].levels[level - 1];
    Quest q;
    q.main = true;
    q.depth = level;
    q.target = L.target;
    std::string where = " (level " + std::to_string(level) + ")";
    switch (L.goal) {
        case G_KILL:
            q.type = Q_KILL_BOSS;
            q.text = "Slay " + gData.monsters[L.target].name + where + ".";
            q.rewardXp = gData.monsters[L.target].xp;
            break;
        case G_FIND:
            q.type = Q_FIND_ITEM;
            q.text = "Find " + gData.items[L.target].name + where + ".";
            q.rewardXp = 20 * L.depth;
            break;
        case G_CLEAR:
            q.type = Q_CLEAR;
            q.needed = 1;  // set when the level is built
            q.text = "Clear the level of monsters" + where + ".";
            q.rewardXp = 15 * L.depth;
            break;
        default:
            q.type = Q_STAIRS;
            q.text = "Find the way down" + where + ".";
            q.rewardXp = 5 * L.depth;
            break;
    }
    if (!L.goalText.empty()) q.text = L.goalText;
    quests_.push_back(q);
}

bool Server::buildCampaignLevel(int level) {
    const Campaign& c = gData.campaigns[campaign_];
    const CampaignLevel& L = c.levels[level - 1];
    std::vector<Placement> placed;
    bool hand = !L.map.empty();
    if (hand) {
        const HandMap& h = gData.handMaps[gData.findHandMap(L.map)];
        std::string err;
        uint32_t seed = map_.seed;
        if (!buildHandMap(h, map_, placed, err)) { std::printf("[server] %s\n", err.c_str()); return false; }
        map_.seed = seed;
        power_ = h.depth;
        map_.depth = level;
        for (auto& p : placed) {
            Vec2 pos(p.x + 0.5f, p.y + 0.5f);
            if (p.kind == Placement::Monster) {
                spawnMonster(p.def, pos, p.awake);
                monsters_.back().asleep = !p.awake && gData.monsters[p.def].sleep > 0;
            } else {
                dropItem(makeItem(p.def), pos);
            }
        }
        if (h.randomMonsters) populate(L.monsters.empty() ? nullptr : &L.monsters);
        else {
            // The quest boss still has to be there.
            for (auto& q : quests_) {
                bool present = false;
                for (auto& m : monsters_) if (m.def == q.target) present = true;
                if (q.main && !q.done && q.type == Q_KILL_BOSS && q.depth == level && !present)
                    spawnMonster(q.target, Vec2(map_.downX + 0.5f, map_.downY + 0.5f));
            }
        }
    } else {
        generateLevel(map_, map_.seed, L.depth);
        power_ = L.depth;
        map_.depth = level;
        populate(L.monsters.empty() ? nullptr : &L.monsters);
    }
    // Goal items that the map didn't place go somewhere far from the start.
    for (auto& q : quests_) {
        if (q.done || q.depth != level) continue;
        if (q.type == Q_FIND_ITEM) {
            bool there = false;
            for (auto& it : items_) if (it.item.def == q.target) there = true;
            for (auto& [id, p] : players_)
                for (auto& s : p.st.inv) if (s.def == q.target) there = true;
            Vec2 pos;
            if (!there && randomFloor(pos, true)) dropItem(makeItem(q.target), pos);
        }
        if (q.type == Q_CLEAR) { q.count = 0; q.needed = std::max(1, (int)monsters_.size()); }
    }
    pendingIntro_ = L.intro;
    return true;
}
