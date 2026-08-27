# Grid Inventory — a Tetris-style grid inventory

An SKSE plugin that replaces the vanilla inventory outright. Every item occupies
real squares on a grid, and **three screens** share the same grid UI:
**inventory · containers (chests, corpses, followers, pickpocketing) · merchants.**
**Favourites (`Q`) is replaced by the quick wheel**, which can be switched off
in the settings to get the vanilla menu back. Everything else (crafting
stations, magic, gifts, journal, map, TAB) stays vanilla/SkyUI.

Icons are captured from each item's own 3D model by the game itself — no icon
pack, and any modded item is supported automatically with no patches.

---

## Requirements

| | |
|------|------|
| Game | Skyrim Special Edition / Anniversary Edition (all of 1.5.x – 1.6.x) |
| Required | [SKSE64](https://skse.silverlock.org/) |
| Required | [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) — take the download that matches your **exe version**, not what you bought: *All in one (AE)* for 1.6.x, *(SE)* for 1.5.97. The wrong one fails at launch with `failed to open address library file`. |

Built on CommonLibSSE-NG; one DLL covers every SE/AE runtime.
**No SkyUI, no MCM.**
Gamepad IS supported: the left stick drives the game's own cursor (honouring your
cursor-speed setting), the right stick scrolls, the d-pad nudges one cell.
Buttons follow **your own control bindings** — Accept picks up / places, Equip
handles equip / read, Drop and Take All map to those, plus Toggle Favorite and
Item Zoom. Rebind in-game and it follows; non-Xbox pads map themselves.
Touching the mouse hands control straight back.

## Installation

Install the archive with your mod manager (MO2, Vortex). Contents:

```
Grid Inventory.esp          (coin/pouch + 18 bag forms + 2 encumbrance abilities + UI sounds)
SKSE/Plugins/GridInventory.dll
SKSE/Plugins/GridInventory_icons.pak      (pre-captured icons for all vanilla + AE CC items)
SKSE/Plugins/GridInventory_items.ini      (tuned footprint/rotation for every item)
SKSE/Plugins/GridInventory_categories.ini (category defaults — for unlisted items)
SKSE/Plugins/GridInventory_slots/         (equipment slot silhouettes)
SKSE/Plugins/GridInventory_fallback/      (drawn icon PNGs — swap in your own)
SKSE/Plugins/GridInventory_lang/          (languages — EN/KO/ZH/JA, add your own)
meshes/, textures/, Sound/  (coin models, sounds)
```

The remaining settings files (`_ui.ini` etc.) are generated in `SKSE/Plugins/` on
first run — there is nothing to edit beforehand. Safe to add to an ongoing save.

## First run

1. **Vanilla and AE Creation Club items come with icons pre-captured** — they
   show instantly, no caching wait. Only modded items are captured as they first
   become visible — a **"caching icons… N"** counter shows next to the **ITEMS
   label** above the grid.
2. The default language is English; other languages switch instantly in Settings.
3. Recommended with many modded items: Settings → Icons → **Precache All** —
   captures every item in your load order, one per frame, while the inventory
   stays open (closing saves and stops).

---

## Features

### The grid
- 10×14 = **140 squares, fixed**. Real footprints per item (1×1 rings to 2×4
  greatbows), free shapes (L-pieces) supported. **Rotate 90° with `A`/`D` while
  carrying.** No auto-sort and no search — organising it yourself is the point.
- **Pick-up-and-place**: left-click to lift, left-click the target square to set
  down (swap supported). Shift+left-click = stack/gold split slider.
- Rarity glow (**unique = red · enchanted = blue**), poison shown as a droplet in
  the top-right, markers (favourite ◆ / quest ▲ / stolen ●), **newly acquired
  squares lit a shade brighter**, Shift comparison tooltip with an
  *Equipped* card.
- **Capacity**: exceeding 140 squares shows an overflow row and slows movement.
  A full grid blocks pickups (quest/script-granted items are deliberately never
  blocked — they come in and push you into overflow instead).
- **`C` inspect in 3D**: rotate and zoom the actual model (Dragon Claw glyphs etc.).
  Works on your grid, the equipment panel and container/merchant cells alike.
  **Opens at its smallest zoom.**
