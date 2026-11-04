// dum_tool - content tool for Dum: generate maps and campaigns, check and preview them.
//
//   dum_tool check                                   load all data files, report problems
//   dum_tool show <map-id>                           print a hand-made map with its placements
//   dum_tool genmap <id> [--name N] [--depth D] [--seed S] [--out FILE] [--force]
//                                                    generate a random level as an editable map
//   dum_tool newcampaign <id> [--name N] [--levels L] [--depth D] [--handmade K] [--seed S] [--force]
//                                                    generate a campaign with levels, monsters and goals
//   dum_tool list                                    list campaigns and maps
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "game/campaign.h"
#include "game/data.h"
#include "game/map.h"

namespace {

std::map<std::string, std::string> gOpt;
std::vector<std::string> gArgs;

std::string opt(const std::string& k, const std::string& def) { return gOpt.count(k) ? gOpt[k] : def; }
int optI(const std::string& k, int def) { return gOpt.count(k) ? std::atoi(gOpt[k].c_str()) : def; }

bool writeFile(const std::string& path, const std::string& text, bool force) {
    if (!force && std::filesystem::exists(path)) {
        std::fprintf(stderr, "%s already exists (use --force to overwrite)\n", path.c_str());
        return false;
    }
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path);
    f << text;
    if (!f) { std::fprintf(stderr, "could not write %s\n", path.c_str()); return false; }
    std::printf("wrote %s\n", path.c_str());
    return true;
}

