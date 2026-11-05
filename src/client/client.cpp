// Dum - game client implementation.
#include "client/client.h"

#include <cstdio>
#include <cstring>

#include "game/rules.h"

namespace {
constexpr uint32_t UI_BG = 0xFF181210, UI_EDGE = 0xFF8A6A40, UI_TEXT = 0xFFE8E0D0, UI_DIM = 0xFF908070,
                   UI_HI = 0xFFFFE080, UI_RED = 0xFFE04040, UI_BLUE = 0xFF4070E0, UI_GREEN = 0xFF60D060;
constexpr float WALK_SPEED = 3.4f, RUN_SPEED = 5.2f;

std::string fmtTime(int secs) {
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", secs / 60, secs % 60);
    return b;
}

void bar(Renderer& r, int x, int y, int w, int h, float frac, uint32_t col, const std::string& label) {
    r.fill(x - 1, y - 1, w + 2, h + 2, 0xFF000000);
    r.fill(x, y, w, h, colMul(col, 0.25f));
    r.fill(x, y, (int)(w * clampf(frac, 0, 1)), h, col);
    r.fill(x, y, (int)(w * clampf(frac, 0, 1)), 1, colMul(col, 1.4f));
    if (!label.empty()) r.text(x + 3, y + (h - 7) / 2, label, 0xFFFFFFFF);
}

void panel(Renderer& r, int x, int y, int w, int h, const std::string& title) {
    r.fill(x, y, w, h, UI_BG, 235);
    r.frame(x, y, w, h, UI_EDGE);
    r.frame(x + 2, y + 2, w - 4, h - 4, colMul(UI_EDGE, 0.5f));
    if (!title.empty()) {
        r.fill(x + 3, y + 3, w - 6, 12, 0xFF3A2A1A);
        r.text(x + (w - r.textW(title)) / 2, y + 5, title, UI_HI);
    }
}
}  // namespace

Client::Client(std::shared_ptr<ClientNet> net, const std::string& name, int cls, Assets* assets, Sound* sound, bool autoPick,
               const KeyBinds* keys)
    : autoPickup(autoPick), keys_(keys), net_(net), assets_(assets), sound_(sound), name_(name), cls_(cls) {
    addLog("Connecting...", UI_DIM);
}

void Client::addLog(const std::string& s, uint32_t c) {
    log_.push_back({s, c, time_});
    while (log_.size() > 60) log_.pop_front();
    std::printf("%s\n", s.c_str());
}

void Client::action(ActionId a, int x, int y) {
    Writer w;
    w.u8(C_ACTION);
    w.u8(a);
    w.i32(x);
    w.i32(y);
    send(w, true);
}

std::vector<int> Client::knownSpells() const {
    std::vector<int> v;
    if (!inGame()) return v;
    for (size_t i = 0; i < me_.c().spells.size(); i++)
        if (me_.knowsSpell((int)i)) v.push_back((int)i);
    return v;
}

// ============================================================== network ====

void Client::handleMessage(const std::vector<uint8_t>& data) {
    Reader r(data);
    uint8_t type = r.u8();
    switch (type) {
        case S_WELCOME:
            myId_ = r.u16();
            difficulty_ = r.u8();
            action(A_SET_AIM, strongAim ? 1 : 0);
            log_.clear();
            if (!hostDataNote.empty()) addLog(hostDataNote, UI_HI);
            action(A_SET_AUTOPICKUP, autoPickup ? 1 : 0);
            break;
        case S_REJECT:
            error = r.str();
            leave = true;
            break;
        case S_DATA: {
            // Our data files differ from the host's: play with theirs (for this game only).
            std::vector<GameData::SharedFile> files;
            int n = r.u8();
            for (int i = 0; i < n && r.ok; i++) {
                GameData::SharedFile f;
                f.name = r.str();
                uint32_t len = r.u32();
                if (!r.need(len)) break;
                f.text.assign((const char*)r.p + r.pos, len);
                r.pos += len;
                files.push_back(std::move(f));
            }
            std::string differ;
            int m = r.u8();
            for (int i = 0; i < m && r.ok; i++) differ += (differ.empty() ? "" : ", ") + r.str();
            std::string err = "no way to switch data files";
            if (!r.ok) err = "bad data from the host";
            else if (hostDataTries_++ == 0 && onHostData && onHostData(files, err)) {
                hostDataNote = m == 1 ? "Your " + differ + " differs from the host's: using theirs for this game."
                                      : "Your game data differs from the host's: using theirs for this game.";
                std::printf("Data files that differ from the host's: %s\n", differ.c_str());
                sendHello();
                break;
            }
            error = "Your game data differs from the host's (" + differ + ") and could not be switched: " + err;
            leave = true;
            break;
        }
        case S_LEVEL: {
            if (!map_.deserialize(r)) { error = "bad level data"; leave = true; return; }
            haveLevel_ = true;
            ents_.clear();
            if (map_.depth == 0) {
                for (auto& f : map_.flags) f |= F_SEEN;   // the town is known
                addLog("--- The town of Dum ---", UI_HI);
                addLog("Shops line the square. Walk into a shop door (or press " + keys_->label(K_USE) + ") to trade.", UI_DIM);
            } else {
                addLog("--- Dungeon level " + std::to_string(map_.depth) + " ---", UI_HI);
            }
            if (ui_ == Ui::Shop) ui_ = Ui::None;
            stopAuto("");
            autoWalk_ = false;
            visitedItems_.clear();
            seenMonsters_.clear();
            break;
        }
        case S_TILE: {
            int x = r.u16(), y = r.u16();
            uint8_t t = r.u8();
            if (r.ok) map_.set(x, y, t);
            break;
        }
        case S_SNAPSHOT: {
            r.u32();
            int n = r.u16();
            std::unordered_map<uint32_t, CEnt> next;
            for (int i = 0; i < n && r.ok; i++) {
                EntSnap s;
                s.read(r);
                auto it = ents_.find(s.id);
                CEnt e;
                if (it != ents_.end()) {
                    e = it->second;
                    if ((s.pos - e.s.pos).len() > 0.01f) { e.anim += (s.pos - e.s.pos).len() * 3.0f; e.moved = time_; }
                    if (dist(e.rpos, s.pos) > 3.0f) e.rpos = s.pos;
                } else {
                    e.rpos = s.pos;
                }
                e.s = s;
                next[s.id] = e;
            }
            ents_.swap(next);
            break;
        }
        case S_PSTATE: {
            uint32_t tick = r.u32();
            if (tick < lastStateTick_) break;
            lastStateTick_ = tick;
            int oldLevel = me_.level;
            bool wasDead = me_.dead;
            me_.read(r);
            if (me_.level > oldLevel && oldLevel > 0 && wasDead == me_.dead) levelFlash_ = 1.0f;
            if (selSpell_ < 0 || selSpell_ >= NUM_QUICK || me_.quick[selSpell_] < 0)
                for (int i = 0; i < NUM_QUICK; i++) if (me_.quick[i] >= 0) { selSpell_ = i; break; }
            break;
        }
        case S_TEXT: {
            uint32_t c = r.u32();
            addLog(r.str(), c);
            break;
        }
        case S_EVENT: {
            uint8_t k = r.u8();
            Vec2 p{r.f32(), r.f32()};
            uint32_t ent = r.u32();
            Vec2 to = p - pos_;
            float d = to.len();
            float vol = clampf(1.0f - d / 20.0f, 0, 1);
            float pan = d > 0.1f ? std::sin(wrapAngle(std::atan2(to.y, to.x) - angle_)) : 0;
            bool mine = ent == myId_;
            switch (k) {
                case EV_HIT: sound_->play(SND_HIT, vol, pan); break;
                case EV_CRIT: sound_->play(SND_CRIT, vol, pan); break;
                case EV_MISS: sound_->play(SND_MISS, vol * 0.7f, pan); break;
                case EV_SWING: if (!mine) sound_->play(SND_SWING, vol * 0.6f, pan); break;
                case EV_SHOOT: if (!mine) sound_->play(SND_SHOOT, vol, pan); break;
                case EV_CAST: if (!mine) sound_->play(SND_CAST, vol * 0.7f, pan); break;
                case EV_EXPLODE: sound_->play(SND_EXPLODE, vol, pan); break;
                case EV_HURT:
                    if (mine) { hurtFlash_ = 0.6f; sound_->play(SND_HURT, 1.0f, 0); }
                    break;
                case EV_DOOR: sound_->play(SND_DOOR, vol, pan); break;
                case EV_PICKUP: if (mine) sound_->play(SND_PICKUP, 0.8f, 0); break;
                case EV_MONSTER_DIE: {
                    sound_->play(SND_DIE, vol, pan);
                    auto it = ents_.find(ent);
                    if (animate && it != ents_.end() && it->second.s.type == ET_MONSTER && !gData.monsters[it->second.s.def].invisible)
                        dying_.push_back({it->second.rpos, it->second.s.def, time_});
                    break;
                }
                case EV_PLAYER_DIE: sound_->play(SND_DIE, vol, pan); break;
                case EV_LEVELUP: if (mine) sound_->play(SND_LEVELUP, 1.0f, 0); break;
                case EV_HEAL: if (mine) { healFlash_ = 0.5f; sound_->play(SND_HEAL, 0.8f, 0); } break;
                case EV_GROWL:
                    // A monster waking up: you may hear it before you see it.
                    if (d < 14) {
                        sound_->play(SND_GROWL, vol * 0.9f, pan);
                        if (d > 3) addLog(std::string("You hear something stir to the ") + (std::fabs(pan) < 0.4f ? (std::cos(wrapAngle(std::atan2(to.y, to.x) - angle_)) > 0 ? "front" : "rear") : (pan > 0 ? "right" : "left")) + ".", UI_DIM);
                    }
                    break;
                case EV_STAIRS: sound_->play(SND_STAIRS, 1.0f, 0); break;
                case EV_DIG: sound_->play(SND_HIT, vol * 0.7f, pan); if (mine) digShake_ = 0.15f; break;
            }
            break;
        }
        case S_QUESTS: {
            int n = r.u8();
            quests_.clear();
            for (int i = 0; i < n && r.ok; i++) {
                Quest q;
                q.read(r);
                quests_.push_back(q);
            }
            break;
        }
        case S_ROSTER: {
            int n = r.u8();
            roster_.clear();
            for (int i = 0; i < n && r.ok; i++) {
                RosterEntry e;
                e.id = r.u16();
                e.name = r.str();
                e.cls = r.u8();
                e.level = r.u8();
                roster_.push_back(e);
            }
            break;
        }
        case S_TELEPORT:
            pos_.x = r.f32();
            pos_.y = r.f32();
            angle_ = r.f32();
            break;
        case S_REVEAL: {
            int cx = r.u16(), cy = r.u16(), rad = r.u8();
            for (int y = cy - rad; y <= cy + rad; y++)
                for (int x = cx - rad; x <= cx + rad; x++) {
                    if (!map_.inside(x, y) || (x - cx) * (x - cx) + (y - cy) * (y - cy) > rad * rad) continue;
                    uint8_t t = map_.at(x, y);
                    if (t != T_ROCK) map_.flags[y * map_.w + x] |= F_SEEN;
                }
            break;
        }
        default: break;
    }
}

// ================================================================ input ====

void Client::openUi(Ui u) {
    ui_ = (ui_ == u) ? Ui::None : u;
    invPick_ = -1;
    if (ui_ == Ui::Chat) { chat_.clear(); SDL_StartTextInput(); }
    else SDL_StopTextInput();
    if (ui_ == Ui::Menu) menuSel_ = 0;
}

void Client::quickUse(ItemKind kind) {
    if (kind == ItemKind::Food) {
        // Eat what fills you best without going to waste: the biggest food that still fits
        // in your stomach, or else the smallest one (an apple before a ration when nearly full).
        float room = FOOD_MAX - me_.food;
        int best = -1, bestFit = -1;
        for (int i = 0; i < INV_SIZE; i++) {
            const ItemStack& s = me_.inv[i];
            if (s.empty() || s.d().kind != ItemKind::Food) continue;
            int f = s.d().food;
            if (best < 0 || f < me_.inv[best].d().food) best = i;
            if (f <= room && (bestFit < 0 || f > me_.inv[bestFit].d().food)) bestFit = i;
        }
        if (bestFit >= 0) best = bestFit;
        if (best >= 0) action(A_INV_USE, best);
        else addLog("You have nothing to eat!", UI_DIM);
        return;
    }
    for (int i = 0; i < INV_SIZE; i++) {
        const ItemStack& s = me_.inv[i];
        if (s.empty() || s.d().kind != kind) continue;
        if (kind == ItemKind::Potion && s.d().heal == 0) continue;
        action(A_INV_USE, i);
        return;
    }
    addLog("You have no healing potions!", UI_DIM);
}

