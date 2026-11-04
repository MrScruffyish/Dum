// Dum - entry point: window, menus, main loop, command line.
//
//   dum                         start with the menu
//   dum --host                  host a co-op game (others join with --connect)
//   dum --connect <address>     join a co-op game
//   dum --dedicated             run a server without a window
//   dum --continue              continue the saved single player game
//   dum --export-art            write all generated art to data/gfx/*.png for editing
//   options: --name <n> --class <warrior|priest|wizard|rogue|ranger> --port <p>
//            --res <WxH> (3D view resolution, default from Options) --window <WxH> --fullscreen --seed <n>
//   developer: --spawn goblin,orc [--awake]   --screenshot out.png [--frames N] [--turn rad]
#include <SDL.h>
#include <SDL_image.h>

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "client/client.h"
#include "client/keys.h"
#include "client/renderer.h"
#include "client/sound.h"
#include "client/textures.h"
#include "game/data.h"
#include "net/net.h"
#include "server/server.h"

namespace {

struct Settings {
    std::string name;
    std::string cls = "warrior";
    std::string address = "localhost";
    int port = DEFAULT_PORT;
    float mouse = 1.0f;
    float volume = 0.7f;
    bool fullscreen = false;
    bool autoPickup = true;
    int difficulty = defaultDifficulty();
    std::string adventure;   // "" = endless dungeon, else a campaign id
    float gameSpeed = 1.0f, turnSpeed = 1.0f;
    bool strongAim = false, autoFace = true, quickCast = true;
    int renderW = 640, renderH = 360;   // resolution of the 3D view
    int winW = 0, winH = 0;             // window size (0 = twice the classic resolution)
    KeyBinds keys;
    std::string path, dir;

