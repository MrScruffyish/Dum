// Dum - hand-made maps and campaigns.
#include "game/campaign.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <queue>
#include <sstream>

#include "game/data.h"

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim(part);
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

std::vector<std::string> filesIn(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".txt") out.push_back(e.path().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool yes(const std::string& v) { return v == "yes" || v == "1" || v == "true"; }

int wallTheme(const std::string& v) {
    static const char* n[WT_COUNT] = {"brick", "cave", "moss", "dark", "wood", "blood"};
    for (int i = 0; i < WT_COUNT; i++)
        if (v == n[i]) return i;
    return -1;
}
int floorTheme(const std::string& v) {
    static const char* n[FT_COUNT] = {"stone", "dirt", "tiles", "wood", "grass", "cobble"};
    for (int i = 0; i < FT_COUNT; i++)
        if (v == n[i]) return i;
    return -1;
}

const std::string kTileChars = " #%.,_=+'LS:><@*";
bool isTileChar(char c) { return kTileChars.find(c) != std::string::npos; }

}  // namespace

bool loadHandMaps(const std::string& dir, std::vector<HandMap>& out, std::string& err) {
    for (auto& path : filesIn(dir)) {
        std::ifstream f(path);
        std::string line;
        int ln = 0;
        HandMap* cur = nullptr;
        bool inMap = false;
        auto where = [&]() { return path + ":" + std::to_string(ln) + ": "; };
        while (std::getline(f, line)) {
            ln++;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (inMap) {
                if (trim(line).empty() || line[0] == '[') { inMap = false; }
                else { cur->rows.push_back(line); continue; }
            }
            std::string t = line;
            size_t hash = t.find('#');
            if (hash != std::string::npos) t = t.substr(0, hash);
            t = trim(t);
            if (t.empty()) continue;
            if (t[0] == '[') {
                std::stringstream ss(t.substr(1, t.find(']') - 1));
                std::string type, id;
                ss >> type >> id;
                if (type != "map") { err = where() + "expected [map <id>]"; return false; }
                out.push_back(HandMap());
                cur = &out.back();
                cur->id = id;
                cur->name = id;
                cur->file = path;
                continue;
            }
            if (!cur) { err = where() + "expected [map <id>] first"; return false; }
            if (t == "map:") { inMap = true; cur->line = ln + 1; continue; }
            size_t eq = t.find('=');
            if (eq == std::string::npos) { err = where() + "expected 'key = value' or 'map:'"; return false; }
            std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
            if (k == "name") cur->name = v;
            else if (k == "depth") cur->depth = std::max(1, std::atoi(v.c_str()));
            else if (k == "lit") cur->lit = yes(v);
            else if (k == "monsters") cur->randomMonsters = yes(v);
            else if (k == "wall") {
                int w = wallTheme(v);
                if (w < 0) { err = where() + "unknown wall theme '" + v + "' (brick cave moss dark wood blood)"; return false; }
                cur->wallTex = (uint8_t)w;
            } else if (k == "floor") {
                int fl = floorTheme(v);
                if (fl < 0) { err = where() + "unknown floor theme '" + v + "' (stone dirt tiles wood grass cobble)"; return false; }
                cur->floorTex = (uint8_t)fl;
            } else if (k.rfind("place", 0) == 0) {
                // place X = monster <id> [awake] | item <id>
                std::string ch = trim(k.substr(5));
                if (ch.size() != 1 || isTileChar(ch[0])) { err = where() + "place needs one letter/digit that isn't a map symbol"; return false; }
                std::stringstream ss(v);
                std::string kind, id, flag;
                ss >> kind >> id >> flag;
                Placement p;
                if (kind == "monster") {
                    p.kind = Placement::Monster;
                    p.def = gData.findMonster(id);
                    p.awake = flag == "awake";
                } else if (kind == "item") {
                    p.kind = Placement::Item;
                    p.def = gData.findItem(id);
                } else { err = where() + "place: expected 'monster <id>' or 'item <id>'"; return false; }
                if (p.def < 0) { err = where() + "unknown " + kind + " '" + id + "'"; return false; }
                cur->legend[ch[0]] = p;
            } else {
                err = where() + "unknown key '" + k + "'";
                return false;
            }
        }
    }
    for (size_t i = 0; i < out.size(); i++)
        for (size_t j = i + 1; j < out.size(); j++)
            if (out[i].id == out[j].id) { err = out[j].file + ": map id '" + out[j].id + "' is used twice"; return false; }
    return true;
}