- **Equipment doll**: 17 slots, place to equip / right-click to unequip.
- **Two rings**: vanilla wears one; here you can wear two.
  - The first goes on with a plain **click**, into the left ring slot.
  - The second must be **dragged onto the right ring slot**. Clicking will not
    put it there — otherwise every ring would keep replacing the first one.
  - **Two rings with the same effect are refused**, including two grades of one
    family: the check is on the magic effect, so a different name or rating does
    not get past it.
  - Drag the left ring onto the right slot and **the two trade places**.
  - The second ring's **effect applies, but it is not visible on your hand.** A
    ring's mesh carries its own slot number inside the file, and while the first
    ring holds that number the second cannot be drawn. That is a limit of the
    game, not something this mod can work around.
- **Gear-set tabs**: one click swaps the whole set (really equips; stats follow).
  Gear held by inactive tabs is hidden and takes no squares.
- **Eighteen bags**: right-click opens the inner grid.
  - **Twelve general** — from a one-square pouch to a 3×3 pack holding a
    hundred squares. General goods vendors rotate three of them each restock.
  - **Six sorting bags** — ingredients, ore and ingots, hides, potions, soul
    gems, keys (lockpicks included). Each takes only its own kind and
    **anything you pick up goes straight into it**; what you take back out
    stays where you put it. **COLLECT** on the title bar gathers that kind
    from wherever it is scattered. Always stocked by the trader who deals in
    what the bag holds.
  - A sorting bag's squares are left out of the Space figure — they cannot
    hold general loot.
  - EDIT mode can designate any other item as a bag too (up to 16×16).
- **Trash bin**: 6×4 staging area; right-click restores; **deletion is final when
  the window closes.**

### Physical gold
- Gold appears as coin tiles, one per 1,000 G plus remainder.
- **Coin Pouch** (2×2, sold by general goods vendors): banks up to 10,000 G,
  auto-stores incoming gold, right-click slider to withdraw. Pouched gold still
  counts toward your total and pays at shops.
- Drop a coin tile outside (or `R`) and a purse object lands on the ground.

### Containers / shops / pickpocketing
- Chests, corpses and followers open as the same grid, and **layouts are
  remembered** (128 containers; emptied squares stay reserved).
- Shops: haggled prices, **per-piece values with tempering included**, merchant
  gold shown, quantity slider with a **MAX** button, proper Speech XP.
- Pickpocketing: a success % on every square (for the whole stack), worn gear
  locked (Perfect Touch unlocks), reverse-pickpocketing + Poisoned-perk planting.
- Containers auto-open after a successful lockpick (yields to QuickLoot-style widgets).

### The quick wheel — change gear without opening anything

**Hold the game's Favourites key (`Q` by default)** and the screen falls back
while a ten-slot ring opens. It **replaces the vanilla favourites menu**, so it
answers that key and follows it if you rebind it in the game's own controls.

- **`A` / `D` switch group** — there are four.

  | Group | What it holds |
  |---|---|
  | PRESET | Your loadout tabs — picking one changes into that set |
  | COSTUME | The same tabs, **appearance only** (see below) |
  | GEAR | Weapons, armour and potions you have starred |
  | MAGIC | Spells and shouts you have starred |

- **Aiming**: roll the mouse wheel, or just move the mouse that way.
  **Letting go of the key applies it.** On a pad: left stick aims, D-pad
  left/right switches group.
- **To choose nothing**, bring the cursor back to the middle and release.
  In the middle the pointer leaves a trail of ink, so you can see where your
  hand is even with nothing selected.
- **GEAR and MAGIC also take clicks** — left is your right hand, right is your
  left. Anything not held in a hand is simply used. Picking something already
  equipped takes it off.
- **Rearranging**: hold the left button on a slot and carry it; each neighbour
  you pass steps aside on its own. Where you drop it is saved, and **a visit
  that rearranged applies nothing.** Each group remembers its own layout, and
  the inventory's tab order is left alone.
- **Starring IS registering** — `F` in the inventory, or the game's own
  favourite in the magic menu. There is no separate step for the wheel.
