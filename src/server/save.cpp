// Dum - save games.
//
// The whole dungeon state is saved: current level (map, monsters, items), quests and
// every character (connected or not). Content is referenced by its text id, not by
// index, so saves keep working after the data files are edited (unknown ids are dropped).
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "server/server.h"

namespace {

const char kMagic[8] = {'D', 'U', 'M', 'S', 'A', 'V', 'E', '1'};
constexpr uint16_t kSaveVersion = 10;  // 4: equipment slots, artifacts; 5: difficulty id; 6: campaigns; 7: quick slots, buffs; 8: poison, drain; 9: Abyss deaths; 10: second ring, key ring
int gLoadVersion = kSaveVersion;

void writeItem(Writer& w, const ItemStack& s) {
    w.str(s.empty() ? "" : s.d().id);
    w.i32(s.count);
    w.i32(s.fuel);
}

ItemStack readItem(Reader& r) {
    ItemStack s;
    std::string id = r.str();
    s.count = r.i32();
    s.fuel = r.i32();
    s.def = (int16_t)(id.empty() ? -1 : gData.findItem(id));
    if (s.def < 0) s = ItemStack();
    return s;
}

void writeChar(Writer& w, const SavedChar& c) {
    const PlayerState& s = c.st;
    w.str(s.name);
    w.str(s.c().id);
    w.i16((int16_t)s.level);
    w.i32(s.xp);
    for (int v : s.stats) w.u8((uint8_t)v);
    w.i16((int16_t)s.hp); w.i16((int16_t)s.maxHp);
    w.f32(s.mp); w.f32(s.maxMp);
    w.f32(s.food);
    w.i32(s.gold);
    for (auto& it : s.inv) writeItem(w, it);
    for (auto& it : s.equip) writeItem(w, it);
    for (uint8_t k : s.keys) w.u8(k);   // v10
    w.f32(s.blessT); w.f32(s.lightT); w.f32(s.detectT); w.f32(s.slowT);
    w.u8(s.dead);
    w.f32(s.respawnT);
    w.i32(s.kills);
    w.f32(c.pos.x); w.f32(c.pos.y); w.f32(c.angle);
    w.u8((uint8_t)c.depth);
    // v7: buffs, quick slots (spell ids / item ids, so data edits don't break them), recharge
    w.f32(s.hasteT); w.f32(s.shieldT); w.f32(s.resistT); w.f32(s.heroT);
    for (int q : s.quick) {
        if (q >= QUICK_ITEM) w.str("item:" + gData.items[q - QUICK_ITEM].id);
        else if (q >= 0) w.str("spell:" + gData.spells[s.c().spells[q]].id);
        else w.str("");
    }
    w.u8((uint8_t)s.recharge.size());
    for (size_t i = 0; i < s.recharge.size(); i++) { w.str(gData.spells[s.c().spells[i]].id); w.f32(s.recharge[i]); }
    w.f32(s.poisonT);
    for (int d : s.drained) w.u8((uint8_t)d);
}

SavedChar readChar(Reader& r) {
    SavedChar c;
    PlayerState& s = c.st;
    s.name = r.str();
    int cls = gData.findClass(r.str());
    s.cls = (uint8_t)(cls < 0 ? 0 : cls);
    s.level = r.i16();
    s.xp = r.i32();
    for (int& v : s.stats) v = r.u8();
    s.hp = r.i16(); s.maxHp = r.i16();
    s.mp = r.f32(); s.maxMp = r.f32();
    s.food = r.f32();
    s.gold = r.i32();
    for (auto& it : s.inv) it = readItem(r);
    if (gLoadVersion >= 10) {
        for (auto& it : s.equip) it = readItem(r);
        for (uint8_t& k : s.keys) k = r.u8();
    } else if (gLoadVersion >= 4) {
        // weapon, armor, light, helm, ring, amulet (no second ring yet)
        const int old[6] = {EQ_WEAPON, EQ_ARMOR, EQ_LIGHT, EQ_HELM, EQ_RING, EQ_AMULET};
        for (int i = 0; i < 6; i++) s.equip[old[i]] = readItem(r);
    } else {  // version 3: weapon, armor, light only
        for (int i = 0; i < 3; i++) s.equip[i] = readItem(r);
    }
    s.blessT = r.f32(); s.lightT = r.f32(); s.detectT = r.f32(); s.slowT = r.f32();
    s.dead = r.u8();
    s.respawnT = r.f32();
    s.kills = r.i32();
    c.pos.x = r.f32(); c.pos.y = r.f32(); c.angle = r.f32();
    c.depth = r.u8();
    if (gLoadVersion >= 7) {
        s.hasteT = r.f32(); s.shieldT = r.f32(); s.resistT = r.f32(); s.heroT = r.f32();
        auto classSpell = [&](const std::string& id) {
            for (size_t i = 0; i < s.c().spells.size(); i++)
                if (gData.spells[s.c().spells[i]].id == id) return (int)i;
            return -1;
        };
        for (auto& q : s.quick) {
            std::string v = r.str();
            q = -1;
            if (v.rfind("item:", 0) == 0) { int d = gData.findItem(v.substr(5)); if (d >= 0) q = (int16_t)(QUICK_ITEM + d); }
            else if (v.rfind("spell:", 0) == 0) q = (int16_t)classSpell(v.substr(6));
        }
        s.recharge.assign(s.c().spells.size(), 0.0f);
        int n = r.u8();
        for (int i = 0; i < n && r.ok; i++) {
            int k = classSpell(r.str());
            float t = r.f32();
            if (k >= 0) s.recharge[k] = t;
        }
    }
    if (gLoadVersion >= 8) {
        s.poisonT = r.f32();
        for (int& d : s.drained) d = r.u8();
    }
    return c;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Header: magic, version, depth, characters (name, class, level) - readable without the rest.
bool readHeader(Reader& r, SaveInfo& info) {
    char magic[8];
    r.bytes((uint8_t*)magic, 8);
    if (!r.ok || std::memcmp(magic, kMagic, 8) != 0) return false;
    uint16_t v = r.u16();
    if (v < 3 || v > kSaveVersion) return false;
    gLoadVersion = v;
    info.version = v;
    info.depth = r.u8();
    if (gLoadVersion >= 5) {
        int d = gData.findDifficulty(r.str());
        info.difficulty = d < 0 ? defaultDifficulty() : d;
    } else {
        info.difficulty = clampi(r.u8(), 0, numDifficulties() - 1);
    }
    int n = r.u16();
    for (int i = 0; i < n && r.ok; i++) {
        SaveInfo::Char c;
        c.name = r.str();
        c.cls = r.str();
        c.level = r.u16();
        info.chars.push_back(c);
    }
    return r.ok;
}

}  // namespace

SaveInfo Server::peekSave(const std::string& path) {
    SaveInfo info;
    std::vector<uint8_t> data;
    if (!readFile(path, data)) return info;
    Reader r(data);
    info.ok = readHeader(r, info);
    return info;
}

bool Server::saveGame() {
    if (savePath_.empty()) return false;
    // Everyone: connected players and characters waiting for their owner.
    std::vector<SavedChar> chars;
    for (auto& [id, p] : players_)
        if (p.joined) chars.push_back(SavedChar{p.st, p.pos, p.angle, map_.depth});
    for (auto& [k, c] : saved_) chars.push_back(c);

    Writer w;
    w.bytes((const uint8_t*)kMagic, 8);
    w.u16(kSaveVersion);
    w.u8((uint8_t)map_.depth);
    w.str(difficultyRules(difficulty_).id);
    w.u16((uint16_t)chars.size());
    for (auto& c : chars) { w.str(c.st.name); w.str(c.st.c().id); w.u16((uint16_t)c.st.level); }

    w.u32(baseSeed_);
    w.u32(nextId_);
    w.i32(mainQuestStage_);
    w.i32(maxDepth_);
    w.f32(recallT_);
    w.u32((uint32_t)(rng_.s >> 32));
    w.u32((uint32_t)rng_.s);
    map_.serialize(w, false);
    w.u16((uint16_t)map_.downX); w.u16((uint16_t)map_.downY);
    w.u16((uint16_t)map_.rooms.size());
    for (auto& rm : map_.rooms) { w.u8((uint8_t)rm.x); w.u8((uint8_t)rm.y); w.u8((uint8_t)rm.w); w.u8((uint8_t)rm.h); w.u8(rm.lit); }

    w.u16((uint16_t)monsters_.size());
    for (auto& m : monsters_) {
        w.str(m.d().id);
        w.f32(m.pos.x); w.f32(m.pos.y); w.f32(m.angle);
        w.i32(m.hp); w.i32(m.maxHp);
        w.u8(m.asleep); w.u8(m.hasSeen);
    }
    w.u16((uint16_t)items_.size());
    for (auto& it : items_) { writeItem(w, it.item); w.f32(it.pos.x); w.f32(it.pos.y); }
    w.u8((uint8_t)quests_.size());
    for (auto& q : quests_) {
        w.u8(q.type); w.u8(q.main); w.u8(q.done);
        w.i16((int16_t)q.depth);
        bool isItem = q.type == Q_FIND_ITEM;
        w.str(q.target < 0 ? "" : isItem ? gData.items[q.target].id : gData.monsters[q.target].id);
        w.i16((int16_t)q.count); w.i16((int16_t)q.needed);
        w.i32(q.rewardXp); w.i32(q.rewardGold);
        w.str(q.text);
    }
    w.u16((uint16_t)chars.size());
    for (auto& c : chars) writeChar(w, c);
    w.u16((uint16_t)artifactsInPlay_.size());
    for (int a : artifactsInPlay_) w.str(gData.items[a].id);
    w.str(campaign_ >= 0 ? gData.campaigns[campaign_].id : "");
    w.u8(campaignWon_);
    w.u16((uint16_t)campaignDone_.size());
    for (int l : campaignDone_) w.u16((uint16_t)l);
    w.i32(power_);
    w.i32(abyssDeaths_);
    w.i32(abyssFirstDeath_);

    // Write to a temporary file first so a crash never leaves a broken save.
    std::string tmp = savePath_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { std::printf("[server] cannot write %s\n", tmp.c_str()); return false; }
        f.write((const char*)w.buf.data(), (std::streamsize)w.buf.size());
        if (!f) return false;
    }
    std::remove(savePath_.c_str());
    if (std::rename(tmp.c_str(), savePath_.c_str()) != 0) { std::printf("[server] cannot rename save\n"); return false; }
    return true;
}

