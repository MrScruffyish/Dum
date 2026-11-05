// Dum - headless self test: dungeon generation and a simulated game with bots.
// Build target: dum_selftest. Run from the build directory.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <memory>

#include "game/data.h"
#include "game/map.h"
#include "game/rules.h"
#include "game/state.h"
#include "net/net.h"
#include "server/server.h"
#include "game/scores.h"

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); failures++; } } while (0)

// A bot that connects through a loopback pipe, walks around and attacks.
// The data file checksums every hello carries. wrong = a file to pretend is different.
void dataChecksums(Writer& w, const std::string& wrong = "") {
    w.u8((uint8_t)gData.shared.size());
    for (auto& sf : gData.shared) { w.str(sf.name); w.u32(sf.name == wrong ? sf.hash ^ 1u : sf.hash); }
}

struct Bot {
    std::shared_ptr<LoopbackPipe> pipe = std::make_shared<LoopbackPipe>();
    ClientNet net;
    Map map;
    PlayerState st;
    uint16_t id = 0;
    Vec2 pos;
    float angle = 0;
    int msgs = 0, snapshots = 0, levels = 0;
    std::vector<EntSnap> ents;
    std::string rejected;   // reason, if the server refused us
    std::string wrongFile;  // pretend this data file differs from the server's
    Bot() { net.useLoopback(pipe); }

    void hello(int cls) {
        Writer w;
        w.u8(C_HELLO);
        w.u16(PROTOCOL_VERSION);
        w.str("Bot" + std::to_string(cls));
        w.u8((uint8_t)cls);
        dataChecksums(w, wrongFile);
        net.send(w.buf, true);
    }
    void action(ActionId a, int x = 0, int y = 0) {
        Writer w;
        w.u8(C_ACTION); w.u8(a); w.i32(x); w.i32(y);
        net.send(w.buf, true);
    }
    void poll() {
        std::vector<NetEvent> evs;
        net.poll(evs);
        for (auto& e : evs) {
            if (e.type != NetEvent::Data) continue;
            msgs++;
            Reader r(e.data);
            uint8_t t = r.u8();
            if (t == S_WELCOME) id = r.u16();
            else if (t == S_REJECT) rejected = r.str();
            else if (t == S_LEVEL) { map.deserialize(r); levels++; }
            else if (t == S_TILE) { int x = r.u16(), y = r.u16(); map.set(x, y, r.u8()); }
            else if (t == S_TELEPORT) { pos.x = r.f32(); pos.y = r.f32(); angle = r.f32(); }
            else if (t == S_PSTATE) { r.u32(); st.read(r); }
            else if (t == S_TEXT && std::getenv("BOT_LOG")) { r.u32(); std::printf("  bot%d log: %s\n", id, r.str().c_str()); }
            else if (t == S_SNAPSHOT) {
                r.u32();
                int n = r.u16();
                ents.clear();
                for (int i = 0; i < n; i++) { EntSnap s; s.read(r); ents.push_back(s); }
                snapshots++;
            }
        }
    }
    // Wander: walk forward, turn when blocked, face the nearest monster and attack/cast.
    void think(Rng& rng) {
        if (!map.w || st.dead) return;
        float best = 1e9f;
        Vec2 tgt;
        for (auto& e : ents)
            if (e.type == ET_MONSTER && dist(e.pos, pos) < best && map.los(pos, e.pos)) { best = dist(e.pos, pos); tgt = e.pos; }
        if (best < 8) {
            angle = std::atan2(tgt.y - pos.y, tgt.x - pos.x) + (rng.frand() - 0.5f) * 0.4f;  // sloppy aim on purpose
            action(A_ATTACK);
            action(A_CAST, 0);
        }
        Vec2 step = dirFromAngle(angle) * (3.4f * TICK_DT);
        if (best > 1.2f && !map.moveCircle(pos, step, PLAYER_RADIUS)) angle += 1.3f + rng.frand();
        uint8_t under = map.at((int)pos.x, (int)pos.y);
        if (rng.chance(2) && under != T_STAIRS_UP && under != T_STAIRS_DOWN) action(A_USE);
        if (st.food < 300) for (int i = 0; i < INV_SIZE; i++) if (!st.inv[i].empty() && st.inv[i].d().kind == ItemKind::Food) { action(A_INV_USE, i); break; }
        Writer w;
        w.u8(C_INPUT); w.f32(pos.x); w.f32(pos.y); w.f32(angle); w.u8(0);
        net.send(w.buf, false);
    }
};

