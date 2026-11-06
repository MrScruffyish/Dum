# How Dum works inside

Notes for whoever changes the code. They cover the newer systems (renderers, animation,
art loading, co-op data checks and leaving, The Abyss). For playing and modding, see the README,
[MODDING.md](MODDING.md) and [ART_AND_SOUND.md](ART_AND_SOUND.md).

## The big picture

One process can hold a **server** (the whole simulation: AI, combat, quests) and a
**client** (input, prediction, drawing, sound). Single player runs both, connected by an
in-process loopback. Co-op hosting runs both plus ENet for the others. Joining runs only a
client. A dedicated server runs only a server. Content (`data/*.txt`) is loaded once into
the global `gData` (`src/game/data.cpp`), and everything refers to monsters, items and
spells by their **index** in `gData`. That is why all players must share the same data
(see below).

## Rendering: two renderers, one picture

`src/client/renderer.cpp` (`Renderer`) is the original **software renderer**: a column
raycaster for walls, row casting for floors and ceilings (split over up to 8 threads),
billboard sprites, and per-tile light (`setupLights`: ambient per tile plus point lights,
each with a line-of-sight mask per tile; `lightAt` adds them up). It also draws all 2D:
text, panels, the weapon in your hand.

`src/client/gpu.cpp` (`GpuRenderer`) is the **OpenGL 3.3 renderer**. It does not replace
`Renderer`; it works with it:

1. `Renderer::setGpu(true)` puts the software renderer in GPU mode. `drawWorld()` then
   still casts one ray per column. That gives the depth buffer (for sprite visibility and
   the target brackets) and marks tiles as seen for the automap. It also computes each
   sprite's light, but writes **no pixels**. Instead it fills a `WorldScene` (camera,
   horizon, map, visible sprites far to near).
2. In GPU mode, all 2D drawing outside `beginUi()`/`endUi()` (weapon, target brackets,
   screen tints) goes to a **transparent world layer**. `Renderer` tracks the changed
   rectangle (`markDirty`), so only that part is cleared and uploaded each frame. The HUD
   and menus go to the 640x360 UI layer as always.
3. `GpuRenderer::present()` draws the scene, then the world layer, then the UI layer:
   - **Level mesh:** built from the map (a floor and a ceiling quad per open tile, a wall
     quad per solid tile face next to an open tile), grouped by texture. It is rebuilt
     when the map's tiles change (doors, rubble, a new level), checked by comparing the
     tile array each frame.
   - **Lighting:** the same sums as `lightAt`, per pixel in the fragment shader. Ambient
     light is an R32F texture (one texel per tile), and the line-of-sight masks of up to
     32 lights are packed as bits into an R32UI texture. The lights themselves are a
     uniform array. Each wall face is lit from the open tile in front of it, like the
     raycaster.
   - **Projection:** reproduces the raycaster exactly. Horizontal: the camera plane
     (`plane` = W/(2H)). Vertical: a wall is H/distance pixels tall, and the head-bob
     `horizon` is a vertical lens shift (`uShift`), not a camera pitch.
   - **Sprites:** quads facing the camera plane, one draw call each, with alpha test,
     checkerboard dither (`gl_FragCoord`), tint, and additive blending for glowing spells.
   - **Textures:** created on first use from the `Texture` pixels, cached by pointer, with
     mipmaps (and anisotropic filtering if available). "Smooth" upscales art of 128 pixels
     or less with Scale2x (twice for 64 and below) and filters it linearly.
     `dropTextures()` must be called whenever `Assets` is rebuilt, because the cache is
     keyed by pointer.
4. Every GL function is looked up with `SDL_GL_GetProcAddress` (the `DUM_GL_FUNCS` list),
   so nothing extra is linked, and the Windows build needs no OpenGL import library.

`main.cpp` (`App::setupVideo`) creates the window with or without `SDL_WINDOW_OPENGL`.
Switching renderer recreates the window. If OpenGL fails, it falls back to the CPU with a
message. In GPU mode, SDL's logical-size letterboxing isn't available, so `App::mapMouse`
converts mouse coordinates itself. Screenshots in GPU mode read the back buffer
(`readPixels`) at the window's size.

To add something to the 3D view, add it to both renderers: `Renderer::drawWorld` /
`drawSprites` for the CPU, and `GpuRenderer::Impl::drawWorld` for the GPU. Sprites are the
easy case: a `SpriteDraw` from the client is drawn by both.

## Art: generated, replaced, animated

