// Dum - data file loader.
//
// File format (INI-like, see data/*.txt):
//   # comment
//   [monster goblin]
//   name = Goblin
//   hp = 2d8
#include "game/data.h"
#include "game/map.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <sys/stat.h>

GameData gData;
const char* kStatNames[NUM_STATS] = {"STR", "DEX", "CON", "INT", "WIS"};

namespace {

struct Section {
    std::string type, id, file;
    int line = 0;
    std::map<std::string, std::string> kv;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim(part);
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

bool readFile(const std::string& path, std::vector<Section>& out, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot open " + path; return false; }
    std::string line;
    int ln = 0;
    Section* cur = nullptr;
    std::string lastKey;
    while (std::getline(f, line)) {
        ln++;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        line = trim(line);
        if (line.empty()) continue;
        // An indented line without '=' continues the previous value (long lists).
        if (indented && cur && !lastKey.empty() && line.find('=') == std::string::npos) {
            cur->kv[lastKey] += " " + line;
            continue;
        }
        if (line[0] == '[') {
            size_t end = line.find(']');
            std::stringstream ss(line.substr(1, end - 1));
            Section s;
            ss >> s.type >> s.id;
            s.file = path;
            s.line = ln;
            out.push_back(s);
            cur = &out.back();
            lastKey.clear();
            continue;
        }
        size_t eq = line.find('=');
        if (!cur || eq == std::string::npos) {
            err = path + ":" + std::to_string(ln) + ": expected 'key = value'";
            return false;
        }
        lastKey = trim(line.substr(0, eq));
        cur->kv[lastKey] = trim(line.substr(eq + 1));
    }
    return true;
}

struct Getter {
    const Section& s;
    std::string* err;
    mutable std::set<std::string> used;
    Getter(const Section& sec, std::string* e) : s(sec), err(e) {}
    // Warn about keys that were never read (usually typos).
    ~Getter() {
        for (auto& [k, v] : s.kv)
            if (!used.count(k)) std::fprintf(stderr, "warning: %s:%d: [%s %s] unknown key '%s' (ignored)\n", s.file.c_str(), s.line, s.type.c_str(), s.id.c_str(), k.c_str());
    }
    bool has(const char* k) const { used.insert(k); return s.kv.count(k) > 0; }
    std::string str(const char* k, const std::string& def = "") const {
        used.insert(k);
        auto it = s.kv.find(k);
        return it == s.kv.end() ? def : it->second;
    }
    int i(const char* k, int def) const { return has(k) ? std::atoi(str(k).c_str()) : def; }
    float f(const char* k, float def) const { return has(k) ? (float)std::atof(str(k).c_str()) : def; }
    bool b(const char* k, bool def) const {
        if (!has(k)) return def;
        std::string v = str(k);
        return v == "1" || v == "yes" || v == "true";
    }
    Dice dice(const char* k, Dice def) const {
        if (!has(k)) return def;
        Dice d;
        if (std::sscanf(str(k).c_str(), "%dd%d", &d.n, &d.s) != 2) { d.n = std::atoi(str(k).c_str()); d.s = 1; }
        return d;
    }
    uint32_t color(const char* k, uint32_t def) const {
        if (!has(k)) return def;
        return 0xFF000000u | (uint32_t)std::strtoul(str(k).c_str(), nullptr, 16);
    }
};

template <class T>
int findIn(const std::vector<T>& v, const std::string& id) {
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].id == id) return (int)i;
    return -1;
}

int statFromName(const std::string& s) {
    for (int i = 0; i < NUM_STATS; i++) {
        std::string n = kStatNames[i];
        std::string low;
        for (char c : n) low += (char)std::tolower(c);
        if (s == low || s == n) return i;
    }
    return INT;
}

}  // namespace

