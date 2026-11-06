// Dum - state shared between server and client: protocol ids, player state, rules helpers.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"
#include "common/serialize.h"
#include "game/data.h"

constexpr uint16_t PROTOCOL_VERSION = 10;  // 8: data file checksums in C_HELLO; 9: second ring, key ring; 10: quiver slot, ammo rule
constexpr uint16_t DEFAULT_PORT = 27666;
constexpr float TICK_DT = 1.0f / 30.0f;
constexpr int INV_SIZE = 20;
constexpr int NUM_QUICK = 9;          // quick slots on keys 1-9
constexpr int QUICK_ITEM = 1000;      // quick slot value >= this: a wand/rod (item def + QUICK_ITEM)
constexpr float FOOD_MAX = 1000.0f;
constexpr float PLAYER_RADIUS = 0.25f;
constexpr float MONSTER_RADIUS = 0.3f;

enum MsgType : uint8_t {
    // client -> server
    C_HELLO = 1,   // u16 version, str name, u8 class, u8 n + n x (str file, u32 checksum)
    C_INPUT,       // f32 x, f32 y, f32 angle, u8 flags
    C_ACTION,      // u8 action, i32 a, i32 b
    C_CHAT,        // str text
    // server -> client
    S_WELCOME = 20,  // u16 your player id, u8 difficulty, u8 rules (1 = ammo is used up)
    S_REJECT,        // str reason
    S_LEVEL,         // map
    S_TILE,          // u16 x, u16 y, u8 tile
    S_SNAPSHOT,      // entities
    S_PSTATE,        // your PlayerState
    S_TEXT,          // u32 color, str
    S_EVENT,         // u8 kind, f32 x, f32 y, u32 entity
    S_QUESTS,        // quest list
    S_ROSTER,        // players
    S_TELEPORT,      // f32 x, f32 y, f32 angle
    S_REVEAL,        // u16 x, u16 y, u8 radius (magic mapping)
};

enum ActionId : uint8_t {
    A_ATTACK,
    A_CAST,       // a = index into class spell list
    A_USE,        // context sensitive: stairs / pick up / door (a=1: direction b/1000 rad)
    A_INV_USE,    // a = inventory slot
    A_INV_DROP,   // a = inventory slot, b = count (0 = all)
    A_INV_GIVE,   // a = inventory slot, b = player id
    A_UNEQUIP,    // a = equip slot
    A_SET_AUTOPICKUP,  // a = 1 on / 0 off
    A_SHOP_BUY,        // a = store, b = index in the store's list
    A_SHOP_SELL,       // a = store, b = inventory slot (sells one)
    A_SET_AIM,         // a = 1: strong aim assist (wider cone)
    A_SET_QUICK,       // a = quick slot 0-8, b = class spell index, QUICK_ITEM + item def, or -1
};

enum EventKind : uint8_t {
    EV_HIT, EV_MISS, EV_SWING, EV_SHOOT, EV_CAST, EV_EXPLODE, EV_HURT, EV_DOOR,
    EV_PICKUP, EV_MONSTER_DIE, EV_PLAYER_DIE, EV_LEVELUP, EV_HEAL, EV_GROWL, EV_STAIRS, EV_CRIT, EV_DIG,
};

enum EntType : uint8_t { ET_PLAYER, ET_MONSTER, ET_ITEM, ET_PROJECTILE, ET_EFFECT };
enum EntFlag : uint8_t { EF_ASLEEP = 1, EF_HURT = 2, EF_ATTACK = 4, EF_DEAD = 8, EF_SLOW = 16, EF_BOSS = 32, EF_BLESS = 64 };
// Chests (items) reuse flag bits: what detection or a search has found out about them.
enum ChestFlag : uint8_t { EF_CHEST_KNOWN = 1, EF_CHEST_TRAP = 2, EF_CHEST_EMPTY = 4 };

