# Maps, campaigns and quests

Besides the endless dungeon, Dum can play **campaigns**: a fixed series of levels, each with a
goal, an intro text and either a **hand-made map** or a random one. Pick a campaign under
**Adventure** in the main menu (or `dum --campaign <id>`, `dum --dedicated --campaign <id>`).

Everything is plain text in `data/`:

- `data/maps/*.txt`: hand-made maps (any number per file)
- `data/campaigns/*.txt`: one campaign per file

The sample campaign *The Sunken Crypt* (`data/campaigns/sunken_crypt.txt` with the maps in
`data/maps/crypt.txt`) shows everything below.

## The tool

`dum_tool` (built next to `dum`) helps you make content:

```sh
./build/dum_tool newcampaign my_quest --name "The Lost Mine" --levels 6 --depth 2 --handmade 2
./build/dum_tool genmap old_mine --depth 4 --name "The Old Mine"
./build/dum_tool show old_mine
./build/dum_tool check
./build/dum_tool list
```

- **newcampaign** writes `data/campaigns/<id>.txt` (plus `data/maps/<id>.txt` when `--handmade`
  is given). It picks monster pools that fit each level's depth, mixes goals (reach the
  stairs, find a treasure, clear the level, kill a strong monster) and ends with the boss
  that best fits the final depth. Edit the result as much as you like.
- **genmap** turns a random level into an editable hand-made map file.
- **show** prints a map and lists what is placed where.
- **check** loads every data file and reports problems with file and line. The game does the
  same at startup, so a broken file is reported instead of crashing.

`--seed N` makes `genmap`/`newcampaign` repeatable, `--force` overwrites existing files, and
`--data DIR` uses another data folder.

## Campaign files

```
[campaign sunken_crypt]
name = The Sunken Crypt
desc = Beneath the town lies a crypt where the dead no longer rest.
town = yes                 # start in town, with shops and recall between levels
victory = Vesk crumbles to dust, and the crypt falls silent at last.

[level 1]
map = crypt_gate           # a hand-made map id, or "random"
intro = Cold air rises from the crypt gate.
goal = stairs
goal_text = Find a way past the crypt gate and down the stairs.

[level 2]
map = random
depth = 3                  # strength of a random level (and its monsters)
monsters = zombie, skeleton, cave_bat      # only these appear at random (optional)
goal = find ancient_tome
```

Levels are played in the order they appear in the file. The stairs down on the last
level lead nowhere.

| Goal | Done when |
|------|-----------|
| `stairs` | the party takes the stairs down from that level |
| `kill <monster>` | that monster is killed. If the map doesn't place it, it waits by the stairs down |
| `find <item>` | the item is picked up. If the map doesn't place it, it lies somewhere far from the start |
| `clear` | every monster that was on the level when you arrived is dead |

When every level's goal is done, the campaign is won and the `victory` text is shown. You
can keep playing afterwards. Goals give XP to the whole party. Level 1 of a campaign is
reached from the town stairs, Word of Recall works as usual (town ⇄ deepest level), and the
"collapsing levels" of the endless dungeon don't apply to campaigns.

## Hand-made map files

```
[map crypt_gate]
name = The Crypt Gate
depth = 2                  # strength of random monsters/items on this map
lit = no                   # yes = the whole map is lit
monsters = no              # yes = also add random monsters and items
wall = dark                # brick cave moss dark wood blood
floor = stone              # stone dirt tiles wood grass cobble
place z = monster zombie
place b = monster cave_bat awake
place t = item torch
map:
#########
#**<****#
#***t***#
####+####
   #.#
...
```

Everything after `map:` up to the next empty line is the map, one character per tile:

| Symbol | Tile |
|--------|------|
| ` ` (space) | solid rock (outside the level) |
| `#` | wall (the map's `wall` theme) |
| `%` | rough cave wall |
| `.` | floor (the map's `floor` theme) |
| `*` | lit floor (like Angband's lit rooms) |
| `,` `_` `=` | dirt / tiled / wooden floor |
| `+` | closed door |
| `'` | open door |
| `L` | locked door |
| `S` | secret door (looks like a wall until found) |
| `:` | rubble |
| `<` | stairs up: the party arrives here |
| `@` | start position without stairs |
| `>` | stairs down |
| any other letter or digit | whatever you define with `place` |

`place X = monster <id>` (add `awake` to have it awake from the start) or
`place X = item <id>` puts that monster or item there, standing on floor. Ids come from
`monsters.txt` and `items.txt`; artifacts and quest treasures work too.

Rules (checked by the game and `dum_tool check`): the map needs a start (`<` or `@`),
the stairs down and everything placed must be reachable from the start (doors, locked doors,
secret doors and rubble count as passable), and a map is at most 248x248. Don't leave empty
lines inside a map: an empty line ends it (use spaces for rock).

## Tips

- Start with `dum_tool newcampaign`, play it, then replace random levels with hand-made maps.
- Use `monsters = ...` on random levels to give a campaign a theme (undead, beasts...).
- A boss placed on a hand-made map with `place V = monster bone_lord` together with
  `goal = kill bone_lord` makes a proper final room.
- `dum --campaign <id> --depth N` jumps straight to level N for testing.