bool Server::loadGame(const std::string& path, std::string& err) {
    std::vector<uint8_t> data;
    if (!readFile(path, data)) { err = "cannot read " + path; return false; }
    Reader r(data);
    SaveInfo info;
    if (!readHeader(r, info)) { err = "not a Dum save file (or from another version)"; return false; }

    if (gLoadVersion < 5) difficulty_ = clampi(r.u8(), 0, numDifficulties() - 1);
    else difficulty_ = info.difficulty;
    baseSeed_ = r.u32();
    nextId_ = r.u32();
    mainQuestStage_ = r.i32();
    maxDepth_ = r.i32();
    recallT_ = r.f32();
    uint64_t hi = r.u32(), lo = r.u32();
    rng_.s = (hi << 32) | lo;
    if (rng_.s == 0) rng_.s = 1;
    Map m;
    if (!m.deserialize(r)) { err = "damaged save (map)"; return false; }
    m.downX = r.u16(); m.downY = r.u16();
    int nr = r.u16();
    for (int i = 0; i < nr && r.ok; i++) {
        Room rm;
        rm.x = r.u8(); rm.y = r.u8(); rm.w = r.u8(); rm.h = r.u8(); rm.lit = r.u8();
        m.rooms.push_back(rm);
    }

    std::vector<Monster> monsters;
    int nm = r.u16();
    for (int i = 0; i < nm && r.ok; i++) {
        Monster mo;
        int def = gData.findMonster(r.str());
        mo.id = 0;
        mo.pos.x = r.f32(); mo.pos.y = r.f32(); mo.angle = r.f32();
        mo.hp = r.i32(); mo.maxHp = r.i32();
        mo.asleep = r.u8(); mo.hasSeen = r.u8();
        if (def < 0) continue;  // monster removed from the data files
        mo.def = def;
        mo.boss = gData.monsters[def].boss;
        mo.id = nextId_++;
        monsters.push_back(mo);
    }
    std::vector<FloorItem> items;
    int ni = r.u16();
    for (int i = 0; i < ni && r.ok; i++) {
        FloorItem f;
        f.item = readItem(r);
        f.pos.x = r.f32(); f.pos.y = r.f32();
        f.id = nextId_++;
        if (!f.item.empty()) items.push_back(f);
    }
    std::vector<Quest> quests;
    int nq = r.u8();
    for (int i = 0; i < nq && r.ok; i++) {
        Quest q;
        q.type = r.u8(); q.main = r.u8(); q.done = r.u8();
        q.depth = r.i16();
        std::string id = r.str();
        q.target = q.type == Q_FIND_ITEM ? gData.findItem(id) : gData.findMonster(id);
        q.count = r.i16(); q.needed = r.i16();
        q.rewardXp = r.i32(); q.rewardGold = r.i32();
        q.text = r.str();
        if (q.target >= 0) quests.push_back(q);
    }
    std::map<std::string, SavedChar> chars;
    int nc = r.u16();
    for (int i = 0; i < nc && r.ok; i++) {
        SavedChar c = readChar(r);
        std::string k = c.st.name;
        for (auto& ch : k) ch = (char)std::tolower((unsigned char)ch);
        chars[k] = c;
    }
    std::set<int> artifacts;
    if (gLoadVersion >= 4) {
        int na = r.u16();
        for (int i = 0; i < na && r.ok; i++) {
            int a = gData.findItem(r.str());
            if (a >= 0) artifacts.insert(a);
        }
    }
    int campaign = -1;
    bool won = false;
    std::set<int> done;
    int power = std::max(1, m.depth);
    if (gLoadVersion >= 6) {
        std::string cid = r.str();
        if (!cid.empty()) {
            campaign = gData.findCampaign(cid);
            if (campaign < 0) { err = "this save uses the campaign '" + cid + "', which isn't installed"; return false; }
        }
        won = r.u8();
        int nd = r.u16();
        for (int i = 0; i < nd && r.ok; i++) done.insert(r.u16());
        power = r.i32();
    }
    int deaths = 0, firstDeath = 0;
    if (gLoadVersion >= 9) { deaths = r.i32(); firstDeath = r.i32(); }
    if (!r.ok) { err = "damaged save file"; return false; }
    abyssDeaths_ = deaths;
    abyssFirstDeath_ = firstDeath;
    artifactsInPlay_ = artifacts;
    campaign_ = campaign;
    campaignWon_ = won;
    campaignDone_ = done;
    power_ = power;

    map_ = m;
    monsters_ = monsters;
    items_ = items;
    quests_ = quests;
    projectiles_.clear();
    effects_.clear();
    saved_ = chars;
    // A finished main quest chain still needs a next goal.
    bool haveMain = false;
    for (auto& q : quests_) if (q.main && !q.done) haveMain = true;
    if (!haveMain && campaign_ < 0) setupMainQuest();
    flowT_ = 0;
    std::printf("[server] loaded %s: depth %d, %zu characters\n", path.c_str(), map_.depth, saved_.size());
    return true;
}