// A closed chest keeps its secret in its ItemStack's fuel (so it is saved with no format change):
// low 4 bits = ChestContent, CHEST_KNOWN = detected (contents and trap shown), CHEST_RICH = vault.
enum ChestContent : uint8_t { CH_TREASURE = 0, CH_EMPTY, CH_TRAP_NEEDLE, CH_TRAP_GAS, CH_TRAP_FIRE, CH_TRAP_SUMMON,
                              CH_TRAP_ALARM, CH_TRAP_TELEPORT, CH_COUNT };
constexpr int CHEST_KNOWN = 16, CHEST_RICH = 32;
inline bool chestTrapped(int fuel) { return (fuel & 15) >= CH_TRAP_NEEDLE; }

// One entity as sent in a snapshot.
struct EntSnap {
    uint32_t id = 0;
    uint8_t type = 0;
    uint16_t def = 0;      // monster/item/class/spell index (0xFFFF = arrow)
    Vec2 pos;
    float angle = 0;
    uint8_t flags = 0;
    uint8_t hp = 255;      // health fraction 0..255
    uint8_t extra = 0;     // player: light radius * 10, effect: radius * 10
    void write(Writer& w) const;
    void read(Reader& r);
};

// Two ring fingers and a quiver (the loaded arrows/bolts/pebbles). (Saves before version 10
// had one ring then the amulet; before 11 no quiver.)
enum EquipSlot { EQ_WEAPON, EQ_ARMOR, EQ_LIGHT, EQ_HELM, EQ_RING, EQ_RING2, EQ_AMULET, EQ_QUIVER, EQ_COUNT };
// Which equipment slot an item goes into (-1 = not wearable).
int equipSlotFor(const ItemDef& d);
// Can this item be worn in that slot? (a ring fits either ring slot)
bool fitsSlot(const ItemDef& d, int slot);
extern const char* kEquipNames[EQ_COUNT];

struct ItemStack {
    int16_t def = -1;
    int32_t count = 0;
    int32_t fuel = 0;       // remaining light fuel (seconds)
    bool empty() const { return def < 0 || count <= 0; }
    const ItemDef& d() const { return gData.items[def]; }
    void write(Writer& w) const { w.i16(def); w.i32(count); w.i32(fuel); }
    void read(Reader& r) { def = r.i16(); count = r.i32(); fuel = r.i32(); }
};

std::string itemName(const ItemStack& s);

// The full private state of a character (server authoritative, mirrored to its owner).
struct PlayerState {
    uint16_t id = 0;
    std::string name;
    uint8_t cls = 0;
    int level = 1, xp = 0;
    int stats[NUM_STATS] = {10, 10, 10, 10, 10};
    int hp = 10, maxHp = 10;
    float mp = 0, maxMp = 0;
    float food = 800;
    int gold = 0;
    uint8_t keys[4] = {0, 0, 0, 0};   // the key ring: keys of each KeyColor (never take a pack slot)
    ItemStack inv[INV_SIZE];
    ItemStack equip[EQ_COUNT];
    float blessT = 0, lightT = 0, detectT = 0, slowT = 0;
    float hasteT = 0, shieldT = 0, resistT = 0, heroT = 0;
    float poisonT = 0;     // poisoned: loses hit points over time
    int drained[NUM_STATS] = {0, 0, 0, 0, 0};   // stat points drained by monsters (restorable)
    float recallT = 0;     // party-wide Word of Recall countdown (display only)
    int16_t quick[NUM_QUICK] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::vector<float> recharge;   // per class spell: seconds until usable again
    float attackCd = 0, castCd = 0;
    bool dead = false;
    float respawnT = 0;
    int kills = 0;

