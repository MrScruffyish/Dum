# Dum

A co-op dungeon crawler with a classic 2.5D first-person look and tabletop RPG rules.
Explore procedurally generated dungeons, find doors and secrets, keep your torch burning
and your belly full, and fight with weapons and spells. You don't have to aim precisely:
look roughly toward a monster and your character's stats and the dice decide what happens.

- Own software raycaster (no 3D graphics card needed), 640x360 internal resolution scaled to the window
- Classes: Warrior, Priest, Wizard, Rogue, Ranger, each with their own stats, skills, gear and spells
  (the wizard learns 18 spells from level 1 to 30; warriors learn fighting techniques instead)
- Wands and rods anyone can use - the better your device skill, the less often they fail
- Stat-based combat: d20 + skill + stat modifier vs. armor class, weapon dice, crits, sneak attacks
- Food, light fuel, mana, hit points, leveling, spells, scrolls, potions, sleeping monsters
- Dungeon generator: rooms, caves, corridors, doors (locked and secret), rubble, lit and dark rooms
- Quests: a chain of boss quests going deeper, plus random per-level tasks
- Co-op multiplayer over UDP (ENet): host from the game or run a dedicated server, give/drop items
- All content in editable text files; all art and sound generated in code and replaceable by PNG/WAV files

## Building (Linux)

```sh
sudo apt install build-essential cmake pkg-config libsdl2-dev libsdl2-image-dev
cmake -S . -B build
cmake --build build -j
./build/dum
```

ENet is included in `third_party/enet`, so you don't need to install it.

### Windows

Cross-compile on Linux (tested: the Windows server runs under Wine and Linux clients join it):

```sh
sudo apt install mingw-w64 cmake curl zip
./tools/build-windows.sh
```

This downloads the official SDL2/SDL2_image MinGW packages into `build-win/deps` and produces
`build-win/Dum-windows/` (`dum.exe`, the two SDL DLLs and `data/`) plus `Dum-windows.zip`,
ready to copy to a Windows PC. Building natively on Windows with MSYS2 or Visual Studio
(SDL2 + SDL2_image via vcpkg) uses the same CMake file.

macOS (Homebrew `sdl2 sdl2_image pkg-config`) should build the same way as Linux, but it
hasn't been tested.

## Playing

| Key | Action |
|-----|--------|
| W A S D / arrow keys | move / strafe (arrows Left/Right turn) |
| Mouse | turn |
| Shift | run |
| (walking) | you are guided into doorways and corridors if you walk into the wall beside them, and walking into a closed door opens it |
| Left mouse / Ctrl | attack with your weapon (hold to keep attacking) |
| Right mouse / F | use the selected quick slot (spell, technique, wand or rod) |
| 1-9 | use quick slot 1-9 at once (Options: or only select it) |
| Mouse wheel | select a quick slot |
| B | **spell book**: every spell/technique of your class and your wands and rods. Press 1-9 to put the selected one on that key, Del to take it off, Enter to choose it. New spells and devices go on a free key by themselves |
| E / Space | use: pick up items, open/close doors, pick locks, clear rubble, take stairs (a prompt shows what E will do) |
| P | auto-pickup on/off (on by default: walking over items picks them up when your pack has room) |
| I | inventory: W/S or mouse to select, Enter or double-click to use, eat, wear or wield (the bottom line says what Enter will do), D drop, Shift+D drop one, G give to ally. **To swap equipment**, select a worn slot (e.g. Armor) and press Enter: the pack items that fit it are marked green, pick one with W/S and press Enter. U takes the item off |
| C / J | character sheet / quest journal |
| Tab / M | dungeon map |
| H / X | quaff a healing potion / eat something (X picks the food that fits your hunger best; to eat a particular food, select it in the inventory and press Enter) |
| T / Enter | chat |
| F1 / F2 | help / hide the key legend |
| Esc | menu (single player pauses) |
| F11 / Alt+Enter | fullscreen |
| F3 | show the frame rate |
| Q | **auto-fight** on/off: attack nearby awake monsters by yourself, turning toward them (casters use their selected attack spell while they have mana) |
| Z | **auto-explore**: walk to unexplored places and items, stops when a new monster appears or you get hurt |
| G | **travel** to the stairs down |
| R | auto-walk forward on/off |
| V | face the nearest monster |