int Client::nearestPlayerId(float maxDist) const {
    int best = 0;
    float bd = maxDist;
    for (auto& [id, e] : ents_) {
        if (e.s.type != ET_PLAYER || (e.s.flags & EF_DEAD)) continue;
        float d = dist(e.rpos, pos_);
        if (d < bd) { bd = d; best = (int)id; }
    }
    return best;
}

bool Client::invFits(int packSlot, int equipSlot) const {
    const ItemStack& s = me_.inv[packSlot];
    return !s.empty() && equipSlotFor(s.d()) == equipSlot;
}

// Move the cursor; while replacing equipment it only stops on pack items that fit.
void Client::invStep(int dir) {
    if (invPick_ < 0) {
        int rows = EQ_COUNT + INV_SIZE;
        invSel_ = (invSel_ + rows + dir) % rows;
        return;
    }
    int cur = invSel_ - EQ_COUNT;
    for (int n = 1; n <= INV_SIZE; n++) {
        int i = ((cur + dir * n) % INV_SIZE + INV_SIZE) % INV_SIZE;
        if (invFits(i, invPick_)) { invSel_ = EQ_COUNT + i; return; }
    }
}

// What Enter does with this pack item, for the hint line ("wear (replaces Leather Armor)").
std::string Client::invUseLabel(const ItemStack& s) const {
    if (s.empty()) return "";
    const ItemDef& d = s.d();
    int eq = equipSlotFor(d);
    if (eq >= 0) {
        const ItemStack& cur = me_.equip[eq];
        if (d.id == "torch" && !cur.empty() && cur.def == s.def) return "add to your burning torch (combine)";
        std::string verb = d.kind == ItemKind::Weapon ? "wield" : d.kind == ItemKind::Light ? "hold" : d.kind == ItemKind::Ring ? "put on" : "wear";
        return cur.empty() ? verb : verb + " (replaces " + cur.d().name + ")";
    }
    switch (d.kind) {
        case ItemKind::Food: return "eat";
        case ItemKind::Potion: return "quaff";
        case ItemKind::Scroll: return "read";
        case ItemKind::Fuel: return "refuel your lantern";
        case ItemKind::Wand: return "aim";
        case ItemKind::Rod: return "zap";
        case ItemKind::Quest: return "examine";
        default: return "use";
    }
}

void Client::invAction(SDL_Keycode k) {
    int rows = EQ_COUNT + INV_SIZE;
    bool enter = k == SDLK_RETURN || k == SDLK_e || k == SDLK_SPACE || k == SDLK_KP_ENTER;
    if (invPick_ >= 0) {
        // Choosing a replacement for equipment slot invPick_.
        if (k == SDLK_UP || k == SDLK_w) invStep(-1);
        else if (k == SDLK_DOWN || k == SDLK_s || k == SDLK_TAB) invStep(1);
        else if (enter) {
            int slot = invSel_ - EQ_COUNT;
            if (slot >= 0 && invFits(slot, invPick_)) action(A_INV_USE, slot);
            invSel_ = invPick_;
            invPick_ = -1;
        } else if (k == SDLK_BACKSPACE || k == SDLK_LEFT || k == SDLK_ESCAPE) {
            invSel_ = invPick_;
            invPick_ = -1;
        } else if (k == SDLK_u || k == SDLK_t) {
            action(A_UNEQUIP, invPick_);
            invSel_ = invPick_;
            invPick_ = -1;
        }
        return;
    }
    if (k == SDLK_UP || k == SDLK_w) invStep(-1);
    else if (k == SDLK_DOWN || k == SDLK_s) invStep(1);
    else if (k == SDLK_PAGEUP) invSel_ = std::max(0, invSel_ - 5);
    else if (k == SDLK_PAGEDOWN) invSel_ = std::min(rows - 1, invSel_ + 5);
    bool equipRow = invSel_ < EQ_COUNT;
    int slot = equipRow ? invSel_ : invSel_ - EQ_COUNT;
    if (enter) {
        if (equipRow) {
            // Replace: jump to the first pack item that fits this slot. Nothing fits -> take it off.
            int first = -1;
            for (int i = 0; i < INV_SIZE && first < 0; i++)
                if (invFits(i, slot)) first = i;
            if (first >= 0) { invPick_ = slot; invSel_ = EQ_COUNT + first; }
            else if (!me_.equip[slot].empty()) action(A_UNEQUIP, slot);
            else addLog(std::string("You have nothing in your pack to use as ") + kEquipNames[slot] + ".", UI_DIM);
        } else action(A_INV_USE, slot);
    } else if ((k == SDLK_u || k == SDLK_t) && equipRow) {
        action(A_UNEQUIP, slot);
    } else if (k == SDLK_d && !equipRow) {
        action(A_INV_DROP, slot, (SDL_GetModState() & KMOD_SHIFT) ? 1 : 0);
    } else if (k == SDLK_g && !equipRow) {
        int pid = nearestPlayerId(3.0f);
        if (!pid) addLog("Nobody is close enough to give anything to.", UI_DIM);
        else action(A_INV_GIVE, slot, pid);
    }
}

void Client::event(const SDL_Event& e) {
    if (e.type == SDL_TEXTINPUT && ui_ == Ui::Chat) {
        if (chat_.size() < 100) chat_ += e.text.text;
        return;
    }
    if (e.type == SDL_MOUSEMOTION && ui_ == Ui::None) {
        turnAccum_ += e.motion.xrel * mouseSens;
        return;
    }
    if (ui_ == Ui::Inventory && e.type == SDL_MOUSEBUTTONDOWN) {
        int row = invRowAt(e.button.x, e.button.y);
        if (row >= 0) {
            // While choosing a replacement, clicking anything that doesn't fit backs out.
            if (invPick_ >= 0 && (row < EQ_COUNT || !invFits(row - EQ_COUNT, invPick_))) invPick_ = -1;
            invSel_ = row;
            if (e.button.clicks >= 2 || e.button.button == SDL_BUTTON_RIGHT) invAction(SDLK_RETURN);
        }
        return;
    }
    if (ui_ == Ui::Shop && e.type == SDL_MOUSEBUTTONDOWN) {
        int side, row = shopRowAt(e.button.x, e.button.y, side);
        if (row >= 0) {
            shopSide_ = side;
            shopSel_ = row;
            if (e.button.clicks >= 2 || e.button.button == SDL_BUTTON_RIGHT) shopKey(SDLK_RETURN);
        }
        return;
    }
    if (ui_ == Ui::Spells && e.type == SDL_MOUSEBUTTONDOWN) {
        int row = bookRowAt(e.button.x, e.button.y);
        if (row >= 0) {
            bookSel_ = row;
            if (e.button.clicks >= 2 || e.button.button == SDL_BUTTON_RIGHT) bookKey(SDLK_RETURN);
        }
        return;
    }
    if (ui_ == Ui::Spells && e.type == SDL_MOUSEWHEEL) {
        bookKey(e.wheel.y > 0 ? SDLK_UP : SDLK_DOWN);
        return;
    }
    if (ui_ == Ui::Inventory && e.type == SDL_MOUSEWHEEL) {
        invAction(e.wheel.y > 0 ? SDLK_UP : SDLK_DOWN);
        return;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && ui_ == Ui::None) {
        if (e.button.button == SDL_BUTTON_LEFT) attackHeld_ = true;
        if (e.button.button == SDL_BUTTON_RIGHT) castHeld_ = true;
        return;
    }
    if (e.type == SDL_MOUSEBUTTONUP) {
        if (e.button.button == SDL_BUTTON_LEFT) attackHeld_ = false;
        if (e.button.button == SDL_BUTTON_RIGHT) castHeld_ = false;
        return;
    }
    if (e.type == SDL_MOUSEWHEEL && ui_ == Ui::None) {
        cycleQuick(e.wheel.y < 0 ? 1 : -1);
        return;
    }
    if (e.type == SDL_KEYUP) {
        if (keys_->is(K_ATTACK, e.key.keysym.scancode)) attackHeld_ = false;
        if (keys_->is(K_CAST, e.key.keysym.scancode)) castHeld_ = false;
        return;
    }
    if (e.type != SDL_KEYDOWN) return;
    SDL_Keycode k = e.key.keysym.sym;
    SDL_Scancode sc = e.key.keysym.scancode;
    const KeyBinds& kb = *keys_;

    if (ui_ == Ui::Chat) {
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            if (!chat_.empty()) {
                Writer w;
                w.u8(C_CHAT);
                w.str(chat_);
                send(w, true);
            }
            openUi(Ui::Chat);
        } else if (k == SDLK_ESCAPE) {
            openUi(Ui::Chat);
        } else if (k == SDLK_BACKSPACE && !chat_.empty()) {
            chat_.pop_back();
        }
        return;
    }
    if (ui_ == Ui::Menu) {
        if (k == SDLK_UP || k == SDLK_w) menuSel_ = (menuSel_ + 2) % 3;
        else if (k == SDLK_DOWN || k == SDLK_s) menuSel_ = (menuSel_ + 1) % 3;
        else if (k == SDLK_ESCAPE) ui_ = Ui::None;
        else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_KP_ENTER) {
            if (menuSel_ == 0) ui_ = Ui::None;
            else if (menuSel_ == 1) leave = true;
            else quitApp = true;
        }
        return;
    }
    if (k == SDLK_ESCAPE) {
        if (ui_ == Ui::Inventory && invPick_ >= 0) invAction(k);   // back out of choosing a replacement
        else if (ui_ != Ui::None) ui_ = Ui::None;
        else openUi(Ui::Menu);
        return;
    }
    if (ui_ == Ui::Inventory && !kb.is(K_INVENTORY, sc) && !kb.is(K_MAP, sc)) { invAction(k); return; }
    if (ui_ == Ui::Shop) { shopKey(k); return; }
    if (ui_ == Ui::Spells && !kb.is(K_SPELLBOOK, sc)) { bookKey(k); return; }
    if (e.key.repeat) return;

    // Screens (also close themselves when pressed again).
    if (kb.is(K_INVENTORY, sc)) openUi(Ui::Inventory);
    else if (kb.is(K_CHARACTER, sc)) openUi(Ui::Character);
    else if (kb.is(K_QUESTS, sc)) openUi(Ui::Quests);
    else if (kb.is(K_MAP, sc)) openUi(Ui::Map);
    else if (kb.is(K_HELP, sc)) openUi(Ui::Help);
    else if (kb.is(K_SPELLBOOK, sc)) { openUi(Ui::Spells); bookScroll_ = 0; }
    else if (kb.is(K_LEGEND, sc)) showLegend_ = !showLegend_;
    else if (kb.is(K_CHAT, sc) && ui_ == Ui::None) { openUi(Ui::Chat); return; }
    if (ui_ != Ui::None) return;

    if (kb.is(K_USE, sc)) { if (!tryOpenShop(angle_)) action(A_USE); }
    else if (kb.is(K_AUTOFIGHT, sc)) {
        autoFight_ = !autoFight_;
        addLog(autoFight_ ? "Auto-fight ON: you attack nearby awake monsters by yourself." : "Auto-fight off.", UI_HI);
    } else if (kb.is(K_EXPLORE, sc)) startAuto(Auto::Explore);
    else if (kb.is(K_TRAVEL, sc)) startAuto(Auto::Travel);
    else if (kb.is(K_AUTOWALK, sc)) { autoWalk_ = !autoWalk_; stopAuto(""); addLog(autoWalk_ ? "Auto-walk on." : "Auto-walk off.", UI_DIM); }
    else if (kb.is(K_FACE, sc)) faceT_ = 1.5f;
    else if (kb.is(K_ATTACK, sc)) attackHeld_ = true;
    else if (kb.is(K_CAST, sc)) castHeld_ = true;
    else if (kb.is(K_POTION, sc)) quickUse(ItemKind::Potion);
    else if (kb.is(K_EAT, sc)) quickUse(ItemKind::Food);
    else if (kb.is(K_AUTOPICKUP, sc)) {
        autoPickup = !autoPickup;
        action(A_SET_AUTOPICKUP, autoPickup ? 1 : 0);
        addLog(autoPickup ? "Auto-pickup is on." : "Auto-pickup is off (use " + kb.label(K_USE) + " to pick things up).", UI_DIM);
    } else if (k >= SDLK_1 && k <= SDLK_9) {
        int i = k - SDLK_1;
        if (me_.quick[i] < 0) addLog("Key " + std::to_string(i + 1) + " is empty. Open the spell book (" + kb.label(K_SPELLBOOK) + ") to put something on it.", UI_DIM);
        else {
            selSpell_ = i;
            if (quickCast) useQuick(i, false);
        }
    }
}