bool loadCampaigns(const std::string& dir, std::vector<Campaign>& out, std::string& err) {
    for (auto& path : filesIn(dir)) {
        std::ifstream f(path);
        std::string line;
        int ln = 0;
        Campaign c;
        c.file = path;
        CampaignLevel* lvl = nullptr;
        auto where = [&]() { return path + ":" + std::to_string(ln) + ": "; };
        while (std::getline(f, line)) {
            ln++;
            size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            std::string t = trim(line);
            if (t.empty()) continue;
            if (t[0] == '[') {
                std::stringstream ss(t.substr(1, t.find(']') - 1));
                std::string type, id;
                ss >> type >> id;
                if (type == "campaign") { c.id = id; c.name = id; lvl = nullptr; }
                else if (type == "level") { c.levels.push_back(CampaignLevel()); lvl = &c.levels.back(); }
                else { err = where() + "expected [campaign <id>] or [level <n>]"; return false; }
                continue;
            }
            size_t eq = t.find('=');
            if (eq == std::string::npos) { err = where() + "expected 'key = value'"; return false; }
            std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
            if (c.id.empty()) { err = where() + "the file must start with [campaign <id>]"; return false; }
            if (!lvl) {
                if (k == "name") c.name = v;
                else if (k == "desc") c.desc = v;
                else if (k == "victory") c.victory = v;
                else if (k == "town") c.town = yes(v);
                else { err = where() + "unknown campaign key '" + k + "'"; return false; }
                continue;
            }
            if (k == "map") lvl->map = v == "random" ? "" : v;
            else if (k == "depth") lvl->depth = std::max(1, std::atoi(v.c_str()));
            else if (k == "intro") lvl->intro = v;
            else if (k == "goal_text") lvl->goalText = v;
            else if (k == "monsters") {
                for (auto& id : splitList(v)) {
                    int m = gData.findMonster(id);
                    if (m < 0) { err = where() + "unknown monster '" + id + "'"; return false; }
                    lvl->monsters.push_back(m);
                }
            } else if (k == "goal") {
                std::stringstream ss(v);
                std::string g, id;
                ss >> g >> id;
                if (g == "stairs") lvl->goal = G_STAIRS;
                else if (g == "clear") lvl->goal = G_CLEAR;
                else if (g == "kill") {
                    lvl->goal = G_KILL;
                    lvl->target = gData.findMonster(id);
                    if (lvl->target < 0) { err = where() + "unknown monster '" + id + "'"; return false; }
                } else if (g == "find") {
                    lvl->goal = G_FIND;
                    lvl->target = gData.findItem(id);
                    if (lvl->target < 0) { err = where() + "unknown item '" + id + "'"; return false; }
                } else { err = where() + "goal must be: stairs | clear | kill <monster> | find <item>"; return false; }
            } else { err = where() + "unknown level key '" + k + "'"; return false; }
        }
        if (c.id.empty()) continue;
        if (c.levels.empty()) { err = path + ": campaign has no [level] sections"; return false; }
        out.push_back(c);
    }
    return true;
}