std::string titleCase(std::string s) {
    bool up = true;
    for (auto& c : s) {
        if (c == '_') { c = ' '; up = true; continue; }
        if (up && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        up = c == ' ';
    }
    return s;
}

int cmdCheck() {
    std::printf("OK: %zu classes, %zu items, %zu monsters, %zu spells, %zu stores, %zu difficulties,\n"
                "    %zu hand-made maps, %zu campaigns\n",
                gData.classes.size(), gData.items.size(), gData.monsters.size(), gData.spells.size(), gData.stores.size(),
                gData.difficulties.size(), gData.handMaps.size(), gData.campaigns.size());
    // Extra warnings that are not errors.
    int warnings = 0;
    for (auto& c : gData.campaigns)
        for (size_t i = 0; i < c.levels.size(); i++) {
            const CampaignLevel& L = c.levels[i];
            if (L.goal == G_KILL && L.target >= 0 && gData.monsters[L.target].depth > L.depth + 4 && L.map.empty()) {
                std::printf("warning: %s level %zu: %s (depth %d) is much deeper than the level (depth %d)\n", c.file.c_str(), i + 1,
                            gData.monsters[L.target].name.c_str(), gData.monsters[L.target].depth, L.depth);
                warnings++;
            }
        }
    for (auto& h : gData.handMaps) {
        Map m;
        std::vector<Placement> pl;
        std::string err;
        buildHandMap(h, m, pl, err);
        if (m.downX == m.startX && m.downY == m.startY) { std::printf("note: map '%s' has no stairs down ('>')\n", h.id.c_str()); }
    }
    std::printf(warnings ? "%d warnings\n" : "no problems found\n", warnings);
    return 0;
}

int cmdShow(const std::string& id) {
    int i = gData.findHandMap(id);
    if (i < 0) { std::fprintf(stderr, "no map '%s' (dum_tool list shows them)\n", id.c_str()); return 1; }
    const HandMap& h = gData.handMaps[i];
    Map m;
    std::vector<Placement> pl;
    std::string err;
    if (!buildHandMap(h, m, pl, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("%s (%s) - %dx%d, depth %d, %s, random monsters %s\n", h.name.c_str(), h.file.c_str(), m.w - 2, m.h - 2, h.depth,
                h.lit ? "lit" : "dark", h.randomMonsters ? "yes" : "no");
    for (auto& r : h.rows) std::printf("  %s\n", r.c_str());
    for (auto& p : pl)
        std::printf("  at %2d,%2d: %s %s\n", p.x - 1, p.y - 1, p.kind == Placement::Monster ? "monster" : "item",
                    p.kind == Placement::Monster ? gData.monsters[p.def].name.c_str() : gData.items[p.def].name.c_str());
    return 0;
}

int cmdGenMap(const std::string& id) {
    int depth = optI("depth", 3);
    uint32_t seed = (uint32_t)optI("seed", (int)(std::hash<std::string>()(id) & 0x7fffffff));
    Map m;
    generateLevel(m, seed, depth);
    std::string text = handMapText(m, id, opt("name", titleCase(id)), depth);
    return writeFile(opt("out", gData.dir + "/maps/" + id + ".txt"), text, gOpt.count("force")) ? 0 : 1;
}

// Pick monsters that fit a depth, for a level's monster pool.
std::vector<int> monsterPool(int depth, Rng& rng, int n) {
    std::vector<int> c;
    for (size_t i = 0; i < gData.monsters.size(); i++) {
        const MonsterDef& d = gData.monsters[i];
        if (!d.boss && d.rarity > 0 && d.depth <= depth + 1 && d.depth >= depth - 4) c.push_back((int)i);
    }
    for (size_t i = 0; i + 1 < c.size(); i++) std::swap(c[i], c[i + rng.range((int)i, (int)c.size() - 1) - (int)i]);
    if ((int)c.size() > n) c.resize(n);
    return c;
}

int cmdNewCampaign(const std::string& id) {
    int levels = std::max(1, optI("levels", 5));
    int startDepth = std::max(1, optI("depth", 1));
    int handmade = std::max(0, optI("handmade", 1));
    std::string name = opt("name", titleCase(id));
    Rng rng((uint32_t)optI("seed", (int)(std::hash<std::string>()(id) & 0x7fffffff)));
    bool force = gOpt.count("force") > 0;

    // Quest items and a final boss.
    std::vector<int> treasures;
    for (size_t i = 0; i < gData.items.size(); i++)
        if (gData.items[i].kind == ItemKind::Quest) treasures.push_back((int)i);
    int finalDepth = startDepth + levels - 1;
    int boss = -1, bestDiff = 1 << 30;
    for (size_t i = 0; i < gData.monsters.size(); i++) {
        const MonsterDef& d = gData.monsters[i];
        if (!d.boss) continue;
        int diff = std::abs(d.depth - finalDepth);
        if (diff < bestDiff) { bestDiff = diff; boss = (int)i; }
    }

    const char* intros[] = {
        "The air grows colder as you descend.", "Water drips somewhere in the dark.", "Claw marks score the walls here.",
        "You hear distant chanting.", "Old bones crunch under your feet.", "A faint glow comes from deeper tunnels.",
        "Someone has been here recently - the torches are still warm.", "The walls are slick with something you'd rather not touch.",
    };

    std::string mapsText;
    std::string t = "# Generated by dum_tool newcampaign. Edit freely - see docs/CAMPAIGNS.md.\n\n";
    t += "[campaign " + id + "]\nname = " + name + "\n";
    t += "desc = A new adventure in " + std::to_string(levels) + " levels.\ntown = yes\n";
    t += "victory = " + (boss >= 0 ? gData.monsters[boss].name + " is defeated, and " + name + " is won!" : name + " is won!") + "\n";
    for (int l = 1; l <= levels; l++) {
        int depth = startDepth + l - 1;
        t += "\n[level " + std::to_string(l) + "]\n";
        bool hand = handmade > 0 && (l == 1 || (l == levels && handmade > 1) || (handmade > 2 && l % 2 == 0));
        if (hand) {
            std::string mid = id + "_" + std::to_string(l);
            Map m;
            generateLevel(m, rng.next(), depth);
            mapsText += handMapText(m, mid, name + " - level " + std::to_string(l), depth) + "\n";
            t += "map = " + mid + "\n";
        } else {
            t += "map = random\ndepth = " + std::to_string(depth) + "\n";
        }
        auto pool = monsterPool(depth, rng, 5);
        if (!pool.empty() && !hand) {
            t += "monsters = ";
            for (size_t i = 0; i < pool.size(); i++) t += (i ? ", " : "") + gData.monsters[pool[i]].id;
            t += "\n";
        }
        t += std::string("intro = ") + intros[rng.range(0, 7)] + "\n";
        if (l == levels && boss >= 0) {
            t += "goal = kill " + gData.monsters[boss].id + "\n";
        } else {
            int kind = l == 1 ? 0 : rng.range(0, 3);
            if (kind == 1 && !treasures.empty()) t += "goal = find " + gData.items[treasures[rng.range(0, (int)treasures.size() - 1)]].id + "\n";
            else if (kind == 2) t += "goal = clear\n";
            else if (kind == 3 && !pool.empty()) {
                int strongest = pool[0];
                for (int m : pool) if (gData.monsters[m].xp > gData.monsters[strongest].xp) strongest = m;
                t += "goal = kill " + gData.monsters[strongest].id + "\n";
            } else t += "goal = stairs\n";
        }
    }
    if (!mapsText.empty() && !writeFile(gData.dir + "/maps/" + id + ".txt", mapsText, force)) return 1;
    if (!writeFile(gData.dir + "/campaigns/" + id + ".txt", t, force)) return 1;
    std::printf("Campaign '%s' created. Pick it under 'Adventure' in the main menu.\n", name.c_str());
    return 0;
}

int cmdList() {
    std::printf("Campaigns:\n");
    for (auto& c : gData.campaigns) std::printf("  %-20s %s (%zu levels)\n", c.id.c_str(), c.name.c_str(), c.levels.size());
    std::printf("Hand-made maps:\n");
    for (auto& h : gData.handMaps) std::printf("  %-20s %s (%s)\n", h.id.c_str(), h.name.c_str(), h.file.c_str());
    return 0;
}

void usage() {
    std::printf(
        "dum_tool - make maps, campaigns and quests for Dum\n\n"
        "  dum_tool check                     check all data files (classes, items, maps, campaigns...)\n"
        "  dum_tool list                      list campaigns and hand-made maps\n"
        "  dum_tool show <map-id>             print a hand-made map and what is placed on it\n"
        "  dum_tool genmap <id> [--name N] [--depth D] [--seed S] [--out FILE] [--force]\n"
        "                                     generate a random level as an editable map file\n"
        "  dum_tool newcampaign <id> [--name N] [--levels L] [--depth D] [--handmade K] [--seed S] [--force]\n"
        "                                     generate a campaign: levels, monster pools, goals, a final boss\n"
        "  common: --data DIR                 use another data folder\n\n"
        "See docs/CAMPAIGNS.md for the file formats.\n");
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            bool hasVal = i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0;
            gOpt[a.substr(2)] = hasVal ? argv[++i] : "1";
        } else {
            gArgs.push_back(a);
        }
    }
    if (gArgs.empty() || gOpt.count("help")) { usage(); return 0; }
    std::string dir = opt("data", findDataDir(argv[0]));
    std::string err;
    if (!gData.load(dir, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const std::string& cmd = gArgs[0];
    std::string arg = gArgs.size() > 1 ? gArgs[1] : "";
    if (cmd == "check") return cmdCheck();
    if (cmd == "list") return cmdList();
    if (cmd == "show" && !arg.empty()) return cmdShow(arg);
    if (cmd == "genmap" && !arg.empty()) return cmdGenMap(arg);
    if (cmd == "newcampaign" && !arg.empty()) return cmdNewCampaign(arg);
    usage();
    return 1;
}
