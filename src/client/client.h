// Dum - game client: talks to the server, predicts own movement, draws the world and UI.
#pragma once

#include <SDL.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "client/keys.h"
#include "client/renderer.h"
#include "client/sound.h"
#include "game/map.h"
#include "game/state.h"
#include "net/net.h"

struct CEnt {
    EntSnap s;
    Vec2 rpos;          // smoothed render position
    float anim = 0;     // walk animation phase
};

struct LogLine {
    std::string text;
    uint32_t color;
    double time;
};

enum class Ui { None, Inventory, Character, Quests, Map, Help, Chat, Menu, Shop, Spells };

class Client {
public:
    Client(std::shared_ptr<ClientNet> net, const std::string& name, int cls, Assets* assets, Sound* sound, bool autoPickup,
           const KeyBinds* keys);
    void event(const SDL_Event& e);
    void update(float dt);
    void draw(Renderer& r);
    bool wantsMouseGrab() const { return ui_ == Ui::None && inGame(); }
    bool inGame() const { return haveLevel_ && myId_ != 0; }
    void debugUi(const std::string& n) {
        ui_ = n == "inventory" ? Ui::Inventory : n == "map" ? Ui::Map : n == "character" ? Ui::Character : n == "quests" ? Ui::Quests : n == "help" ? Ui::Help : n == "shop" ? Ui::Shop : n == "spells" ? Ui::Spells : Ui::None;
    }
    // Developer/testing: start an automatic mode ("explore", "travel", "fight").
    void debugAuto(const std::string& m) {
        if (m == "explore") startAuto(Auto::Explore);
        else if (m == "travel") startAuto(Auto::Travel);
        else if (m == "fight") autoFight_ = true;
    }
    int debugSeenTiles() const { int n = 0; for (auto f : map_.flags) n += (f & F_SEEN) ? 1 : 0; return n; }
    int debugDepth() const { return map_.depth; }
    bool debugAutoActive() const { return auto_ != Auto::None; }
    void debugTurn(float a) { angle_ = wrapAngle(angle_ + a); }
    // Single player pauses on every screen except chat.
    bool paused() const { return ui_ != Ui::None && ui_ != Ui::Chat; }

    bool leave = false;      // back to main menu
    bool quitApp = false;
    std::string error;       // set when disconnected / rejected
    float mouseSens = 0.0035f;
    bool autoPickup = true;
    // accessibility options (from the settings)
    float turnSpeed = 1.0f;     // keyboard turning speed multiplier
    bool autoFace = true;       // auto-fight turns toward enemies
    bool strongAim = false;     // wider auto-aim cone (sent to the server)
    bool quickCast = true;      // number keys use their slot at once (not just select it)

private:
    enum class Auto { None, Explore, Travel };
    Auto auto_ = Auto::None;
    bool autoWalk_ = false, autoFight_ = false;
    std::vector<std::pair<int, int>> path_;
    float repathT_ = 0, stuckT_ = 0, faceT_ = 0;
    float hpAtStart_ = 0;
    std::vector<uint32_t> seenMonsters_;      // monsters already noticed (auto-explore stops for new ones)
    std::vector<uint32_t> visibleMonsters_;   // drawn on screen last frame
    std::vector<uint32_t> visitedItems_;      // items auto-explore already walked to
    uint32_t fightTarget_ = 0;
    void startAuto(Auto a);
    void stopAuto(const std::string& why);
    Vec2 autopilot(float dt);
    bool findPath(bool explore);
    void updateFacing(float dt);
    void autoFightStep();
    uint32_t nearestAwakeVisible(float maxDist) const;
    const KeyBinds* keys_;
    std::shared_ptr<ClientNet> net_;
    Assets* assets_;
    Sound* sound_;
    std::string name_;
    int cls_;
    bool helloSent_ = false;
    uint16_t myId_ = 0;
    int difficulty_ = 1;
    uint32_t lastStateTick_ = 0;
    Map map_;
    bool haveLevel_ = false;
    PlayerState me_;
    Vec2 pos_;
    float angle_ = 0;
    std::unordered_map<uint32_t, CEnt> ents_;
    std::vector<Quest> quests_;
    std::vector<RosterEntry> roster_;
    std::deque<LogLine> log_;
    double time_ = 0;
    float sendT_ = 0;
    float turnAccum_ = 0;
    float bobPhase_ = 0, bobAmt_ = 0;
    float attackAnim_ = 0, castAnim_ = 0;
    int castSpellAnim_ = -1;
    float localAttackCd_ = 0, localCastCd_ = 0;
    float hurtFlash_ = 0, healFlash_ = 0, levelFlash_ = 0;
    float stepT_ = 0, bumpCd_ = 0, digShake_ = 0;
    bool rubbleHinted_ = false;
    int shopIdx_ = 0, shopSel_ = 0, shopSide_ = 0;   // shop window: side 0 = buy, 1 = sell
    int shopX_ = 0, shopY_ = 0;
    int selSpell_ = 0;
    Ui ui_ = Ui::None;
    int invSel_ = 0;
    int menuSel_ = 0;
    std::string chat_;
    bool attackHeld_ = false, castHeld_ = false;
    uint32_t targetId_ = 0;
    bool targetIsSpell_ = false;
    bool showLegend_ = true;
    int invX_ = 0, invY_ = 0, invW_ = 0;   // inventory panel position (for the mouse)

    void handleMessage(const std::vector<uint8_t>& data);
    void send(const Writer& w, bool reliable) { net_->send(w.buf, reliable); }
    void action(ActionId a, int x = 0, int y = 0);
    void addLog(const std::string& s, uint32_t c);
    void move(float dt);
    void tryAttack();
    void tryCast();
    void computeTarget();
    uint32_t autoTarget(float range, float arcDeg) const;
    int nearestPlayerId(float maxDist) const;
    std::vector<int> knownSpells() const;
    // Quick slots (keys 1-9): a class spell/ability, or a wand/rod.
    const SpellDef* quickSpell(int slot) const;      // what the slot casts (nullptr = empty)
    int quickInvSlot(int value) const;               // inventory slot of a device (-1 = none)
    std::string quickName(int value) const;
    bool useQuick(int slot, bool quiet);
    void cycleQuick(int dir);
    // Spell book screen
    int bookSel_ = 0, bookScroll_ = 0, bookX_ = 0, bookY_ = 0;
    int quickBarRows_ = 1;
    std::vector<int> bookEntries() const;            // quick-slot values: class spells, then devices
    void bookKey(SDL_Keycode k);
    int bookRowAt(int mx, int my) const;
    void drawSpellBook(Renderer& r);
    std::string spellInfo(const SpellDef& s, int power) const;
    void openUi(Ui u);
    void invAction(SDL_Keycode k);
    void quickUse(ItemKind kind);
    std::string usePrompt() const;
    void hintRubble();
    bool tryOpenShop(float angle);
    void shopKey(SDL_Keycode k);
    int shopRowAt(int mx, int my, int& side) const;
    void drawShop(Renderer& r);
    int invRowAt(int mx, int my) const;

    void drawHud(Renderer& r);
    void drawMinimap(Renderer& r, int x, int y, int w, int h, int scale, bool full);
    void drawInventory(Renderer& r);
    void drawCharacter(Renderer& r);
    void drawQuests(Renderer& r);
    void drawHelp(Renderer& r);
    void drawMenu(Renderer& r);
    void drawViewModel(Renderer& r);
};