// =============================================================== update ====

uint32_t Client::autoTarget(float range, float arcDeg) const {
    // Same rules as the server's auto-aim, for showing what you'd hit.
    uint32_t best = 0;
    float bestScore = 1e9f, half = arcDeg * 0.5f * PI / 180.0f;
    for (auto& [id, e] : ents_) {
        if (e.s.type != ET_MONSTER) continue;
        Vec2 to = e.s.pos - pos_;
        float d = to.len();
        float r = range + MONSTER_RADIUS * gData.monsters[e.s.def].size;
        if (d > r) continue;
        if (gData.monsters[e.s.def].invisible && me_.detectT <= 0 && d > 3.0f) continue;
        float da = std::fabs(wrapAngle(std::atan2(to.y, to.x) - angle_));
        float effHalf = half + (d < 1.2f ? 0.35f : 0.0f);
        if (da > effHalf || !map_.los(pos_, e.s.pos)) continue;
        float score = da / std::max(0.01f, effHalf) + d / r * 0.7f;
        if (score < bestScore) { bestScore = score; best = id; }
    }
    return best;
}

void Client::computeTarget() {
    const ItemDef* w = me_.weapon();
    float aimK = strongAim ? 1.6f : 1.0f;
    targetId_ = autoTarget((w ? w->range : 1.3f) + (strongAim ? 0.3f : 0.0f), std::min(170.0f, (w ? w->arc : 70.0f) * aimK));
    targetIsSpell_ = false;
    if (!targetId_) {
        if (const SpellDef* s = quickSpell(selSpell_)) {
            if (s->type == SpellType::Bolt || s->type == SpellType::Ball) {
                targetId_ = autoTarget(s->range, std::min(170.0f, s->arc * aimK));
                targetIsSpell_ = targetId_ != 0;
            }
        }
    }
}

void Client::tryAttack() {
    if (localAttackCd_ > 0 || me_.dead) return;
    const ItemDef* w = me_.weapon();
    localAttackCd_ = (w ? w->cooldown : 0.6f) * me_.cdMult();
    attackAnim_ = 1.0f;
    sound_->play(w && w->ranged ? SND_SHOOT : SND_SWING, 0.8f, 0);
    action(A_ATTACK);
}

void Client::tryCast() {
    if (localCastCd_ > 0 || me_.dead) return;
    if (!useQuick(selSpell_, false)) castHeld_ = false;
}

void Client::move(float dt) {
    angle_ = wrapAngle(angle_ + turnAccum_);
    turnAccum_ = 0;
    if (ui_ != Ui::None && ui_ != Ui::Map) return;
    if (me_.dead) { stopAuto(""); return; }
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    const KeyBinds& kb = *keys_;
    float turn = 0;
    if (kb.held(K_TURN_L, ks)) turn -= 1;
    if (kb.held(K_TURN_R, ks)) turn += 1;
    if (turn != 0) faceT_ = 0;
    angle_ = wrapAngle(angle_ + turn * 2.6f * turnSpeed * dt);
    Vec2 fwd = dirFromAngle(angle_), right(-fwd.y, fwd.x);
    Vec2 mv;
    if (kb.held(K_FORWARD, ks)) mv += fwd;
    if (kb.held(K_BACK, ks)) mv -= fwd;
    if (kb.held(K_STRAFE_L, ks)) mv -= right;
    if (kb.held(K_STRAFE_R, ks)) mv += right;
    bool manual = mv.len2() > 0;
    if (manual && (auto_ != Auto::None)) stopAuto("");           // any movement key takes over
    if (kb.held(K_BACK, ks)) autoWalk_ = false;
    if (!manual && autoWalk_) mv = fwd;
    if (!manual && !autoWalk_ && auto_ != Auto::None) mv = autopilot(dt);
    updateFacing(dt);

    bool moving = mv.len2() > 0;
    if (moving) {
        bool run = kb.held(K_RUN, ks);
        float speed = (run ? RUN_SPEED : WALK_SPEED) * me_.moveMult();
        Vec2 step = mv.norm() * (speed * dt);
        Vec2 before = pos_;
        pos_ = assistedMove(map_, pos_, step, PLAYER_RADIUS);
        float moved = dist(before, pos_);
        // Bump to open: walking into a closed door opens it.
        bumpCd_ = std::max(0.0f, bumpCd_ - dt);
        int dx, dy;
        if (moved < step.len() * 0.3f && bumpCd_ <= 0 && useTileInFront(map_, pos_, std::atan2(mv.y, mv.x), dx, dy) &&
            (map_.at(dx, dy) == T_DOOR || map_.at(dx, dy) == T_DOOR_LOCKED || map_.at(dx, dy) == T_RUBBLE || map_.at(dx, dy) == T_SHOP)) {
            uint8_t bt = map_.at(dx, dy);
            bumpCd_ = bt == T_RUBBLE ? 0.5f : bt == T_DOOR_LOCKED ? 1.05f : 0.6f;
            if (map_.at(dx, dy) == T_SHOP) { stopAuto(""); autoWalk_ = false; tryOpenShop(std::atan2(mv.y, mv.x)); bumpCd_ = 1.0f; return; }
            // Tell the server which way we bumped (may differ from where we look when strafing).
            action(A_USE, 1, (int)(std::atan2(mv.y, mv.x) * 1000.0f));
        }
        // Stuck detection for the automatic modes (digging/opening doors doesn't count).
        if (bumpCd_ > 0) stuckT_ = 0;
        else if (!manual) {
            stuckT_ = moved < step.len() * 0.1f ? stuckT_ + dt : 0;
            if (stuckT_ > 1.5f) {
                stuckT_ = 0;
                if (autoWalk_) { autoWalk_ = false; addLog("Auto-walk stopped: something is in the way.", UI_DIM); }
                else stopAuto("Something blocks the way.");
            }
        }
        bobPhase_ += moved * 2.6f;
        stepT_ += moved;
        if (stepT_ > 1.1f) { stepT_ = 0; sound_->play(SND_STEP, 0.35f, 0); }
        bobAmt_ = std::min(1.0f, bobAmt_ + dt * 4);
    } else {
        bobAmt_ = std::max(0.0f, bobAmt_ - dt * 4);
    }
}

void Client::sendHello() {
    Writer w;
    w.u8(C_HELLO);
    w.u16(PROTOCOL_VERSION);
    w.str(name_);
    w.u8((uint8_t)cls_);
    w.u8((uint8_t)gData.shared.size());   // our data files' checksums: the host checks they match
    for (auto& sf : gData.shared) { w.str(sf.name); w.u32(sf.hash); }
    send(w, true);
    helloSent_ = true;
}

void Client::update(float dt) {
    time_ += dt;
    std::vector<NetEvent> evs;
    net_->poll(evs);
    for (auto& e : evs) {
        if (e.type == NetEvent::Connect) {
            if (!helloSent_) sendHello();
        } else if (e.type == NetEvent::Disconnect) {
            if (error.empty()) error = "Disconnected from server.";
            leave = true;
        } else {
            handleMessage(e.data);
        }
    }
    if (!inGame()) return;

    localAttackCd_ = std::max(0.0f, localAttackCd_ - dt);
    localCastCd_ = std::max(0.0f, localCastCd_ - dt);
    attackAnim_ = std::max(0.0f, attackAnim_ - dt * 3.0f);
    castAnim_ = std::max(0.0f, castAnim_ - dt * 2.5f);
    hurtFlash_ = std::max(0.0f, hurtFlash_ - dt * 2);
    digShake_ = std::max(0.0f, digShake_ - dt);
    hintRubble();
    healFlash_ = std::max(0.0f, healFlash_ - dt * 2);
    levelFlash_ = std::max(0.0f, levelFlash_ - dt);

    move(dt);
    computeTarget();
    if (ui_ == Ui::None) {
        if (attackHeld_) tryAttack();
        if (castHeld_) tryCast();
        if (autoFight_) autoFightStep();
    }

    // Smooth other entities toward their latest snapshot position.
    for (auto& [id, e] : ents_) {
        float k = clampf(dt * 12.0f, 0, 1);
        e.rpos = e.rpos + (e.s.pos - e.rpos) * k;
    }

    sendT_ += dt;
    if (sendT_ >= TICK_DT) {
        sendT_ = 0;
        Writer w;
        w.u8(C_INPUT);
        w.f32(pos_.x);
        w.f32(pos_.y);
        w.f32(angle_);
        w.u8(0);
        send(w, false);
    }
}

// ================================================================= draw ====

