// Dum - The Abyss high score list: one line per run in a small text file
// (abyss_scores.txt next to the saves). A run's line is updated as it goes deeper.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct AbyssScore {
    uint32_t run = 0;        // the run's dungeon seed
    int deepest = 0;         // deepest level reached
    int deaths = 0;          // party wipes
    int firstDeath = 0;      // level of the first wipe (0 = never died)
    int level = 1;           // highest character level in the party
    std::string date;        // YYYY-MM-DD of the last update
    std::string party;       // "Gunnar the Warrior, Ann the Wizard"
};

// Best first: deeper, then fewer deaths, then deeper before the first death.
inline bool abyssBetter(const AbyssScore& a, const AbyssScore& b) {
    if (a.deepest != b.deepest) return a.deepest > b.deepest;
    if (a.deaths != b.deaths) return a.deaths < b.deaths;
    return a.firstDeath > b.firstDeath;
}

inline std::vector<AbyssScore> loadAbyssScores(const std::string& path) {
    std::vector<AbyssScore> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        AbyssScore s;
        if (!(in >> s.run >> s.deepest >> s.deaths >> s.firstDeath >> s.level >> s.date)) continue;
        std::getline(in >> std::ws, s.party);
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), abyssBetter);
    return out;
}

// Add or update the line for s.run. Returns the run's place on the list (1 = best).
inline int saveAbyssScore(const std::string& path, AbyssScore s) {
    std::time_t t = std::time(nullptr);
    char d[16];
    std::strftime(d, sizeof d, "%Y-%m-%d", std::localtime(&t));
    s.date = d;
    auto all = loadAbyssScores(path);
    bool found = false;
    for (auto& e : all)
        if (e.run == s.run) { e = s; found = true; }
    if (!found) all.push_back(s);
    std::sort(all.begin(), all.end(), abyssBetter);
    if (all.size() > 100) all.resize(100);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return 0;
        f << "# Dum - The Abyss high scores: run deepest deaths first_death level date party\n";
        for (auto& e : all)
            f << e.run << ' ' << e.deepest << ' ' << e.deaths << ' ' << e.firstDeath << ' ' << e.level << ' ' << e.date << ' ' << e.party << '\n';
    }
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
    for (size_t i = 0; i < all.size(); i++)
        if (all[i].run == s.run) return (int)i + 1;
    return 0;
}