### Resolution and window size

Under **Options**:

- **Resolution** is how sharp the 3D view is drawn: 640x360 (classic), 960x540, 1280x720,
  1600x900 or 1920x1080. Higher looks sharper but needs more CPU (everything is drawn
  without the graphics card), so press F3 to see the frame rate and pick what runs smoothly.
  The HUD, screens and menus look the same at every resolution.
  The 3D view is drawn entirely by the CPU (split over up to 8 cores); the graphics card only
  scales the finished picture to the window. A faster graphics card won't help, but a faster
  CPU with more cores will.
- **Window size** sets the window to 960x540 up to 2560x1440. You can also drag the window's
  edges, and Dum remembers the size. F11 switches to fullscreen.

Both are saved in the settings. `--res WxH` and `--window WxH` on the command line override them.

### Playing with less precise or less frequent input

Dum is meant to be playable without fast or exact hand movements. Z (explore), G (stairs) and
Q (auto-fight) let you play a whole level with a few key presses. Under **Options** you can
slow the game down (single player), make keyboard turning slower or faster, choose strong aim
assist (a much wider target cone) and let auto-fight turn you toward enemies. Walking into
doors opens them, walking into locked doors picks the lock, walking into rubble digs, and
walking into the wall beside a doorway slides you into it. Single player pauses whenever a
menu or screen is open.

All keys can be changed in the main menu under **Controls**. They're saved in the settings
file. F1 in the game shows your current keys.

## Campaigns, hand-made maps, quests

Under **Adventure** in the main menu you can pick the endless dungeon or a campaign: a series
of levels (hand-made or random) with goals and story texts. The sample campaign is
*The Sunken Crypt*. Make your own with `./build/dum_tool newcampaign <id>` and edit the text
files. Everything is explained in **[docs/CAMPAIGNS.md](docs/CAMPAIGNS.md)**.

## The Abyss (high scores)

Choose **Adventure: The Abyss** in the main menu. It is the endless dungeon, but it starts at
about Easy and every level down adds another 50% of level 1's monster count, health and
damage (level 3 = 2x, level 5 = 3x, level 9 = 5x; to-hit also creeps up). XP and gold grow the
same way, so you can keep up for a while. When the whole party dies you wake up halfway back
up (die on level 9 and you're back on level 5) with everything you carry. The deepest level
reached, the number of deaths and where you first died go on **High scores: The Abyss** in
the main menu (`abyss_scores.txt` next to the saves). Levels don't collapse in The Abyss. The
numbers are in `[difficulty abyss]` in `data/difficulty.txt` (`per_level = 0.5`).
A dedicated server runs it with `--campaign abyss`.

## The town, recall and collapsed levels

- Every adventure starts in **town** (level 0): an open square with five shops (General Store,
  Armoury, Weaponsmith, Alchemist, Magic Shop) and the stairs down. Walk into a shop door or
  press E to trade. Shops sell for the item's value and buy for half (quest treasures: full
  value). What each shop sells and buys is in `data/stores.txt`.
- **Word of Recall** (every class starts with one; General Store and Alchemist sell more):
  read it in the dungeon and after 15-25 s the party is taken to town. Read it in town to go
  straight back down to the deepest level reached. Read another to cancel.
- **Collapsed levels:** once you have been deep enough, the top levels collapse in blocks of 5,
  always keeping at least 5 levels open above your deepest one (deepest 10-14 → the town stairs
  lead to level 6, 15-19 → level 11, ...). An unkilled quest boss on a collapsed level moves
  down to the first open level. Stairs up from the first open level lead straight to town.
- Levels are generated anew on every visit (like Angband), so items left lying around are lost
  when you leave a level.

## Saving, difficulty, tips

- The game **saves automatically** every minute, on every level change and when you leave.
  **Continue** in the main menu picks up where you left off. Co-op games have their own save:
  when hosting, you can continue the saved dungeon. Friends who rejoin with the same name get
  their character back, and a player who leaves keeps their character and belongings.
- **Difficulty** (main menu): Story, Easy, Normal, Hard or Nightmare (editable in
  `data/difficulty.txt`). It changes monster damage, health, accuracy, alertness and speed,
  how many monsters there are, hunger and torch burn rate, respawn time, rewards, and whether
  a dead party starts over or gets back up.
  The host's setting applies in co-op. See [docs/BALANCING.md](docs/BALANCING.md).
