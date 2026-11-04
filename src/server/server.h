// Dum - authoritative game server: simulation, combat, AI, quests, networking.
#pragma once

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "game/map.h"
#include "game/state.h"
#include "net/net.h"

struct SPlayer {
    PlayerState st;
    Vec2 pos;
    float angle = 0;
    int peer = -1;
    bool joined = false;
    float regenHp = 0, regenMp = 0, starve = 0, search = 0;
    float hurtT = 0, attackT = 0;
    float noise = 0;           // recent noise level (wakes monsters)
    bool warnedLight = false, warnedFood = false;
    bool dirty = true;         // send full state reliably
    float useCd = 0;
    bool autoPickup = true;
    float stairsConfirmT = 0;   // going up needs a second press
    float fuelAcc = 0;
    bool strongAim = false;
    float rodAcc = 0;          // rod recharge timer (tenths of seconds)
};

struct Monster {
    uint32_t id;
    int def;
    Vec2 pos;
    float angle = 0;
    int hp, maxHp;
    bool asleep = true;
    bool boss = false;
    float attackCd = 0, spellCd = 0, slowT = 0, hurtT = 0, attackT = 0, doorT = 0;
    int target = -1;           // player id
    float wanderT = 0;
    Vec2 wanderDir;
    bool hasSeen = false;      // has noticed a player
    float sleepT = 0;          // magical sleep: can't wake up by itself
    float fearT = 0;           // frightened: runs away instead of fighting
    float regenAcc = 0, erraticT = 0, blinkT = 0, breedT = 0;
    int summons = 0;           // times it has called for help
    Vec2 erraticDir;
    const MonsterDef& d() const;
};

struct FloorItem {
    uint32_t id;
    ItemStack item;
    Vec2 pos;
    int droppedBy = -1;        // player who dropped it (not auto-picked up by them)
};

struct Projectile {
    uint32_t id;
    int spell = -1;            // -1 = arrow
    Vec2 pos, vel;
    float ttl = 3.0f;
    bool fromMonster = false;
    int ownerPlayer = -1;
    uint32_t target = 0;       // homing target entity (monster id or player id)
    int dmg = 0;               // spells: rolled damage
    int toHit = 0;             // arrows: attack bonus
    Dice dice;                 // arrows: damage dice
    int dmgBonus = 0;
    float radius = 0;          // explosion radius
    Element elem = Element::Physical;
    bool sneak = false;
    int power = 0;             // spells: caster power (for sleep/fear effects)
};

struct Effect {
    uint32_t id;
    int spell;
    Vec2 pos;
    float t = 0, life = 0.4f, radius = 1;
};

// A character that is not currently connected (left the game, or loaded from a save).
struct SavedChar {
    PlayerState st;
    Vec2 pos;
    float angle = 0;
    int depth = 1;
};

// Summary of a save file, for menus.
struct SaveInfo {
    bool ok = false;
    int depth = 0;
    int difficulty = 1;
    int version = 0;
    struct Char { std::string name, cls; int level; };
    std::vector<Char> chars;
};

class Server {
public:
    Server(std::shared_ptr<ServerNet> net, uint32_t seed);
    ~Server();