bool buildHandMap(const HandMap& h, Map& m, std::vector<Placement>& placed, std::string& err) {
    size_t w = 0;
    for (auto& r : h.rows) w = std::max(w, r.size());
    if (w == 0 || h.rows.empty()) { err = h.file + ": map '" + h.id + "' has no map: rows"; return false; }
    int W = (int)w + 2, H = (int)h.rows.size() + 2;
    if (W > 250 || H > 250) { err = h.file + ": map '" + h.id + "' is too big (max 248x248)"; return false; }
    m.resize(W, H);
    bool haveStart = false, haveDown = false;
    placed.clear();
    for (int y = 0; y < (int)h.rows.size(); y++)
        for (int x = 0; x < (int)h.rows[y].size(); x++) {
            char c = h.rows[y][x];
            int mx = x + 1, my = y + 1, i = my * W + mx;
            m.wallTex[i] = h.wallTex;
            m.floorTex[i] = h.floorTex;
            uint8_t t = T_FLOOR;
            switch (c) {
                case ' ': t = T_ROCK; break;
                case '#': t = T_WALL; break;
                case '%': t = T_WALL; m.wallTex[i] = WT_CAVE; break;
                case '.': break;
                case '*': m.flags[i] |= F_LIT; break;
                case ',': m.floorTex[i] = FT_DIRT; break;
                case '_': m.floorTex[i] = FT_TILES; break;
                case '=': m.floorTex[i] = FT_WOOD; break;
                case '+': t = T_DOOR; break;
                case '\'': t = T_DOOR_OPEN; break;
                case 'L': t = T_DOOR_LOCKED; break;
                case 'S': t = T_DOOR_SECRET; break;
                case ':': t = T_RUBBLE; break;
                case '>': t = T_STAIRS_DOWN; m.downX = mx; m.downY = my; haveDown = true; break;
                case '<': t = T_STAIRS_UP; m.startX = mx; m.startY = my; haveStart = true; break;
                case '@': if (!haveStart) { m.startX = mx; m.startY = my; haveStart = true; } break;
                default: {
                    auto it = h.legend.find(c);
                    if (it == h.legend.end()) {
                        err = h.file + ":" + std::to_string(h.line + y) + ": unknown map symbol '" + std::string(1, c) + "' (define it with place " + c + " = ...)";
                        return false;
                    }
                    Placement p = it->second;
                    p.x = mx;
                    p.y = my;
                    placed.push_back(p);
                }
            }
            m.tiles[i] = t;
            m.flags[i] |= F_ROOM;
            if (h.lit) m.flags[i] |= F_LIT;
        }
    if (!haveStart) { err = h.file + ": map '" + h.id + "' needs a start: '<' (stairs up) or '@'"; return false; }
    // Make sure the stairs and everything placed can be reached (doors/rubble count as open).
    std::vector<char> seen(W * H, 0);
    std::queue<int> q;
    q.push(m.startY * W + m.startX);
    seen[m.startY * W + m.startX] = 1;
    while (!q.empty()) {
        int c = q.front();
        q.pop();
        int x = c % W, y = c / W;
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k];
            if (!m.inside(nx, ny) || seen[ny * W + nx]) continue;
            uint8_t t = m.at(nx, ny);
            if (t == T_ROCK || t == T_WALL) continue;
            seen[ny * W + nx] = 1;
            q.push(ny * W + nx);
        }
    }
    if (haveDown && !seen[m.downY * W + m.downX]) { err = h.file + ": map '" + h.id + "': the stairs down can't be reached from the start"; return false; }
    for (auto& p : placed)
        if (!seen[p.y * W + p.x]) {
            err = h.file + ":" + std::to_string(h.line + p.y - 1) + ": something placed at column " + std::to_string(p.x) + " can't be reached from the start";
            return false;
        }
    if (!haveDown) { m.downX = m.startX; m.downY = m.startY; }
    m.rooms.push_back(Room{1, 1, W - 2, H - 2, h.lit});
    return true;
}

std::string handMapText(const Map& m, const std::string& id, const std::string& name, int depth) {
    std::string out = "# Generated by dum_tool genmap. Edit freely - see docs/CAMPAIGNS.md for the symbols.\n";
    out += "[map " + id + "]\nname = " + name + "\ndepth = " + std::to_string(depth) + "\nlit = no\nmonsters = yes\n";
    out += "# place K = monster goblin_king\n# place a = item art_phial\nmap:\n";
    // Trim empty rock borders.
    int x0 = m.w, x1 = 0, y0 = m.h, y1 = 0;
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++)
            if (m.at(x, y) != T_ROCK) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    for (int y = y0; y <= y1; y++) {
        std::string row;
        for (int x = x0; x <= x1; x++) {
            uint8_t t = m.at(x, y);
            uint8_t ft = m.floorTex[y * m.w + x];
            bool lit = m.flag(x, y) & F_LIT;
            char c = ' ';
            switch (t) {
                case T_WALL: c = m.wallTex[y * m.w + x] == WT_CAVE ? '%' : '#'; break;
                case T_FLOOR: c = lit ? '*' : ft == FT_DIRT ? ',' : ft == FT_TILES ? '_' : ft == FT_WOOD ? '=' : '.'; break;
                case T_DOOR: c = '+'; break;
                case T_DOOR_OPEN: c = '\''; break;
                case T_DOOR_LOCKED: c = 'L'; break;
                case T_DOOR_SECRET: c = 'S'; break;
                case T_RUBBLE: c = ':'; break;
                case T_STAIRS_DOWN: c = '>'; break;
                case T_STAIRS_UP: c = '<'; break;
                default: c = ' ';
            }
            row += c;
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        out += row + "\n";
    }
    return out;
}