void Client::draw(Renderer& r) {
    if (!inGame()) {
        r.fill(0, 0, r.W, r.H, 0xFF000000);
        r.beginUi();
        std::string s = error.empty() ? "Entering the dungeon..." : error;
        r.text((r.W - r.textW(s, 2)) / 2, r.H / 2 - 8, s, UI_TEXT, 2);
        return;
    }
    const Assets& A = *assets_;

    // ---- lights ----
    std::vector<LightSource> lights;
    float flicker = 0.92f + 0.08f * std::sin((float)time_ * 13.0f) * std::sin((float)time_ * 7.3f + 1.0f);
    int lr = me_.dead ? 0 : me_.lightRadius();
    if (lr > 0) {
        bool torch = !me_.equip[EQ_LIGHT].empty() && me_.equip[EQ_LIGHT].d().id == "torch" && me_.lightT <= 0;
        lights.push_back({pos_, lr + 0.5f, torch ? flicker : 1.0f});
    }
    lights.push_back({pos_, 2.0f, 0.3f});  // you can always just about see your surroundings
    for (auto& [id, e] : ents_) {
        if (e.s.type == ET_PLAYER && e.s.extra > 0) lights.push_back({e.rpos, e.s.extra / 10.0f + 0.5f, flicker});
        if (e.s.type == ET_PROJECTILE && e.s.def != 0xFFFF) lights.push_back({e.rpos, 2.5f, 0.8f});
        if (e.s.type == ET_EFFECT) lights.push_back({e.rpos, e.s.extra / 10.0f + 2.5f, 1.2f * (1.0f - e.s.hp / 255.0f)});
    }
    r.setupLights(map_, lights);

    // ---- sprites ----
    std::vector<SpriteDraw> sprites;
    for (auto& [id, e] : ents_) {
        SpriteDraw s;
        s.pos = e.rpos;
        s.ent = id;
        switch (e.s.type) {
            case ET_MONSTER: {
                const MonsterDef& d = gData.monsters[e.s.def];
                bool atk = e.s.flags & EF_ATTACK;
                s.tex = atk ? &A.monstersAttack[e.s.def] : creatureFrame(e, A.monsters[e.s.def], A.monstersWalk[e.s.def], animStyleFor(d.sprite));
                s.height = 0.85f * d.size;
                bool flyer = d.sprite == "bat" || d.sprite == "ghost" || d.sprite == "eye" || d.sprite == "imp";
                if (flyer) s.zoff = 0.25f + 0.06f * std::sin((float)time_ * 5.0f + id);
                else if (!(e.s.flags & EF_ASLEEP)) s.zoff = std::fabs(std::sin(e.anim)) * 0.025f;  // walk bounce
                s.dither = d.sprite == "ghost" || d.invisible;
                // Invisible monsters are a faint shimmer unless you detect them (or they are right next to you).
                if (d.invisible && me_.detectT <= 0) s.brightness = dist(e.rpos, pos_) < 3.0f ? 0.45f : 0.12f;
                if (d.sprite == "elemental") s.fullbright = d.color2 != 0;  // glowing
                if (e.s.flags & EF_HURT) { s.tint = 0xFFFF2020; s.tintAmt = 0.55f; }
                else if (e.s.flags & EF_SLOW) { s.tint = 0xFF80C0FF; s.tintAmt = 0.3f; }
                break;
            }
            case ET_PLAYER: {
                if (e.s.flags & EF_DEAD) continue;
                int ci = e.s.def % A.players.size();
                s.tex = (e.s.flags & EF_ATTACK) ? &A.playersAttack[ci] : creatureFrame(e, A.players[ci], A.playersWalk[ci], AnimStyle::Legs);
                s.height = 0.82f;
                s.zoff = std::fabs(std::sin(e.anim)) * 0.02f;
                if (e.s.flags & EF_HURT) { s.tint = 0xFFFF2020; s.tintAmt = 0.5f; }
                break;
            }
            case ET_ITEM:
                s.tex = &A.items[e.s.def];
                s.height = 0.28f;
                break;
            case ET_PROJECTILE:
                if (e.s.def == 0xFFFF) { s.tex = &A.arrow; s.height = 0.2f; s.zoff = 0.38f; }
                else { s.tex = &A.spellOrbs[e.s.def]; s.height = 0.3f; s.zoff = 0.32f; s.additive = true; s.fullbright = true; }
                break;
            case ET_EFFECT: {
                float age = e.s.hp / 255.0f;
                float rad = e.s.extra / 10.0f;
                s.tex = e.s.def == 0xFFFF ? &A.spellOrbs[0] : &A.spellOrbs[e.s.def];
                s.height = std::max(0.4f, rad * 1.6f) * (0.4f + age);
                s.zoff = 0.5f - s.height * 0.5f;
                s.additive = true;
                s.fullbright = true;
                s.brightness = 1.0f - age;
                break;
            }
        }
        sprites.push_back(s);
    }

    // Death animations: the frames at 10 per second, then the last one lies there and fades.
    const double DIE_FPS = 10, LINGER = 1.2, FADE = 0.6;
    for (size_t i = 0; i < dying_.size();) {
        const Dying& dy = dying_[i];
        const auto& frames = A.monstersDie[dy.def];
        double age = time_ - dy.start, playT = frames.size() / DIE_FPS;
        if (frames.empty() || age > playT + LINGER + FADE || age < 0) { dying_.erase(dying_.begin() + i); continue; }
        const MonsterDef& d = gData.monsters[dy.def];
        SpriteDraw s;
        s.pos = dy.pos;
        s.tex = &frames[std::min((int)frames.size() - 1, (int)(age * DIE_FPS))];
        s.height = 0.85f * d.size;
        s.dither = d.sprite == "ghost";
        if (d.sprite == "elemental") s.fullbright = d.color2 != 0;
        if (age > playT + LINGER) s.brightness = (float)std::max(0.0, 1.0 - (age - playT - LINGER) / FADE);
        sprites.push_back(s);
        i++;
    }

    float bob = std::sin(bobPhase_ * 2) * 2.5f * bobAmt_ + (digShake_ > 0 ? std::sin((float)time_ * 90) * 2.0f : 0.0f);
    float horizon = r.H / 2.0f + bob;
    r.drawWorld(map_, A, pos_, angle_, horizon, sprites);
    visibleMonsters_.clear();
    for (auto& s : sprites) {
        if (!s.visible) continue;
        auto it = ents_.find(s.ent);
        if (it != ents_.end() && it->second.s.type == ET_MONSTER) visibleMonsters_.push_back(s.ent);
    }

    // ---- target marker ----
    for (auto& s : sprites) {
        if (s.ent != targetId_ || !s.visible || !targetId_) continue;
        uint32_t c = targetIsSpell_ ? 0xFF80A0FF : 0xFFFFD040;
        int x0 = s.screenX0 - 2, x1 = s.screenX1 + 2, y0 = s.screenY0 - 2, y1 = s.screenY1 + 2;
        int t = std::max(1, r.H / 360);   // line thickness grows with the resolution
        int L = std::max(3 * t, std::min(10 * t, (x1 - x0) / 4));
        r.fill(x0, y0, L, t, c); r.fill(x0, y0, t, L, c);
        r.fill(x1 - L, y0, L, t, c); r.fill(x1, y0, t, L, c);
        r.fill(x0, y1, L, t, c); r.fill(x0, y1 - L, t, L, c);
        r.fill(x1 - L, y1, L, t, c); r.fill(x1, y1 - L + 1, t, L, c);
    }

    drawViewModel(r);

    if (hurtFlash_ > 0) r.tintScreen(0xFFC00000, (int)(hurtFlash_ * 120));
    if (healFlash_ > 0) r.tintScreen(0xFF40FF60, (int)(healFlash_ * 60));
    if (levelFlash_ > 0) r.tintScreen(0xFFFFE060, (int)(levelFlash_ * 70));
    if (me_.slowT > 0) r.tintScreen(0xFF4080FF, 25);
    if (me_.poisonT > 0) r.tintScreen(0xFF40C020, 18);

    r.beginUi();   // from here on: HUD and screens, laid out for 640x360
    drawHud(r);

    switch (ui_) {
        case Ui::Inventory: drawInventory(r); break;
        case Ui::Character: drawCharacter(r); break;
        case Ui::Quests: drawQuests(r); break;
        case Ui::Map: {
            int scale = std::max(2, std::min((r.W - 20) / map_.w, (r.H - 70) / map_.h));
            int w = map_.w * scale, h = map_.h * scale;
            int x = (r.W - w) / 2, y = (r.H - 48 - h) / 2;
            r.fill(x - 4, y - 14, w + 8, h + 18, 0xFF000000, 200);
            r.text(x, y - 11, (map_.depth ? "Dungeon level " + std::to_string(map_.depth) : std::string("The town")) + "   (Tab / M to close)", UI_HI);
            drawMinimap(r, x, y, w, h, scale, true);
            break;
        }
        case Ui::Help: drawHelp(r); break;
        case Ui::Menu: drawMenu(r); break;
        case Ui::Shop: drawShop(r); break;
        case Ui::Spells: drawSpellBook(r); break;
        case Ui::Chat: {
            r.fill(4, r.H - 66, r.W - 8, 12, 0xFF000000, 200);
            r.text(8, r.H - 64, "Say: " + chat_ + ((int)(time_ * 2) % 2 ? "_" : ""), UI_TEXT);
            break;
        }
        default: break;
    }

    if (me_.dead) {
        r.tintScreen(0xFF400000, 120);
        std::string a = "You have died.";
        r.text((r.W - r.textW(a, 3)) / 2, r.H / 3, a, 0xFFFF4040, 3);
        int alive = 0;
        for (auto& [id, e] : ents_)
            if (e.s.type == ET_PLAYER && !(e.s.flags & EF_DEAD)) alive++;
        std::string b = alive ? "Respawning at the stairs in " + std::to_string((int)me_.respawnT + 1) + "..." : "The party has fallen...";
        r.text((r.W - r.textW(b)) / 2, r.H / 3 + 30, b, UI_TEXT);
    }
}

const Texture* Client::creatureFrame(const CEnt& e, const Texture& still, const std::vector<Texture>& walk, AnimStyle style) const {
    if (!animate || (e.s.flags & EF_ASLEEP) || walk.empty()) return &still;
    int n = (int)walk.size(), f;
    if (animAlways(style)) {
        // Wings and hovering creatures never stop; each one keeps its own rhythm.
        f = (int)(time_ * 8.0 + e.s.id * 0.37) % n;
    } else {
        if (e.moved < 0 || time_ - e.moved > 0.25) return &still;   // standing: the normal picture
        f = (int)(e.anim * 1.4f) % n;
    }
    return &walk[f];
}

void Client::drawViewModel(Renderer& r) {
    if (me_.dead) return;
    const ItemDef* w = me_.weapon();
    int vm = w ? viewModelFromName(w->viewmodel) : VM_FIST;
    const Texture& t = assets_->viewmodels[vm];
    float scale = r.H / 360.0f * 1.45f;
    int sw = (int)(t.w * scale), sh = (int)(t.h * scale);
    float bx = std::sin(bobPhase_) * 6 * bobAmt_, by = std::fabs(std::cos(bobPhase_)) * 5 * bobAmt_;
    float a = attackAnim_;
    float swing = std::sin((1.0f - a) * PI) * (a > 0 ? 1 : 0);
    float ox = 0, oy = 0;
    if (w && w->ranged) { oy = swing * 14; }
    else if (vm == VM_STAFF) { ox = -swing * 50; oy = -swing * 25; }
    else { ox = -swing * r.W * 0.18f; oy = -swing * 40 + swing * swing * 60; }
    float c = castAnim_;
    if (c > 0) oy += std::sin((1.0f - c) * PI) * 12;
    int x = (int)(r.W * 0.56f + bx + ox), y = (int)(r.H - 44 - sh + 34 * scale + by + oy);
    int tile = (int)pos_.y * map_.w + (int)pos_.x;
    float light = std::max(0.3f, std::min(1.1f, r.lightAt(pos_, tile)));
    r.blit(t, x, y, sw, sh, light);
    if (c > 0 && castSpellAnim_ >= 0) {
        const Texture& o = assets_->spellOrbs[castSpellAnim_];
        int s = (int)(40 * scale * (0.6f + std::sin((1.0f - c) * PI)));
        r.blit(o, x + sw / 2 - s / 2 - (int)(20 * scale), y + (int)(10 * scale), s, s, 1.0f, true);
    }
}

