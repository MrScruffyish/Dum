// Dum - hand-made maps and campaigns (data/maps/*.txt, data/campaigns/*.txt).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "game/map.h"

// Something placed on a hand-made map by a legend letter.
struct Placement {
    enum Kind { Monster, Item } kind = Monster;
    int def = -1;
    bool awake = false;
    int x = 0, y = 0;
};

struct HandMap {
    std::string id, name, file;
    int depth = 1;                    // for random monsters/items and their strength
    bool lit = false;
    bool randomMonsters = true;       // also populate with random monsters and items
    uint8_t wallTex = WT_BRICK, floorTex = FT_STONE;
    std::vector<std::string> rows;
    std::map<char, Placement> legend;
    int line = 0;                     // where the map starts in the file (for errors)
};

enum CampaignGoal { G_STAIRS, G_KILL, G_FIND, G_CLEAR };

struct CampaignLevel {
    std::string map;                  // hand-made map id, or "" for a random level
    int depth = 1;                    // random level difficulty
    std::vector<int> monsters;        // monster pool for random monsters (empty = normal)
    int goal = G_STAIRS;
    int target = -1;                  // monster (kill) or item (find)
    std::string intro, goalText;
};

struct Campaign {
    std::string id, name, desc, victory, file;
    bool town = true;                 // start in town with shops between levels
    std::vector<CampaignLevel> levels;
};

// Loading (called from GameData::load). Errors are "file:line: message".
bool loadHandMaps(const std::string& dir, std::vector<HandMap>& out, std::string& err);
bool loadCampaigns(const std::string& dir, std::vector<Campaign>& out, std::string& err);

// Build the tiles of a hand-made map. Returns the placements with their positions.
// Fails (with a message) if the map has no way from start to stairs etc.
bool buildHandMap(const HandMap& h, Map& m, std::vector<Placement>& placed, std::string& err);

// Write a Map as a hand-made map file (used by dum_tool genmap).
std::string handMapText(const Map& m, const std::string& id, const std::string& name, int depth);

// Characters used in map files.
//   ' ' solid rock   '#' wall (theme)   '%' cave wall   '.' floor (theme)   ',' dirt floor
//   '_' tiled floor  '=' wooden floor   '+' door        '\'' open door      'L' locked door
//   'S' secret door  ':' rubble         '>' stairs down '<' stairs up (= start)  '@' start
//   any other letter/digit: defined with "place X = monster <id> [awake]" or "place X = item <id>"