`src/client/textures.cpp` (`Assets::build`) generates all art in code: walls, floors,
monsters per `sprite` type and colors, items, weapons, spell orbs. `namedTextures()` gives
each picture its file name (`monster_<id>`, `item_<id>`...). That list drives both
`--export-art` (`exportPngs`: pattern, scale, never overwrites without `--force`) and
`loadOverrides` (PNGs in `data/gfx/`).

- **Loading PNGs:** anything over `MAX_ART_SIZE` (256) is shrunk on load by `shrinkToFit`:
  alpha-weighted box filtering, a pixel becomes see-through if less than half was opaque,
  and powers of two stay powers of two (needed for tiling textures in the CPU renderer).
- **Animation frames:** `monstersWalk[i]`, `monstersDie[i]` and `playersWalk[i]` are
  frame lists. Generated frames come from `walkFrame()` and `dieFrame()`, which warp the
  standing picture (no hand-drawn frames). The warp depends on an `AnimStyle` picked from
  the monster's `sprite`: Legs (lower third scissors), Skitter, Flap (outer columns move
  most), Float, Slither, Squash. PNG series `_walk1..N` / `_die1..N` replace a whole list.
  If only a standing picture is replaced, the generated frames are remade from it
  (`buildWalkFrames(keep)`).
- **Choosing a frame** (client, `Client::creatureFrame`): sleeping or still (not moved for
  0.25 s) shows the standing picture. Walking steps through the frames by distance
  travelled (`CEnt::anim`). Flapping and floating types animate all the time on a clock,
  each monster offset by its id. Attacking shows the attack picture.
- **Death:** on `EV_MONSTER_DIE` the client remembers the monster's position and type in
  `dying_`. That entry is drawn as a sprite: frames at 10 per second, then the last frame
  for 1.2 s, then a 0.6 s fade. The server knows nothing about it. All of this happens
  only with the Animation option on.

## Co-op: same data for everyone

Monsters, items and spells travel over the network by index, so a client with different
data files would show the wrong things. The handshake (protocol version 8) checks this.
Only checksums are compared: no file is ever sent, so a host can't push data onto
players' computers.

1. `GameData::load` computes an FNV-1a checksum of each **shared file**
   (`GameData::sharedNames()`: spells, items, monsters, classes, stores, difficulty).
   Carriage returns are skipped, so Windows and Linux line endings hash the same. Maps and
   campaigns are not checked because only the server uses them.
2. `C_HELLO` carries the client's checksums. `Server::onHello` compares them with its own.
   If any differ, it answers `S_REJECT` naming the files, disconnects, and tells the
   players already in the game who was turned away.

The self-test has a bot claim a different `monsters.txt` and checks that it is refused,
that the reason names that file (and only that one), and that it never enters the game.

## Co-op: players leaving

When a player disconnects, `Server::onDisconnect` keeps their character in `saved_`
(restored by name when they come back, and stored in the save), and the others play on.
The host is the special case, because the server runs in the host's program. If the host
leaves (menu or window close) while others are connected, `App::keepHosting` drops only
the host's own client. The server sees that as an ordinary player leaving. The main loop
then keeps ticking the server on the *Still hosting* screen
(`Screen::HostOnly`). `rejoinHosted` connects a fresh client through a new loopback pipe.
`endHosting` announces the end, saves (the `Server` destructor saves), and goes back to the
menu, or quits if the host had asked to quit. It ends by itself when the last player has
left.

## Keys, vaults and chests

- **Vaults** are made by the generator (`Gen::placeVaults` in `src/game/map.cpp`), from
  level 2 on: a room whose cells touch exactly one open cell outside it (a dead end), and
  never the rooms with the stairs. That doorway becomes `T_DOOR_KEY`, which is solid like a
  closed door. Its lock colour is stored as `wallTex = 8 + colour` (`Map::keyDoorColor`),
  so it travels with the map and saves with no format change.
- **Keys and chests** are placed by the server (`Server::placeChestsAndKeys`, called from
  `populate`). A flood fill from the up stairs that treats coloured doors as walls finds
  where you can go without keys. Each door's key goes into a room cell there, the vault's
  rich chests into cells outside it, and 0-3 ordinary chests into reachable rooms.