void Client::drawHud(Renderer& r) {
    const int H = r.H, W = r.W;
    // Message log (top left)
    int ly = 4;
    int shown = 0;
    for (auto it = log_.rbegin(); it != log_.rend() && shown < 7; ++it, ++shown) {}
    for (int i = (int)log_.size() - shown; i < (int)log_.size(); i++) {
        const LogLine& l = log_[i];
        double age = time_ - l.time;
        if (age > 9.0 && ui_ != Ui::Chat) continue;
        r.text(4, ly, l.text, l.color);
        ly += 9;
    }

    // Target info (top center)
    if (targetId_) {
        auto it = ents_.find(targetId_);
        if (it != ents_.end()) {
            const MonsterDef& d = gData.monsters[it->second.s.def];
            std::string n = d.name;
            if (!n.empty()) n[0] = (char)std::toupper(n[0]);
            if (it->second.s.flags & EF_ASLEEP) n += " (asleep)";
            int tw = std::max(r.textW(n), 80);
            int x = (W - tw) / 2;
            int y = 78;
            r.text(x + (tw - r.textW(n)) / 2, y, n, d.boss ? 0xFFFFA040 : UI_TEXT);
            bar(r, x, y + 10, tw, 4, it->second.s.hp / 255.0f, UI_RED, "");
        }
    }

    // Minimap (top right)
    int mmW = 110, mmH = 80;
    r.fill(W - mmW - 6, 4, mmW + 2, mmH + 2, 0xFF000000, 170);
    drawMinimap(r, W - mmW - 5, 5, mmW, mmH, 3, false);
    // Main quest under the minimap
    int qy = mmH + 10;
    for (auto& q : quests_) {
        if (q.done) continue;
        std::string s = q.text;
        if (q.type == Q_SLAY) s += " (" + std::to_string(q.count) + "/" + std::to_string(q.needed) + ")";
        uint32_t c = q.main ? 0xFFFFC060 : 0xFF80D0FF;
        // wrap to ~ 34 chars
        while (!s.empty()) {
            size_t n = std::min<size_t>(s.size(), 34);
            if (n < s.size()) { size_t sp = s.rfind(' ', n); if (sp != std::string::npos && sp > 10) n = sp; }
            std::string line = s.substr(0, n);
            r.text(W - 6 - r.textW(line), qy, line, c);
            qy += 9;
            s = s.substr(std::min(s.size(), n + (n < s.size() && s[n] == ' ' ? 1 : 0)));
        }
        qy += 2;
    }

    // Quick slot bar (keys 1-9). Wraps to a second row when it doesn't fit.
    {
        int x = 6, y = H - 58;
        quickBarRows_ = 1;
        for (int i = 0; i < NUM_QUICK; i++) {
            int v = me_.quick[i];
            if (v < 0) continue;
            const SpellDef* s = quickSpell(i);
            std::string nm = quickName(v);
            for (const char* pre : {"Wand of ", "Rod of "})
                if (nm.rfind(pre, 0) == 0) nm = nm.substr(std::strlen(pre));
            std::string t = std::to_string(i + 1) + ":" + nm.substr(0, 16);
            bool ok = s != nullptr;
            if (v >= QUICK_ITEM) {
                int inv = quickInvSlot(v);
                if (inv < 0) { t += " -"; ok = false; }
                else {
                    const ItemStack& it = me_.inv[inv];
                    if (it.d().kind == ItemKind::Wand) { t += " (" + std::to_string(it.fuel) + ")"; ok = it.fuel > 0; }
                    else if (it.fuel > 0) { t += " " + std::to_string((it.fuel + 9) / 10) + "s"; ok = false; }
                }
            } else if (s) {
                float rc = me_.rechargeLeft(v);
                if (!me_.knowsSpell(v)) { t += " L" + std::to_string(me_.c().spellLevels[v]); ok = false; }
                else if (rc > 0) { t += " " + std::to_string((int)std::ceil(rc)) + "s"; ok = false; }
                else if (s->mana > 0) { t += " " + std::to_string(s->mana); ok = me_.mp >= s->mana; }
            }
            bool sel = i == selSpell_;
            int tw = r.textW(t) + 6;
            if (x + tw > W - 6) { x = 6; y -= 13; quickBarRows_++; }
            r.fill(x, y, tw, 11, sel ? 0xFF3A3060 : 0xFF000000, sel ? 230 : 150);
            if (sel) r.frame(x, y, tw, 11, 0xFF9080FF);
            r.text(x + 3, y + 2, t, ok ? (sel ? 0xFFFFFFFF : 0xFFB0A8E0) : 0xFF706070);
            x += tw + 3;
        }
    }

    // Bottom status bar
    int by = H - 44;
    r.fill(0, by, W, 44, 0xFF14100C, 235);
    r.fill(0, by, W, 1, UI_EDGE);
    std::string who = me_.name + " the " + me_.c().name + "  Lvl " + std::to_string(me_.level);
    r.text(6, by + 4, who, UI_HI);
    bar(r, 6, by + 14, 140, 8, (float)me_.hp / std::max(1, me_.maxHp), UI_RED, "HP " + std::to_string(me_.hp) + "/" + std::to_string(me_.maxHp));
    if (me_.maxMp > 0) bar(r, 6, by + 24, 140, 8, me_.mp / std::max(1.0f, me_.maxMp), UI_BLUE, "MP " + std::to_string((int)me_.mp) + "/" + std::to_string((int)me_.maxMp));
    float xpPrev = (float)(5 * (me_.level - 1) * me_.level);
    bar(r, 6, by + 35, 140, 4, (me_.xp - xpPrev) / std::max(1.0f, me_.xpForNext() - xpPrev), 0xFFC0A030, "");

    int mx = 160;
    std::string foodS = me_.food <= 0 ? "Starving!" : me_.food < 150 ? "Hungry" : me_.food > 900 ? "Full" : "Fed";
    bar(r, mx, by + 14, 110, 8, me_.food / FOOD_MAX, me_.food < 150 ? 0xFFE06020 : 0xFFB08040, "Food: " + foodS);
    const ItemStack& L = me_.equip[EQ_LIGHT];
    bool forever = !L.empty() && L.d().everburning;
    std::string lightS = L.empty() ? "No light!" : forever ? L.d().name.substr(0, 17) : L.d().name + " " + fmtTime(std::max(0, L.fuel));
    float lf = L.empty() ? 0 : forever ? 1.0f : (float)L.fuel / std::max(1, L.d().fuel);
    bar(r, mx, by + 24, 110, 8, lf, L.empty() || (!forever && L.fuel < 60) ? 0xFFE06020 : 0xFFE0C040, lightS);
    std::string status;
    if (me_.blessT > 0) status += "Blessed ";
    if (me_.lightT > 0) status += "Light ";
    if (me_.detectT > 0) status += "Detect ";
    if (me_.slowT > 0) status += "Slowed ";
    if (me_.hasteT > 0) status += "Fast ";
    if (me_.shieldT > 0) status += "Stoneskin ";
    if (me_.resistT > 0) status += "Resist ";
    if (me_.heroT > 0) status += "Berserk ";
    if (me_.poisonT > 0) status += "Poisoned ";
    {
        int dr = 0;
        for (int v : me_.drained) dr += v;
        if (dr) status += "Drained ";
    }
    if (autoFight_) status += "AutoFight ";
    if (auto_ == Auto::Explore) status += "Exploring ";
    if (auto_ == Auto::Travel) status += "Travelling ";
    if (autoWalk_) status += "AutoWalk ";
    if (me_.recallT > 0) status += "Recall " + std::to_string((int)me_.recallT + 1) + "s ";
    r.text(mx, by + 35, status, 0xFF80FF80);

    int rx = 284;
    const ItemDef* wd = me_.weapon();
    r.text(rx, by + 4, wd ? itemName(me_.equip[EQ_WEAPON]) : "Bare hands (1d2)", UI_TEXT);
    r.text(rx, by + 14, "AC " + std::to_string(me_.armorClass()), UI_TEXT);
    r.text(rx + 48, by + 14, "STR " + std::to_string(me_.stat(STR)) + " DEX " + std::to_string(me_.stat(DEX)), UI_DIM);
    r.text(rx, by + 24, map_.depth ? "Depth " + std::to_string(map_.depth) : std::string("Town"), UI_TEXT);
    r.text(rx + 66, by + 24, "Gold " + std::to_string(me_.gold), 0xFFFFD040);
    r.text(rx, by + 34, "XP " + std::to_string(me_.xp) + "/" + std::to_string(me_.xpForNext()), UI_DIM);

    // Party list
    int px = W - 130;
    int py = by + 4;
    for (auto& re : roster_) {
        if (re.id == myId_) continue;
        auto it = ents_.find(re.id);
        float hp = it != ents_.end() ? it->second.s.hp / 255.0f : 1.0f;
        bool dead = it != ents_.end() && (it->second.s.flags & EF_DEAD);
        std::string n = re.name + " " + gData.classes[re.cls % gData.classes.size()].name.substr(0, 3) + std::to_string(re.level);
        r.text(px, py, n, dead ? 0xFF805050 : UI_TEXT);
        bar(r, px, py + 8, 120, 2, dead ? 0 : hp, UI_RED, "");
        py += 13;
        if (py > H - 10) break;
    }
    if (roster_.size() <= 1) r.text(px, by + 34, "F1: help", UI_DIM);

    // What E does here
    if (ui_ == Ui::None) {
        std::string pr = usePrompt();
        if (!pr.empty()) {
            int tw = r.textW(pr);
            r.fill((W - tw) / 2 - 4, H / 2 + 34, tw + 8, 11, 0xFF000000, 160);
            r.text((W - tw) / 2, H / 2 + 36, pr, UI_HI);
        }
    }

    // Key legend (F2 hides it)
    if (showLegend_) {
        std::string pick = autoPickup ? "pickup:on" : "pickup:off";
        std::pair<std::string, std::string> keys[] = {{keys_->label(K_INVENTORY), "inventory"}, {keys_->label(K_CHARACTER), "character"},
                                                      {keys_->label(K_QUESTS), "quests"}, {keys_->label(K_MAP), "map"},
                                                      {keys_->label(K_POTION), "potion"}, {keys_->label(K_EAT), "eat"},
                                                      {keys_->label(K_AUTOPICKUP), pick}, {keys_->label(K_HELP), "help"},
                                                      {keys_->label(K_LEGEND), "hide"}};
        std::pair<std::string, std::string> keys2[] = {{keys_->label(K_AUTOFIGHT), autoFight_ ? "auto-fight:ON" : "auto-fight"},
                                                       {keys_->label(K_EXPLORE), "explore"}, {keys_->label(K_TRAVEL), "go to stairs"},
                                                       {keys_->label(K_AUTOWALK), "auto-walk"}, {keys_->label(K_FACE), "face enemy"},
                                                       {keys_->label(K_SPELLBOOK), "spells"}, {keys_->label(K_USE), "use"}};
        auto legendRow = [&](const auto& arr, int ly) {
            int total = 0;
            for (auto& k : arr) total += r.textW(k.first) + r.textW(k.second) + 6 + 10;
            int lx = (W - total) / 2;
            r.fill(lx - 4, ly - 1, total + 4, 10, 0xFF000000, 110);
            for (auto& k : arr) {
                r.text(lx, ly, k.first, UI_HI);
                lx += r.textW(k.first) + 4;
                r.text(lx, ly, k.second, 0xFFB0A080);
                lx += r.textW(k.second) + 12;
            }
        };
        int up = (quickBarRows_ - 1) * 13;
        legendRow(keys, H - 70 - up);
        legendRow(keys2, H - 81 - up);
    }

    // Crosshair dot
    r.fill(W / 2, H / 2 - 22, 1, 1, 0xFFFFFFFF, 120);
}

void Client::drawMinimap(Renderer& r, int x0, int y0, int w, int h, int scale, bool full) {
    // In the small map the player is centered; the full map shows everything.
    float ox, oy;
    if (full) { ox = 0; oy = 0; }
    else { ox = pos_.x - (float)w / scale / 2; oy = pos_.y - (float)h / scale / 2; }
    int tx0 = (int)std::floor(ox), ty0 = (int)std::floor(oy);
    int tw = w / scale + 2, th = h / scale + 2;
    for (int ty = ty0; ty < ty0 + th; ty++)
        for (int tx = tx0; tx < tx0 + tw; tx++) {
            if (!map_.inside(tx, ty) || !(map_.flag(tx, ty) & F_SEEN)) continue;
            int sx = x0 + (int)((tx - ox) * scale), sy = y0 + (int)((ty - oy) * scale);
            if (sx < x0 || sy < y0 || sx + scale > x0 + w || sy + scale > y0 + h) continue;
            uint8_t t = map_.at(tx, ty);
            uint32_t c;
            switch (t) {
                case T_WALL: case T_ROCK: case T_DOOR_SECRET: c = 0xFF8A8070; break;
                case T_DOOR: case T_DOOR_LOCKED: c = 0xFFB07030; break;
                case T_DOOR_OPEN: c = 0xFF704820; break;
                case T_STAIRS_DOWN: c = 0xFFFFFF40; break;
                case T_STAIRS_UP: c = 0xFF40FFFF; break;
                case T_RUBBLE: c = 0xFFC08040; break;
                case T_SHOP: c = 0xFFFF60FF; break;
                default: c = (map_.flag(tx, ty) & F_LIT) ? 0xFF3A3830 : 0xFF262420; break;
            }
            r.fill(sx, sy, scale, scale, c);
        }
    auto dot = [&](Vec2 p, uint32_t c, int sz) {
        int sx = x0 + (int)((p.x - ox) * scale), sy = y0 + (int)((p.y - oy) * scale);
        if (sx < x0 || sy < y0 || sx >= x0 + w || sy >= y0 + h) return;
        r.fill(sx - sz / 2, sy - sz / 2, sz, sz, c);
    };
    for (auto& [id, e] : ents_) {
        if (e.s.type == ET_PLAYER) dot(e.rpos, 0xFF40FF40, std::max(2, scale));
        else if (e.s.type == ET_MONSTER && me_.detectT > 0 && dist(e.rpos, pos_) < 24) dot(e.rpos, (e.s.flags & EF_BOSS) ? 0xFFFF8000 : 0xFFFF3030, std::max(2, scale));
    }
    // Player with facing direction
    dot(pos_, 0xFFFFFFFF, std::max(2, scale));
    Vec2 f = pos_ + dirFromAngle(angle_) * (4.0f / scale + 0.8f);
    dot(f, 0xFFFFFFFF, 1);
    Vec2 f2 = pos_ + dirFromAngle(angle_) * (2.0f / scale + 0.4f);
    dot(f2, 0xFFFFFFFF, 1);
}

