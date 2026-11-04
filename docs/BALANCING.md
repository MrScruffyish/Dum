# Rules and balancing

Almost everything that affects game balance is in the four text files in `data/`. They are read
when the game (or server) starts, so: edit, save, restart. In co-op, **the server's files
count**: the host or dedicated server decides the rules (clients need the same files for
names and pictures, so share your `data/` folder with your friends).

- `classes.txt`: character classes
- `items.txt`: weapons, armor, light, food, potions, scrolls, quest items
- `monsters.txt`: monsters and bosses
- `spells.txt`: player and monster spells
- `stores.txt`: the town shops (what they sell and buy)

Each file starts with a description of its keys. You can add new entries. New monsters and
items show up in the dungeon automatically, and spells/items become usable by referencing
their ids (for example `spells = magic_missile, my_new_spell:4` in a class).

The rest of this file explains **how the numbers are used**, so you can predict what a change does.

## Dice

`2d6` means "roll two six-sided dice and add them" (2-12, average 7). Average of `NdS` is
`N × (S+1)/2`. Stat modifiers work like D&D: `(stat - 10) / 2` rounded down, so
8 → -1, 10 → 0, 14 → +2, 18 → +4.

## Characters

| What | Formula |
|------|---------|
| Starting stats | class value ± 1 (random), clamped to 3-18 |
| Starting HP | `hitdie + 4 + CON mod` (at least 4) |
| HP per level | `1d(hitdie) + CON mod` (at least 1) |
| Starting mana | `mana + cast stat mod` (only if `mana` > 0) |
| Mana per level | `manaperlevel + cast stat mod / 2` (at least 1) |
| XP for next level | `5 × level × (level + 1)`: 10, 30, 60, 100, 150... |
| Armor class (AC) | `10 + DEX mod + armor of worn armor` (+2 while blessed) |
| HP regeneration | per second: `(1 + 0.25 × CON mod) × (0.05 + maxHP/400)`, only when not starving |
| Mana regeneration | per second: `0.12 + 0.03 × cast stat mod + maxMana/300` |
| Spells known | a spell is learned at its `level` (or the class override `spell:level`) |

Experience: **every living player gets the full XP** of every monster killed (co-op friendly).
Quests give extra XP and gold to everyone.

## Your attacks

When you attack, the game picks a target (the **auto-aim**). It's the monster inside a cone in
front of you (`arc` degrees wide, `range` tiles long, line of sight required) with the best
mix of "close to where you look" and "close to you". Monsters right next to you count even
slightly outside the cone. The yellow brackets on screen show the pick.

**Melee and ranged weapons:**

| What | Formula |
|------|---------|
| Attack roll | `d20 + skill + level/2 + stat mod + weapon tohit` (+2 blessed, +4 vs sleeping monster) |
| skill | class `melee` or `ranged` |
| stat mod | STR for normal melee weapons; `finesse = yes` uses the better of STR/DEX; ranged weapons use DEX |
| Hit if | roll total ≥ monster `ac`. A natural 20 always hits (critical), a natural 1 always misses |
| Damage | `weapon dice + STR mod (melee only) + skill × level / 10`, at least 1 |
| Critical (natural 20) | one extra roll of the weapon dice |
| Sneak attack | classes with `stealth` ≥ 5 (the rogue) do double damage to sleeping monsters |
| Attack speed | one attack per `cooldown` seconds (×1.6 when slowed) |
| Bare hands | 1d2, range 1.3, cooldown 0.6 |

Ranged weapons fire a visible missile that steers toward the auto-aimed target. The attack
roll happens when it arrives.

**Spells** always hit (no attack roll):

| What | Formula |
|------|---------|
| Damage / healing | `spell dice + scale × level + spell power` |
| Spell power | `class magic / 2 + cast stat mod + staff spellpower` |
| Holy damage | ×2 against undead monsters |
| Cold | slows the target (half speed) for 4 seconds |
| Ball / nova | full damage in the inner half of the radius, 2/3 in the outer part |
| Bless / Light / Detect | last `duration` seconds. Bless affects allies within `radius` |
| Casting speed | one spell per `cooldown` seconds (×1.6 slowed, ×0.7 hasted) |

