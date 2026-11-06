# Adding your own content to Dum

Monsters, items, spells, classes, shops and difficulty levels are all plain text files in
`data/`. Edit them with any text editor. You don't need to recompile anything: start the
game and your changes are there. Pictures and sounds are PNG and WAV files next to them
(see [ART_AND_SOUND.md](ART_AND_SOUND.md)), and campaigns and hand-made maps are covered in
[CAMPAIGNS.md](CAMPAIGNS.md).

| File | What's in it |
|------|--------------|
| `data/monsters.txt` | every monster: looks, strength, abilities, where it appears |
| `data/items.txt` | weapons, armor, rings, amulets, lights, food, potions, scrolls, wands, rods, treasures |
| `data/spells.txt` | spells, warrior abilities, and the effects of scrolls, wands, rods and potions |
| `data/classes.txt` | the character classes: stats, skills, spells, starting gear |
| `data/stores.txt` | the town shops: what they sell and buy |
| `data/difficulty.txt` | difficulty levels (and The Abyss) |
| `data/gfx/*.png` | your own pictures (optional) |
| `data/sfx/*.wav` | your own sounds (optional) |

The top of each `.txt` file explains every setting it understands. This guide shows how
to use them.

## The format

```ini
# A comment. Everything after # on a line is ignored.
[monster cave_troll]        # [kind id]
name = cave troll           # setting = value
hp = 6d10
```

- Each thing starts with `[kind id]`. The **id** is how everything else refers to it: other
  data files, saves, and picture file names. Use lower case letters, digits and `_`, no
  spaces. Ids must be unique within their file.
- Settings you leave out get a sensible default.
- **Dice** are written like in tabletop games: `2d6` (two six-sided dice), `1d8+2`.
- **Colors** are hex `RRGGBB`, like `ff8020`.
- **Lists** are separated by commas: `items = dagger, torch, ration`.
- **Order matters in one place:** a monster can only summon monsters defined *above* it
  in the file.

## Finding ids (and picture names)

The id is in the brackets: `[monster goblin]` has the id `goblin`. To see them all, with
the names shown in the game and the PNG file name for each:

```sh
./build/dum_tool ids              # everything
./build/dum_tool ids monsters     # or: items, spells, classes, stores
```

```
Monsters (data/monsters.txt)            depth  picture
  giant_rat            giant rat                1       monster_giant_rat.png
  cave_bat             cave bat                 1       monster_cave_bat.png
  goblin               goblin                   2       monster_goblin.png
```

You can also search the file in your editor for the name you see in the game
(`grep -n "goblin" data/monsters.txt`). On Windows, use `dum_tool.exe` the same way.