int main(int argc, char** argv) {
    std::string err;
    std::string dir = findDataDir(argv[0]);
    if (!gData.load(dir, err)) { std::printf("data load failed: %s\n", err.c_str()); return 1; }
    if (argc >= 4 && std::string(argv[1]) == "--map") {  // dum_selftest --map <seed> <depth>: print a level
        Map m;
        generateLevel(m, (uint32_t)std::atoi(argv[2]), std::atoi(argv[3]));
        for (int y = 0; y < m.h; y++) {
            std::string row;
            for (int x = 0; x < m.w; x++) {
                const char* c = " #.+'LS><:$";
                row += c[m.at(x, y)];
            }
            std::printf("%s\n", row.c_str());
        }
        return 0;
    }
    std::printf("data: %zu classes, %zu items, %zu monsters, %zu spells\n", gData.classes.size(), gData.items.size(), gData.monsters.size(), gData.spells.size());

    // 1. Dungeon generation: many seeds/depths, all connected, deterministic.
    for (uint32_t seed = 1; seed <= 60; seed++)
        for (int depth = 1; depth <= 10; depth += 3) {
            Map a, b;
            generateLevel(a, seed, depth);
            generateLevel(b, seed, depth);
            CHECK(a.tiles == b.tiles, "generation not deterministic seed %u depth %d", seed, depth);
            CHECK(a.at(a.startX, a.startY) == T_STAIRS_UP, "no up stairs");
            CHECK(a.at(a.downX, a.downY) == T_STAIRS_DOWN, "no down stairs");
            CHECK(a.rooms.size() >= 4, "too few rooms");
            // serialization round trip
            Writer w;
            a.serialize(w, false);
            Reader r(w.buf);
            Map c;
            CHECK(c.deserialize(r) && c.tiles == a.tiles, "map serialization");
        }
    std::printf("dungeon generation ok\n");

    {   // Corridor assist: walking at a wall just beside a corridor opening slides you into it.
        Map m;
        m.resize(12, 12);
        for (int y = 1; y < 11; y++)
            for (int x = 1; x < 11; x++) m.set(x, y, (y >= 6 || x == 5) ? T_FLOOR : T_WALL);
        // Room at the bottom (y >= 6), a 1-wide corridor going north at x = 5.
        for (float off : {-0.45f, -0.3f, 0.3f, 0.45f}) {
            Vec2 p(5.5f + off, 7.5f);
            for (int i = 0; i < 60; i++) p = assistedMove(m, p, Vec2(0, -3.4f * TICK_DT), PLAYER_RADIUS);
            CHECK(p.y < 4.5f, "corridor assist: offset %.2f ended at %.2f,%.2f", off, p.x, p.y);
        }
        // Far from any opening you still just stop at the wall.
        Vec2 q(2.5f, 7.5f);
        for (int i = 0; i < 60; i++) q = assistedMove(m, q, Vec2(0, -3.4f * TICK_DT), PLAYER_RADIUS);
        CHECK(q.y > 5.9f && std::fabs(q.x - 2.5f) < 0.01f, "assist moved player without an opening (%.2f,%.2f)", q.x, q.y);
        std::printf("corridor assist ok\n");
    }

    // 2. Simulated game: 3 bots of different classes play for 3 minutes of game time.
    auto snet = std::make_shared<ServerNet>();
    Bot bots[3];
    // ServerNet supports one loopback peer; extra bots use their own server-side peers via
    // a tiny shim: we attach bots one at a time to separate servers is not possible, so the
    // first bot is local and the others connect over real ENet on localhost.
    snet->attachLoopback(bots[0].pipe);
    bool enetOk = snet->listen(27999, 8, err);
    CHECK(enetOk, "listen failed: %s", err.c_str());
    Server server(snet, 12345);
    ClientNet remote[2];
    for (auto& rc : remote) CHECK(rc.connect("127.0.0.1", 27999, err), "connect: %s", err.c_str());

    Rng rng(99);
    bots[0].hello(0);
    for (int t = 0; t < 10; t++) { server.tick(); bots[0].poll(); }
    {   // Combining torches: the warrior starts with a spare torch in the pack.
        int slot = -1;
        for (int i = 0; i < INV_SIZE; i++)
            if (!bots[0].st.inv[i].empty() && bots[0].st.inv[i].d().id == "torch") slot = i;
        CHECK(slot >= 0, "no spare torch");
        int before = bots[0].st.equip[EQ_LIGHT].fuel;
        int radius = bots[0].st.lightRadius();
        bots[0].action(A_INV_USE, slot);
        for (int t = 0; t < 5; t++) { server.tick(); bots[0].poll(); }
        CHECK(bots[0].st.equip[EQ_LIGHT].fuel > before + 500, "torch fuel %d -> %d", before, bots[0].st.equip[EQ_LIGHT].fuel);
        CHECK(slot < 0 || bots[0].st.inv[slot].empty(), "spare torch not used up");
        CHECK(bots[0].st.lightRadius() == radius + 1, "combined torch not brighter");
        std::printf("torch combining ok\n");
    }
    {   // Town: we start there. Buy something from the General Store and sell it back.
        CHECK(server.map().depth == 0, "game does not start in town");
        const Map& tm = server.map();
        int sx = -1, sy = -1;
        for (int y = 0; y < tm.h; y++)
            for (int x = 0; x < tm.w; x++)
                if (tm.at(x, y) == T_SHOP && tm.floorTex[y * tm.w + x] == 0) { sx = x; sy = y; }
        CHECK(sx >= 0, "no general store in town");
        server.debugMovePlayer(bots[0].id, Vec2(sx + 0.5f, sy + 1.5f));
        for (int t = 0; t < 5; t++) { server.tick(); bots[0].poll(); }
        int gold = bots[0].st.gold;
        const StoreDef& st = gData.stores[0];
        int ware = 0;  // ration
        bots[0].action(A_SHOP_BUY, 0, ware);
        for (int t = 0; t < 5; t++) { server.tick(); bots[0].poll(); }
        CHECK(bots[0].st.gold == gold - buyPrice(gData.items[st.items[ware]]), "buying: gold %d -> %d", gold, bots[0].st.gold);
        int slot = -1;
        for (int i = 0; i < INV_SIZE; i++)
            if (!bots[0].st.inv[i].empty() && bots[0].st.inv[i].def == st.items[ware]) slot = i;
        CHECK(slot >= 0, "bought item not in pack");
        gold = bots[0].st.gold;
        bots[0].action(A_SHOP_SELL, 0, slot);
        for (int t = 0; t < 5; t++) { server.tick(); bots[0].poll(); }
        CHECK(bots[0].st.gold == gold + sellPriceEach(gData.items[st.items[ware]], 0), "selling: gold %d -> %d", gold, bots[0].st.gold);
        std::printf("town shops ok\n");
    }
    {   // Auto-pickup: a dropped item must stay on the floor for the one who dropped it.
        int before = server.floorItemCount();
        int slot = -1;
        for (int i = 0; i < INV_SIZE; i++) if (!bots[0].st.inv[i].empty()) { slot = i; break; }
        CHECK(slot >= 0, "bot has no items");
        bots[0].action(A_INV_DROP, slot, 0);
        for (int t = 0; t < 60; t++) { server.tick(); bots[0].poll(); }
        CHECK(server.floorItemCount() == before + 1, "dropped item was re-picked (%d -> %d)", before, server.floorItemCount());
        bots[0].action(A_USE);  // E picks it up again manually
        for (int t = 0; t < 10; t++) { server.tick(); bots[0].poll(); }
        CHECK(server.floorItemCount() == before, "manual pickup failed");
        std::printf("auto-pickup ok\n");
    }
    bool sentHello[2] = {false, false};
    uint16_t remoteIds[2] = {0, 0};
    int remoteSnaps[2] = {0, 0};
    int ticks = 30 * 180;
    int maxDepth = 0;
    for (int t = 0; t < ticks; t++) {
        server.tick();
        bots[0].poll();
        bots[0].think(rng);
        for (int i = 0; i < 2; i++) {
            std::vector<NetEvent> evs;
            remote[i].poll(evs);
            for (auto& e : evs) {
                if (e.type == NetEvent::Connect && !sentHello[i]) {
                    Writer w;
                    w.u8(C_HELLO); w.u16(PROTOCOL_VERSION); w.str("Remote" + std::to_string(i)); w.u8((uint8_t)(i + 2));
                    dataChecksums(w);
                    remote[i].send(w.buf, true);
                    sentHello[i] = true;
                } else if (e.type == NetEvent::Data) {
                    Reader r(e.data);
                    uint8_t ty = r.u8();
                    if (ty == S_WELCOME) remoteIds[i] = r.u16();
                    if (ty == S_SNAPSHOT) remoteSnaps[i]++;
                }
            }
        }
        maxDepth = std::max(maxDepth, server.map().depth);
        // Every 40 s, force the party down a level to exercise level changes.
        if (t % (30 * 40) == 30 * 40 - 1) {
            server.debugMovePlayer(bots[0].id, Vec2(server.map().downX + 0.5f, server.map().downY + 0.5f));
            server.tick();
            bots[0].poll();
            // remove nearby items from consideration: USE picks items first, so press a few times
            for (int k = 0; k < 6 && server.map().depth == maxDepth; k++) {
                bots[0].action(A_USE);
                for (int j = 0; j < 9; j++) server.tick();
                bots[0].poll();
            }
        }
    }
    {   // Word of Recall: from the dungeon to town, then back down to the deepest level.
        server.debugRevive(bots[0].id);
        for (int t = 0; t < 3; t++) { server.tick(); bots[0].poll(); }
        int deepest = server.maxDepthReached();
        int slot = -1;
        for (int i = 0; i < INV_SIZE; i++)
            if (!bots[0].st.inv[i].empty() && bots[0].st.inv[i].d().id == "scroll_recall") slot = i;
        if (slot < 0) std::printf("(bot lost its recall scroll, skipping recall test)\n");
        else {
            bool inDungeon = server.map().depth > 0;
            bots[0].action(A_INV_USE, slot);
            for (int t = 0; t < 30 * 27; t++) { server.tick(); bots[0].poll(); }
            CHECK(server.map().depth == (inDungeon ? 0 : std::max(1, deepest)), "recall went to depth %d (deepest %d)", server.map().depth, deepest);
            std::printf("word of recall ok\n");
        }
    }
    {   // Artifacts: unique, never-ending light, brand; one left on the floor returns to the pool.
        auto n4 = std::make_shared<ServerNet>();
        Bot b;
        n4->attachLoopback(b.pipe);
        Server s4(n4, 5);
        b.hello(0);
        for (int t = 0; t < 5; t++) { s4.tick(); b.poll(); }
        s4.debugGoto(2);
        s4.debugGive(b.id, "art_phial,art_dawnbringer");
        for (int t = 0; t < 5; t++) { s4.tick(); b.poll(); }
        CHECK(s4.debugArtifactInPlay("art_phial") && s4.debugArtifactInPlay("art_dawnbringer"), "given artifacts not in play");
        for (int i = 0; i < INV_SIZE; i++)
            if (!b.st.inv[i].empty() && b.st.inv[i].d().artifact) b.action(A_INV_USE, i);
        for (int t = 0; t < 30 * 5; t++) { s4.tick(); b.poll(); }
        CHECK(b.st.equip[EQ_LIGHT].d().id == "art_phial" && b.st.equip[EQ_LIGHT].fuel == 1, "phial not equipped or burning");
        CHECK(b.st.lightRadius() == 6, "phial + sword light radius %d (want 5+1)", b.st.lightRadius());
        CHECK(b.st.weaponBrand() == Element::Fire && b.st.resists(Element::Cold), "dawnbringer brand/resist");
        s4.debugDrop(b.id, "art_frostbite");
        CHECK(s4.debugArtifactInPlay("art_frostbite"), "dropped artifact not in play");
        s4.debugGoto(3);  // leave it lying on level 2
        CHECK(!s4.debugArtifactInPlay("art_frostbite"), "artifact left on a level did not return to the pool");
        CHECK(s4.debugArtifactInPlay("art_phial"), "carried artifact was lost");
        std::printf("artifacts ok\n");
    }
    {   // Keys, coloured doors and chests.
        auto nk = std::make_shared<ServerNet>();
        Bot b;
        nk->attachLoopback(b.pipe);
        Server sk(nk, 4242);
        b.hello(3);   // rogue
        for (int t = 0; t < 5; t++) { sk.tick(); b.poll(); }
        int vaults = 0, chests = 0, keysOk = 0;
        for (int depth = 2; depth <= 14 && vaults < 4; depth++) {
            for (int again = 0; again < 3; again++) {
                sk.debugGoto(depth);
                const Map& m = sk.map();
                // Flood from the stairs without passing coloured doors.
                std::vector<uint8_t> reach(m.w * m.h, 0);
                std::vector<int> st = {m.startY * m.w + m.startX};
                reach[st[0]] = 1;
                while (!st.empty()) {
                    int c = st.back(); st.pop_back();
                    int x = c % m.w, y = c / m.w;
                    for (int k = 0; k < 4; k++) {
                        int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
                        uint8_t t = m.at(nx, ny);
                        if (!m.inside(nx, ny) || reach[ny * m.w + nx] || t == T_ROCK || t == T_WALL || t == T_DOOR_KEY) continue;
                        reach[ny * m.w + nx] = 1;
                        st.push_back(ny * m.w + nx);
                    }
                }
                CHECK(reach[m.downY * m.w + m.downX], "depth %d: stairs down behind a coloured door", depth);
                for (auto& it : sk.debugItems()) if (it.item.d().kind == ItemKind::Chest) chests++;
                for (int y = 0; y < m.h; y++)
                    for (int x = 0; x < m.w; x++) {
                        if (m.at(x, y) != T_DOOR_KEY) continue;
                        vaults++;
                        bool key = false;
                        for (auto& it : sk.debugItems())
                            if (it.item.d().kind == ItemKind::Key && it.item.d().keyColor == m.keyDoorColor(x, y) &&
                                reach[(int)it.pos.y * m.w + (int)it.pos.x]) key = true;
                        CHECK(key, "depth %d: no reachable %s key", depth, keyColorName(m.keyDoorColor(x, y)));
                        if (key) keysOk++;
                    }
            }
        }
        CHECK(vaults > 0 && chests > 0, "no vaults (%d) or chests (%d) made", vaults, chests);
        // The detection scroll marks the chests around you as known.
        {
            sk.debugGive(b.id, "scroll_detect_objects");
            for (int t = 0; t < 3; t++) { sk.tick(); b.poll(); }
            int slot = -1;
            for (int i = 0; i < INV_SIZE; i++) if (!b.st.inv[i].empty() && b.st.inv[i].d().id == "scroll_detect_objects") slot = i;
            CHECK(slot >= 0, "no detection scroll");
            Vec2 me(sk.map().startX + 0.5f, sk.map().startY + 0.5f);   // (after a level change you stand at the stairs)
            b.action(A_INV_USE, slot);
            for (int t = 0; t < 3; t++) { sk.tick(); b.poll(); }
            int near = 0, known = 0;
            for (auto& it : sk.debugItems()) {
                if (it.item.d().id != "chest" || dist(it.pos, me) > 25) continue;
                near++;
                if (it.item.fuel & CHEST_KNOWN) known++;
            }
            CHECK(near == known, "detection: %d of %d nearby chests known", known, near);
        }
        // Open a treasure chest: it becomes an open chest and loot appears.
        auto& items = sk.debugItems();
        int ci = -1;
        for (size_t i = 0; i < items.size(); i++) if (items[i].item.d().id == "chest") ci = (int)i;
        if (ci >= 0) {
            items[ci].item.fuel = CH_TREASURE;
            Vec2 cp = items[ci].pos;
            int before = (int)items.size();
            sk.debugMovePlayer(b.id, cp + Vec2(-0.7f, 0));
            sk.debugFace(b.id, 0);
            b.action(A_USE);
            for (int t = 0; t < 3; t++) { sk.tick(); b.poll(); }
            bool opened = false;
            for (auto& it : sk.debugItems()) if (it.item.d().id == "chest_open" && dist(it.pos, cp) < 0.01f) opened = true;
            CHECK(opened && (int)sk.debugItems().size() > before, "chest: opened %d, items %d -> %d", opened, before, (int)sk.debugItems().size());
        }
        // A key opens its door (and is used up).
        const Map& m = sk.map();
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) {
                if (m.at(x, y) != T_DOOR_KEY) continue;
                int color = m.keyDoorColor(x, y);
                sk.debugGive(b.id, std::string("key_") + keyColorName(color));
                // Stand on the open side of the door, facing it.
                for (int k = 0; k < 4; k++) {
                    int nx = x + (k == 0) - (k == 1), ny = y + (k == 2) - (k == 3);
                    if (Map::tileSolid(m.at(nx, ny))) continue;
                    sk.debugMovePlayer(b.id, Vec2(nx + 0.5f, ny + 0.5f));
                    sk.debugFace(b.id, std::atan2((float)(y - ny), (float)(x - nx)));
                    break;
                }
                b.action(A_USE);
                for (int t = 0; t < 3; t++) { sk.tick(); b.poll(); }
                CHECK(m.at(x, y) == T_DOOR_OPEN, "the %s key did not open its door", keyColorName(color));
                bool keyLeft = false;
                for (auto& it : sk.debugPlayer(b.id)->inv) if (!it.empty() && it.d().kind == ItemKind::Key) keyLeft = true;
                CHECK(!keyLeft, "key not used up");
                y = m.h; break;
            }
        std::printf("keys and chests ok (%d vaults, %d chests)\n", vaults, chests);
    }
    {   // Joining with different data files: refused, told which files, and nothing is sent.
        auto nd = std::make_shared<ServerNet>();
        Bot bad;
        nd->attachLoopback(bad.pipe);
        Server sd(nd, 31);
        bad.wrongFile = "monsters.txt";
        bad.hello(1);
        for (int t = 0; t < 10; t++) { sd.tick(); bad.poll(); }
        CHECK(bad.id == 0 && bad.rejected.find("monsters.txt") != std::string::npos && bad.rejected.find("items.txt") == std::string::npos,
              "different data: id %d, reason '%s'", bad.id, bad.rejected.c_str());
        CHECK(sd.playerCount() == 0, "different data: %d players in the game", sd.playerCount());
        std::printf("data file check ok\n");
    }
    {   // The Abyss: harder every level, a wipe sends the party halfway back up, scores are kept.
        auto na = std::make_shared<ServerNet>();
        Bot b;
        na->attachLoopback(b.pipe);
        Server sa(na, 77);
        std::string scores = "selftest_abyss_scores.txt";
        std::remove(scores.c_str());
        sa.setScorePath(scores);
        CHECK(sa.startAbyss(), "no abyss difficulty");
        b.hello(0);
        for (int t = 0; t < 5; t++) { sa.tick(); b.poll(); }
        const DifficultyRules& base = difficultyRules(sa.difficulty());
        CHECK(base.hidden && base.atDepth(5).monsterHp > base.monsterHp * 2.9f, "abyss scaling");
        sa.debugGoto(9);
        PlayerState* ps = sa.debugPlayer(b.id);
        ps->hp = 0;
        ps->dead = true;
        ps->respawnT = 100;   // as when killed (8 s in the Abyss)
        for (int t = 0; t < 30 * 8; t++) { sa.tick(); b.poll(); }
        CHECK(sa.map().depth == 5, "abyss wipe on 9 went to depth %d (want 5)", sa.map().depth);
        CHECK(!sa.debugPlayer(b.id)->dead, "abyss: party not revived");
        auto list = loadAbyssScores(scores);
        CHECK(list.size() == 1 && list[0].deepest == 9 && list[0].deaths == 1 && list[0].firstDeath == 9, "abyss score not recorded");
        std::remove(scores.c_str());
        std::printf("abyss ok\n");
    }
    {   // Warrior abilities, wands and rods, quick slots (and saving them).
        auto n7 = std::make_shared<ServerNet>();
        Bot b;
        n7->attachLoopback(b.pipe);
        Server s7(n7, 21);
        b.hello(0);  // warrior
        auto run = [&](int ticks) { for (int t = 0; t < ticks; t++) { s7.tick(); b.poll(); } };
        run(5);
        s7.debugGoto(1);
        run(5);
        PlayerState* ps = s7.debugPlayer(b.id);
        CHECK(ps && ps->quick[0] == -1, "level 1 warrior should have no abilities on keys yet");
        s7.debugSetLevel(b.id, 12);
        run(3);
        CHECK(b.st.quick[0] == 0 && b.st.quick[1] == 1 && b.st.quick[2] == -1, "abilities not put on keys 1-2 (%d %d %d)", b.st.quick[0], b.st.quick[1], b.st.quick[2]);
        // Second Wind heals and then needs to recharge.
        ps->hp = 1;
        b.action(A_CAST, 0);
        run(3);
        CHECK(b.st.hp > 1 && b.st.rechargeLeft(0) > 40, "second wind: hp %d, recharge %.1f", b.st.hp, b.st.rechargeLeft(0));
        ps->hp = 1;
        run(20);
        b.action(A_CAST, 0);
        run(3);
        CHECK(ps->hp < 5, "second wind should not work while recharging (hp %d)", ps->hp);
        ps->maxHp = ps->hp = 5000;   // stay alive through the rest of the test
        // Battle Cry frightens monsters around you.
        s7.debugSpawn("kobold,kobold,giant_rat", true);
        run(2);
        b.action(A_CAST, 1);
        run(2);
        int afraid = 0;
        for (auto& m : s7.monsters()) if (m.fearT > 0) afraid++;
        CHECK(afraid >= 2, "battle cry frightened %d of %zu monsters", afraid, s7.monsters().size());
        // Wand: goes on a key by itself, uses charges (warriors fail sometimes).
        s7.debugGive(b.id, "wand_magic_missile,rod_illumination");
        run(3);
        int wandDef = gData.findItem("wand_magic_missile"), wandSlot = -1, rodSlot = -1;
        for (int i = 0; i < INV_SIZE; i++) {
            if (!b.st.inv[i].empty() && b.st.inv[i].def == wandDef) wandSlot = i;
            if (!b.st.inv[i].empty() && b.st.inv[i].d().id == "rod_illumination") rodSlot = i;
        }
        CHECK(wandSlot >= 0 && rodSlot >= 0, "wand/rod not given");
        CHECK(b.st.quick[2] == QUICK_ITEM + wandDef, "wand not on key 3 (%d)", b.st.quick[2]);
        if (wandSlot >= 0) {
            int before = b.st.inv[wandSlot].fuel;
            for (int t = 0; t < 12 && b.st.inv[wandSlot].fuel == before; t++) { b.action(A_INV_USE, wandSlot); run(25); }
            CHECK(b.st.inv[wandSlot].fuel == before - 1, "wand charges %d -> %d", before, b.st.inv[wandSlot].fuel);
        }
        if (rodSlot >= 0) {
            for (int t = 0; t < 12 && b.st.inv[rodSlot].fuel == 0; t++) { b.action(A_INV_USE, rodSlot); run(25); }
            CHECK(b.st.inv[rodSlot].fuel > 0 && b.st.lightT > 0, "rod of illumination did not work");
            run(30 * 31);
            CHECK(b.st.inv[rodSlot].fuel == 0, "rod did not recharge (%d)", b.st.inv[rodSlot].fuel);
        }
        // Moving things between keys.
        b.action(A_SET_QUICK, 8, 0);
        run(3);
        CHECK(b.st.quick[8] == 0 && b.st.quick[0] == -1, "A_SET_QUICK should move second wind to key 9");
        // Saved with the character.
        s7.setSavePath("quick_test.sav");
        CHECK(s7.saveGame(), "save failed");
        auto n8 = std::make_shared<ServerNet>();
        Bot b2;
        n8->attachLoopback(b2.pipe);
        Server s8(n8, 3);
        std::string e;
        CHECK(s8.loadGame("quick_test.sav", e), "load: %s", e.c_str());
        b2.hello(0);
        for (int t = 0; t < 5; t++) { s8.tick(); b2.poll(); }
        CHECK(b2.st.quick[8] == 0 && b2.st.quick[2] == QUICK_ITEM + wandDef && b2.st.level == 12, "quick slots not saved (%d %d, level %d)",
              b2.st.quick[8], b2.st.quick[2], b2.st.level);
        std::remove("quick_test.sav");
        std::printf("abilities, wands, rods, quick slots ok\n");
    }
    {   // Wizard: many spells over the levels, Sleep and Stone to Mud.
        int wiz = gData.findClass("wizard");
        CHECK(gData.classes[wiz].spells.size() >= 15, "wizard has only %zu spells", gData.classes[wiz].spells.size());
        auto n9 = std::make_shared<ServerNet>();
        Bot b;
        n9->attachLoopback(b.pipe);
        Server s9(n9, 33);
        b.hello(wiz);
        auto run = [&](int ticks) { for (int t = 0; t < ticks; t++) { s9.tick(); b.poll(); } };
        auto face = [&](float a) {
            b.angle = a;
            Writer w;
            w.u8(C_INPUT); w.f32(b.pos.x); w.f32(b.pos.y); w.f32(a); w.u8(0);
            b.net.send(w.buf, false);
            run(2);
        };
        auto spellIdx = [&](const char* id) {
            const ClassDef& c = gData.classes[wiz];
            for (size_t i = 0; i < c.spells.size(); i++) if (gData.spells[c.spells[i]].id == id) return (int)i;
            return -1;
        };
        run(5);
        s9.debugGoto(1);
        run(5);
        s9.debugSetLevel(b.id, 12);
        PlayerState* ps = s9.debugPlayer(b.id);
        ps->maxMp = ps->mp = 200;
        run(3);
        int onKeys = 0;
        for (int q : b.st.quick) if (q >= 0) onKeys++;
        CHECK(onKeys == 9, "wizard level 12 should fill all 9 keys (%d)", onKeys);
        // Sleep a monster.
        s9.debugSpawn("kobold", true);
        run(1);
        if (!s9.monsters().empty()) {
            Vec2 mp = s9.monsters().back().pos;
            face(std::atan2(mp.y - b.pos.y, mp.x - b.pos.x));
            b.action(A_CAST, spellIdx("sleep_monster"));
            run(30);
            const Monster& m = s9.monsters().back();
            CHECK(m.asleep && m.sleepT > 0, "sleep: monster asleep %d sleepT %.1f", (int)m.asleep, m.sleepT);
        }
        // Stone to mud: find a wall next to us.
        const Map& mp = s9.map();
        int wx = -1, wy = -1;
        float wa = 0;
        for (int dir = 0; dir < 4 && wx < 0; dir++) {
            float a = dir * PI / 2;
            for (float d = 0.5f; d < 5; d += 0.25f) {
                Vec2 c = b.pos + dirFromAngle(a) * d;
                int x = (int)c.x, y = (int)c.y;
                if (!mp.solidAt(c)) continue;
                if (mp.at(x, y) == T_WALL && x > 0 && y > 0 && x < mp.w - 1 && y < mp.h - 1) { wx = x; wy = y; wa = a; }
                break;
            }
        }
        if (wx >= 0) {
            face(wa);
            b.action(A_CAST, spellIdx("stone_to_mud"));
            run(3);
            CHECK(s9.map().at(wx, wy) == T_FLOOR && b.map.at(wx, wy) == T_FLOOR, "stone to mud did not dig %d,%d", wx, wy);
        } else std::printf("(no wall near the wizard, skipping stone to mud)\n");
        std::printf("wizard spells ok\n");
    }
    {   // New monster abilities: no breeding, limited calls for help, poison, stat drain; potions; delving.
        auto n10 = std::make_shared<ServerNet>();
        Bot b;
        n10->attachLoopback(b.pipe);
        Server s10(n10, 44);
        b.hello(0);
        auto run = [&](int ticks) { for (int t = 0; t < ticks; t++) { s10.tick(); b.poll(); } };
        run(5);
        s10.debugGoto(3);
        run(5);
        PlayerState* ps = s10.debugPlayer(b.id);
        auto tough = [&]() { ps->maxHp = ps->hp = 5000; };
        tough();
        auto count = [&](const char* id) {
            int n = 0, def = gData.findMonster(id);
            for (auto& m : s10.monsters()) if (m.def == def) n++;
            return n;
        };
        for (auto& m : gData.monsters) CHECK(m.summon == -1 || m.summonRate > 0, "%s summons with rate 0", m.id.c_str());
        // Grubs multiply, but slowly and only up to a limit.
        s10.debugSpawn("white_worm,white_worm,white_worm", true);
        int grubs = count("white_worm");
        run(30 * 25);
        tough();
        CHECK(count("white_worm") <= grubs, "grubs multiplied too fast: %d -> %d in 25 s", grubs, count("white_worm"));
        for (int t = 0; t < 10; t++) { run(30 * 30); tough(); }
        CHECK(count("white_worm") <= 20, "grubs passed the limit: %d", count("white_worm"));
        std::printf("(grubs: %d -> %d after 5 minutes)\n", grubs, count("white_worm"));
        // A brood mother calls spiders, but at most 3 times (1-2 each).
        int spiders = count("giant_spider");
        s10.debugSpawn("brood_mother", true);
        for (int t = 0; t < 8; t++) { run(30 * 15); tough(); }
        int called = count("giant_spider") - spiders;
        bool motherAlive = count("brood_mother") > 0;
        CHECK(called <= 6, "brood mother called %d spiders (max 6)", called);
        CHECK(!motherAlive || called >= 1, "brood mother never called for help");
        // Poison from a centipede, cured by an antidote.
        ps->poisonT = 0;
        s10.debugSpawn("cave_centipede,cave_centipede", true);
        for (int t = 0; t < 40 && ps->poisonT <= 0; t++) { run(15); tough(); }
        CHECK(ps->poisonT > 0, "centipedes never poisoned the player");
        s10.debugGive(b.id, "potion_antidote,potion_restoration,potion_speed,scroll_delving");
        run(3);
        auto use = [&](const char* id) {
            for (int i = 0; i < INV_SIZE; i++)
                if (!b.st.inv[i].empty() && b.st.inv[i].d().id == id) { b.action(A_INV_USE, i); run(3); return true; }
            return false;
        };
        CHECK(use("potion_antidote") && ps->poisonT == 0, "antidote did not cure poison (%.1f)", ps->poisonT);
        // Drained stats are restored by the potion.
        ps->drained[STR] = 2;
        int weak = ps->stat(STR);
        CHECK(weak == std::max(3, ps->stats[STR] - 2 + ps->equipSum([](const ItemDef& d) { return d.statBonus[STR]; })), "drain not applied to STR");
        CHECK(use("potion_restoration") && ps->drained[STR] == 0, "restoration did not restore STR");
        CHECK(use("potion_speed") && ps->hasteT > 0, "potion of speed did not haste");
        // Delving: two levels down after a few seconds.
        int depth = s10.map().depth;
        CHECK(use("scroll_delving"), "no delving scroll");
        run(30 * 7);
        CHECK(s10.map().depth == depth + 2, "delving went from %d to %d", depth, s10.map().depth);
        std::printf("monster abilities, potions, delving ok\n");
    }
    {   // Campaign: the sample "Sunken Crypt" - hand-made first level, goals, saving.
        int ci = gData.findCampaign("sunken_crypt");
        CHECK(ci >= 0, "sample campaign missing");
        if (ci >= 0) {
            auto n5 = std::make_shared<ServerNet>();
            Bot b;
            n5->attachLoopback(b.pipe);
            Server s5(n5, 11);
            s5.startCampaign(ci);
            b.hello(0);
            for (int t = 0; t < 5; t++) { s5.tick(); b.poll(); }
            CHECK(s5.map().depth == 0, "campaign should start in town");
            auto useStairs = [&]() {
                s5.debugMovePlayer(b.id, Vec2(s5.map().downX + 0.5f, s5.map().downY + 0.5f));
                for (int t = 0; t < 3; t++) { s5.tick(); b.poll(); }
                b.action(A_USE);
                for (int t = 0; t < 5; t++) { s5.tick(); b.poll(); }
            };
            useStairs();
            CHECK(s5.map().depth == 1 && s5.map().w == 36, "level 1 should be the hand-made crypt gate (depth %d, w %d)", s5.map().depth, s5.map().w);
            bool goal = false;
            for (auto& q : s5.quests()) if (q.main && q.depth == 1 && q.type == Q_STAIRS && !q.done) goal = true;
            CHECK(goal, "level 1 goal missing");
            useStairs();
            bool done = false;
            for (auto& q : s5.quests()) if (q.main && q.depth == 1 && q.done) done = true;
            CHECK(s5.map().depth == 2 && done, "stairs goal not completed (depth %d)", s5.map().depth);
            bool tome = false;
            for (auto& q : s5.quests()) if (q.depth == 2 && q.type == Q_FIND_ITEM) tome = true;
            CHECK(tome, "level 2 find-goal missing");
            // Save and load mid-campaign.
            s5.setSavePath("campaign_test.sav");
            CHECK(s5.saveGame(), "campaign save failed");
            auto n6 = std::make_shared<ServerNet>();
            Server s6(n6, 2);
            std::string e;
            CHECK(s6.loadGame("campaign_test.sav", e), "campaign load: %s", e.c_str());
            CHECK(s6.campaign() == ci && s6.map().depth == 2, "campaign state not restored");
            std::remove("campaign_test.sav");
            // The final level holds the boss on the hand-made map.
            s5.debugGoto(5);
            CHECK(s5.map().depth == 5 && s5.map().w == 33, "level 5 should be the crypt heart (w %d)", s5.map().w);
            s5.setSavePath("");
            std::printf("campaign ok\n");
        }
    }
    {   // Collapsing: with level 12 reached, the town stairs lead to level 6 and the
        // unkilled boss of level 3 seeps down to level 6.
        auto n3 = std::make_shared<ServerNet>();
        Bot b;
        n3->attachLoopback(b.pipe);
        Server s3(n3, 77);
        b.hello(1);
        for (int t = 0; t < 5; t++) { s3.tick(); b.poll(); }
        s3.debugSetMaxDepth(12);
        s3.debugMovePlayer(b.id, Vec2(s3.map().downX + 0.5f, s3.map().downY + 0.5f));
        for (int t = 0; t < 3; t++) { s3.tick(); b.poll(); }
        b.action(A_USE);
        for (int t = 0; t < 5; t++) { s3.tick(); b.poll(); }
        CHECK(s3.map().depth == 6, "town stairs led to %d instead of 6", s3.map().depth);
        int bossDepth = -1;
        for (auto& q : s3.quests()) if (q.main && !q.done) bossDepth = q.depth;
        CHECK(bossDepth == 6, "boss quest depth %d, expected 6", bossDepth);
        // Up from the first open level goes straight to town (two presses).
        s3.debugMovePlayer(b.id, Vec2(s3.map().startX + 0.5f, s3.map().startY + 0.5f));
        for (int t = 0; t < 3; t++) { s3.tick(); b.poll(); }
        for (int k = 0; k < 2; k++) { b.action(A_USE); for (int t = 0; t < 10; t++) { s3.tick(); b.poll(); } }
        CHECK(s3.map().depth == 0, "up from level 6 went to %d", s3.map().depth);
        std::printf("level collapsing ok\n");
    }
    CHECK(bots[0].id != 0, "bot got no welcome");
    CHECK(bots[0].snapshots > 100, "bot got few snapshots: %d", bots[0].snapshots);
    CHECK(remoteIds[0] && remoteIds[1], "remote players did not join (%d %d)", remoteIds[0], remoteIds[1]);
    CHECK(remoteSnaps[0] > 50, "remote got few snapshots: %d", remoteSnaps[0]);
    CHECK(server.playerCount() == 3, "player count %d", server.playerCount());
    CHECK(maxDepth >= 3, "level changes did not happen (max depth %d)", maxDepth);
    std::printf("simulation: bot level %d, xp %d, kills %d, hp %d/%d, depth reached %d, msgs %d\n",
                bots[0].st.level, bots[0].st.xp, bots[0].st.kills, bots[0].st.hp, bots[0].st.maxHp, maxDepth, bots[0].msgs);

    for (auto& rc : remote) rc.disconnect();
    for (int t = 0; t < 30; t++) server.tick();
    CHECK(server.playerCount() == 1, "remote disconnect not handled (%d players)", server.playerCount());

    {   // Save and load: the character comes back with level, items and position.
        std::string path = "selftest.sav";
        server.setSavePath(path);
        CHECK(server.saveGame(), "save failed");
        SaveInfo info = Server::peekSave(path);
        CHECK(info.ok && info.chars.size() == 3, "peekSave: ok=%d chars=%zu", info.ok, info.chars.size());
        CHECK(info.depth == server.map().depth, "peekSave depth");
        auto net2 = std::make_shared<ServerNet>();
        Bot again;
        net2->attachLoopback(again.pipe);
        Server s2(net2, 1);
        std::string e2;
        CHECK(s2.loadGame(path, e2), "load failed: %s", e2.c_str());
        CHECK(s2.map().tiles == server.map().tiles, "loaded map differs");
        CHECK(s2.floorItemCount() == server.floorItemCount(), "loaded items differ (%d vs %d)", s2.floorItemCount(), server.floorItemCount());
        Writer w;
        w.u8(C_HELLO); w.u16(PROTOCOL_VERSION); w.str("Bot0"); w.u8(4);  // other class requested: save wins
        dataChecksums(w);
        again.net.send(w.buf, true);
        for (int t = 0; t < 10; t++) { s2.tick(); again.poll(); }
        CHECK(again.st.level == bots[0].st.level && again.st.cls == bots[0].st.cls, "restored character: level %d/%d class %d/%d",
              again.st.level, bots[0].st.level, again.st.cls, bots[0].st.cls);
        CHECK(again.st.xp == bots[0].st.xp && again.st.gold == bots[0].st.gold, "restored xp/gold");
        std::remove(path.c_str());
        std::printf("save/load ok\n");
    }

    std::printf(failures ? "%d FAILURES\n" : "ALL TESTS PASSED\n", failures);
    netShutdown();
    return failures ? 1 : 0;
}