Example: a level 1 wizard (INT 17 → +3, magic 4, quarterstaff +1) casting Magic Missile
(2d4, scale 0.5) does `2d4 + 0 + (2 + 3 + 1)` = 8-14 damage. A goblin has 2d8 HP (avg 9).

More spell types and effects (all set in `spells.txt`):

| What | Rule |
|------|------|
| `effect = sleep` | the monster falls into a magical sleep for `12 + power/2` s; it can't wake up by itself, but any damage wakes it |
| `effect = fear` | the monster runs away for `6 + power × 0.3` s |
| `effect = slow` | half speed for 8 s |
| Effect chance | `55 + 3 × (power - monster depth)` %, between 10 and 95. Here power = spell power + caster level (devices: device power + level). Bosses are never affected |
| Haste | ×1.35 movement, attacks and spells ×0.7 cooldown |
| Shield (Stoneskin, Iron Skin) | +8 armor class |
| Resist | half damage from fire, cold and poison (like resist equipment) |
| Berserk | +4 to hit, +3 damage, -2 armor class |
| Stone to Mud | the first wall, rubble or door within `range` tiles in front of you becomes floor (not the outer walls, not in town) |
| `recharge` | seconds before that spell can be used again. Warrior techniques use this instead of mana |

**Warrior techniques** are ordinary spells with `ability = yes`, `mana = 0` and a `recharge`,
listed in the warrior's `spells =` line: Second Wind (5), Battle Cry (10), Whirlwind (15),
Berserk (20), Iron Skin (25). Their power uses the warrior's cast stat (CON).

**Wands and rods** (`kind = wand` / `kind = rod` in `items.txt`) cast their `spell` for anyone:

| What | Rule |
|------|------|
| Failure chance | `22 + 2 × item depth - 4 × device skill - 3 × INT mod - level / 3` %, between 2 and 60 |
| Power | `device skill + INT mod / 2 + level / 5` (instead of spell power) |
| Wand | `charges = MIN-MAX` uses; an empty wand sells for a fifth of its value |
| Rod | never runs out; after a use it needs `recharge` seconds (it recharges in your pack) |
| Device skill (`device` in `classes.txt`) | warrior 1, priest 3, ranger 3, rogue 5, wizard 6 |

Example: a level 1 warrior (INT 8, device 1) fails a Wand of Magic Missile 23% of the time;
a wizard almost never fails (2%). A Wand of Fire Balls (depth 14) fails a level 15 warrior 44%.

Scrolls use the better of your spell power and your device power.

## Monster attacks

| What | Formula |
|------|---------|
| Melee attack roll | `d20 + tohit + dungeon level / 2` ≥ your AC. A natural 20 doubles damage |
| Melee damage | `dmg` dice |
| Reach | monster attacks when within `reach` (+ body sizes) |
| Attack speed | one attack per `attackcd` seconds |
| Spells | when it sees you and you're within 11 tiles: chance `spellrate` per second, at most once per 2 s |
| Spell damage | `spell dice + dungeon level / 2`. You dodge half of it on `d20 + DEX mod ≥ 15` |
| Movement | `speed` tiles per second (players walk 3.4, run 5.2) |

## Equipment bonuses and artifacts

Six slots: weapon, armor, light, helm, ring, amulet. Bonuses of everything you wear add up:

| Key in `items.txt` | Effect |
|------|--------|
| `str` `dex` `con` `int` `wis` | added to the stat (max 25), so the modifiers change too |
| `tohit`, `todam` | added to attack rolls / damage of all attacks |
| `armor` | added to armor class (the main value of armor and helms) |
| `light` | extra light radius (alone it gives a little light) |
| `speed` | +10% walking speed per point |
| `stealth`, `search` | added to the class skills |
| `slowdigest` | % less hunger (max 80) |
| `regen` | % faster healing |
| `spellpower` | added to spell power |
| `resist` | half damage from those elements (cold resistance also prevents slowing) |
| `brand` (weapons) | the weapon's damage element: fire, cold (slows), holy (x2 vs undead)... |
| `gainstat` (potions) | permanently +1 to a stat (max 20) |