- **Torches:** use a spare torch while holding one to combine them (more fuel, and a bit
  brighter).
- **Rubble** (rocks with a dark gap above them) blocks some passages. Walk into it or press E
  a few times to dig through. Going **up** stairs asks for a second E press.
- Settings and saves are stored in `~/.local/share/dum/dum/` (Linux) or
  `%APPDATA%\dum\dum\` (Windows).

**Auto-aim:** the yellow brackets show which monster your weapon will hit (blue = spell target).
Attacks roll a d20 plus your skill and modifiers against the monster's armor class.

## Co-op

- In the menu choose **Host co-op game**, or run `./build/dum --host`. Others choose
  **Join co-op game** and enter your address, or run `./build/dum --connect <ip>`.
- Dedicated server without a window: `./build/dum --dedicated [--port 27666]`
- The default port is UDP 27666. Over the internet, the host needs to forward that port.
- Anyone who takes the stairs brings the whole party along. Dead players respawn at the up
  staircase after 12 s while someone is still alive. If the whole party dies, a new party
  starts over.
- Kills give full XP to everyone. Drop items with D or hand them over with G (the ally must
  be within 3 steps).

## Modding

See **[docs/ART_AND_SOUND.md](docs/ART_AND_SOUND.md)** (replacing pictures and sounds) and
**[docs/BALANCING.md](docs/BALANCING.md)** (all the rules and formulas, how to rebalance).

Everything is in `data/` and read at startup. The game finds `data/` next to the executable
or in its parent directory, so `build/dum` uses the repository's `data/` directly.

- `data/classes.txt`, `items.txt`, `monsters.txt`, `spells.txt`: all game content. The
  formats are described at the top of each file.
- **Art:** run `./build/dum --export-art` to write every picture to `data/gfx/*.png`. Edit
  any of them and the game will use your file instead of the generated one. Delete a file to
  get the generated picture back. Wall, floor and door textures must be power-of-two sizes
  (64x64, 128x128...). Sprites can be any size, and transparent pixels are see-through.
  Names: `wall_brick`, `floor_stone`, `monster_goblin`, `monster_goblin_attack`,
  `player_wizard`, `item_potion_heal`, `hand_sword` (first-person weapon), `spell_fireball`...
- **Sound:** put `data/sfx/<name>.wav` files in place to replace the synthesized effects:
  swing hit miss shoot cast explode hurt door pickup die levelup growl step stairs crit heal.

## Developer options

```
--continue            continue the saved single player game
--campaign ID         play a campaign from data/campaigns (new games, --host, --dedicated)
--difficulty D        story | easy | normal | hard | nightmare (--host / --single / --dedicated)
--save FILE           dedicated server: save file (default dum-server.sav)
--seed N              fixed dungeon seed
--res WxH             internal resolution (default 640x360)
--spawn goblin,orc    spawn monsters in front of you (single player / host) [--awake]
--depth N             jump to dungeon level N at start
--give id,id          put items in your pack (ids from data/items.txt)
--auto explore,fight  start auto-explore / travel / auto-fight (for testing)
--screenshot f.png    run headless-capable for --frames N frames, save a screenshot, exit
                      (--turn RAD, --ui inventory|map|character|quests|help)
```

`./build/dum_selftest` runs a headless test: it generates hundreds of dungeons and runs a
3-minute game with three bots, one local and two over real UDP.

## Layout

```
src/common   math, RNG, binary serialization
src/game     data loading, map + dungeon generator, shared state/protocol
src/server   authoritative simulation: AI, combat, quests, inventory
src/client   renderer, procedural art, sound, HUD/UI, input/prediction
src/net      ENet + in-process loopback transport
src/tools    dum_tool: generate/check maps and campaigns
data/        game content
third_party/ ENet (MIT)
```

## Licence

Dum is free software under the **GNU General Public License v3.0** (see `LICENSE`): you may
use, change and share it, and versions you distribute must stay open source under the same
licence. The libraries it uses (ENet, SDL2, SDL2_image and, in the Windows build, the MinGW
runtime) have their own licences, listed in `THIRD_PARTY_NOTICES.md`.