    const ClassDef& c() const { return gData.classes[cls]; }
    int stat(int s) const { return clampi(stats[s] - drained[s] + equipSum([s](const ItemDef& d) { return d.statBonus[s]; }), 3, 25); }
    int mod(int s) const { return statMod(stat(s)); }
    template <class F> int equipSum(F f) const {
        int t = 0;
        for (int i = 0; i < EQ_COUNT; i++)   // (the quiver's bonuses count only for shots)
            if (i != EQ_QUIVER && !equip[i].empty()) t += f(equip[i].d());
        return t;
    }
    int toHitBonus() const { return equipSum([](const ItemDef& d) { return d.toHit; }); }
    int toDamBonus() const { return equipSum([](const ItemDef& d) { return d.toDam; }); }
    int speedBonus() const { return equipSum([](const ItemDef& d) { return d.speed; }); }
    int stealthBonus() const { return equipSum([](const ItemDef& d) { return d.stealth; }); }
    int searchBonus() const { return equipSum([](const ItemDef& d) { return d.search; }); }
    int slowDigest() const { return clampi(equipSum([](const ItemDef& d) { return d.slowDigest; }), 0, 80); }
    int regenBonus() const { return equipSum([](const ItemDef& d) { return d.regen; }); }
    bool resists(Element e) const {
        if (resistT > 0 && (e == Element::Fire || e == Element::Cold || e == Element::Poison)) return true;
        for (int i = 0; i < EQ_COUNT; i++)
            if (i != EQ_QUIVER && !equip[i].empty()) for (Element r : equip[i].d().resist) if (r == e) return true;
        return false;
    }
    Element weaponBrand() const { return equip[EQ_WEAPON].empty() ? Element::Physical : equip[EQ_WEAPON].d().brand; }
    // Haste and slowness change how fast you attack, cast and move.
    float cdMult() const { return (slowT > 0 ? 1.6f : 1.0f) * (hasteT > 0 ? 0.7f : 1.0f); }
    float moveMult() const { return (slowT > 0 ? 0.5f : 1.0f) * (hasteT > 0 ? 1.35f : 1.0f) * (1.0f + 0.1f * speedBonus()); }
    float rechargeLeft(int classSpell) const { return classSpell >= 0 && classSpell < (int)recharge.size() ? recharge[classSpell] : 0.0f; }
    int devicePower() const { return c().device + mod(INT) / 2 + level / 5; }
    // Chance (0-100) that using a wand or rod fails.
    int deviceFail(const ItemDef& d) const { return clampi(22 + d.depth * 2 - c().device * 4 - mod(INT) * 3 - level / 3, 2, 60); }
    // Put a spell or device into the first free quick slot (if it is not in one already).
    void autoQuick(int value);
    int armorClass() const;
    int lightRadius() const;           // in tiles (0 = darkness)
    int spellPower() const;            // bonus added to spell damage/healing
    bool knowsSpell(int classSpellIdx) const;
    int xpForNext() const { return 5 * level * (level + 1); }
    const ItemDef* weapon() const { return equip[EQ_WEAPON].empty() ? nullptr : &equip[EQ_WEAPON].d(); }

    void write(Writer& w) const;
    void read(Reader& r);
};

struct Quest {
    uint8_t type = 0;      // QuestType
    bool main = false, done = false;
    int depth = 1;
    int target = -1;       // monster or item def
    int count = 0, needed = 1;
    int rewardXp = 0, rewardGold = 0;
    std::string text;
    void write(Writer& w) const;
    void read(Reader& r);
};
enum QuestType : uint8_t { Q_KILL_BOSS, Q_FIND_ITEM, Q_SLAY, Q_CLEAR, Q_STAIRS };

// Difficulty levels come from data/difficulty.txt (see DifficultyRules in data.h).
inline int numDifficulties() { return (int)gData.difficulties.size(); }
inline const DifficultyRules& difficultyRules(int d) { return gData.difficulties[clampi(d, 0, numDifficulties() - 1)]; }
inline int defaultDifficulty() {
    int i = gData.findDifficulty("normal");
    return i < 0 ? numDifficulties() / 2 : i;
}

struct RosterEntry {
    uint16_t id;
    std::string name;
    uint8_t cls;
    uint8_t level;
};