**Artifacts** (`artifact = yes`) exist once per game. A random item has a
`1.5% + 0.2% × level` chance to be an artifact (one whose `depth` ≤ level + 2 that isn't in play),
and bosses always drop one if any are left. An artifact left on the floor when the party leaves
a level, or sold to a shop, returns to the pool and can be found again.

## Monster abilities

| Key in `monsters.txt` | Effect |
|------|--------|
| `steals = yes` | 60% of its hits take 5 + 10-30% of your gold, then it teleports away |
| `summon = <id>` or `kin` | while fighting, chance `summonrate` per second (default 0.08) to call 1-2 more; at most 3 times per monster, never past 100 monsters on the level |
| `breeds = yes`, `breedtime = N` | while awake, a copy every N seconds or so (default 30; it starts again at a random 0-50% of N). Never more than 6 of its kind within 6 tiles, 20 of its kind on the level, or 100 monsters in all |
| `poison = N` | each hit poisons you for N more seconds (max 30, half with poison resistance): 1 + level/8 damage per second. Any healing or an antidote cures it |
| `drain = str` (dex, con, int, wis) | 30% of its hits lower that stat by 1 (never below 3) until a Potion of Restoration or your next level up |
| `blink = yes` | every 2.5-5.5 s while fighting it teleports to another spot 1.5-4.5 tiles from you |
| `invisible = yes` | drawn as a faint shimmer; auto-aim only finds it within 3 tiles, unless you have Detect Monsters active |
| `regen = N` | regains N hit points per second |
| `erratic = N` | spends roughly N% of its time fluttering in random directions |
| `drainlight = yes` | each hit burns 30-90 seconds of fuel from your torch or lantern |
| `explode = <spell>` | casts that spell (a ball) where it dies: it hurts *players* |
| `resist = fire, cold` | half damage from those elements (cold also no longer slows it) |
| `vulnerable = holy` | double damage from those elements |
| `speed = 0` | never moves (staring orbs) |

Weapons do physical damage; spells use their `element`. Holy damage is doubled against
undead on top of `vulnerable`.

## Noticing and sleeping (the "time to react" part)

- A monster starts asleep with `sleep`% chance.
- Every second, a sleeping monster may notice each player within `1.5 × sight` tiles:
  `chance = base² × 0.35 × stealthFactor + noise × base × 0.8`,
  where `base = 1 - distance / (1.5 × sight)` and
  `stealthFactor = 1 - 0.12 × class stealth - 0.03 × DEX mod` (minimum 0.1).
  The chance is doubled if the monster can see you inside your own light radius.
- **Noise** rises when you attack (+0.5), cast (+0.4) or bash a door (to 1.0) and fades by 0.5/s.
- When a monster wakes up it growls (you hear it, with direction) and may wake sleepers of
  its own kind within 4 tiles.
- Awake monsters see you up to `sight + your light radius / 2` tiles away with line of sight.
  Beyond that they follow your scent through the corridors if you are within about 18 steps
  (40 once they have seen you).
- **Detect Monsters** shows creatures within 24 tiles on the map.

The biggest levers for "how much time do I get" are `sleep`, `sight` and `speed` in
`monsters.txt`, the class `stealth`, and the light radius of torches/lanterns (a bigger light
lets you see further **and** makes you easier to see).

## Survival

| What | Value |
|------|-------|
| Food | starts at 800, maximum 1000, drops 1 per second. "Hungry" below 150 |
| Starving | at 0 food: 1 damage every 3 seconds, no regeneration below 50 |
| Food items | restore `food` points |
| Light | equipped light burns 1 `fuel` per second. Torches disappear when empty, lanterns keep and are refilled with `flask_oil` |
| Light radius | the item's `light` in tiles. Call Light gives 7. Without light you see about 2 tiles dimly |
| Combining torches | using a torch while holding one adds its fuel, up to 2.5 torches' worth. A torch with more than one torch's fuel gives +1 light |

## Dungeon contents

| What | Formula |
|------|---------|
| Monster groups per level | `7 + dungeon level + 0..3` |
| Which monster | random, weighted by `rarity`. Only monsters with `depth` ≤ dungeon level (10% of the time up to 2 levels deeper). Monsters within 2 levels of the current depth are 3× as likely |
| Group size | `group` (e.g. `2-4`) |
| Items per level | `9 + 0..5` random items (weighted by `rarity`, `depth` ≤ dungeon level + 1), plus always 2 rations, 1 torch, 1 flask of oil, and `4 + level/2` gold piles of `5..20 × level` gold |
| Monster drops | `drop`% chance of an item, `drop/2`% chance of gold. Bosses drop 3 good items and lots of gold |
| `rarity = 0` | never generated randomly (quest items, bosses) |

Dungeon layout (room count, sizes, door chances, lit rooms) is in `src/game/map.cpp`.
Lit rooms get rarer deeper down: `85 - 12 × level`%, but at least 10%.

## Town, shops, recall

| What | Value |
|------|-------|
| Buy price | item `value` |
| Sell price | half the `value` (lights: scaled by remaining fuel). Quest treasures: full `value` |
| Which shop buys what | `buys = ...` in `stores.txt`. Every shop buys quest treasures |
| Starting gold | 10-40 (plus the class starting gear, including one Word of Recall) |
| Word of Recall delay | `duration` + 0..`range` seconds (`spells.txt`: 15 + 0-10) |
| Collapsed levels | deepest level ≥ 10: levels `1..((deepest-5)/5)*5` are skipped |

## Quests

- **Main quests** come from the bosses (`boss = yes`), sorted by `depth`. The boss sleeps
  next to the down staircase of its level, with 2-4 escorts. Killing it gives its `xp`
  again as a quest reward and starts the next boss quest. After the last boss, the last
  one returns 3 levels deeper each time (endless mode).
- **Level tasks** (65% of levels): "find the X" (a `kind = quest` item is placed far
  away; reward 15 × level XP, 50 × level gold) or "slay N of a monster type" (reward that
  many times its XP, 20 × level gold). They are abandoned when you leave the level.

## Doors and secrets

| What | Chance |
|------|--------|
| Finding a secret door within 3 tiles | per second: `4 + 4 × search + 2 × WIS mod`% (+10 when adjacent) |
| Picking a lock (each E press, 1 s) | `15 + 8 × disarm + 4 × DEX mod + 2 × level`% |
| …otherwise bashing it open | `10 + 6 × STR mod + level`% (max 50, makes noise) |
| Digging through rubble | 5 points needed; each dig (every 0.5 s) adds `1 + STR mod / 2` (+1 with 30% chance). Walking into rubble digs too |
| Monsters with `doors = yes` | open doors. Locked doors: 15% chance per try to burst them open |

## Difficulty

Difficulty levels are defined in `data/difficulty.txt` (Story, Easy, Normal, Hard, Nightmare);
edit them or add your own. The host's or dedicated server's choice applies to everyone and is
stored in the save. Each level sets multipliers for monster damage, hit points, speed and
alertness, monster accuracy, extra monster groups, hunger and light burn rate, respawn time,
what happens when the whole party dies (`restart` or `revive`), and XP/gold rewards.

## Death

While anyone in the party is alive, a dead player respawns at the up staircase after
`respawn` seconds (see `difficulty.txt`) with half HP, and drops half their gold where they died. If the whole party
dies, everyone starts again on level 1 with new characters.

## Constants in the code

A few values are not in data files yet:

| Constant | Where |
|----------|-------|
| Walk / run speed (3.4 / 5.2) | `src/client/client.cpp` (`WALK_SPEED`, `RUN_SPEED`) |
| Food maximum, tick rate, player/monster size | `src/game/state.h` |
| Food per second, regeneration, respawn time, wake-up formula, lock picking | `src/server/server.cpp` (`updatePlayers`, `updateMonsters`, `doUse`) |
| Level contents (group counts, item counts) | `src/server/server.cpp` (`populate`) |
| Map size, room count, door chances | `src/game/map.cpp` |
| XP curve, AC, spell power | `src/game/state.cpp` / `state.h` |

## Testing your changes

```sh
./build/dum --seed 42                       # same dungeon every time
./build/dum --spawn orc,orc,cave_troll      # put monsters in front of you
./build/dum --spawn goblin_king --awake     # ...awake
./build/dum_selftest                        # three bots play for 3 minutes; checks nothing crashes
```

If a data file has an error (bad value, unknown spell/item id...), the game prints the file
and line number and exits. Misspelled keys are reported as warnings at startup, so check
the terminal output after editing.

Rules of thumb:
- A level 1 character should survive 2-3 depth-1 monsters at once, and die to a
  careless pack of 5.
- Compare the average damage per second of each class (`average damage / cooldown`). The
  warrior should lead in melee, the wizard should beat everyone until out of mana.
- Keep food generous at first. Running out should come from wandering, not bad luck.
