// Dum - rebindable keys. Each action has a primary and a secondary key (scancodes, so
// they follow the physical key position on any keyboard layout).
#pragma once

#include <SDL.h>

#include <string>

enum KeyAction {
    K_FORWARD, K_BACK, K_STRAFE_L, K_STRAFE_R, K_TURN_L, K_TURN_R, K_RUN,
    K_ATTACK, K_CAST, K_USE, K_INVENTORY, K_CHARACTER, K_QUESTS, K_MAP,
    K_POTION, K_EAT, K_AUTOPICKUP, K_CHAT, K_HELP, K_LEGEND,
    K_AUTOFIGHT, K_EXPLORE, K_TRAVEL, K_AUTOWALK, K_FACE, K_SPELLBOOK,
    K_COUNT
};

struct KeyBinds {
    SDL_Scancode key[K_COUNT][2];

    static const char* actionName(int a) {
        static const char* n[K_COUNT] = {"Move forward", "Move back", "Strafe left", "Strafe right", "Turn left", "Turn right",
                                         "Run", "Attack", "Cast spell", "Use / open / pick up", "Inventory", "Character sheet",
                                         "Quest journal", "Map", "Quaff healing potion", "Eat", "Auto-pickup on/off", "Chat",
                                         "Help", "Key legend on/off", "Auto-fight on/off", "Auto-explore",
                                         "Travel to the stairs", "Auto-walk forward on/off", "Face nearest monster",
                                         "Spell book / abilities"};
        return n[a];
    }
    static const char* actionId(int a) {
        static const char* n[K_COUNT] = {"forward", "back", "strafe_left", "strafe_right", "turn_left", "turn_right", "run",
                                         "attack", "cast", "use", "inventory", "character", "quests", "map", "potion", "eat",
                                         "autopickup", "chat", "help", "legend", "autofight", "explore", "travel",
                                         "autowalk", "face", "spellbook"};
        return n[a];
    }

    KeyBinds() { defaults(); }
    void defaults() {
        auto set = [&](int a, SDL_Scancode p, SDL_Scancode s = SDL_SCANCODE_UNKNOWN) { key[a][0] = p; key[a][1] = s; };
        set(K_FORWARD, SDL_SCANCODE_W, SDL_SCANCODE_UP);
        set(K_BACK, SDL_SCANCODE_S, SDL_SCANCODE_DOWN);
        set(K_STRAFE_L, SDL_SCANCODE_A);
        set(K_STRAFE_R, SDL_SCANCODE_D);
        set(K_TURN_L, SDL_SCANCODE_LEFT);
        set(K_TURN_R, SDL_SCANCODE_RIGHT);
        set(K_RUN, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT);
        set(K_ATTACK, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL);
        set(K_CAST, SDL_SCANCODE_F);
        set(K_USE, SDL_SCANCODE_E, SDL_SCANCODE_SPACE);
        set(K_INVENTORY, SDL_SCANCODE_I);
        set(K_CHARACTER, SDL_SCANCODE_C);
        set(K_QUESTS, SDL_SCANCODE_J);
        set(K_MAP, SDL_SCANCODE_M, SDL_SCANCODE_TAB);
        set(K_POTION, SDL_SCANCODE_H);
        set(K_EAT, SDL_SCANCODE_X);
        set(K_AUTOPICKUP, SDL_SCANCODE_P);
        set(K_CHAT, SDL_SCANCODE_T, SDL_SCANCODE_RETURN);
        set(K_HELP, SDL_SCANCODE_F1);
        set(K_LEGEND, SDL_SCANCODE_F2);
        set(K_AUTOFIGHT, SDL_SCANCODE_Q);
        set(K_EXPLORE, SDL_SCANCODE_Z);
        set(K_TRAVEL, SDL_SCANCODE_G);
        set(K_AUTOWALK, SDL_SCANCODE_R);
        set(K_FACE, SDL_SCANCODE_V);
        set(K_SPELLBOOK, SDL_SCANCODE_B);
    }
    bool is(int a, SDL_Scancode sc) const { return sc != SDL_SCANCODE_UNKNOWN && (key[a][0] == sc || key[a][1] == sc); }
    bool held(int a, const Uint8* ks) const {
        return (key[a][0] != SDL_SCANCODE_UNKNOWN && ks[key[a][0]]) || (key[a][1] != SDL_SCANCODE_UNKNOWN && ks[key[a][1]]);
    }
    static std::string keyName(SDL_Scancode sc) {
        if (sc == SDL_SCANCODE_UNKNOWN) return "-";
        const char* n = SDL_GetScancodeName(sc);
        return n && *n ? n : "?";
    }
    // Short label for on-screen hints, e.g. "E" or "Space".
    std::string label(int a) const { return keyName(key[a][0]); }
    // Assign a key: removes it from any other action first so keys never clash.
    void bind(int a, int slot, SDL_Scancode sc) {
        for (int i = 0; i < K_COUNT; i++)
            for (int j = 0; j < 2; j++)
                if (key[i][j] == sc) key[i][j] = SDL_SCANCODE_UNKNOWN;
        key[a][slot] = sc;
    }
    // Settings file: "bind <action> <key> <key>" (key names with spaces use '_').
    std::string save() const {
        std::string out;
        for (int a = 0; a < K_COUNT; a++) out += std::string("bind ") + actionId(a) + " " + enc(key[a][0]) + " " + enc(key[a][1]) + "\n";
        return out;
    }
    void loadLine(const std::string& line) {
        char act[64] = {0}, k1[64] = {0}, k2[64] = {0};
        if (std::sscanf(line.c_str(), "%63s %63s %63s", act, k1, k2) < 2) return;
        for (int a = 0; a < K_COUNT; a++)
            if (std::string(act) == actionId(a)) { key[a][0] = dec(k1); key[a][1] = dec(k2); }
    }

private:
    static std::string enc(SDL_Scancode sc) {
        std::string n = sc == SDL_SCANCODE_UNKNOWN ? "-" : SDL_GetScancodeName(sc);
        for (auto& c : n) if (c == ' ') c = '_';
        return n.empty() ? "-" : n;
    }
    static SDL_Scancode dec(std::string n) {
        if (n.empty() || n == "-") return SDL_SCANCODE_UNKNOWN;
        for (auto& c : n) if (c == '_') c = ' ';
        return SDL_GetScancodeFromName(n.c_str());
    }
};
