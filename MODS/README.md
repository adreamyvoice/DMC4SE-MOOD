# MODS folder

There are **two** ways to mod, and they're for different things. Both live next to
`dinput8.dll` in your game's `Special Edition` folder, and **neither overwrites your
real game files.**

---

## 1. MODS folder — for characters, skins, costumes, movesets

Use this for anything that's part of a **character** (a skin, a costume, a Bloody
Palace arena). Drop each mod in **its own folder** inside `MODS\`, as a packaged
`.arc`:

```
Special Edition\
  dinput8.dll
  MODS\
    My Dante Skin\
      rom\player\costume\plmod_pl006.arc
    Some BP Arena\
      rom\room\st705.arc
```

In-game: open the menu (**`7`** or **L3+R3**) → **Mods** tab → **Reload mod list**,
then pick your mod:

- **Skins** are grouped per character (Dante / Nero / Vergil / Trish / Lady) — one
  skin per character.
- **Bloody Palace** mods get their own slots.
- Anything that can't be auto-sorted shows under **Other / unsorted** as a checkbox.

Reload the level / restart the BP round to apply.

> Skin and animation textures are packed **inside** the arc and read from memory, so
> they can only be changed by a whole modified `.arc` — that's what this folder is for.

---

## 2. HDD toggle — for the GUI / menus and standalone files

The **Loose-file loading (HDD)** checkbox in the Mods tab is an **on/off switch for
the look of the GUI and menus** (plus any other override arc you add):

- **OFF** → stock game GUI (regular red Dante, normal menus).
- **ON** → your modified GUI.

How it works: drop a modified arc under **`MODS\HDD\`**, mirroring the game's own
folder layout, and the toggle swaps it in/out:

```
MODS\HDD\
  rom_pc\gui\mm01_eng.arc      ← e.g. a recolored character-select screen
```

HDD also lets genuinely **loose** standalone files (e.g. a system/effect `.tex`
dropped straight into `nativeDX10\`) load over the arc. It does **not** cover
character skins or animations — use the MODS folder above for those.

Back out of the menu / reload the screen to apply.

---

## Notes

- Nothing here overwrites your game; a broken mod just won't apply (it falls back to
  the original file).
- `movenames.cfg` is the combo move-name list — leave it in place.
- You provide your own mod `.arc` files; none are bundled with this release.