    // ---- saving (see save.cpp) ----
    // Autosave to this file every minute, on level change and when the server stops.
    void setSavePath(const std::string& path) { savePath_ = path; }
    // Single player: the saved character is given to whoever joins, whatever their name.
    void setAdoptAnyCharacter(bool on) { adoptAny_ = on; }
    // Play a campaign from data/campaigns (-1 = endless dungeon). Starts a new game.
    void startCampaign(int c) { campaign_ = c; newGame(); }
    int campaign() const { return campaign_; }
    void setDifficulty(int d) { difficulty_ = clampi(d, 0, numDifficulties() - 1); }
    int difficulty() const { return difficulty_; }
    bool loadGame(const std::string& path, std::string& err);
    bool saveGame();
    static SaveInfo peekSave(const std::string& path);
    void tick();
    int playerCount() const;
    const Map& map() const { return map_; }
    int floorItemCount() const { return (int)items_.size(); }
    int maxDepthReached() const { return maxDepth_; }
    const std::vector<Quest>& quests() const { return quests_; }
    void debugSetMaxDepth(int d) { maxDepth_ = d; }
    void debugGoto(int depth) { newLevel(depth, true); }
    // Debug: give items (comma separated ids) to a player (0 = the first player).
    void debugGive(int playerId, const std::string& ids);
    // Debug: drop an item at a player's feet.
    void debugDrop(int playerId, const std::string& id);
    int debugArtifactsInPlay() const { return (int)artifactsInPlay_.size(); }
    bool debugArtifactInPlay(const std::string& id) const { return artifactsInPlay_.count(gData.findItem(id)) > 0; }
    void debugRevive(int playerId) {
        if (SPlayer* sp = playerById(playerId)) { sp->st.dead = false; sp->st.hp = sp->st.maxHp; sp->dirty = true; }
    }
    // Testing: direct access to a player's state and the monsters.
    PlayerState* debugPlayer(int playerId) { SPlayer* sp = playerById(playerId); return sp ? &sp->st : nullptr; }
    void debugSetLevel(int playerId, int level) {
        SPlayer* sp = playerId ? playerById(playerId) : (players_.empty() ? nullptr : &players_.begin()->second);
        if (sp) { sp->st.level = level; fixupChar(sp->st); sp->dirty = true; }
    }
    const std::vector<Monster>& monsters() const { return monsters_; }
    // Debug: spawn monsters (comma separated ids) in front of the first player, awake or asleep.
    void debugSpawn(const std::string& ids, bool awake);
    // Testing/debug: move a player directly (bypasses movement validation).
    void debugMovePlayer(int playerId, Vec2 p) {
        if (SPlayer* sp = playerById(playerId)) { sp->pos = p; sendTeleport(*sp); }
    }

private:
    std::shared_ptr<ServerNet> net_;
    Rng rng_;
    uint32_t baseSeed_;
    uint32_t tickNo_ = 0;
    uint32_t nextId_ = 1000;
    Map map_;
    std::map<int, SPlayer> players_;     // by player id
    std::map<int, int> peerToPlayer_;
    std::vector<Monster> monsters_;
    std::vector<FloorItem> items_;
    std::vector<Projectile> projectiles_;
    std::vector<Effect> effects_;
    std::vector<Quest> quests_;
    int mainQuestStage_ = 0;
    std::vector<int> flowDoors_, flowNoDoors_;
    float flowT_ = 0;
    float partyWipeT_ = -1;
    int nextPlayerId_ = 1;
    int maxDepth_ = 0;          // deepest level the party has reached (recall target)
    int power_ = 1;             // monster/item strength of the current level
    int campaign_ = -1;         // index into gData.campaigns, -1 = endless dungeon
    std::set<int> campaignDone_;   // campaign levels whose goal is done
    bool campaignWon_ = false;
    std::string pendingIntro_;
    void addCampaignGoal(int level);
    bool buildCampaignLevel(int level);
    float recallT_ = 0;         // seconds until Word of Recall takes the party
    float descentT_ = 0;        // seconds until Deep Descent takes the party down
    int descentTo_ = 0;
    // Collapsed levels: the top levels are skipped in blocks of 5, always keeping at least
    // the 5 levels above the deepest one reached open. Town stairs lead below them.
    int collapsedDepth() const { return campaign_ < 0 && maxDepth_ >= 10 ? ((maxDepth_ - 5) / 5) * 5 : 0; }
    void collapseQuests();
    std::set<int> artifactsInPlay_;             // item defs of artifacts that exist somewhere
    std::map<int, int> rubbleDig_;                 // tile -> digging progress
    std::map<std::string, SavedChar> saved_;   // by lower-case name
    std::string savePath_;
    bool adoptAny_ = false;
    int difficulty_ = 0;
    const DifficultyRules& rules() const { return difficultyRules(difficulty_); }
    Vec2 spawnPoint(int exceptPlayer);

