// Dum - game content definitions, loaded from text files in data/.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "common/common.h"
#include "game/campaign.h"

enum StatId { STR, DEX, CON, INT, WIS, NUM_STATS };
extern const char* kStatNames[NUM_STATS];

enum class ItemKind : uint8_t { Weapon, Armor, Light, Food, Potion, Scroll, Fuel, Gold, Quest, Helm, Ring, Amulet, Wand, Rod };
enum class Element : uint8_t { Physical, Arcane, Fire, Cold, Holy, Poison };
enum class SpellType : uint8_t {
    Bolt, Ball, Nova, Heal, Bless, Light, Detect, Blink, Map, Recall,
    Teleport, Haste, Shield, Resist, Dig, Feed, Whirlwind, Berserk, Descent
};
// Extra effect of a bolt/ball/nova on the monsters it hits.
enum class SpellEffect : uint8_t { None, Sleep, Fear, Slow, TeleportAway };

struct ClassDef {
    std::string id, name, desc;
    int stats[NUM_STATS] = {10, 10, 10, 10, 10};
    int hitDie = 8;
    int manaBase = 0, manaPerLevel = 0;
    int melee = 0, ranged = 0, magic = 0;   // skill bonuses (to-hit / spell power)
    int search = 0, stealth = 0, disarm = 0;
    int device = 0;                // skill with wands and rods
    int castStat = INT;
    uint32_t color = 0xFFFFFFFF;
    std::vector<int> spells;       // spell indices
    std::vector<int> spellLevels;  // character level needed for each spell
    std::vector<int> startItems;   // item indices
};

struct ItemDef {
    std::string id, name, sprite, desc;
    ItemKind kind = ItemKind::Quest;
    uint32_t color = 0xFFFFFFFF;
    // weapons
    Dice dmg{1, 2};
    int toHit = 0;
    bool ranged = false;
    bool finesse = false;     // uses DEX instead of STR
    float range = 1.6f;       // tiles
    float arc = 70.0f;        // auto-aim cone width in degrees
    float cooldown = 0.8f;    // seconds between attacks
    std::string viewmodel;    // first-person weapon picture
    // armor / light / consumables
    int armor = 0;
    int light = 0;            // light radius in tiles
    int fuel = 0;             // seconds of fuel (light) or fuel restored (oil)
    int food = 0;             // food points restored
    int heal = 0, mana = 0;   // potions
    int spell = -1;           // staffs: improve this spell, scrolls: cast this spell
    int spellPower = 0;       // staff bonus to spell power
    // wands (charges, kept in ItemStack::fuel) and rods (recharge seconds)
    int chargesMin = 0, chargesMax = 0;
    int recharge = 0;
    // bonuses while worn/wielded (any equipment)
    int statBonus[5] = {0, 0, 0, 0, 0};
    int toDam = 0;            // extra damage
    int speed = 0;            // +1 = 10% faster movement
    int stealth = 0, search = 0;
    int slowDigest = 0;       // % less hunger
    int regen = 0;            // % faster healing
    std::vector<Element> resist;
    Element brand = Element::Physical;  // weapons: damage element (fire, cold, holy...)
    bool artifact = false;    // unique: only one exists per game
    bool everburning = false; // light that never runs out
    int gainStat = -1;        // potion: permanently raises this stat
    bool restore = false;     // potion: restores drained stats
    bool curePoison = false;  // potion: cures poison (any healing does too)
    // generation
    int depth = 1, rarity = 0; // rarity 0 = never generated randomly
    int value = 1;
    int maxStack = 1;
};

struct MonsterDef {
    std::string id, name, plural, sprite;
    uint32_t color = 0xFF808080, color2 = 0xFFFF0000;
    Dice hp{2, 6};
    int ac = 10, toHit = 0;
    Dice dmg{1, 4};
    float speed = 2.0f;        // tiles / second
    float attackCd = 1.2f;
    float reach = 1.1f;
    float sight = 8.0f;        // tiles
    float size = 1.0f;         // sprite scale
    int sleep = 50;            // % chance to be generated asleep
    int xp = 1, depth = 1, rarity = 1;
    int groupMin = 1, groupMax = 1;
    bool undead = false, boss = false, opensDoors = true;
    int spell = -1;
    float spellRate = 0.0f;    // casts per second (on average) when target visible
    int drop = 0;              // % chance to drop an item
    // special abilities
    bool steals = false;       // steals gold on a hit, then teleports away
    float regen = 0;           // hit points regained per second
    int erratic = 0;           // % of the time it flutters about randomly
    bool drainsLight = false;  // its hits burn away your light's fuel
    int deathSpell = -1;       // explodes with this spell when it dies
    float breedTime = 0;       // multiplies slowly: one copy every ~N seconds while awake (0 = never)
    int poison = 0;            // its hits poison you for this many seconds
    int drainStat = -1;        // its hits may drain this stat
    bool blink = false;        // teleports short distances while fighting
    bool invisible = false;    // hard to see: only targeted up close (or with detection)
    int summon = -1;           // calls monsters of this kind for help (at most 3 times)
    float summonRate = 0;      // chance per second while fighting
    std::vector<Element> resist, vulnerable;
};