    void load() {
        char* p = SDL_GetPrefPath("dum", "dum");
        if (p) { dir = p; path = dir + "settings.txt"; SDL_free(p); }
        const char* user = std::getenv("USER");
        if (!user) user = std::getenv("USERNAME");
        name = user ? user : "Hero";
        if (!name.empty()) name[0] = (char)std::toupper(name[0]);
        std::ifstream f(path);
        std::string k, v;
        while (f >> k >> std::ws && std::getline(f, v)) {
            if (k == "name") name = v;
            else if (k == "class") cls = v;
            else if (k == "address") address = v;
            else if (k == "port") port = std::atoi(v.c_str());
            else if (k == "mouse") mouse = (float)std::atof(v.c_str());
            else if (k == "volume") volume = (float)std::atof(v.c_str());
            else if (k == "fullscreen") fullscreen = v == "1";
            else if (k == "autopickup") autoPickup = v == "1";
            else if (k == "bind") keys.loadLine(v);
            else if (k == "adventure") adventure = v == "-" ? "" : v;
            else if (k == "gamespeed") gameSpeed = clampf((float)std::atof(v.c_str()), 0.25f, 1.0f);
            else if (k == "turnspeed") turnSpeed = clampf((float)std::atof(v.c_str()), 0.25f, 3.0f);
            else if (k == "strongaim") strongAim = v == "1";
            else if (k == "autoface") autoFace = v == "1";
            else if (k == "quickcast") quickCast = v == "1";
            else if (k == "resolution") std::sscanf(v.c_str(), "%dx%d", &renderW, &renderH);
            else if (k == "window") std::sscanf(v.c_str(), "%dx%d", &winW, &winH);
            else if (k == "difficulty") { int d = gData.findDifficulty(v); if (d >= 0) difficulty = d; }
        }
    }
    void save() const {
        if (path.empty()) return;
        std::ofstream f(path);
        f << "name " << name << "\nclass " << cls << "\naddress " << address << "\nport " << port
          << "\nmouse " << mouse << "\nvolume " << volume << "\nfullscreen " << (fullscreen ? 1 : 0) << "\nautopickup " << (autoPickup ? 1 : 0) << "\nadventure " << (adventure.empty() ? "-" : adventure) << "\ndifficulty " << difficultyRules(difficulty).id << "\ngamespeed " << gameSpeed << "\nturnspeed " << turnSpeed
          << "\nstrongaim " << (strongAim ? 1 : 0) << "\nautoface " << (autoFace ? 1 : 0) << "\nquickcast " << (quickCast ? 1 : 0)
          << "\nresolution " << renderW << "x" << renderH << "\nwindow " << winW << "x" << winH << "\n" << keys.save();
    }
};

enum class Screen { Main, Class, Address, Name, HostChoice, Controls, Options, Playing };
enum MenuId { M_CONTINUE, M_NEW, M_HOST, M_JOIN, M_NAME, M_ADVENTURE, M_DIFFICULTY, M_OPTIONS, M_CONTROLS, M_QUIT };
enum OptId { O_SPEED, O_TURN, O_MOUSE, O_AIM, O_FACE, O_QUICK, O_RES, O_WINDOW, O_VOLUME, O_FULLSCREEN, O_COUNT };
// Resolutions of the 3D view (the HUD and menus always look the same) and window sizes.
const int kRes[][2] = {{640, 360}, {960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}};
const int kWin[][2] = {{960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
enum class Mode { Single, Host, Join };

struct App {
    Settings set;
    SDL_Window* win = nullptr;
    SDL_Renderer* sdlr = nullptr;
    SDL_Texture* tex = nullptr;
    SDL_Texture* uiTex = nullptr;   // HUD/menu layer when the 3D view is not 640x360
    std::unique_ptr<Renderer> r;
    Assets assets;
    Sound sound;
    Screen screen = Screen::Main;
    Mode mode = Mode::Single;
    int sel = 0;
    std::string editBuf;
    std::string status;
    uint32_t seed = 0;
    bool loadSave = false;   // continue from the save file
    int bindSlot = -1;       // Controls: waiting for a key for this slot (0/1)
    std::string savePath(Mode m) const { return set.dir + (m == Mode::Single ? "single.sav" : "coop.sav"); }
    std::unique_ptr<Server> server;
    std::shared_ptr<ServerNet> serverNet;
    std::unique_ptr<Client> client;
    bool running = true;
    // menu backdrop
    Map menuMap;
    float menuAngle = 0;

    int classIndex() const { int i = gData.findClass(set.cls); return i < 0 ? 0 : i; }

    bool startGame(std::string& err) {
        server.reset();
        client.reset();
        serverNet.reset();
        auto cnet = std::make_shared<ClientNet>();
        if (mode != Mode::Join) {
            serverNet = std::make_shared<ServerNet>();
            if (mode == Mode::Host && !serverNet->listen((uint16_t)set.port, 8, err)) return false;
            auto pipe = std::make_shared<LoopbackPipe>();
            serverNet->attachLoopback(pipe);
            cnet->useLoopback(pipe);
            uint32_t s = seed ? seed : (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
            server = std::make_unique<Server>(serverNet, s);
            server->setDifficulty(set.difficulty);
            if (loadSave && !server->loadGame(savePath(mode), err)) { server.reset(); return false; }
            int ci = gData.findCampaign(set.adventure);
            if (!loadSave && ci >= 0) server->startCampaign(ci);
            if (!set.dir.empty()) server->setSavePath(savePath(mode));
            server->setAdoptAnyCharacter(mode == Mode::Single);
        } else {
            if (!cnet->connect(set.address, (uint16_t)set.port, err)) return false;
        }
        client = std::make_unique<Client>(cnet, set.name, classIndex(), &assets, &sound, set.autoPickup, &set.keys);
        client->mouseSens = 0.00175f * set.mouse;   // per window pixel of mouse movement
        client->turnSpeed = set.turnSpeed;
        client->strongAim = set.strongAim;
        client->autoFace = set.autoFace;
        client->quickCast = set.quickCast;
        if (mode == Mode::Host)
            status = "Hosting on UDP port " + std::to_string(set.port) + ". Friends join with: dum --connect <your-ip>";
        screen = Screen::Playing;
        return true;
    }

    void stopGame() {
        if (client && client->autoPickup != set.autoPickup) { set.autoPickup = client->autoPickup; set.save(); }
        client.reset();
        server.reset();
        serverNet.reset();
        SDL_SetRelativeMouseMode(SDL_FALSE);
        screen = Screen::Main;
        sel = 0;
    }

    // -------------------------------------------------------------- menus --
    std::vector<std::pair<MenuId, std::string>> mainItems() const {
        char m[32], v[32];
        std::snprintf(m, sizeof m, "%.1f", set.mouse);
        std::snprintf(v, sizeof v, "%d%%", (int)(set.volume * 100 + 0.5f));
        std::vector<std::pair<MenuId, std::string>> it;
        SaveInfo si = Server::peekSave(savePath(Mode::Single));
        if (si.ok && !si.chars.empty()) {
            int c = gData.findClass(si.chars[0].cls);
            it.push_back({M_CONTINUE, "Continue: " + si.chars[0].name + " the " + (c >= 0 ? gData.classes[c].name : "?") +
                                          ", level " + std::to_string(si.chars[0].level) + ", depth " + std::to_string(si.depth)});
        }
        it.push_back({M_NEW, "New single player game"});
        it.push_back({M_HOST, "Host co-op game"});
        it.push_back({M_JOIN, "Join co-op game"});
        it.push_back({M_NAME, "Name: " + set.name});
        int ci = gData.findCampaign(set.adventure);
        it.push_back({M_ADVENTURE, "Adventure: " + (ci < 0 ? std::string("Endless dungeon") : gData.campaigns[ci].name) + "  (Left/Right)"});
        it.push_back({M_DIFFICULTY, std::string("Difficulty: ") + difficultyRules(set.difficulty).name + "  (Left/Right)"});
        (void)0;
        it.push_back({M_OPTIONS, "Options (game speed, aiming, sound...)"});
        it.push_back({M_CONTROLS, "Controls"});
        it.push_back({M_QUIT, "Quit"});
        (void)m; (void)v;
        return it;
    }

    // After choosing a class (or continuing): start, or show the error.
    void launch() {
        std::string err;
        if (!startGame(err)) { status = "Error: " + err; screen = Screen::Main; sel = 0; }
    }

    // Continue a co-op save: skip class selection if our character is in it.
    bool saveHasMe(const SaveInfo& si) const {
        for (auto& c : si.chars) {
            std::string a = c.name, b = set.name;
            for (auto& ch : a) ch = (char)std::tolower((unsigned char)ch);
            for (auto& ch : b) ch = (char)std::tolower((unsigned char)ch);
            if (a == b) return true;
        }
        return false;
    }

    void menuKey(SDL_Keycode k) {
        if (screen == Screen::Name || screen == Screen::Address) {
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                if (screen == Screen::Name) { if (!editBuf.empty()) set.name = editBuf; screen = Screen::Main; }
                else { if (!editBuf.empty()) set.address = editBuf; screen = Screen::Class; sel = classIndex(); }
                SDL_StopTextInput();
                set.save();
            } else if (k == SDLK_ESCAPE) {
                SDL_StopTextInput();
                screen = Screen::Main;
            } else if (k == SDLK_BACKSPACE && !editBuf.empty()) {
                editBuf.pop_back();
            }
            return;
        }
        if (screen == Screen::Class) {
            int n = (int)gData.classes.size();
            if (k == SDLK_UP || k == SDLK_w) sel = (sel + n - 1) % n;
            else if (k == SDLK_DOWN || k == SDLK_s) sel = (sel + 1) % n;
            else if (k == SDLK_ESCAPE) { screen = Screen::Main; sel = 0; }
            else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_KP_ENTER) {
                set.cls = gData.classes[sel].id;
                set.save();
                launch();
            }
            return;
        }
        if (screen == Screen::Controls) {
            if (bindSlot >= 0) {
                if (k != SDLK_ESCAPE) set.keys.bind(sel, bindSlot, SDL_GetScancodeFromKey(k));
                bindSlot = -1;
                set.save();
                return;
            }
            if (k == SDLK_UP || k == SDLK_w) sel = (sel + K_COUNT - 1) % K_COUNT;
            else if (k == SDLK_DOWN || k == SDLK_s) sel = (sel + 1) % K_COUNT;
            else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) bindSlot = 0;
            else if (k == SDLK_TAB) bindSlot = 1;
            else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) { set.keys.key[sel][1] = SDL_SCANCODE_UNKNOWN; set.save(); }
            else if (k == SDLK_r) { set.keys.defaults(); set.save(); }
            else if (k == SDLK_ESCAPE) { screen = Screen::Main; sel = 0; }
            return;
        }
        if (screen == Screen::Options) {
            if (k == SDLK_UP || k == SDLK_w) sel = (sel + O_COUNT - 1) % O_COUNT;
            else if (k == SDLK_DOWN || k == SDLK_s) sel = (sel + 1) % O_COUNT;
            else if (k == SDLK_ESCAPE) { screen = Screen::Main; sel = 0; }
            else if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_RETURN || k == SDLK_SPACE) {
                int d = k == SDLK_LEFT ? -1 : 1;
                switch (sel) {
                    case O_SPEED: {
                        const float sp[3] = {0.5f, 0.75f, 1.0f};
                        int i = set.gameSpeed < 0.6f ? 0 : set.gameSpeed < 0.9f ? 1 : 2;
                        set.gameSpeed = sp[(i + d + 3) % 3];
                        break;
                    }
                    case O_TURN: set.turnSpeed = clampf(set.turnSpeed + d * 0.25f, 0.25f, 3.0f); break;
                    case O_MOUSE: set.mouse = clampf(set.mouse + d * 0.1f, 0.1f, 5.0f); break;
                    case O_AIM: set.strongAim = !set.strongAim; break;
                    case O_FACE: set.autoFace = !set.autoFace; break;
                    case O_QUICK: set.quickCast = !set.quickCast; break;
                    case O_RES: {
                        int n = (int)(sizeof kRes / sizeof kRes[0]), i = 0;
                        for (int j = 0; j < n; j++) if (kRes[j][0] == set.renderW) i = j;
                        i = (i + d + n) % n;
                        set.renderW = kRes[i][0];
                        set.renderH = kRes[i][1];
                        applyResolution(set.renderW, set.renderH);
                        break;
                    }
                    case O_WINDOW: {
                        if (set.fullscreen) toggleFullscreen();
                        int cw, ch, n = (int)(sizeof kWin / sizeof kWin[0]);
                        SDL_GetWindowSize(win, &cw, &ch);
                        int i = d > 0 ? 0 : n - 1;   // next bigger / smaller size than now
                        if (d > 0) { while (i < n && kWin[i][0] <= cw) i++; if (i == n) i = 0; }
                        else { while (i >= 0 && kWin[i][0] >= cw) i--; if (i < 0) i = n - 1; }
                        SDL_DisplayMode dm;
                        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 && kWin[i][0] > dm.w) i = 0;
                        set.winW = kWin[i][0];
                        set.winH = kWin[i][1];
                        SDL_SetWindowSize(win, set.winW, set.winH);
                        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                        break;
                    }
                    case O_VOLUME: set.volume = clampf(set.volume + d * 0.1f, 0.0f, 1.0f); sound.setMasterVolume(set.volume); sound.play(SND_PICKUP); break;
                    case O_FULLSCREEN: toggleFullscreen(); return;
                }
                set.save();
            }
            return;
        }
        if (screen == Screen::HostChoice) {
            if (k == SDLK_UP || k == SDLK_w || k == SDLK_DOWN || k == SDLK_s) sel = 1 - sel;
            else if (k == SDLK_ESCAPE) { screen = Screen::Main; sel = 0; }
            else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_KP_ENTER) {
                loadSave = sel == 0;
                if (loadSave && saveHasMe(Server::peekSave(savePath(Mode::Host)))) launch();
                else { screen = Screen::Class; sel = classIndex(); }
            }
            return;
        }
        auto items = mainItems();
        int n = (int)items.size();
        sel = clampi(sel, 0, n - 1);
        MenuId id = items[sel].first;
        if (k == SDLK_UP || k == SDLK_w) sel = (sel + n - 1) % n;
        else if (k == SDLK_DOWN || k == SDLK_s) sel = (sel + 1) % n;
        else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
            int d = k == SDLK_LEFT ? -1 : 1;
            if (id == M_ADVENTURE) cycleAdventure(d);
            if (id == M_DIFFICULTY) set.difficulty = (set.difficulty + d + numDifficulties()) % numDifficulties();
            set.save();
        } else if (k == SDLK_ESCAPE) {
            running = false;
        } else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_KP_ENTER) {
            status.clear();
            switch (id) {
                case M_CONTINUE: mode = Mode::Single; loadSave = true; launch(); break;
                case M_NEW: mode = Mode::Single; loadSave = false; screen = Screen::Class; sel = classIndex(); break;
                case M_HOST:
                    mode = Mode::Host;
                    loadSave = false;
                    if (Server::peekSave(savePath(Mode::Host)).ok) { screen = Screen::HostChoice; sel = 0; }
                    else { screen = Screen::Class; sel = classIndex(); }
                    break;
                case M_JOIN: mode = Mode::Join; screen = Screen::Address; editBuf = set.address; SDL_StartTextInput(); break;
                case M_NAME: screen = Screen::Name; editBuf = set.name; SDL_StartTextInput(); break;
                case M_DIFFICULTY: set.difficulty = (set.difficulty + 1) % numDifficulties(); set.save(); break;
                case M_ADVENTURE: cycleAdventure(1); set.save(); break;
                case M_CONTROLS: screen = Screen::Controls; sel = 0; break;
                case M_OPTIONS: screen = Screen::Options; sel = 0; break;
                case M_QUIT: running = false; break;
                default: break;
            }
        }
    }

    void cycleAdventure(int d) {
        int n = (int)gData.campaigns.size() + 1;  // 0 = endless
        int cur = gData.findCampaign(set.adventure) + 1;
        cur = (cur + d + n) % n;
        set.adventure = cur == 0 ? "" : gData.campaigns[cur - 1].id;
    }

    void toggleFullscreen() {
        set.fullscreen = !set.fullscreen;
        SDL_SetWindowFullscreen(win, set.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        set.save();
    }

    void drawMenuBackdrop(float dt) {
        menuAngle += dt * 0.12f;
        std::vector<LightSource> lights = {{Vec2(menuMap.startX + 0.5f, menuMap.startY + 0.5f), 7.0f, 1.0f}};
        r->setupLights(menuMap, lights);
        std::vector<SpriteDraw> none;
        r->drawWorld(menuMap, assets, Vec2(menuMap.startX + 0.5f, menuMap.startY + 0.5f), menuAngle, r->H / 2.0f, none);
        r->darken(0.55f);
    }

    void drawMenu(float dt) {
        drawMenuBackdrop(dt);
        r->beginUi();
        Renderer& R = *r;
        std::string title = "DUM";
        int ts = std::max(4, R.H / 40);
        R.text((R.W - R.textW(title, ts)) / 2 + 2, 22 + 2, title, 0xFF300000, ts, false);
        R.text((R.W - R.textW(title, ts)) / 2, 22, title, 0xFFD03020, ts, false);
        std::string sub = "a dungeon crawl for brave parties";
        R.text((R.W - R.textW(sub)) / 2, 30 + 7 * ts, sub, 0xFFC0A080);
        int y = 50 + 7 * ts;

        if (screen == Screen::Main) {
            auto items = mainItems();
            int scale = 2, step = items.size() > 10 ? 18 : 21;
            for (int i = 0; i < (int)items.size(); i++) {
                bool s = i == sel;
                const std::string& t = items[i].second;
                int w = R.textW(t, scale);
                if (s) R.fill((R.W - w) / 2 - 8, y - 3, w + 16, 8 * scale + 4, 0xFF4A2A10, 200);
                R.text((R.W - w) / 2, y, t, s ? 0xFFFFE080 : (items[i].first == M_CONTINUE ? 0xFF90E090 : 0xFFD0C8B8), scale);
                y += step;
            }
        } else if (screen == Screen::Options) {
            R.text((R.W - R.textW("Options", 2)) / 2, y - 8, "Options", 0xFFD0C8B8, 2);
            char buf[64];
            std::string rows[O_COUNT], help[O_COUNT];
            std::snprintf(buf, sizeof buf, "%d%%", (int)(set.gameSpeed * 100 + 0.5f));
            rows[O_SPEED] = std::string("Game speed: ") + buf;
            help[O_SPEED] = "Slower game = more time to react (single player).";
            std::snprintf(buf, sizeof buf, "%.2f", set.turnSpeed);
            rows[O_TURN] = std::string("Keyboard turn speed: ") + buf;
            help[O_TURN] = "How fast the turn keys (and auto-facing) turn you.";
            std::snprintf(buf, sizeof buf, "%.1f", set.mouse);
            rows[O_MOUSE] = std::string("Mouse speed: ") + buf;
            help[O_MOUSE] = "Mouse turning sensitivity.";
            rows[O_AIM] = std::string("Aim assist: ") + (set.strongAim ? "strong" : "normal");
            help[O_AIM] = "Strong: a much wider cone picks your target.";
            rows[O_FACE] = std::string("Auto-fight turns toward enemies: ") + (set.autoFace ? "on" : "off");
            help[O_FACE] = "With auto-fight (Q) you don't need to turn at all.";
            rows[O_QUICK] = std::string("Keys 1-9: ") + (set.quickCast ? "cast at once" : "select only");
            help[O_QUICK] = "Cast at once: one key press uses the spell, ability or wand.";
            rows[O_RES] = "Resolution: " + std::to_string(set.renderW) + "x" + std::to_string(set.renderH) + (set.renderW == 640 ? " (classic)" : "");
            help[O_RES] = "Sharper 3D view, but slower. Menus and the HUD look the same.";
            {
                int cw, ch;
                SDL_GetWindowSize(win, &cw, &ch);
                rows[O_WINDOW] = "Window size: " + (set.fullscreen ? std::string("fullscreen") : std::to_string(cw) + "x" + std::to_string(ch));
            }
            help[O_WINDOW] = "You can also drag the window edges. F11: fullscreen.";
            std::snprintf(buf, sizeof buf, "%d%%", (int)(set.volume * 100 + 0.5f));
            rows[O_VOLUME] = std::string("Volume: ") + buf;
            help[O_VOLUME] = "";
            rows[O_FULLSCREEN] = std::string("Fullscreen: ") + (set.fullscreen ? "on" : "off");
            help[O_FULLSCREEN] = "Also F11 or Alt+Enter.";
            int oy = y + 14;
            for (int i = 0; i < O_COUNT; i++) {
                bool s = i == sel;
                int w = R.textW(rows[i], 2);
                if (s) R.fill((R.W - w) / 2 - 8, oy - 3, w + 16, 20, 0xFF4A2A10, 200);
                R.text((R.W - w) / 2, oy, rows[i], s ? 0xFFFFE080 : 0xFFD0C8B8, 2);
                oy += O_COUNT > 8 ? 18 : 21;
            }
            R.text((R.W - R.textW(help[sel])) / 2, oy + 6, help[sel], 0xFFB0A890);
            std::string h = "Left/Right: change   Esc: back   (changes apply to the next game)";
            R.text((R.W - R.textW(h)) / 2, oy + 20, h, 0xFF908070);
        } else if (screen == Screen::Controls) {
            R.text((R.W - R.textW("Controls", 2)) / 2, y - 8, "Controls", 0xFFD0C8B8, 2);
            int cx = R.W / 2 - 190, cy = y + 12;
            R.fill(cx - 8, cy - 4, 396, K_COUNT * 10 + 8, 0xFF100C08, 200);
            for (int a = 0; a < K_COUNT; a++) {
                bool s = a == sel;
                if (s) R.fill(cx - 4, cy - 1, 388, 10, 0xFF4A2A10);
                R.text(cx, cy, KeyBinds::actionName(a), s ? 0xFFFFE080 : 0xFFD0C8B8);
                std::string k1 = (s && bindSlot == 0) ? "press a key..." : KeyBinds::keyName(set.keys.key[a][0]);
                std::string k2 = (s && bindSlot == 1) ? "press a key..." : KeyBinds::keyName(set.keys.key[a][1]);
                R.text(cx + 170, cy, k1, 0xFFFFFFFF);
                R.text(cx + 280, cy, k2, 0xFFB0B0B0);
                cy += 10;
            }
            std::string h = "Enter: change key   Tab: change 2nd key   Del: clear 2nd   R: defaults   Esc: back";
            R.text((R.W - R.textW(h)) / 2, cy + 8, h, 0xFF908070);
            R.text((R.W - R.textW("Mouse: left = attack, right = cast, wheel = select spell")) / 2, cy + 20,
                   "Mouse: left = attack, right = cast, wheel = select spell", 0xFF908070);
        } else if (screen == Screen::HostChoice) {
            SaveInfo si = Server::peekSave(savePath(Mode::Host));
            std::string party;
            for (auto& c : si.chars) party += (party.empty() ? "" : ", ") + c.name + " (" + std::to_string(c.level) + ")";
            std::string opts[2] = {"Continue the saved dungeon (depth " + std::to_string(si.depth) + ", " + difficultyRules(si.difficulty).name + ")",
                                   std::string("Start a new dungeon (") + difficultyRules(set.difficulty).name + ")"};
            for (int i = 0; i < 2; i++) {
                int w = R.textW(opts[i], 2);
                if (i == sel) R.fill((R.W - w) / 2 - 8, y - 3, w + 16, 20, 0xFF4A2A10, 200);
                R.text((R.W - w) / 2, y, opts[i], i == sel ? 0xFFFFE080 : 0xFFD0C8B8, 2);
                y += 24;
            }
            std::string pl = "Saved party: " + party;
            R.text((R.W - R.textW(pl)) / 2, y + 10, pl.substr(0, 100), 0xFF908070);
        } else if (screen == Screen::Name || screen == Screen::Address) {
            std::string label = screen == Screen::Name ? "Your name:" : "Server address (host or IP):";
            R.text((R.W - R.textW(label, 2)) / 2, y, label, 0xFFD0C8B8, 2);
            std::string v = editBuf + (((SDL_GetTicks() / 400) % 2) ? "_" : " ");
            R.text((R.W - R.textW(v, 2)) / 2, y + 30, v, 0xFFFFE080, 2);
            R.text((R.W - R.textW("Enter to accept, Esc to cancel")) / 2, y + 60, "Enter to accept, Esc to cancel", 0xFF908070);
        } else if (screen == Screen::Class) {
            R.text((R.W - R.textW("Choose your class", 2)) / 2, y - 6, "Choose your class", 0xFFD0C8B8, 2);
            y += 18;
            int lx = R.W / 2 - 230;
            for (int i = 0; i < (int)gData.classes.size(); i++) {
                const ClassDef& c = gData.classes[i];
                bool s = i == sel;
                if (s) R.fill(lx - 6, y + i * 22 - 3, 130, 20, 0xFF4A2A10, 200);
                R.text(lx, y + i * 22, c.name, s ? 0xFFFFE080 : 0xFFD0C8B8, 2);
            }
            const ClassDef& c = gData.classes[sel];
            int dx = lx + 150, dy = y;
            R.fill(dx - 8, dy - 6, 330, 150, 0xFF100C08, 200);
            // word-wrap description
            std::string d = c.desc;
            while (!d.empty()) {
                size_t n = std::min<size_t>(d.size(), 52);
                if (n < d.size()) { size_t sp = d.rfind(' ', n); if (sp != std::string::npos) n = sp; }
                R.text(dx, dy, d.substr(0, n), 0xFFE0D8C8);
                d = d.substr(std::min(d.size(), n + 1));
                dy += 10;
            }
            dy += 6;
            std::string stats;
            for (int s = 0; s < NUM_STATS; s++) stats += std::string(kStatNames[s]) + " " + std::to_string(c.stats[s]) + "  ";
            R.text(dx, dy, stats, 0xFFC0B090);
            dy += 12;
            R.text(dx, dy, "Hit die d" + std::to_string(c.hitDie) + "   Melee " + std::to_string(c.melee) + "  Ranged " + std::to_string(c.ranged) + "  Magic " + std::to_string(c.magic), 0xFFC0B090);
            dy += 12;
            std::string sp = "Spells: ";
            if (c.spells.empty()) sp += "none";
            for (size_t i = 0; i < c.spells.size(); i++) sp += (i ? ", " : "") + gData.spells[c.spells[i]].name;
            while (!sp.empty()) {
                size_t n = std::min<size_t>(sp.size(), 52);
                if (n < sp.size()) { size_t p = sp.rfind(' ', n); if (p != std::string::npos) n = p; }
                R.text(dx, dy, sp.substr(0, n), 0xFFB0A8E0);
                sp = sp.substr(std::min(sp.size(), n + 1));
                dy += 10;
            }
            dy += 4;
            std::string items = "Gear: ";
            for (size_t i = 0; i < c.startItems.size(); i++) {
                std::string n = gData.items[c.startItems[i]].name;
                if (items.find(n) != std::string::npos) continue;
                items += (i ? ", " : "") + n;
            }
            while (!items.empty()) {
                size_t n = std::min<size_t>(items.size(), 52);
                if (n < items.size()) { size_t p = items.rfind(' ', n); if (p != std::string::npos) n = p; }
                R.text(dx, dy, items.substr(0, n), 0xFFA0C0A0);
                items = items.substr(std::min(items.size(), n + 1));
                dy += 10;
            }
            // class portrait
            const Texture& pt = assets.players[sel];
            R.blit(pt, dx + 250, y + 70, 64, 64);
            std::string ms = mode == Mode::Single ? "Single player" : mode == Mode::Host ? "Hosting a co-op game" : "Joining " + set.address;
            R.text((R.W - R.textW(ms)) / 2, R.H - 30, ms + "  -  Enter to start, Esc to go back", 0xFF908070);
        }
        if (screen == Screen::Main) {
            auto items = mainItems();
            if (sel >= 0 && sel < (int)items.size() && items[sel].first == M_DIFFICULTY) {
                const std::string& d = difficultyRules(set.difficulty).desc;
                R.text((R.W - R.textW(d)) / 2, R.H - 28, d, 0xFFB0A890);
            }
            if (sel >= 0 && sel < (int)items.size() && items[sel].first == M_ADVENTURE) {
                int ci = gData.findCampaign(set.adventure);
                std::string d = ci < 0 ? "Descend forever. Bosses wait on levels 3, 6, 9, 12, 15 and 20." : gData.campaigns[ci].desc;
                R.text((R.W - R.textW(d)) / 2, R.H - 28, d.substr(0, 104), 0xFFB0A890);
            }
        }
        if (!status.empty()) R.text((R.W - R.textW(status)) / 2, R.H - 16, status, 0xFFFF8060);
    }

    void present() {
        r->endUi();
        SDL_UpdateTexture(tex, nullptr, r->fb.data(), r->W * 4);
        SDL_RenderClear(sdlr);
        SDL_RenderCopy(sdlr, tex, nullptr, nullptr);
        if (r->layered() && r->uiUsed()) {
            SDL_UpdateTexture(uiTex, nullptr, r->uiLayer().data(), Renderer::UI_W * 4);
            SDL_RenderCopy(sdlr, uiTex, nullptr, nullptr);
        }
        SDL_RenderPresent(sdlr);
    }

    // Change the resolution of the 3D view (can be done any time).
    void applyResolution(int w, int h) {
        w = clampi(w, 320, 3840);
        h = clampi(h, 180, 2160);
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(sdlr, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!uiTex) {
            uiTex = SDL_CreateTexture(sdlr, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Renderer::UI_W, Renderer::UI_H);
            SDL_SetTextureBlendMode(uiTex, SDL_BLENDMODE_BLEND);
        }
        SDL_RenderSetLogicalSize(sdlr, w, h);
        if (r) r->resize(w, h); else r = std::make_unique<Renderer>(w, h);
    }
};

