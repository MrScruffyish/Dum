// Dum - shared state serialization and rule helpers.
#include "game/state.h"

const char* kEquipNames[EQ_COUNT] = {"Weapon", "Armor", "Light", "Helm", "Ring", "Amulet"};

int equipSlotFor(const ItemDef& d) {
    switch (d.kind) {
        case ItemKind::Weapon: return EQ_WEAPON;
        case ItemKind::Armor: return EQ_ARMOR;
        case ItemKind::Light: return EQ_LIGHT;
        case ItemKind::Helm: return EQ_HELM;
        case ItemKind::Ring: return EQ_RING;
        case ItemKind::Amulet: return EQ_AMULET;
        default: return -1;
    }
}

void EntSnap::write(Writer& w) const {
    w.u32(id);
    w.u8(type);
    w.u16(def);
    w.u16((uint16_t)clampf(pos.x * 100.0f, 0, 65535));
    w.u16((uint16_t)clampf(pos.y * 100.0f, 0, 65535));
    w.u8((uint8_t)((int)(wrapAngle(angle) / (2 * PI) * 256.0f) & 255));
    w.u8(flags);
    w.u8(hp);
    w.u8(extra);
}

void EntSnap::read(Reader& r) {
    id = r.u32();
    type = r.u8();
    def = r.u16();
    pos.x = r.u16() / 100.0f;
    pos.y = r.u16() / 100.0f;
    angle = (int8_t)r.u8() / 256.0f * 2 * PI;
    flags = r.u8();
    hp = r.u8();
    extra = r.u8();
}

std::string itemName(const ItemStack& s) {
    if (s.empty()) return "(nothing)";
    const ItemDef& d = s.d();
    std::string n = d.name;
    if (d.kind == ItemKind::Gold) return std::to_string(s.count) + " gold";
    if (s.count > 1) n = std::to_string(s.count) + " x " + n;
    auto sg = [](int v) { return (v >= 0 ? "+" : "") + std::to_string(v); };
    if (d.kind == ItemKind::Weapon) n += " (" + d.dmg.str() + ")";
    if (d.toHit || d.toDam) n += " (" + sg(d.toHit) + "," + sg(d.toDam) + ")";
    if (d.armor && d.kind != ItemKind::Weapon) n += " [" + (d.kind == ItemKind::Armor || d.kind == ItemKind::Helm ? std::to_string(d.armor) : sg(d.armor)) + "]";
    if (d.kind == ItemKind::Wand) n += " (" + std::to_string(s.fuel) + (s.fuel == 1 ? " charge)" : " charges)");
    if (d.kind == ItemKind::Rod && s.fuel > 0) n += " (charging)";
    if (d.kind == ItemKind::Light && !d.everburning) n += " (" + std::to_string(s.fuel / 60) + ":" + (s.fuel % 60 < 10 ? "0" : "") + std::to_string(s.fuel % 60) + ")";
    // Other bonuses in angle brackets.
    std::string b;
    auto add = [&](const std::string& x) { b += (b.empty() ? "" : ", ") + x; };
    for (int i = 0; i < NUM_STATS; i++) if (d.statBonus[i]) add(sg(d.statBonus[i]) + " " + kStatNames[i]);
    if (d.spellPower) add(sg(d.spellPower) + " spell");
    if (d.speed) add(sg(d.speed) + " speed");
    if (d.stealth) add(sg(d.stealth) + " stealth");
    if (d.search) add(sg(d.search) + " search");
    if (d.light && d.kind != ItemKind::Light) add(sg(d.light) + " light");
    if (d.slowDigest) add("slow hunger");
    if (d.regen) add("regeneration");
    static const char* en[] = {"physical", "arcane", "fire", "cold", "holy", "poison"};
    if (d.brand != Element::Physical) add(std::string(en[(int)d.brand]) + " brand");
    for (Element e : d.resist) add(std::string("resist ") + en[(int)e]);
    if (d.everburning) add("never goes out");
    if (!b.empty()) n += " <" + b + ">";
    return n;
}