void Client::drawInventory(Renderer& r) {
    int w = 420, h = 300;
    int x = (r.W - w) / 2, y = (r.H - 44 - h) / 2;
    invX_ = x; invY_ = y; invW_ = w;
    // The pack may have changed under us (item used up, picked up...): keep the replace mode valid.
    if (invPick_ >= 0 && (invSel_ < EQ_COUNT || !invFits(invSel_ - EQ_COUNT, invPick_))) {
        invStep(1);
        if (invSel_ < EQ_COUNT || !invFits(invSel_ - EQ_COUNT, invPick_)) { invSel_ = invPick_; invPick_ = -1; }
    }
    panel(r, x, y, w, h, "Inventory - " + std::to_string(me_.gold) + " gold");
    // Which equipment slot is in focus: the one being replaced, the selected one, or the one the
    // selected pack item would go into. Pack items that fit it are marked green.
    bool equipRow = invSel_ < EQ_COUNT;
    const ItemStack& selStack = equipRow ? me_.equip[invSel_] : me_.inv[invSel_ - EQ_COUNT];
    int focusEq = invPick_ >= 0 ? invPick_ : equipRow ? invSel_ : (selStack.empty() ? -1 : equipSlotFor(selStack.d()));
    const uint32_t FIT = 0xFF80E080, HILITE = 0xFF4A3A20, TARGET = 0xFF24402A;
    int ry = y + 20;
    auto row = [&](int idx, const std::string& label, const ItemStack& s) {
        int pack = idx - EQ_COUNT;
        bool sel = idx == invSel_;
        bool fits = focusEq >= 0 && (equipRow || invPick_ >= 0) && invFits(pack, focusEq);
        bool dim = invPick_ >= 0 && !fits;
        if (sel) r.fill(x + 6, ry - 1, w - 12, 10, HILITE);
        if (fits) r.fill(x + 6, ry - 1, 2, 10, FIT);
        r.text(x + 10, ry, label, UI_DIM);
        uint32_t c = s.empty() || dim ? 0xFF605850 : s.d().artifact ? 0xFFFFC040 : sel ? 0xFFFFFFFF : fits ? FIT : UI_TEXT;
        r.text(x + 34, ry, itemName(s).substr(0, 63), c);
        ry += 10;
    };
    // Equipment in two columns, then the pack.
    for (int i = 0; i < EQ_COUNT; i++) {
        int col = i / 3, rowi = i % 3;
        int ex = x + 10 + col * 205, ey = y + 20 + rowi * 10;
        bool sel = i == invSel_ || i == invPick_;
        if (sel) r.fill(ex - 4, ey - 1, 202, 10, HILITE);
        else if (i == focusEq) r.fill(ex - 4, ey - 1, 202, 10, TARGET);   // where the selected item would go
        r.text(ex, ey, kEquipNames[i], UI_DIM);
        const ItemStack& es = me_.equip[i];
        uint32_t c = es.empty() ? 0xFF605850 : es.d().artifact ? 0xFFFFC040 : (sel ? 0xFFFFFFFF : UI_TEXT);
        r.text(ex + 42, ey, itemName(es).substr(0, 25), c);
    }
    ry = y + 20 + 30 + 4;
    for (int i = 0; i < INV_SIZE; i++) {
        std::string lbl = std::string(1, (char)('a' + i)) + ")";
        row(EQ_COUNT + i, lbl, me_.inv[i]);
    }
    // Description of the selected item
    const ItemStack& s = selStack;
    int dy = y + h - 28;
    if (!s.empty()) {
        const ItemDef& d = s.d();
        std::string info = d.desc;
        if (d.kind == ItemKind::Weapon) info = std::string(d.ranged ? "Ranged" : "Melee") + " " + d.dmg.str() + ", " + (d.finesse ? "DEX" : "STR") + ", speed " + std::to_string((int)(10 / d.cooldown) / 10.0f).substr(0, 3) + "/s. " + info;
        if (d.kind == ItemKind::Food) info = "Restores " + std::to_string(d.food) + " food. " + info;
        if (d.kind == ItemKind::Potion) info = (d.heal ? "Heals " + std::to_string(d.heal) + ". " : "") + (d.mana ? "Restores " + std::to_string(d.mana) + " mana." : "");
        if (d.kind == ItemKind::Scroll && d.spell >= 0) info = "Casts " + gData.spells[d.spell].name + ": " + gData.spells[d.spell].desc;
        if (d.kind == ItemKind::Wand || d.kind == ItemKind::Rod)
            info = gData.spells[d.spell].name + (d.kind == ItemKind::Rod ? " (recharges in " + std::to_string(d.recharge) + "s)" : "") +
                   ". Fail " + std::to_string(me_.deviceFail(d)) + "%. " + gData.spells[d.spell].desc;
        r.text(x + 10, dy, info.substr(0, 66), 0xFFC0B8A0);
    }
    // The key hint says exactly what Enter will do with the selected row.
    std::string hint;
    if (invPick_ >= 0) {
        std::string verb = invUseLabel(s);
        verb = verb.substr(0, verb.find(" ("));
        hint = std::string("W/S choose  Enter: ") + verb + "  U: take off  Esc: back";
    } else if (equipRow) {
        bool any = false;
        for (int i = 0; i < INV_SIZE; i++) any |= invFits(i, invSel_);
        if (any) hint = "Enter: replace with a green item   U: take off   Esc: close";
        else if (!s.empty()) hint = "Enter/U: take off (nothing in your pack fits)   Esc: close";
        else hint = "Nothing in your pack fits here      Esc: close";
    } else if (!s.empty()) {
        hint = "Enter: " + invUseLabel(s) + "  D: drop  G: give";
    } else {
        hint = "W/S: select   Enter: use/wear/eat   Esc: close";
    }
    r.text(x + 10, dy + 12, hint.substr(0, 68), UI_DIM);
}

void Client::drawCharacter(Renderer& r) {
    int w = 360, h = 230;
    int x = (r.W - w) / 2, y = (r.H - 44 - h) / 2;
    panel(r, x, y, w, h, me_.name + " the " + me_.c().name);
    int ty = y + 22;
    r.text(x + 12, ty, me_.c().desc.substr(0, 56), UI_DIM);
    ty += 14;
    for (int i = 0; i < NUM_STATS; i++) {
        int m = me_.mod(i);
        int eff = me_.stat(i);
        std::string s = std::string(kStatNames[i]) + "  " + std::to_string(eff) + "  (" + (m >= 0 ? "+" : "") + std::to_string(m) + ")";
        if (eff != me_.stats[i]) s += eff > me_.stats[i] ? "  *" : "  -";
        if (me_.drained[i]) s += " (drained " + std::to_string(me_.drained[i]) + ")";
        r.text(x + 12, ty + i * 11, s, UI_TEXT);
    }
    int cx = x + 160;
    const ItemDef* wd = me_.weapon();
    bool ranged = wd && wd->ranged, fin = wd && wd->finesse;
    int skill = ranged ? me_.c().ranged : me_.c().melee;
    int stat = (ranged || fin) ? std::max(me_.mod(DEX), ranged ? me_.mod(DEX) : me_.mod(STR)) : me_.mod(STR);
    int toHit = skill + me_.level / 2 + stat + me_.toHitBonus() + (me_.blessT > 0 ? 2 : 0);
    std::vector<std::string> lines = {
        "Level " + std::to_string(me_.level) + "   XP " + std::to_string(me_.xp) + "/" + std::to_string(me_.xpForNext()),
        "Hit points " + std::to_string(me_.hp) + "/" + std::to_string(me_.maxHp),
        "Mana " + std::to_string((int)me_.mp) + "/" + std::to_string((int)me_.maxMp),
        "Armor class " + std::to_string(me_.armorClass()),
        std::string("Attack: d20 ") + (toHit >= 0 ? "+ " : "- ") + std::to_string(std::abs(toHit)),
        "Spell power +" + std::to_string(me_.spellPower()),
        "Light radius " + std::to_string(me_.lightRadius()),
        "Kills " + std::to_string(me_.kills),
        std::string("Difficulty: ") + difficultyRules(difficulty_).name,
        [&]() {
            std::string rs;
            static const char* en[] = {"physical", "arcane", "fire", "cold", "holy", "poison"};
            for (int e = 1; e < 6; e++) if (me_.resists((Element)e)) rs += (rs.empty() ? "" : ", ") + std::string(en[e]);
            return rs.empty() ? std::string("Resist: none") : "Resist: " + rs;
        }(),
    };
    for (size_t i = 0; i < lines.size(); i++) r.text(cx, ty + (int)i * 11, lines[i], UI_TEXT);
    ty += 96;
    r.text(x + 12, ty, "Skills: melee " + std::to_string(me_.c().melee) + "  ranged " + std::to_string(me_.c().ranged) + "  magic " + std::to_string(me_.c().magic), UI_DIM);
    r.text(x + 12, ty + 11, "search " + std::to_string(me_.c().search) + "  stealth " + std::to_string(me_.c().stealth) + "  disarm " + std::to_string(me_.c().disarm), UI_DIM);
    ty += 26;
    r.text(x + 12, ty, "Spells and abilities (" + keys_->label(K_SPELLBOOK) + ": spell book):", UI_HI);
    ty += 11;
    const ClassDef& c = me_.c();
    if (c.spells.empty()) r.text(x + 12, ty, "(none - you trust in steel)", UI_DIM);
    if (c.spells.size() > 8) r.text(x + w - 12 - r.textW("+" + std::to_string(c.spells.size() - 8) + " more"), ty - 11, "+" + std::to_string(c.spells.size() - 8) + " more", UI_DIM);
    for (size_t i = 0; i < c.spells.size() && i < 8; i++) {
        const SpellDef& s = gData.spells[c.spells[i]];
        bool known = me_.knowsSpell((int)i);
        std::string t = s.name + (s.mana > 0 ? " (" + std::to_string(s.mana) + " mp)" : "") + (known ? "" : " - level " + std::to_string(c.spellLevels[i]));
        r.text(x + 12 + (int)(i % 2) * 170, ty + (int)(i / 2) * 10, t, known ? 0xFFB0A8E0 : 0xFF605870);
    }
}

void Client::drawQuests(Renderer& r) {
    int w = 400, h = 180;
    int x = (r.W - w) / 2, y = (r.H - 44 - h) / 2;
    panel(r, x, y, w, h, "Quest journal");
    int ty = y + 22;
    for (auto& q : quests_) {
        std::string s = (q.main ? "[Main] " : "[Level] ") + q.text;
        if (q.type == Q_SLAY && !q.done) s += " (" + std::to_string(q.count) + "/" + std::to_string(q.needed) + ")";
        if (q.done) s += " - done!";
        r.text(x + 12, ty, s.substr(0, 64), q.done ? UI_GREEN : (q.main ? 0xFFFFC060 : 0xFF80D0FF));
        ty += 12;
    }
    ty += 8;
    r.text(x + 12, ty, "Party:", UI_HI);
    ty += 12;
    for (auto& re : roster_) {
        r.text(x + 12, ty, re.name + " - level " + std::to_string(re.level) + " " + gData.classes[re.cls % gData.classes.size()].name, re.id == myId_ ? UI_HI : UI_TEXT);
        ty += 10;
    }
}

void Client::drawHelp(Renderer& r) {
    int w = 520, h = 290;
    int x = (r.W - w) / 2, y = (r.H - 44 - h) / 2;
    panel(r, x, y, w, h, "How to play Dum   (change keys in the main menu: Controls)");
    const KeyBinds& kb = *keys_;
    // Left column: the current key bindings.
    int ty = y + 20;
    auto keyRow = [&](const std::string& k, const std::string& what) {
        r.text(x + 10, ty, k, UI_HI);
        r.text(x + 82, ty, what, UI_TEXT);
        ty += 9;
    };
    auto both = [&](int a) {
        std::string k = KeyBinds::keyName(kb.key[a][0]);
        if (kb.key[a][1] != SDL_SCANCODE_UNKNOWN) k += "/" + KeyBinds::keyName(kb.key[a][1]);
        return k.substr(0, 12);
    };
    for (int a = 0; a < K_COUNT; a++) keyRow(both(a), KeyBinds::actionName(a));
    keyRow("Left mouse", "Attack (hold)");
    keyRow("Right mouse", "Use the selected quick slot");
    keyRow("1-9 / wheel", quickCast ? "Use quick slot (spell/wand)" : "Select quick slot");
    keyRow("Esc", "Menu (pauses single player)");
    // Right column: tips.
    const char* tips[] = {
        "You don't need to aim precisely:",
        "look roughly toward a monster.",
        "Yellow brackets = who you'll hit",
        "(blue = spell target). Dice do",
        "the rest: d20 + skill vs armor.",
        "",
        "Walk over items to pick them up.",
        "Inventory: Enter uses, eats or",
        "wears the item. On a worn slot,",
        "Enter picks a replacement.",
        "",
        "Keep your light burning and your",
        "belly full. Torches burn out:",
        "use a spare one to combine them.",
        "Sleeping monsters can be snuck",
        "past - or sneak-attacked.",
        "Walk into doors to open them.",
        "Rubble blocks some passages:",
        "use it to dig through.",
        "Find the stairs down (yellow on",
        "the map) to go deeper.",
    };
    int tx = x + 300, tyy = y + 20;
    for (const char* t : tips) { r.text(tx, tyy, t, 0xFFC0B8A0); tyy += 10; }
    r.text(x + 10, y + h - 12, "Press " + kb.label(K_HELP) + " or Esc to close", UI_DIM);
}