- **It can be turned off** — set **QUICK WHEEL** to Off in the settings and the
  vanilla favourites menu comes back whole, number hotkeys included. The
  wheel's layout is kept, so turning it back on returns you to it.

### Costumes — keep the stats, change the look

Point one loadout tab at your appearance and your body shows that set while
everything the game measures still comes from what you are really wearing.

- Set it from the marker on the inventory's tabs, or from the wheel's
  **COSTUME** group. One at a time — choosing another clears the last.
- **Choosing EQUIP takes the costume off** and your own gear shows again. (It
  does not switch you to the EQUIP set; that is the PRESET wheel's job.)
- Armour rating, enchantments, tempering and perks are **the equipped item's**.
  Nothing is unequipped for this, not even for an instant — only the appearance
  list is borrowed while the body is rebuilt.
- The set you are currently wearing cannot be used as a costume (it would be
  wearing the same thing twice).

### Settings (SETTINGS in the title bar)
- Scale, 19 skins (two of them brush-and-ink), languages (live switch), icon
  brightness/style, item shadow (distance/blur/opacity), capture light, icon
  cache reset, precache all, **quick wheel on/off**, trade options (unlimited
  merchant gold / merchant buys anything).
- Sliders take a drag, the ± buttons at either end (held for continuous), a
  double-click to type an exact value, and a right-click to restore the default.

### Adding a translation
Every language is a text file in `SKSE/Plugins/GridInventory_lang/`. One you write
yourself joins the list on **exactly the same footing** as the four that ship.

1. Copy `en.ini` and rename it — `pl.ini`, say.
2. Edit the directives at the top.

```ini
#name  = Polski                  the name shown in the list
#order = 50                      position in the list (default 100)
#font  = C:\Windows\Fonts\...    only for scripts the built-in atlas lacks
#range = cyrillic                a preset, or an explicit 0x0400-0x052F

Inventory = EKWIPUNEK            keep the key, translate the right-hand side
```

- **Never rename a key.** Lookup is by name, not position — which is why a file
  written against an older build keeps working when new strings are added.
- Any key you leave out shows English. A partial translation is fine.
- Write a line break as `\n`.
- **`#range` matters more than `#font`.** Without the range you get tofu even when
  the face on disk has the glyphs.
- `en.ini` overlays the built-in English instead of adding a language.
- Delete the folder entirely and the UI runs in English. Nothing breaks.
- **Three icon styles**:
  - **Realistic** — captures the game's own 3D models (default).
  - **Drawn** — hand-drawn **category icons only**. **Captures nothing**, so
    there is no first scan and the grid is complete the moment the menu opens.
    Recommended if you run a lot of item mods. **127 categories**, 492 files
    once the material and tier tints are counted: weapons and armour split by
    16 materials, spell tomes by their five schools, skill books by their three
    guardian-stone lines, potions by their six effect colours.
  - **Pixel** — the realistic capture redrawn as dots. It ships no artwork, so
    **modded items are covered automatically**. Each item is quantised to
    twelve colours taken from its own capture, so no hue appears that was not
    already there.

  The `C` 3D view works in all three. Drawn icons have their own rotation, zoom
  and horizontal nudge in EDIT, stored separately from the realistic ones.
  (Pixel is derived from realistic, so it shares that style's rotation, scale
  and footprint.)

- **Presets**: save the whole look — skin, every item definition, and the icon
  pictures — under a name; load any from a dropdown. Share the two files
  (`GridInventory_<name>.ini` + `GridInventory_<name>_icons.pak`) and another
  player gets your exact setup **with no caching wait.**

### Replacing drawn icons (no tool)

**Swap a PNG in `SKSE/Plugins/GridInventory_fallback/` and the game uses it.**
The same notes are in that folder's `_README.txt`.

| File you add | Applies to |
|---|---|
| `item\Skyrim.esm_0x012E49.png` | that one item |
| `wpn_sword@steel.png` | every steel sword |
| `wpn_sword.png` | every one-handed sword |

- **The game tells you the name.** Click an item in EDIT and the `icon file`
  row shows both names; click either to copy it.
- 128×128 RGBA recommended. Any square size works.
- A PNG you add **always outranks the shipped set**, so one `wpn_sword.png`
  covers the material variants too.
- After editing: `F5 → ICONS → DRAWN ICONS → Reload`.
- ⚠️ **Under MO2, adding a file for the first time needs one game restart.**
  MO2 builds its file list at launch, so a file created while the game runs is
  invisible to it. After that one restart, every edit to that file shows up on
  Reload.

### Editing (EDIT in the title bar)
- Click an item → rotation and position (two tabs) / scale / footprint (8×8
  painter, free-form shapes) / capture light / bag / stack size, plus "save as
  category default". Share the results via presets.

---

## Controls

| Input | Action |
|------|------|
| Inventory key (default `I`) | Open / close — follows the game's own key binding |
| Favourites key (default `Q`, hold) | **Quick wheel** — wheel/mouse to aim, release to apply · `A`/`D` group · left/right click = right/left hand · drag to rearrange |
| Left-click | Pick up → left-click the destination (on another item = swap) |
| Right-click | Equip/use · open bag · pouch withdraw · (loot/shop/pickpocket) store·sell·plant · (while carrying) cancel |
| Shift+Left-click | Stack / gold split slider |
| `A` / `D` (while carrying) | Rotate — `A` anticlockwise / `D` clockwise, 90° a step. Only items whose footprint changes |
| In quantity popups | `A`/`D`·`←`/`→` adjust by 1 · **MAX** button · `Enter`/`Space` confirm · `ESC` cancel |
| Shift (hold) | Compare against equipped |
| `C` | Inspect in 3D — your grid, the equipment panel and container/merchant cells alike. Drag rotate · wheel zoom · `R` reset |
| `R` | Over your grid = drop one / over a container = take all |
| `F` | Favourite — goes straight onto the quick wheel's GEAR group |
| Drop outside | Discard (cancelled while a chest/shop window is open) |
| `ESC` | One layer at a time: 3D inspect → popups → trash → pouch → settings → EDIT → inventory. A carried item is dropped first |

## Good to know

- **Opening the inventory pauses the game** (conflicts with Skyrim Souls RE).
- **Spell tome learning is incompatible with book-reading overhauls (Book 'em
  and the like)** — Skyrim's in-menu reading is not a callable API, so this
  mod teaches and consumes tomes directly; an overhaul's hooks (unconsumed
  tomes, study sessions) never see our screen. Regular books, notes, scrolls
  and Elder Scrolls go through the engine's own doors and are unaffected, and
  any book read in the world works with overhauls as normal.