int GameData::findItem(const std::string& id) const { return findIn(items, id); }
int GameData::findMonster(const std::string& id) const { return findIn(monsters, id); }
int GameData::findSpell(const std::string& id) const { return findIn(spells, id); }
int GameData::findClass(const std::string& id) const { return findIn(classes, id); }
int GameData::findDifficulty(const std::string& id) const { return findIn(difficulties, id); }
int GameData::findHandMap(const std::string& id) const { return findIn(handMaps, id); }
int GameData::findCampaign(const std::string& id) const { return findIn(campaigns, id); }

const std::vector<std::string>& GameData::sharedNames() {
    static const std::vector<std::string> n = {"spells.txt", "items.txt", "monsters.txt", "classes.txt", "stores.txt", "difficulty.txt"};
    return n;
}

uint32_t GameData::textHash(const std::string& text) {
    uint32_t h = 2166136261u;   // FNV-1a
    for (char c : text) {
        if (c == '\r') continue;
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return h;
}

bool GameData::load(const std::string& dataDir, std::string& err) {
    dir = dataDir;
    shared.clear();
    for (auto& n : sharedNames()) {
        std::ifstream f(dataDir + "/" + n, std::ios::binary);
        SharedFile sf;
        sf.name = n;
        if (f) sf.text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        sf.hash = textHash(sf.text);
        shared.push_back(std::move(sf));
    }
    std::vector<Section> secs;
    for (const char* name : {"spells.txt", "items.txt", "monsters.txt", "classes.txt", "stores.txt"})
        if (!readFile(dataDir + "/" + name, secs, err)) return false;
    {   // optional
        std::string e2;
        readFile(dataDir + "/difficulty.txt", secs, e2);
    }

    auto where = [](const Section& s) { return s.file + ":" + std::to_string(s.line) + ": "; };

    // Pass 1: spells (referenced by everything else).
    for (auto& s : secs) {
        if (s.type != "spell") continue;
        Getter g(s, &err);
        SpellDef d;
        d.id = s.id;
        d.name = g.str("name", s.id);
        d.desc = g.str("desc");
        std::string t = g.str("type", "bolt");
        static const std::map<std::string, SpellType> types = {
            {"bolt", SpellType::Bolt}, {"ball", SpellType::Ball}, {"nova", SpellType::Nova},
            {"heal", SpellType::Heal}, {"bless", SpellType::Bless}, {"light", SpellType::Light},
            {"detect", SpellType::Detect}, {"blink", SpellType::Blink}, {"map", SpellType::Map},
            {"recall", SpellType::Recall}, {"teleport", SpellType::Teleport}, {"haste", SpellType::Haste},
            {"shield", SpellType::Shield}, {"resist", SpellType::Resist}, {"dig", SpellType::Dig},
            {"feed", SpellType::Feed}, {"whirlwind", SpellType::Whirlwind}, {"berserk", SpellType::Berserk},
            {"descent", SpellType::Descent}};
        if (!types.count(t)) { err = where(s) + "unknown spell type " + t; return false; }
        d.type = types.at(t);
        std::string e = g.str("element", "arcane");
        static const std::map<std::string, Element> elems = {
            {"physical", Element::Physical}, {"arcane", Element::Arcane}, {"fire", Element::Fire},
            {"cold", Element::Cold}, {"holy", Element::Holy}, {"poison", Element::Poison}};
        if (!elems.count(e)) { err = where(s) + "unknown element " + e; return false; }
        d.elem = elems.at(e);
        d.mana = g.i("mana", 1);
        d.level = g.i("level", 1);
        d.dmg = g.dice("dice", {0, 0});
        d.scale = g.f("scale", 0.5f);
        d.radius = g.f("radius", 0);
        d.range = g.f("range", 12);
        d.speed = g.f("speed", 12);
        d.duration = g.f("duration", 0);
        d.cooldown = g.f("cooldown", 0.6f);
        d.arc = g.f("arc", 40);
        d.color = g.color("color", 0xFFFFFFFF);
        std::string ef = g.str("effect", "none");
        static const std::map<std::string, SpellEffect> effects = {
            {"none", SpellEffect::None}, {"sleep", SpellEffect::Sleep}, {"fear", SpellEffect::Fear}, {"slow", SpellEffect::Slow},
            {"teleport", SpellEffect::TeleportAway}};
        if (!effects.count(ef)) { err = where(s) + "unknown spell effect " + ef; return false; }
        d.effect = effects.at(ef);
        d.recharge = g.f("recharge", 0);
        d.selfOnly = g.str("target", "") == "self";
        d.ability = g.b("ability", false);
        spells.push_back(d);
    }

    // Pass 2: items.
    for (auto& s : secs) {
        if (s.type != "item") continue;
        Getter g(s, &err);
        ItemDef d;
        d.id = s.id;
        d.name = g.str("name", s.id);
        d.desc = g.str("desc");
        d.sprite = g.str("sprite", "potion");
        std::string k = g.str("kind", "quest");
        static const std::map<std::string, ItemKind> kinds = {
            {"weapon", ItemKind::Weapon}, {"armor", ItemKind::Armor}, {"light", ItemKind::Light},
            {"food", ItemKind::Food}, {"potion", ItemKind::Potion}, {"scroll", ItemKind::Scroll},
            {"fuel", ItemKind::Fuel}, {"gold", ItemKind::Gold}, {"quest", ItemKind::Quest},
            {"helm", ItemKind::Helm}, {"ring", ItemKind::Ring}, {"amulet", ItemKind::Amulet},
            {"wand", ItemKind::Wand}, {"rod", ItemKind::Rod}};
        if (!kinds.count(k)) { err = where(s) + "unknown item kind " + k; return false; }
        d.kind = kinds.at(k);
        d.color = g.color("color", 0xFFC0C0C0);
        d.dmg = g.dice("dice", {1, 2});
        d.toHit = g.i("tohit", 0);
        d.ranged = g.b("ranged", false);
        d.finesse = g.b("finesse", false);
        d.range = g.f("range", d.ranged ? 14.0f : 1.6f);
        d.arc = g.f("arc", d.ranged ? 30.0f : 70.0f);
        d.cooldown = g.f("cooldown", 0.8f);
        d.viewmodel = g.str("viewmodel", "fist");
        d.armor = g.i("armor", 0);
        d.light = g.i("light", 0);
        d.fuel = g.i("fuel", 0);
        d.food = g.i("food", 0);
        d.heal = g.i("heal", 0);
        d.mana = g.i("mana", 0);
        if (g.has("spell")) {
            d.spell = findSpell(g.str("spell"));
            if (d.spell < 0) { err = where(s) + "unknown spell " + g.str("spell"); return false; }
        }
        d.spellPower = g.i("spellpower", 0);
        std::string ch = g.str("charges", "0");
        if (std::sscanf(ch.c_str(), "%d-%d", &d.chargesMin, &d.chargesMax) != 2) d.chargesMin = d.chargesMax = std::atoi(ch.c_str());
        d.recharge = g.i("recharge", 0);
        if ((d.kind == ItemKind::Wand || d.kind == ItemKind::Rod) && d.spell < 0) { err = where(s) + "wands and rods need a 'spell'"; return false; }
        d.depth = g.i("depth", 1);
        d.rarity = g.i("rarity", 0);
        d.value = g.i("value", 1);
        d.maxStack = g.i("stack", 1);
        const char* statKeys[5] = {"str", "dex", "con", "int", "wis"};
        for (int i = 0; i < 5; i++) d.statBonus[i] = g.i(statKeys[i], 0);
        d.toDam = g.i("todam", 0);
        d.speed = g.i("speed", 0);
        d.stealth = g.i("stealth", 0);
        d.search = g.i("search", 0);
        d.slowDigest = g.i("slowdigest", 0);
        d.regen = g.i("regen", 0);
        d.artifact = g.b("artifact", false);
        d.everburning = g.b("everburning", false);
        static const std::map<std::string, Element> elemNames = {
            {"physical", Element::Physical}, {"arcane", Element::Arcane}, {"fire", Element::Fire},
            {"cold", Element::Cold}, {"holy", Element::Holy}, {"poison", Element::Poison}};
        for (auto& e : splitList(g.str("resist"))) {
            if (!elemNames.count(e)) { err = where(s) + "unknown element " + e; return false; }
            d.resist.push_back(elemNames.at(e));
        }
        if (g.has("brand")) {
            if (!elemNames.count(g.str("brand"))) { err = where(s) + "unknown element " + g.str("brand"); return false; }
            d.brand = elemNames.at(g.str("brand"));
        }
        if (g.has("gainstat")) d.gainStat = statFromName(g.str("gainstat"));
        d.restore = g.b("restore", false);
        d.curePoison = g.b("curepoison", false);
        items.push_back(d);
    }

    // Pass 3: monsters.
    for (auto& s : secs) {
        if (s.type != "monster") continue;
        Getter g(s, &err);
        MonsterDef d;
        d.id = s.id;
        d.name = g.str("name", s.id);
        d.plural = g.str("plural", d.name + "s");
        d.sprite = g.str("sprite", "humanoid");
        d.color = g.color("color", 0xFF808080);
        d.color2 = g.color("color2", 0xFFFF2020);
        d.hp = g.dice("hp", {2, 6});
        d.ac = g.i("ac", 10);
        d.toHit = g.i("tohit", 0);
        d.dmg = g.dice("dmg", {1, 4});
        d.speed = g.f("speed", 2.0f);
        d.attackCd = g.f("attackcd", 1.2f);
        d.reach = g.f("reach", 1.1f);
        d.sight = g.f("sight", 8.0f);
        d.size = g.f("size", 1.0f);
        d.sleep = g.i("sleep", 50);
        d.xp = g.i("xp", 1);
        d.depth = g.i("depth", 1);
        d.rarity = g.i("rarity", 1);
        std::string grp = g.str("group", "1");
        if (std::sscanf(grp.c_str(), "%d-%d", &d.groupMin, &d.groupMax) != 2) d.groupMin = d.groupMax = std::max(1, std::atoi(grp.c_str()));
        d.undead = g.b("undead", false);
        d.boss = g.b("boss", false);
        d.opensDoors = g.b("doors", true);
        if (g.has("spell")) {
            d.spell = findSpell(g.str("spell"));
            if (d.spell < 0) { err = where(s) + "unknown spell " + g.str("spell"); return false; }
        }
        d.spellRate = g.f("spellrate", 0.0f);
        d.drop = g.i("drop", 20);
        d.steals = g.b("steals", false);
        d.regen = g.f("regen", 0);
        d.erratic = g.i("erratic", 0);
        d.drainsLight = g.b("drainlight", false);
        if (g.has("explode")) {
            d.deathSpell = findSpell(g.str("explode"));
            if (d.deathSpell < 0) { err = where(s) + "unknown spell " + g.str("explode"); return false; }
        }
        if (g.b("breeds", false)) d.breedTime = g.f("breedtime", 30.0f);
        else if (g.has("breedtime")) g.f("breedtime", 0);
        d.poison = g.i("poison", 0);
        if (g.has("drain")) d.drainStat = statFromName(g.str("drain"));
        d.blink = g.b("blink", false);
        d.invisible = g.b("invisible", false);
        if (g.has("summon")) {
            d.summon = g.str("summon") == "kin" ? -2 : findMonster(g.str("summon"));
            if (d.summon == -1) { err = where(s) + "unknown monster " + g.str("summon") + " (monsters must be defined before they are summoned, or use 'kin')"; return false; }
        }
        d.summonRate = g.f("summonrate", 0.08f);
        static const std::map<std::string, Element> elemNames = {
            {"physical", Element::Physical}, {"arcane", Element::Arcane}, {"fire", Element::Fire},
            {"cold", Element::Cold}, {"holy", Element::Holy}, {"poison", Element::Poison}};
        for (const char* key : {"resist", "vulnerable"})
            for (auto& e : splitList(g.str(key))) {
                if (!elemNames.count(e)) { err = where(s) + "unknown element " + e; return false; }
                (std::string(key) == "resist" ? d.resist : d.vulnerable).push_back(elemNames.at(e));
            }
        monsters.push_back(d);
    }

    // Pass 5 (below, after classes) handles stores.
    // Pass 4: classes.
    for (auto& s : secs) {
        if (s.type != "class") continue;
        Getter g(s, &err);
        ClassDef d;
        d.id = s.id;
        d.name = g.str("name", s.id);
        d.desc = g.str("desc");
        const char* keys[NUM_STATS] = {"str", "dex", "con", "int", "wis"};
        for (int i = 0; i < NUM_STATS; i++) d.stats[i] = g.i(keys[i], 10);
        d.hitDie = g.i("hitdie", 8);
        d.manaBase = g.i("mana", 0);
        d.manaPerLevel = g.i("manaperlevel", 0);
        d.melee = g.i("melee", 0);
        d.ranged = g.i("ranged", 0);
        d.magic = g.i("magic", 0);
        d.search = g.i("search", 0);
        d.stealth = g.i("stealth", 0);
        d.disarm = g.i("disarm", 0);
        d.device = g.i("device", 0);
        d.castStat = statFromName(g.str("caststat", "int"));
        d.color = g.color("color", 0xFFFFFFFF);
        // "spell_id" or "spell_id:level" (class specific level requirement)
        for (auto& entry : splitList(g.str("spells"))) {
            std::string id = entry;
            int lvl = -1;
            size_t colon = entry.find(':');
            if (colon != std::string::npos) { id = trim(entry.substr(0, colon)); lvl = std::atoi(entry.c_str() + colon + 1); }
            int sp = findSpell(id);
            if (sp < 0) { err = where(s) + "unknown spell " + id; return false; }
            d.spells.push_back(sp);
            d.spellLevels.push_back(lvl > 0 ? lvl : spells[sp].level);
        }
        for (auto& id : splitList(g.str("items"))) {
            int it = findItem(id);
            if (it < 0) { err = where(s) + "unknown item " + id; return false; }
            d.startItems.push_back(it);
        }
        classes.push_back(d);
    }

    // Pass 5: stores.
    static const std::map<std::string, ItemKind> kindNames = {
        {"weapon", ItemKind::Weapon}, {"armor", ItemKind::Armor}, {"light", ItemKind::Light},
        {"food", ItemKind::Food}, {"potion", ItemKind::Potion}, {"scroll", ItemKind::Scroll},
        {"fuel", ItemKind::Fuel}, {"quest", ItemKind::Quest}, {"helm", ItemKind::Helm},
        {"ring", ItemKind::Ring}, {"amulet", ItemKind::Amulet}, {"wand", ItemKind::Wand}, {"rod", ItemKind::Rod}};
    for (auto& s : secs) {
        if (s.type != "store") continue;
        Getter g(s, &err);
        StoreDef d;
        d.id = s.id;
        d.name = g.str("name", s.id);
        d.owner = g.str("owner", "");
        d.sign = findItem(g.str("sign", ""));
        for (auto& id : splitList(g.str("items"))) {
            int it = findItem(id);
            if (it < 0) { err = where(s) + "unknown item " + id; return false; }
            d.items.push_back(it);
        }
        for (auto& k : splitList(g.str("buys"))) {
            if (k == "staff") { d.buysStaffs = true; continue; }
            if (!kindNames.count(k)) { err = where(s) + "unknown item kind " + k; return false; }
            d.buys.push_back(kindNames.at(k));
        }
        stores.push_back(d);
        if (stores.size() > 8) { err = where(s) + "at most 8 stores"; return false; }
    }

    // Difficulty levels (built-in Normal if the file is missing).
    for (auto& s : secs) {
        if (s.type != "difficulty") continue;
        Getter g(s, &err);
        DifficultyRules d;
        d.id = s.id;
        d.name = g.str("name", s.id);
        d.desc = g.str("desc");
        d.monsterDamage = g.f("monster_damage", 1);
        d.monsterHp = g.f("monster_hp", 1);
        d.monsterToHit = g.i("monster_tohit", 0);
        d.alertness = g.f("alertness", 1);
        d.monsterSpeed = g.f("monster_speed", 1);
        d.extraGroups = g.i("extra_groups", 0);
        d.hunger = g.f("hunger", 1);
        d.fuel = g.f("fuel", 1);
        d.respawnTime = g.f("respawn", 12);
        d.wipeRestarts = g.str("party_wipe", "restart") != "revive";
        d.xp = g.f("xp", 1);
        d.gold = g.f("gold", 1);
        d.monsterCount = g.f("monster_count", 1);
        d.perLevel = g.f("per_level", 0);
        d.hidden = g.b("hidden", false);
        difficulties.push_back(d);
    }
    if (difficulties.empty()) { DifficultyRules d; d.id = "normal"; d.name = "Normal"; difficulties.push_back(d); }

    gTownStores = (int)stores.size();

    // Hand-made maps and campaigns (optional folders).
    if (!loadHandMaps(dataDir + "/maps", handMaps, err)) return false;
    for (auto& h : handMaps) {
        Map test;
        std::vector<Placement> pl;
        if (!buildHandMap(h, test, pl, err)) return false;
    }
    if (!loadCampaigns(dataDir + "/campaigns", campaigns, err)) return false;
    for (auto& c : campaigns)
        for (size_t i = 0; i < c.levels.size(); i++)
            if (!c.levels[i].map.empty() && findHandMap(c.levels[i].map) < 0) {
                err = c.file + ": level " + std::to_string(i + 1) + " uses unknown map '" + c.levels[i].map + "'";
                return false;
            }
    if (classes.empty() || items.empty() || monsters.empty()) { err = "data files contain no classes/items/monsters"; return false; }
    if (findItem("gold") < 0) { err = "items.txt must define [item gold]"; return false; }
    return true;
}

static bool dirExists(const std::string& p) {
    struct stat st;
    return stat((p + "/classes.txt").c_str(), &st) == 0;
}

std::string findDataDir(const char* argv0) {
    std::string exe = argv0 ? argv0 : "";
    size_t slash = exe.find_last_of("/\\");
    std::string exeDir = slash == std::string::npos ? "." : exe.substr(0, slash);
    for (const std::string& c : {exeDir + "/data", exeDir + "/../data", std::string("data"), std::string("../data")})
        if (dirExists(c)) return c;
    return "data";
}

bool GameData::storeBuys(int store, const ItemDef& d) const {
    if (store < 0 || store >= (int)stores.size()) return false;
    if (d.kind == ItemKind::Quest) return true;  // treasures sell anywhere
    const StoreDef& st = stores[store];
    if (st.buysStaffs && d.kind == ItemKind::Weapon && d.spellPower > 0) return true;
    for (ItemKind k : st.buys)
        if (k == d.kind) return true;
    return false;
}

int buyPrice(const ItemDef& d) { return std::max(1, d.value); }

int sellPriceEach(const ItemDef& d, int fuel) {
    if (d.kind == ItemKind::Gold) return 0;
    if (d.kind == ItemKind::Quest) return d.value;  // the reward for finding treasures
    float f = 0.5f;
    if (d.kind == ItemKind::Light && d.fuel > 0) f *= clampf((float)fuel / d.fuel, 0.0f, 1.0f);
    if (d.kind == ItemKind::Wand && fuel <= 0) f *= 0.2f;   // empty wands are worth little
    return (int)(d.value * f);
}