void Client::drawMenu(Renderer& r) {
    int w = 200, h = 80;
    int x = (r.W - w) / 2, y = (r.H - h) / 2 - 20;
    panel(r, x, y, w, h, "Paused");
    const char* items[3] = {"Resume", "Leave game", "Quit Dum"};
    for (int i = 0; i < 3; i++) {
        bool sel = i == menuSel_;
        if (sel) r.fill(x + 20, y + 22 + i * 16 - 2, w - 40, 12, 0xFF4A3A20);
        r.text(x + (w - r.textW(items[i])) / 2, y + 22 + i * 16, items[i], sel ? UI_HI : UI_TEXT);
    }
}

int Client::invRowAt(int mx, int my) const {
    if (mx < invX_ + 6 || mx > invX_ + invW_ - 6) return -1;
    for (int i = 0; i < EQ_COUNT; i++) {
        int ex = invX_ + 10 + (i / 3) * 205, ey = invY_ + 20 + (i % 3) * 10;
        if (mx >= ex - 4 && mx < ex + 198 && my >= ey - 1 && my < ey + 9) return i;
    }
    int ry = invY_ + 20 + 30 + 4;
    for (int i = 0; i < INV_SIZE; i++, ry += 10)
        if (my >= ry - 1 && my < ry + 9) return EQ_COUNT + i;
    return -1;
}

// What pressing E would do right now (same rules as the server).
std::string Client::usePrompt() const {
    if (me_.dead) return "";
    std::vector<Vec2> pos;
    std::vector<const CEnt*> items;
    for (auto& [id, e] : ents_)
        if (e.s.type == ET_ITEM) { pos.push_back(e.s.pos); items.push_back(&e); }
    int i = pickupCandidate(map_, pos_, angle_, pos);
    if (i >= 0) {
        ItemStack s;
        s.def = (int16_t)items[i]->s.def;
        s.count = std::max<int>(1, items[i]->s.extra);
        std::string n = itemName(s);
        if (s.d().kind == ItemKind::Light) n = s.d().name;  // fuel unknown here
        return "[" + keys_->label(K_USE) + "] Pick up " + n;
    }
    uint8_t under = map_.at((int)pos_.x, (int)pos_.y);
    if (under == T_STAIRS_DOWN) return "[" + keys_->label(K_USE) + "] Go down the stairs";
    if (under == T_STAIRS_UP && map_.depth > 1) return "[" + keys_->label(K_USE) + "] Go up the stairs";
    int x, y;
    if (useTileInFront(map_, pos_, angle_, x, y)) {
        switch (map_.at(x, y)) {
            case T_SHOP: {
                int st = map_.floorTex[y * map_.w + x];
                if (st < (int)gData.stores.size()) return "[" + keys_->label(K_USE) + "] Enter the " + gData.stores[st].name;
                break;
            }
            case T_DOOR: return "[" + keys_->label(K_USE) + "] Open the door";
            case T_DOOR_OPEN: return "[" + keys_->label(K_USE) + "] Close the door";
            case T_DOOR_LOCKED: return "[" + keys_->label(K_USE) + "] Pick the lock (or keep walking into it)";
            case T_RUBBLE: return "[" + keys_->label(K_USE) + "] Dig through the rubble (or walk into it)";
            default: break;
        }
    }
    return "";
}

// The first time rubble comes into view, explain what to do with it.
void Client::hintRubble() {
    if (rubbleHinted_) return;
    int px = (int)pos_.x, py = (int)pos_.y;
    for (int y = py - 5; y <= py + 5; y++)
        for (int x = px - 5; x <= px + 5; x++) {
            if (map_.at(x, y) != T_RUBBLE || !(map_.flag(x, y) & F_SEEN)) continue;
            if (!map_.los(pos_, Vec2(x + 0.5f, y + 0.5f))) continue;
            rubbleHinted_ = true;
            addLog("Rubble blocks a passage ahead. Walk into it (or press " + keys_->label(K_USE) + ") to dig through.", UI_HI);
            return;
        }
}

// ================================================================ shops ====

bool Client::tryOpenShop(float angle) {
    int x, y;
    if (map_.depth != 0 || !useTileInFront(map_, pos_, angle, x, y) || map_.at(x, y) != T_SHOP) return false;
    int st = map_.floorTex[y * map_.w + x];
    if (st >= (int)gData.stores.size()) return false;
    shopIdx_ = st;
    shopSel_ = 0;
    shopSide_ = 0;
    ui_ = Ui::Shop;
    sound_->play(SND_DOOR, 0.6f, 0);
    return true;
}

void Client::shopKey(SDL_Keycode k) {
    const StoreDef& st = gData.stores[shopIdx_];
    int rows = shopSide_ == 0 ? (int)st.items.size() : INV_SIZE;
    if (k == SDLK_ESCAPE) { ui_ = Ui::None; return; }
    if (k == SDLK_TAB || k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_a || k == SDLK_d) {
        shopSide_ = 1 - shopSide_;
        shopSel_ = 0;
        return;
    }
    if (k == SDLK_UP || k == SDLK_w) shopSel_ = (shopSel_ + rows - 1) % std::max(1, rows);
    else if (k == SDLK_DOWN || k == SDLK_s) shopSel_ = (shopSel_ + 1) % std::max(1, rows);
    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_e || k == SDLK_b) {
        if (shopSide_ == 0) action(A_SHOP_BUY, shopIdx_, shopSel_);
        else action(A_SHOP_SELL, shopIdx_, shopSel_);
    }
}

int Client::shopRowAt(int mx, int my, int& side) const {
    const StoreDef& st = gData.stores[shopIdx_];
    int colW = 262;
    for (int sd = 0; sd < 2; sd++) {
        int cx = shopX_ + 8 + sd * (colW + 4);
        if (mx < cx || mx > cx + colW) continue;
        int n = sd == 0 ? (int)st.items.size() : INV_SIZE;
        for (int i = 0; i < n; i++) {
            int ry = shopY_ + 34 + i * 10;
            if (my >= ry - 1 && my < ry + 9) { side = sd; return i; }
        }
    }
    return -1;
}

void Client::drawShop(Renderer& r) {
    const StoreDef& st = gData.stores[shopIdx_];
    int w = 540, h = 270;
    int x = (r.W - w) / 2, y = (r.H - 44 - h) / 2;
    shopX_ = x; shopY_ = y;
    panel(r, x, y, w, h, st.name + (st.owner.empty() ? "" : "  -  " + st.owner));
    int colW = 262;
    r.text(x + 10, y + 20, "For sale", shopSide_ == 0 ? UI_HI : UI_DIM);
    r.text(x + 12 + colW, y + 20, "Your pack (what they pay)", shopSide_ == 1 ? UI_HI : UI_DIM);
    for (size_t i = 0; i < st.items.size(); i++) {
        const ItemDef& d = gData.items[st.items[i]];
        int ry = y + 34 + (int)i * 10;
        bool sel = shopSide_ == 0 && (int)i == shopSel_;
        if (sel) r.fill(x + 6, ry - 1, colW, 10, 0xFF4A3A20);
        int price = buyPrice(d);
        ItemStack s;
        s.def = (int16_t)st.items[i];
        s.count = 1;
        s.fuel = d.fuel;
        std::string n = d.kind == ItemKind::Light ? d.name : itemName(s);
        r.text(x + 10, ry, n.substr(0, 34), price <= me_.gold ? (sel ? 0xFFFFFFFF : UI_TEXT) : 0xFF806050);
        std::string ps = std::to_string(price);
        r.text(x + 6 + colW - 4 - r.textW(ps), ry, ps, 0xFFFFD040);
    }
    for (int i = 0; i < INV_SIZE; i++) {
        const ItemStack& s = me_.inv[i];
        int ry = y + 34 + i * 10;
        bool sel = shopSide_ == 1 && i == shopSel_;
        int cx = x + 12 + colW;
        if (sel) r.fill(cx - 2, ry - 1, colW, 10, 0xFF4A3A20);
        if (s.empty()) { r.text(cx, ry, "-", 0xFF605850); continue; }
        bool buys = gData.storeBuys(shopIdx_, s.d());
        int price = buys ? sellPriceEach(s.d(), s.fuel) : 0;
        r.text(cx, ry, itemName(s).substr(0, 34), buys && price > 0 ? (sel ? 0xFFFFFFFF : UI_TEXT) : 0xFF706860);
        std::string ps = buys ? (price > 0 ? std::to_string(price) : "0") : "-";
        r.text(cx + colW - 6 - r.textW(ps), ry, ps, buys ? 0xFFFFD040 : 0xFF706860);
    }
    std::string g = "Your gold: " + std::to_string(me_.gold);
    r.text(x + 10, y + h - 26, g, 0xFFFFD040);
    // Description of the selected ware
    if (shopSide_ == 0 && shopSel_ < (int)st.items.size()) {
        const ItemDef& d = gData.items[st.items[shopSel_]];
        r.text(x + 10 + r.textW(g) + 16, y + h - 26, d.desc.substr(0, 60), 0xFFC0B8A0);
    }
    r.text(x + 10, y + h - 14, "Enter/double-click: buy or sell one   Tab/Left/Right: switch side   Esc: leave", UI_DIM);
}

// =========================================================== quick slots ===

const SpellDef* Client::quickSpell(int slot) const {
    if (slot < 0 || slot >= NUM_QUICK || !inGame()) return nullptr;
    int v = me_.quick[slot];
    if (v >= QUICK_ITEM) {
        int d = v - QUICK_ITEM;
        return d < (int)gData.items.size() && gData.items[d].spell >= 0 ? &gData.spells[gData.items[d].spell] : nullptr;
    }
    if (v >= 0 && v < (int)me_.c().spells.size()) return &gData.spells[me_.c().spells[v]];
    return nullptr;
}

int Client::quickInvSlot(int value) const {
    if (value < QUICK_ITEM) return -1;
    int def = value - QUICK_ITEM, best = -1;
    for (int i = 0; i < INV_SIZE; i++) {
        const ItemStack& it = me_.inv[i];
        if (it.def != def || it.empty()) continue;
        // Prefer a wand with charges / a charged rod.
        bool ready = it.d().kind == ItemKind::Wand ? it.fuel > 0 : it.fuel == 0;
        if (best < 0 || ready) best = i;
        if (ready) break;
    }
    return best;
}

std::string Client::quickName(int value) const {
    if (value >= QUICK_ITEM) return value - QUICK_ITEM < (int)gData.items.size() ? gData.items[value - QUICK_ITEM].name : "?";
    if (value >= 0 && value < (int)me_.c().spells.size()) return gData.spells[me_.c().spells[value]].name;
    return "";
}