- **The weight limit is retired by raising carry weight enormously** — weight
  management mods lose their point. The effect removes itself on uninstall.
- ⚠️ **A filled Coin Pouch left in a respawning container is lost with its gold.**
  Use player-home chests. Pouch contents and dropped purses live only in the
  co-save — **retrieve them before uninstalling.**
- Selling a pouch auto-withdraws the stored gold first; only the empty pouch sells.
- After swapping retextures, run Settings → Icons → **Icon cache reset**.
- No keys of its own to rebind — the inventory uses the game's **Inventory**
  key and the quick wheel uses the game's **Favourites** key. Change them in
  the game's control settings.
- CJK text uses your Windows system fonts (no fonts redistributed).
- Deleting the language folder leaves the UI running in English. Back up any
  translation you wrote — a mod update overwrites `GridInventory_lang\`.
- Zero Papyrus scripts — no script load, nothing left in your save.
- Generated files (MO2: Overwrite): `GridInventory_ui.ini` (settings & windows),
  and any presets you export.
  The bundled `_items.ini` / `_categories.ini` / `_icons.pak` are updated during
  play (EDIT changes, new captures) — a mod update overwrites them, so **back up
  your own tuning as a preset.**
  Log: `Documents\My Games\Skyrim Special Edition\SKSE\GridInventory.log`

## Credits / licence

See [CREDITS.md](CREDITS.md) — plugin source under **GPL-3.0** (with the Modding
Exception). Thanks to Modex (patchulidev), Address Library (meh321),
CommonLibSSE-NG, Dear ImGui, and the SKSE team.