**Starting pictures:** `./build/dum --export-art "monster_*" --out art --scale 4` writes the
generated picture of every monster (or `"item_*"` for items) as a 256x256 PNG to paint over.
See [ART_AND_SOUND.md](ART_AND_SOUND.md#quick-start).

## Checking and testing your changes

```sh
./build/dum_tool check
```

This loads all data files and reports mistakes, such as a misspelled id or an unknown
setting, with the file and line. **Run it after every change.** If a data file has an
error, the game won't start, and on Windows it can't show you the message. Unknown
settings are only warned about and then ignored, so a typo like `hitpoints = 20` instead of
`hp = 20` silently does nothing; `check` shows it.

To try things out quickly, the game has test options:

```sh
./build/dum --single --spawn cave_troll            # your monster appears in front of you
./build/dum --single --spawn cave_troll --awake    # ... and comes at you
./build/dum --single --give flame_tongue,potion_giant   # put items in your pack
./build/dum --single --depth 8 --level 10          # go straight to level 8 as a level 10 hero
./build/dum --single --class wizard                # start as a particular class
```

They can be combined. `--single` skips the menu and starts a new single player game.

**Saves:** saves store things by id, so renaming an id makes saved games lose those things
(they are dropped, not crashed on).

**Co-op:** everyone in a game must have the same data files, because monsters and items
are sent over the network by number. When you join, your game sends a checksum of each
data file (`spells.txt`, `items.txt`, `monsters.txt`, `classes.txt`, `stores.txt`,
`difficulty.txt`). Only the checksums are sent, never the files. If any differ from the
host's, you can't join. You see a message naming the files, and the host's screen shows
who was turned away and why. So when you play your own content together, **give your
friends a copy of your changed `.txt` files** (zip your `data/` folder). Line endings don't
count, so a file copied between Windows and Linux still matches.

Pictures and sounds don't have to match. Without your PNGs, a friend sees the generated
picture of a new monster (from its `sprite` and colors), which is fine for playing. Copy
`data/gfx/` too if you want everyone to see the same art.

## A new monster

Add a section to `data/monsters.txt`:

```ini
[monster cave_troll]
name = cave troll
plural = cave trolls       # default: name + "s"
sprite = big               # generated picture: rat bat humanoid caster skeleton spider beast
                           #   big ghost blob snake worm eye imp elemental golem dragon
color = 607048             # body color
color2 = ffd020            # eye/accent color
hp = 6d10                  # hit points (dice)
ac = 14                    # armor class: you roll d20 + your bonus, and need this much to hit
dmg = 2d6                  # damage per hit
tohit = 3                  # bonus to its own attack rolls
speed = 1.8                # tiles per second (you walk about 3.5)
attackcd = 1.4             # seconds between its attacks
sight = 8                  # how far away it notices you (tiles)
size = 1.2                 # drawn height: 0.85 x size of a wall (1 = person-sized)
sleep = 60                 # % chance it starts asleep
xp = 40                    # experience for killing it
depth = 7                  # the shallowest dungeon level it appears on
rarity = 2                 # how common (0 = never placed randomly; higher = more common)
group = 1-2                # how many appear together
drop = 40                  # % chance it drops an item
regen = 2                  # abilities: heals 2 hit points per second
vulnerable = fire          # double damage from fire
```

That's all it takes. It appears from level 7 on, with a generated picture in your colors.
Other things you can give it (all explained at the top of `monsters.txt`):

- **A ranged attack:** `spell = m_firebolt` and `spellrate = 0.3` (tries per second). The
  monster versions of spells have ids starting with `m_` (`dum_tool ids spells` lists them),
  but a monster can use any spell.
- **Undead:** `undead = yes` (holy damage does double, and priests are good against it).
- **Special abilities:** `steals`, `poison = 5`, `drain = str`, `blink`, `invisible`,
  `breeds`, `erratic = 30`, `drainlight`, `explode = m_fireburst`, `summon = goblin` (or
  `kin`) with `summonrate`, `resist = cold`, `doors = no` (can't open doors), `reach`.
- **A boss:** `boss = yes`. Bosses never appear randomly. Every boss becomes a stage of the
  main quest ("Slay ... on dungeon level N"), in order of their `depth`, and waits on that
  level next to the stairs down with a few escorts. Bosses get a crown on the generated
  picture.

**Its picture:** without a PNG it uses the generated `sprite` in your colors. To draw your
own, save `data/gfx/monster_cave_troll.png`. Add `_attack`, `_walk1`, `_walk2`... and
`_die1`... frames if you like. The size rules and a step-by-step guide are in
[ART_AND_SOUND.md](ART_AND_SOUND.md#drawing-your-own-monster-step-by-step).

**Balance:** compare with a monster of the same depth (`dum_tool ids monsters` lists them
by depth), and see [BALANCING.md](BALANCING.md).

## A new item

Add a section to `data/items.txt`. Every item has a `kind`, which decides what it does:
`weapon`, `armor`, `helm`, `ring`, `amulet`, `light`, `food`, `potion`, `scroll`, `fuel`,
`wand`, `rod`, `gold` or `quest` (a treasure for side quests and selling).

Settings for every item: `name`, `desc` (shown in the inventory), `sprite` (floor picture,
see below), `color`, `depth` and `rarity` (like monsters; rarity 0 = only in shops or as a
starting item), `value` (shop price), `stack` (how many fit in one pack slot).

**A weapon:**

```ini
[item war_pick]
name = War Pick
desc = Heavy, slow, and it goes through armor.
kind = weapon
sprite = axe               # floor picture
viewmodel = axe            # in your hand: fist dagger sword mace axe staff bow spear
color = b0b0c0
dice = 1d12                # damage
tohit = 1
cooldown = 1.2             # seconds between swings
depth = 4
rarity = 2
value = 90
```

Bows, crossbows and slings: `ranged = yes` plus `range` (tiles), and `ammo = arrows`,
`bolts` or `pebbles` for what they shoot (bows shoot arrows if you leave it out). Light, quick weapons:
`finesse = yes` (uses DEX instead of STR). Magic weapons: `brand = fire` (or `cold`,
`holy`, `poison`), and bonuses like `todam = 2`. Staves: `spellpower = 2`.

**Armor, helms, rings, amulets:** `armor = 4` (armor class), plus any bonuses while worn:
`str = 1` (likewise `dex con int wis`), `tohit`, `todam`, `speed` (+1 = 10% faster),
`stealth`, `search`, `light` (extra light radius), `regen` (% faster healing),
`slowdigest` (% less hunger), `resist = fire, cold`.

```ini
[item ring_embers]
name = Ring of Embers
kind = ring
sprite = amulet
color = ff6020
resist = fire
todam = 1
depth = 6
rarity = 2
value = 300
```

**Potions, scrolls, food:**

```ini
[item potion_giant]
name = Potion of Giant Strength
kind = potion
sprite = potion
color = c08040
gainstat = str             # permanent +1 STR; or heal = 40, mana = 20, restore = yes, curepoison = yes
depth = 10
rarity = 1
stack = 5
value = 800

[item scroll_firestorm]
name = Scroll of Firestorm
kind = scroll
sprite = scroll
color = ff8040
spell = fireball          # reading it casts this spell (any spell from spells.txt)
depth = 5
rarity = 2
stack = 10
value = 120
```

Food uses `food = 300` (a full stomach is 1000) and can `heal`. Lights use `light` (radius)
and `fuel` (seconds), or `everburning = yes`. A wand uses `spell` and `charges = 4-8`; a
rod uses `spell` and `recharge = 20` (seconds).

**Unique artifacts:** `artifact = yes` (only one per game, never in shops).

**Ammunition:** `kind = ammo`, `ammo = arrows` (or `bolts`, `pebbles`), `bundle = 20` (how
many come together, bought or found), `stack = 99`. Give it `tohit`, `todam` and a `brand`
(fire, cold, holy, poison): they are added to the weapon's own for every shot.

```ini
[item frost_bolts]
name = Frost Bolts
kind = ammo
ammo = bolts
sprite = bolts
color = 80c0ff
todam = 2
brand = cold
bundle = 10
stack = 99
depth = 8
rarity = 1
value = 80
```

**Keys and chests:** `kind = key` with `keycolor = red` (or `blue`, `green`, `yellow`)
opens doors with that colour of lock. The dungeon places the four keys `key_red`...
`key_yellow` by itself whenever a level has such a door. `kind = chest` items are the
chests: `chest` (closed) and `chest_open` (opened). The dungeon places them and decides
what is inside, so leave their ids as they are; change their `name`, `color` or pictures
(`item_chest.png`, `item_chest_open.png`) freely.

**Floor pictures** (`sprite`): dagger sword mace axe staff bow armor robe torch lantern
ration apple meat potion scroll flask gold amulet tome chalice wand rod, drawn in your
`color`. Or draw your own: `data/gfx/item_war_pick.png`.

**Selling it in a shop:** add the id to a store's `items` list in `data/stores.txt`.
**Starting with it:** add it to a class's `items` in `data/classes.txt`.

## A new spell or ability

Add a section to `data/spells.txt`:

```ini
[spell frost_lance]
name = Frost Lance
desc = A spear of ice that slows what it hits.
type = bolt                # bolt ball nova heal bless light detect blink map recall teleport
                           #   haste shield resist dig feed whirlwind berserk descent
element = cold             # physical arcane fire cold holy poison
mana = 4
level = 5                  # character level when a class learns it
dice = 3d6                 # damage (or healing)
scale = 1.0                # + this much per character level
speed = 16                 # how fast the projectile flies
cooldown = 0.8             # seconds before you can cast again
color = a0e0ff             # color of the projectile and its light
effect = slow              # optional: sleep fear slow teleport (on monsters it hits)
```

Balls also take `radius`, and novas and some other types take `range` and `duration`.
`type = treasure` reveals the chests within `radius` tiles: trapped, empty or full, and on
the map (that's `detect_objects`).
Warrior techniques are spells with `ability = yes` and `recharge = 30` (seconds) instead
of mana.

A spell does nothing until something uses it:

- **A class learns it:** add the id to the class's `spells` in `data/classes.txt`. Write
  `frost_lance:7` to learn it at level 7 instead of the spell's own `level`.
- **An item casts it:** `spell = frost_lance` on a scroll, potion, wand or rod.
- **A monster casts it:** `spell = frost_lance` and `spellrate` on the monster.

Its picture (the glowing projectile) is `data/gfx/spell_frost_lance.png`. Black is
invisible, since it is added to the screen like light.

## A new class

Add a section to `data/classes.txt`:

```ini
[class paladin]
name = Paladin
desc = A holy warrior: strong in melee, with a few prayers.
str = 15
dex = 10
con = 14
int = 8
wis = 13
hitdie = 9                 # hit points per level: 1d9 + CON bonus
mana = 2
manaperlevel = 1
caststat = wis             # stat that powers spells and mana
melee = 3                  # skills: bonus to hit (melee/ranged) and spell power (magic)
ranged = 1
magic = 1
search = 1
stealth = 0
disarm = 1
device = 1
color = e0d080             # colors the generated player picture
spells = heal, bless:3, smite:6
items = mace, chain_mail, torch, torch, ration, ration, potion_heal, scroll_recall
```

It appears on the class selection screen at once. In co-op, others see it as
`data/gfx/player_paladin.png` if you draw one, or as a generated figure in your `color`.

## A new shop

Add a section to `data/stores.txt` (at most 8 shops):

```ini
[store bowyer]
name = The Bowyer
owner = Elra Swiftstring
sign = long_bow            # item painted on the door
items = sling, short_bow, long_bow
buys = weapon              # kinds it buys back (for half the value)
```

The town makes a door for every shop. Its door picture can be replaced with
`data/gfx/shop_bowyer.png`.

## A new difficulty level

Add a section to `data/difficulty.txt`. Every setting is a multiplier or bonus on Normal,
explained at the top of the file. It shows up in the main menu's Difficulty list.

## Sharing your content

Everything lives in `data/`. Zip that folder (or just the files you changed) and share it.
To keep the original, copy the whole folder somewhere and start the game with
`./build/dum --data /path/to/my-data` (`dum_tool` takes `--data` too).