// Use what is in a quick slot. Returns false when it can't be used right now.
bool Client::useQuick(int slot, bool quiet) {
    if (me_.dead || slot < 0 || slot >= NUM_QUICK) return false;
    int v = me_.quick[slot];
    auto say = [&](const std::string& t) { if (!quiet) addLog(t, UI_DIM); };
    if (v < 0) { say("Nothing on that key. Open the spell book (" + keys_->label(K_SPELLBOOK) + ") to choose."); return false; }
    if (localCastCd_ > 0) return true;
    const SpellDef* s = quickSpell(slot);
    if (!s) return false;
    if (v >= QUICK_ITEM) {
        int inv = quickInvSlot(v);
        if (inv < 0) { say("You don't have a " + quickName(v) + "."); return false; }
        const ItemStack& it = me_.inv[inv];
        if (it.d().kind == ItemKind::Wand && it.fuel <= 0) { say("The " + it.d().name + " has no charges left."); return false; }
        if (it.d().kind == ItemKind::Rod && it.fuel > 0) { say("The " + it.d().name + " is still charging."); return false; }
        localCastCd_ = std::max(0.6f, s->cooldown * me_.cdMult());
        castAnim_ = 1.0f;
        castSpellAnim_ = it.d().spell;
        sound_->play(SND_CAST, 0.7f, 0);
        action(A_INV_USE, inv);
        return true;
    }
    if (!me_.knowsSpell(v)) { say(s->name + " needs level " + std::to_string(me_.c().spellLevels[v]) + "."); return false; }
    float rc = me_.rechargeLeft(v);
    if (rc > 0) { say(s->name + " is not ready yet (" + std::to_string((int)std::ceil(rc)) + "s)."); localCastCd_ = 0.4f; return false; }
    if (me_.mp < s->mana) { say("Not enough mana for " + s->name + "."); localCastCd_ = 0.5f; return false; }
    localCastCd_ = s->cooldown * me_.cdMult();
    castAnim_ = 1.0f;
    castSpellAnim_ = me_.c().spells[v];
    sound_->play(SND_CAST, 0.7f, 0);
    action(A_CAST, v);
    // Abilities with a recharge fire once per press.
    return s->recharge <= 0;
}

void Client::cycleQuick(int dir) {
    for (int k = 1; k <= NUM_QUICK; k++) {
        int i = ((selSpell_ + dir * k) % NUM_QUICK + NUM_QUICK) % NUM_QUICK;
        if (me_.quick[i] >= 0) { selSpell_ = i; return; }
    }
}

// ============================================================ spell book ===

std::vector<int> Client::bookEntries() const {
    std::vector<int> v;
    if (!inGame()) return v;
    for (size_t i = 0; i < me_.c().spells.size(); i++) v.push_back((int)i);
    for (int i = 0; i < INV_SIZE; i++) {
        const ItemStack& it = me_.inv[i];
        if (it.empty() || (it.d().kind != ItemKind::Wand && it.d().kind != ItemKind::Rod)) continue;
        int val = QUICK_ITEM + it.def;
        if (std::find(v.begin(), v.end(), val) == v.end()) v.push_back(val);
    }
    // Devices that are on a key but no longer carried still show (so they can be removed).
    for (int q : me_.quick)
        if (q >= QUICK_ITEM && std::find(v.begin(), v.end(), q) == v.end()) v.push_back(q);
    return v;
}

namespace {
constexpr int BOOK_W = 470, BOOK_ROWS = 19;
}

int Client::bookRowAt(int mx, int my) const {
    int n = (int)bookEntries().size();
    if (mx < bookX_ || mx > bookX_ + BOOK_W) return -1;
    int row = (my - (bookY_ + 30)) / 10;
    if (my < bookY_ + 30 || row >= BOOK_ROWS) return -1;
    row += bookScroll_;
    return row < n ? row : -1;
}

void Client::bookKey(SDL_Keycode k) {
    auto e = bookEntries();
    int n = (int)e.size();
    if (n == 0) { if (k == SDLK_RETURN || k == SDLK_ESCAPE) ui_ = Ui::None; return; }
    bookSel_ = clampi(bookSel_, 0, n - 1);
    if (k == SDLK_UP || k == SDLK_w) bookSel_ = (bookSel_ + n - 1) % n;
    else if (k == SDLK_DOWN || k == SDLK_s) bookSel_ = (bookSel_ + 1) % n;
    else if (k == SDLK_PAGEUP) bookSel_ = std::max(0, bookSel_ - 5);
    else if (k == SDLK_PAGEDOWN) bookSel_ = std::min(n - 1, bookSel_ + 5);
    int v = e[bookSel_];
    int cur = -1;
    for (int i = 0; i < NUM_QUICK; i++) if (me_.quick[i] == v) cur = i;
    if ((k >= SDLK_1 && k <= SDLK_9) || (k >= SDLK_KP_1 && k <= SDLK_KP_9)) {
        int slot = k >= SDLK_KP_1 ? (int)(k - SDLK_KP_1) : (int)(k - SDLK_1);
        if (cur == slot) { action(A_SET_QUICK, slot, -1); me_.quick[slot] = -1; }
        else {
            action(A_SET_QUICK, slot, v);
            for (auto& q : me_.quick) if (q == v) q = -1;
            me_.quick[slot] = (int16_t)v;
            selSpell_ = slot;
        }
    } else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
        if (cur >= 0) { action(A_SET_QUICK, cur, -1); me_.quick[cur] = -1; }
    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_e) {
        // Select it (putting it on the first free key if needed) and go back to the game.
        if (cur < 0) {
            for (int i = 0; i < NUM_QUICK && cur < 0; i++)
                if (me_.quick[i] < 0) cur = i;
            if (cur < 0) { addLog("All 9 keys are in use: press 1-9 to replace one.", UI_DIM); return; }
            action(A_SET_QUICK, cur, v);
            me_.quick[cur] = (int16_t)v;
        }
        selSpell_ = cur;
        ui_ = Ui::None;
    }
    if (bookSel_ < bookScroll_) bookScroll_ = bookSel_;
    if (bookSel_ >= bookScroll_ + BOOK_ROWS) bookScroll_ = bookSel_ - BOOK_ROWS + 1;
}

std::string Client::spellInfo(const SpellDef& s, int power) const {
    static const char* en[] = {"physical", "arcane", "fire", "cold", "holy", "poison"};
    auto amount = [&]() {
        int bonus = (int)(s.scale * me_.level) + power;
        return s.dmg.str() + (bonus ? (bonus > 0 ? "+" : "") + std::to_string(bonus) : "");
    };
    std::string t;
    auto add = [&](const std::string& x) { t += (t.empty() ? "" : ", ") + x; };
    switch (s.type) {
        case SpellType::Bolt: add(std::string(en[(int)s.elem]) + " bolt"); break;
        case SpellType::Ball: add(std::string(en[(int)s.elem]) + " ball, radius " + std::to_string((int)(s.radius + 0.5f))); break;
        case SpellType::Nova: add("burst around you, radius " + std::to_string((int)(s.radius + 0.5f))); break;
        default: break;
    }
    if (s.dmg.n > 0) add((s.type == SpellType::Heal || s.type == SpellType::Berserk ? "heals " : "damage ") + amount());
    if (s.effect == SpellEffect::Sleep) add("puts monsters to sleep");
    if (s.effect == SpellEffect::Fear) add("frightens monsters");
    if (s.effect == SpellEffect::Slow) add("slows monsters");
    if (s.duration > 0 && s.type != SpellType::Recall && s.type != SpellType::Detect) add("lasts " + std::to_string((int)s.duration) + "s");
    if (s.mana > 0) add(std::to_string(s.mana) + " mana");
    if (s.recharge > 0) add("ready every " + std::to_string((int)s.recharge) + "s");
    return t;
}

void Client::drawSpellBook(Renderer& r) {
    const ClassDef& c = me_.c();
    bool allAbilities = !c.spells.empty();
    for (int sp : c.spells) if (!gData.spells[sp].ability) allAbilities = false;
    int h = 30 + BOOK_ROWS * 10 + 58;
    int x = (r.W - BOOK_W) / 2, y = std::max(2, (r.H - 44 - h) / 2);
    bookX_ = x; bookY_ = y;
    panel(r, x, y, BOOK_W, h, allAbilities ? "Fighting techniques" : c.spells.empty() ? "Wands and rods" : "Spell book");
    r.text(x + 10, y + 19, "Key", UI_DIM);
    r.text(x + 36, y + 19, "Name", UI_DIM);
    r.text(x + 210, y + 19, "Level", UI_DIM);
    r.text(x + 250, y + 19, "Cost / uses", UI_DIM);
    auto e = bookEntries();
    if (e.empty()) r.text(x + 36, y + 32, "You know no spells and carry no wands or rods.", UI_DIM);
    bookSel_ = clampi(bookSel_, 0, std::max(0, (int)e.size() - 1));
    for (int row = 0; row < BOOK_ROWS && bookScroll_ + row < (int)e.size(); row++) {
        int i = bookScroll_ + row, v = e[i];
        int ry = y + 30 + row * 10;
        bool sel = i == bookSel_;
        if (sel) r.fill(x + 6, ry - 1, BOOK_W - 12, 10, 0xFF3A3060);
        int key = -1;
        for (int q = 0; q < NUM_QUICK; q++) if (me_.quick[q] == v) key = q;
        if (key >= 0) r.text(x + 14, ry, std::to_string(key + 1), UI_HI);
        std::string name = quickName(v), lvl, cost;
        uint32_t col = UI_TEXT;
        if (v >= QUICK_ITEM) {
            int inv = quickInvSlot(v);
            const ItemDef& d = gData.items[v - QUICK_ITEM];
            lvl = d.kind == ItemKind::Wand ? "wand" : "rod";
            if (inv < 0) { cost = "not carried"; col = 0xFF605870; }
            else if (d.kind == ItemKind::Wand) cost = std::to_string(me_.inv[inv].fuel) + " charges";
            else cost = me_.inv[inv].fuel > 0 ? "charging" : "ready";
            col = inv < 0 ? col : 0xFFA0D0FF;
        } else {
            const SpellDef& s = gData.spells[c.spells[v]];
            bool known = me_.knowsSpell(v);
            lvl = std::to_string(c.spellLevels[v]);
            cost = s.mana > 0 ? std::to_string(s.mana) + " mana" : s.recharge > 0 ? "every " + std::to_string((int)s.recharge) + "s" : "free";
            if (known && me_.rechargeLeft(v) > 0) cost += " (" + std::to_string((int)std::ceil(me_.rechargeLeft(v))) + "s)";
            col = known ? (s.ability ? 0xFFFFC090 : 0xFFB0A8E0) : 0xFF605870;
            if (!known) cost = "not learned yet";
        }
        r.text(x + 36, ry, name, sel ? 0xFFFFFFFF : col);
        r.text(x + 210, ry, lvl, col);
        r.text(x + 250, ry, cost, col);
    }
    if ((int)e.size() > BOOK_ROWS) r.text(x + BOOK_W - 60, y + 19, std::to_string(bookScroll_ + 1) + "-" + std::to_string(std::min((int)e.size(), bookScroll_ + BOOK_ROWS)) + "/" + std::to_string(e.size()), UI_DIM);

    // Details of the selected entry.
    int dy = y + 30 + BOOK_ROWS * 10 + 4;
    r.fill(x + 6, dy - 2, BOOK_W - 12, 1, colMul(UI_EDGE, 0.6f));
    if (!e.empty()) {
        int v = e[bookSel_];
        const SpellDef* s = nullptr;
        std::string info;
        if (v >= QUICK_ITEM) {
            const ItemDef& d = gData.items[v - QUICK_ITEM];
            if (d.spell >= 0) s = &gData.spells[d.spell];
            if (s) info = spellInfo(*s, me_.devicePower()) + ", fail " + std::to_string(me_.deviceFail(d)) + "%";
            if (d.kind == ItemKind::Rod) info += ", recharges in " + std::to_string(d.recharge) + "s";
        } else {
            s = &gData.spells[c.spells[v]];
            info = spellInfo(*s, me_.spellPower());
        }
        if (s) {
            std::string desc = s->desc;
            size_t cut = desc.size() > 76 ? desc.rfind(' ', 76) : std::string::npos;
            r.text(x + 10, dy + 2, cut == std::string::npos ? desc : desc.substr(0, cut), 0xFFC0B8A0);
            if (cut != std::string::npos) r.text(x + 10, dy + 12, desc.substr(cut + 1).substr(0, 76), 0xFFC0B8A0);
            r.text(x + 10, dy + 23, info.substr(0, 78), 0xFF90D0A0);
        }
    }
    r.text(x + 10, dy + 37, "1-9: put on key (again: take off)  Del: take off  Enter: choose  Esc: close", UI_DIM);
}