    // networking
    void handleNet();
    void onData(int peer, const std::vector<uint8_t>& data);
    void onHello(int peer, Reader& r);
    void onDisconnect(int peer);
    void send(int playerId, const Writer& w, bool reliable);
    void broadcast(const Writer& w, bool reliable);
    void text(int playerId, uint32_t color, const std::string& s);  // playerId -1 = everyone
    void event(EventKind k, Vec2 pos, uint32_t ent = 0);
    void sendLevel(int playerId);
    void sendTile(int x, int y);
    void sendQuests();
    void sendRoster();
    void sendTeleport(SPlayer& p);
    void sendSnapshots();
    void sendPlayerStates();

    // world
    void newGame();
    void newLevel(int depth, bool down);
    void populate(const std::vector<int>* pool = nullptr);
    void placePlayers();
    void rollCharacter(SPlayer& p, int cls);
    bool randomFloor(Vec2& out, bool awayFromStart, int room = -1);
    int pickMonster(int depth, const std::vector<int>* pool = nullptr);
    int pickItem(int depth);
    int pickArtifact(int depth);
    ItemStack makeItem(int def, int count = 1);
    void spawnMonster(int def, Vec2 pos, bool forceAwake = false);
    void dropItem(const ItemStack& it, Vec2 pos, int droppedBy = -1);
    uint32_t newId() { return nextId_++; }

    // simulation
    void updatePlayers(float dt);
    void updateMonsters(float dt);
    void updateProjectiles(float dt);
    void computeFlow();
    void wakeMonster(Monster& m, bool shout);
    void checkPartyWipe(float dt);

    // combat
    // seeInvisible: the player has detection active (invisible monsters count at any range).
    Monster* findTarget(Vec2 from, float angle, float range, float arcDeg, bool seeInvisible = false);
    Monster* monsterById(uint32_t id);
    SPlayer* playerById(int id);
    void damageMonster(Monster& m, int dmg, Element e, SPlayer* by, bool crit);
    void killMonster(size_t idx, SPlayer* by);
    void damagePlayer(SPlayer& p, int dmg, const std::string& source, Element e = Element::Physical);
    void giveXp(int amount);
    void explode(Vec2 pos, float radius, int dmg, Element e, int spell, bool fromMonster, SPlayer* by, int power = 0);

    // actions
    void onAction(SPlayer& p, Reader& r);
    void doAttack(SPlayer& p);
    void doCast(SPlayer& p, int classSpell);
    // Cast a spell. power = bonus to damage/healing (spell power, device skill...).
    bool castSpell(SPlayer& p, int spell, int power);
    void applyEffect(Monster& m, SpellEffect e, SPlayer* by, int power);
    bool meleeHit(SPlayer& p, Monster& t, int extraHit, int extraDmg);
    void useDevice(SPlayer& p, int slot);
    void fixupChar(PlayerState& s);
    void doUse(SPlayer& p, bool doorsOnly = false);
    void useItem(SPlayer& p, int slot);
    void dropFromInv(SPlayer& p, int slot, int count);
    void giveItem(SPlayer& p, int slot, int toPlayer);
    void unequip(SPlayer& p, int slot);
    bool addToInventory(SPlayer& p, const ItemStack& it);
    bool canCarry(const SPlayer& p, const ItemStack& it) const;
    bool nearShop(const SPlayer& p, int store) const;
    void shopBuy(SPlayer& p, int store, int ware);
    void shopSell(SPlayer& p, int store, int slot);
    void pickupItem(SPlayer& p, size_t idx, bool quiet = false);

    // quests
    void setupMainQuest();
    void makeSideQuest();
    void questKill(int monsterDef, bool boss);
    void questPickup(int itemDef);
    void completeQuest(Quest& q);
};