struct SpellDef {
    std::string id, name, desc;
    SpellType type = SpellType::Bolt;
    Element elem = Element::Arcane;
    int mana = 1, level = 1;
    Dice dmg{1, 4};
    float scale = 0.5f;        // extra damage per caster level
    float radius = 0, range = 12, speed = 12, duration = 0, cooldown = 0.6f;
    float arc = 40.0f;         // auto-aim cone for bolts/balls
    uint32_t color = 0xFFFFFFFF;
    SpellEffect effect = SpellEffect::None;
    float recharge = 0;        // seconds before this spell/ability can be used again
    bool selfOnly = false;     // heal: only the caster
    bool ability = false;      // a fighting technique, not magic (no mana, not "spell")
};

struct StoreDef {
    std::string id, name, owner;
    int sign = -1;                    // item whose picture is painted on the shop sign
    std::vector<int> items;           // what it sells (unlimited stock)
    std::vector<ItemKind> buys;       // kinds of items it buys (quest treasures: every shop)
    bool buysStaffs = false;          // magic shop: buys weapons with spell power
};

// Difficulty: server-side rule multipliers (the host's choice applies to everyone).
struct DifficultyRules {
    std::string id, name, desc;
    float monsterDamage = 1, monsterHp = 1;
    int monsterToHit = 0;
    float alertness = 1;      // how quickly sleeping monsters notice you
    float monsterSpeed = 1;
    int extraGroups = 0;      // monster groups per level
    float hunger = 1, fuel = 1;
    float respawnTime = 12;   // seconds until a dead player returns
    bool wipeRestarts = true; // whole party dead: start over (else revive at the stairs)
    float xp = 1, gold = 1;
    float monsterCount = 1;   // multiplier for the number of monster groups
    // The Abyss: every dungeon level below the first adds per_level (e.g. 0.5 = +50%) of the
    // base monster count, health, damage, xp and gold. Hidden = not in the difficulty menu.
    float perLevel = 0;
    bool hidden = false;
    // These rules as they apply on dungeon level 'depth' (only differs when perLevel > 0).
    DifficultyRules atDepth(int depth) const {
        DifficultyRules r = *this;
        if (perLevel <= 0 || depth <= 1) return r;
        float f = 1 + perLevel * (depth - 1);
        r.monsterCount *= f; r.monsterHp *= f; r.monsterDamage *= f;
        r.xp *= f; r.gold *= f;
        r.monsterToHit += (depth - 1) / 2;
        return r;
    }
};

struct GameData {
    std::vector<ClassDef> classes;
    std::vector<DifficultyRules> difficulties;
    std::vector<HandMap> handMaps;
    std::vector<Campaign> campaigns;
    std::vector<StoreDef> stores;
    std::vector<ItemDef> items;
    std::vector<MonsterDef> monsters;
    std::vector<SpellDef> spells;
    std::string dir;

    bool load(const std::string& dataDir, std::string& err);
    int findItem(const std::string& id) const;
    int findMonster(const std::string& id) const;
    int findSpell(const std::string& id) const;
    int findClass(const std::string& id) const;
    int findDifficulty(const std::string& id) const;
    int findHandMap(const std::string& id) const;
    int findCampaign(const std::string& id) const;
    bool storeBuys(int store, const ItemDef& d) const;
};

// Shop prices (also used by the client to show them).
int buyPrice(const ItemDef& d);
int sellPriceEach(const ItemDef& d, int fuel);

extern GameData gData;

// Locate the data directory (next to the executable, or ./data, or ../data).
std::string findDataDir(const char* argv0);