- **A chest is an ordinary floor item** (`kind = chest`). Its secret lives in the
  `ItemStack`'s `fuel`: the low 4 bits are the `ChestContent` (treasure, empty, or one of
  six traps), `CHEST_KNOWN` means detected, and `CHEST_RICH` marks a vault chest. So it is
  saved and loaded like any item. Opening one (`Server::openChest`, through `doUse` and
  `pickupItem`) turns it into `chest_open`, which stays as scenery: it is skipped by E,
  auto-pickup and the prompt. There is one roll to disarm a trap; otherwise
  `springTrap` runs it. Then the loot is dropped around the chest.
- **What clients know:** only detected chests reveal anything, through the item snapshot's
  flag bits (`EF_CHEST_KNOWN`, `EF_CHEST_TRAP`, `EF_CHEST_EMPTY`). The client uses them for
  the red, dim or golden look, the E prompt and the map dots. `SpellType::Treasure` sets
  `CHEST_KNOWN` within its radius. Players with the `search` skill roll every half second
  to notice what an undetected chest within 2 tiles holds.
- **Keys** never take a pack slot: `addToInventory` puts them on `PlayerState::keys[colour]`
  (the key ring, sent to the owner and saved since protocol 9 / save version 10), and
  auto-pickup takes them like gold. `fixupChar` moves keys out of the pack in older saves.
- **Unlocking:** `doUse` on a `T_DOOR_KEY` checks the key ring for that colour, uses one
  and opens the door (it becomes `T_DOOR_OPEN`). Walking into the door does the
  same (bump-to-open). Monsters can't open these doors, and Stone to Mud can't dig them.

## Items: stacks, rings, using from the floor

- `stacksWith` decides what merges in the pack: same item, stackable, and for lights the same
  fuel left (so fresh torches stack). Using a light or wearable from a stack takes one;
  the item it replaces goes back into the pack.
- There are two ring slots (`EQ_RING`, `EQ_RING2`; `fitsSlot` says a ring fits either),
  and a quiver (`EQ_QUIVER`) for the loaded ammo; save version 11 added it.
  `A_INV_USE` carries the target slot (+1, 0 = automatic): the replace mode sends it, and
  otherwise a ring goes on the free hand. Saves before version 10 had six slots and are
  mapped on load.
- `useItem` uses an inventory slot through `useStack`, which also works on a floor item's
  stack: with a full pack, `pickupItem` eats, drinks or reads food, potions and scrolls
  where they lie.
- Single player pauses while a menu is open, but `Server::pausedTick` still handles the
  player's actions and sends the result, so inventory and shop changes show at once.

## Ammo

- The Ammo option is a server rule (`Server::setAmmoRule`, from the host's settings), sent
  to clients in `S_WELCOME`. With it off, nothing below happens: shots are free, ammo isn't
  generated or given at the start, and shops refuse to sell it.
- Ranged weapons name their `AmmoKind` (`ItemDef::ammo`). Ammo items are `kind = ammo`,
  equipped in `EQ_QUIVER` (loaded with `useStack`, whole stacks; `addToInventory` puts more
  of the loaded kind there first). `PlayerState::equipSum` and `resists` skip the quiver,
  so ammo bonuses don't count for melee.
- `doAttack` (ranged): `reloadQuiver` loads matching ammo from the pack if the quiver is
  empty or holds another kind (the kind shot last first, `SPlayer::lastAmmo`), then takes
  one shot's worth and adds the ammo's to-hit, damage and brand to the projectile.
- Projectiles remember the ammo item (`ammoDef`). When one hits a wall, flies its full
  range, misses or hits a monster, `dropAmmo` may leave one on the floor (it joins a pile
  lying right there). The snapshot's `extra` tells clients which picture to draw (arrow,
  bolt, pebble).

## The Abyss

The Abyss is a hidden difficulty (`hidden = yes`, `per_level = 0.5` in
`data/difficulty.txt`), not a separate game mode. `DifficultyRules::atDepth(depth)` scales
monster health, damage, xp and gold by `1 + per_level x (depth - 1)`, the monster count by
`1 + count_per_level x (depth - 1)`, and adds to-hit every 2 levels. It replaces the menu's
difficulty completely. `Server::rules()` returns that, cached per depth. Monster groups
are capped at 45 per level. `Server::abyss()` is simply "the difficulty has `per_level`".
A party wipe there doesn't restart: `checkPartyWipe` counts a death, records the score,
and after 6 s revives everyone on level `(depth + 1) / 2`. Levels never collapse in the
Abyss. Scores live in `abyss_scores.txt` next to the saves (`src/game/scores.h`: one line
per run, keyed by the run's seed, updated whenever the run goes deeper or dies). The save
file (version 9) stores the death count and first death.