int PlayerState::armorClass() const {
    int ac = 10 + mod(DEX) + equipSum([](const ItemDef& d) { return d.armor; });
    if (blessT > 0) ac += 2;
    if (shieldT > 0) ac += 8;
    if (heroT > 0) ac -= 2;
    return ac;
}

int PlayerState::lightRadius() const {
    int r = 0;
    if (!equip[EQ_LIGHT].empty() && equip[EQ_LIGHT].fuel > 0) {
        const ItemDef& d = equip[EQ_LIGHT].d();
        r = d.light;
        if (d.id == "torch" && equip[EQ_LIGHT].fuel > d.fuel) r++;  // combined torches burn brighter
    }
    // Glowing equipment (not the light itself) adds to the radius, and gives a little light alone.
    int glow = 0;
    for (int i = 0; i < EQ_COUNT; i++)
        if (i != EQ_LIGHT && !equip[i].empty()) glow += equip[i].d().light;
    if (glow > 0) r = r > 0 ? r + glow : glow + 1;
    if (lightT > 0) r = std::max(r, 7);
    return r;
}

int PlayerState::spellPower() const {
    int p = c().magic / 2 + mod(c().castStat) + equipSum([](const ItemDef& d) { return d.spellPower; });
    return p;
}

bool PlayerState::knowsSpell(int i) const {
    return i >= 0 && i < (int)c().spells.size() && level >= c().spellLevels[i];
}

void PlayerState::autoQuick(int value) {
    for (int q : quick) if (q == value) return;
    for (auto& q : quick) if (q < 0) { q = (int16_t)value; return; }
}

void PlayerState::write(Writer& w) const {
    w.u16(id);
    w.str(name);
    w.u8(cls);
    w.i16((int16_t)level);
    w.i32(xp);
    for (int s : stats) w.u8((uint8_t)s);
    w.i16((int16_t)hp);
    w.i16((int16_t)maxHp);
    w.f32(mp);
    w.f32(maxMp);
    w.f32(food);
    w.i32(gold);
    for (auto& s : inv) s.write(w);
    for (auto& s : equip) s.write(w);
    w.f32(blessT); w.f32(lightT); w.f32(detectT); w.f32(slowT);
    w.f32(hasteT); w.f32(shieldT); w.f32(resistT); w.f32(heroT);
    w.f32(poisonT);
    for (int d : drained) w.u8((uint8_t)d);
    w.f32(recallT);
    for (auto q : quick) w.i16(q);
    w.u8((uint8_t)recharge.size());
    for (float t : recharge) w.f32(t);
    w.f32(attackCd); w.f32(castCd);
    w.u8(dead);
    w.f32(respawnT);
    w.i32(kills);
}

void PlayerState::read(Reader& r) {
    id = r.u16();
    name = r.str();
    cls = r.u8();
    level = r.i16();
    xp = r.i32();
    for (int& s : stats) s = r.u8();
    hp = r.i16();
    maxHp = r.i16();
    mp = r.f32();
    maxMp = r.f32();
    food = r.f32();
    gold = r.i32();
    for (auto& s : inv) s.read(r);
    for (auto& s : equip) s.read(r);
    blessT = r.f32(); lightT = r.f32(); detectT = r.f32(); slowT = r.f32();
    hasteT = r.f32(); shieldT = r.f32(); resistT = r.f32(); heroT = r.f32();
    poisonT = r.f32();
    for (int& d : drained) d = r.u8();
    recallT = r.f32();
    for (auto& q : quick) q = r.i16();
    recharge.resize(r.u8());
    for (float& t : recharge) t = r.f32();
    attackCd = r.f32(); castCd = r.f32();
    dead = r.u8();
    respawnT = r.f32();
    kills = r.i32();
}

void Quest::write(Writer& w) const {
    w.u8(type); w.u8(main); w.u8(done);
    w.i16((int16_t)depth); w.i16((int16_t)target);
    w.i16((int16_t)count); w.i16((int16_t)needed);
    w.str(text);
}

void Quest::read(Reader& r) {
    type = r.u8(); main = r.u8(); done = r.u8();
    depth = r.i16(); target = r.i16();
    count = r.i16(); needed = r.i16();
    text = r.str();
}