volatile std::sig_atomic_t gStop = 0;

int parseDifficulty(const std::string& s) {
    for (int i = 0; i < numDifficulties(); i++) {
        std::string n = difficultyRules(i).name;
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (s == n || s == difficultyRules(i).id) return i;
    }
    return defaultDifficulty();
}

int runDedicated(uint16_t port, uint32_t seed, int difficulty, const std::string& savePath, const std::string& campaign) {
    auto net = std::make_shared<ServerNet>();
    std::string err;
    if (!net->listen(port, 8, err)) { std::fprintf(stderr, "dum: %s\n", err.c_str()); return 1; }
    if (!seed) seed = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    Server server(net, seed);
    server.setDifficulty(difficulty);
    if (!campaign.empty()) {
        int ci = gData.findCampaign(campaign);
        if (ci < 0) { std::fprintf(stderr, "dum: no campaign '%s'\n", campaign.c_str()); return 1; }
        server.startCampaign(ci);
    }
    if (Server::peekSave(savePath).ok) {
        if (!server.loadGame(savePath, err)) { std::fprintf(stderr, "dum: %s: %s\n", savePath.c_str(), err.c_str()); return 1; }
        std::printf("Continuing %s\n", savePath.c_str());
    }
    server.setSavePath(savePath);
    std::signal(SIGINT, [](int) { gStop = 1; });
    std::signal(SIGTERM, [](int) { gStop = 1; });
    std::printf("Dum dedicated server on UDP port %u, %s, saving to %s (Ctrl+C to stop)\n", port,
                difficultyRules(server.difficulty()).name.c_str(), savePath.c_str());
    auto next = std::chrono::steady_clock::now();
    while (!gStop) {
        server.tick();
        next += std::chrono::microseconds((int)(TICK_DT * 1e6));
        std::this_thread::sleep_until(next);
        auto now = std::chrono::steady_clock::now();
        if (now - next > std::chrono::seconds(1)) next = now;
    }
    std::printf("Saving and stopping.\n");
    return 0;  // the Server destructor saves
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a.rfind("--", 0) != 0) continue;
        a = a.substr(2);
        bool hasVal = i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0;
        opt[a] = hasVal ? argv[++i] : "1";
    }
    if (opt.count("help")) {
        std::printf("Usage: dum [--continue | --host | --connect <addr> | --dedicated [--save FILE]]\n"
                    "           [--name N] [--class C] [--difficulty easy|normal|hard] [--port P]\n"
                    "           [--res WxH] [--window WxH] [--fullscreen] [--seed N] [--export-art]\n");
        return 0;
    }

    std::string dataDir = opt.count("data") ? opt["data"] : findDataDir(argv[0]);
    std::string err;
    if (!gData.load(dataDir, err)) {
        std::fprintf(stderr, "dum: failed to load game data from '%s': %s\n", dataDir.c_str(), err.c_str());
        return 1;
    }
    uint16_t port = opt.count("port") ? (uint16_t)std::atoi(opt["port"].c_str()) : DEFAULT_PORT;
    uint32_t seed = opt.count("seed") ? (uint32_t)std::strtoul(opt["seed"].c_str(), nullptr, 10) : 0;

    if (opt.count("dedicated")) {
        SDL_Init(0);
        int rc = runDedicated(port, seed, opt.count("difficulty") ? parseDifficulty(opt["difficulty"]) : defaultDifficulty(),
                              opt.count("save") ? opt["save"] : "dum-server.sav", opt.count("campaign") ? opt["campaign"] : "");
        netShutdown();
        return rc;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "dum: SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    IMG_Init(IMG_INIT_PNG);

    App app;
    app.set.load();
    if (opt.count("export-art")) {
        app.assets.build();
        std::string dir = dataDir + "/gfx";
#ifdef _WIN32
        std::string cmd = "mkdir \"" + dir + "\" 2>nul";
#else
        std::string cmd = "mkdir -p '" + dir + "'";
#endif
        if (std::system(cmd.c_str()) != 0) std::printf("note: could not create %s\n", dir.c_str());
        int n = app.assets.exportPngs(dir);
        std::printf("Wrote %d PNG files to %s\nEdit any of them; Dum uses them instead of the built-in art.\n"
                    "Delete a file to get the generated picture back.\n", n, dir.c_str());
        IMG_Quit();
        SDL_Quit();
        return 0;
    }

    int rw = app.set.renderW, rh = app.set.renderH;
    if (opt.count("res")) std::sscanf(opt["res"].c_str(), "%dx%d", &rw, &rh);
    rw = clampi(rw, 320, 3840);
    rh = clampi(rh, 180, 2160);
    int ww = app.set.winW > 0 ? app.set.winW : 1280, wh = app.set.winH > 0 ? app.set.winH : 720;
    if (opt.count("window")) std::sscanf(opt["window"].c_str(), "%dx%d", &ww, &wh);
    if (opt.count("fullscreen")) app.set.fullscreen = true;
    if (opt.count("name")) app.set.name = opt["name"];
    if (opt.count("class")) app.set.cls = opt["class"];
    if (opt.count("difficulty")) app.set.difficulty = parseDifficulty(opt["difficulty"]);
    if (opt.count("campaign")) app.set.adventure = opt["campaign"] == "1" ? "" : opt["campaign"];
    app.set.port = port;
    app.seed = seed;

    // Mouse turning uses real mouse movement, whatever the window size or resolution.
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SCALING, "0");
    app.win = SDL_CreateWindow("Dum", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                               SDL_WINDOW_RESIZABLE | (app.set.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!app.win) { std::fprintf(stderr, "dum: window: %s\n", SDL_GetError()); return 1; }
    app.sdlr = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.sdlr) app.sdlr = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);
    if (!app.sdlr) { std::fprintf(stderr, "dum: renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // crisp pixels
    app.applyResolution(rw, rh);

    app.assets.build();
    app.assets.loadOverrides(dataDir);
    app.sound.init();
    app.sound.loadOverrides(dataDir);
    app.sound.setMasterVolume(app.set.volume);
    generateLevel(app.menuMap, 4242, 1);

    // Command line shortcuts skip the menu.
    if (opt.count("host") || opt.count("connect") || opt.count("single") || opt.count("continue")) {
        app.mode = opt.count("connect") ? Mode::Join : opt.count("host") ? Mode::Host : Mode::Single;
        app.loadSave = opt.count("continue") > 0;
        if (opt.count("connect")) app.set.address = opt["connect"] == "1" ? "localhost" : opt["connect"];
        if (!app.startGame(err)) { std::fprintf(stderr, "dum: %s\n", err.c_str()); app.status = "Error: " + err; app.screen = Screen::Main; }
    }

    // --screenshot <file.png> [--frames N] [--turn radians]: render, save and exit (for testing).
    std::string shotPath = opt.count("screenshot") ? opt["screenshot"] : "";
    int shotFrames = opt.count("frames") ? std::atoi(opt["frames"].c_str()) : 90;
    float shotTurn = opt.count("turn") ? (float)std::atof(opt["turn"].c_str()) : 0.0f;
    int frameNo = 0;

    auto last = std::chrono::steady_clock::now();
    bool showFps = false;
    float fpsAcc = 0, fpsShown = 0;
    int fpsFrames = 0;
    double acc = 0;
    while (app.running) {
        static bool wentDeep = false;
        if (opt.count("depth") && app.server && app.client && app.client->inGame() && !wentDeep) {
            app.server->debugGoto(std::atoi(opt["depth"].c_str()));
            wentDeep = true;
        }
        static bool autoStarted = false;
        if (opt.count("auto") && app.client && app.client->inGame() && !autoStarted && frameNo > 5) {
            for (auto& m : {std::string("fight"), std::string("explore"), std::string("travel")})
                if (opt["auto"].find(m) != std::string::npos) app.client->debugAuto(m);
            autoStarted = true;
        }
        // Keep exploring/travelling in tests whenever it stops (e.g. after a fight).
        if (opt.count("auto") && autoStarted && app.client && frameNo % 90 == 0 && !app.client->debugAutoActive()) {
            if (opt["auto"].find("explore") != std::string::npos) app.client->debugAuto("explore");
            else if (opt["auto"].find("travel") != std::string::npos) app.client->debugAuto("travel");
        }
        static bool given = false;
        if (opt.count("give") && app.server && app.client && app.client->inGame() && !given) {
            app.server->debugGive(0, opt["give"]);
            given = true;
        }
        static bool leveled = false;
        if (opt.count("level") && app.server && app.client && app.client->inGame() && !leveled) {
            app.server->debugSetLevel(0, std::atoi(opt["level"].c_str()));
            leveled = true;
        }
        static bool spawned = false;
        if (opt.count("spawn") && app.server && app.client && app.client->inGame() && !spawned) {
            app.server->debugSpawn(opt["spawn"], opt.count("awake") > 0);
            spawned = true;
        }
        if (!shotPath.empty()) {
            if (++frameNo == shotFrames / 2 && app.client) app.client->debugTurn(shotTurn);
            if (frameNo == shotFrames - 2 && app.client && opt.count("ui")) app.client->debugUi(opt["ui"]);
            if (frameNo >= shotFrames) {
                app.r->endUi();
                std::vector<uint32_t> pic = app.r->composite();
                SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(pic.data(), app.r->W, app.r->H, 32, app.r->W * 4, SDL_PIXELFORMAT_ARGB8888);
                IMG_SavePNG(s, shotPath.c_str());
                SDL_FreeSurface(s);
                std::printf("saved %s\n", shotPath.c_str());
                if (app.client) std::printf("client: depth %d, %d tiles seen\n", app.client->debugDepth(), app.client->debugSeenTiles());
                break;
            }
        }
        app.r->beginFrame();
        auto now = std::chrono::steady_clock::now();
        float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        if (!shotPath.empty()) dt = TICK_DT;  // deterministic timing for screenshots

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) app.running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !app.set.fullscreen) {
                app.set.winW = e.window.data1;
                app.set.winH = e.window.data2;
            }
            // Mouse positions come in 3D-view pixels; the HUD and menus use 640x360.
            float us = app.r->uiScale();
            if (us != 1.0f) {
                if (e.type == SDL_MOUSEMOTION) { e.motion.x = (int)(e.motion.x * us); e.motion.y = (int)(e.motion.y * us); }
                if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { e.button.x = (int)(e.button.x * us); e.button.y = (int)(e.button.y * us); }
            }
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_F11 ||
                                          (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
                app.toggleFullscreen();
                continue;
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F3 && !e.key.repeat) { showFps = !showFps; continue; }
            if (app.screen == Screen::Playing && app.client) {
                app.client->event(e);
            } else {
                if (e.type == SDL_KEYDOWN) app.menuKey(e.key.keysym.sym);
                if (e.type == SDL_TEXTINPUT && (app.screen == Screen::Name || app.screen == Screen::Address) && app.editBuf.size() < 40)
                    app.editBuf += e.text.text;
            }
        }

        if (app.screen == Screen::Playing && app.client) {
            // Single player pauses while a menu is open.
            bool paused = app.mode == Mode::Single && app.client->paused();
            float speed = app.mode == Mode::Single ? app.set.gameSpeed : 1.0f;
            if (app.server && !paused) {
                acc += dt * speed;
                int n = 0;
                while (acc >= TICK_DT && n++ < 5) { app.server->tick(); acc -= TICK_DT; }
                if (n >= 5) acc = 0;
            }
            app.client->update(dt * speed);
            bool grab = app.client->wantsMouseGrab() && (SDL_GetWindowFlags(app.win) & SDL_WINDOW_INPUT_FOCUS);
            if ((bool)SDL_GetRelativeMouseMode() != grab) SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);
            app.client->draw(*app.r);
            if (!app.status.empty() && app.mode == Mode::Host && app.client->inGame()) {
                static double shown = 0;
                shown += dt;
                if (shown < 12) app.r->beginUi();
                if (shown < 12) app.r->text((app.r->W - app.r->textW(app.status)) / 2, app.r->H - 72, app.status, 0xFFFFE080);
            }
            if (app.client->quitApp) app.running = false;
            if (app.client->leave) {
                std::string msg = app.client->error;
                app.stopGame();
                app.status = msg;
            }
        } else {
            app.drawMenu(dt);
        }
        if (showFps) {   // F3: frame rate, to help choosing a resolution
            fpsAcc += dt;
            fpsFrames++;
            if (fpsAcc >= 0.5f) { fpsShown = fpsFrames / fpsAcc; fpsAcc = 0; fpsFrames = 0; }
            app.r->endUi();
            std::string t = std::to_string((int)(fpsShown + 0.5f)) + " fps, 3D view " + std::to_string(app.r->W) + "x" + std::to_string(app.r->H);
            app.r->beginUi();
            app.r->text(app.r->W - app.r->textW(t) - 4, 150, t, 0xFF80FF80);
        }
        app.present();
    }

    app.client.reset();
    app.server.reset();
    app.serverNet.reset();
    netShutdown();
    app.sound.shutdown();
    app.set.save();   // remembers the window size
    SDL_DestroyTexture(app.tex);
    if (app.uiTex) SDL_DestroyTexture(app.uiTex);
    SDL_DestroyRenderer(app.sdlr);
    SDL_DestroyWindow(app.win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
