# HUD recolor tools (MT Framework .arc / .tex)

DMC4SE assets are MT Framework `.arc` archives (magic `ARC\0`, v7): 8-byte header
(`ARC\0`, u16 version, u16 file count) + 80-byte TOC entries
(64-byte path, u32 ext-hash, u32 compSize, u32 decompSize+flags, u32 offset),
every file zlib-compressed. Drop a modified `.arc` into the matching
`nativeDX10/<path>` and the game loads it over the packed original — same
override trick costume mods (e.g. Update-324 `plmod_pl006_ex00.arc`) use.

## The combat HUD
`nativeDX10/rom/gui/Cookpit.arc` (Capcom's "Cockpit" typo) holds the in-game HUD:
- `gui\cockpit\target0|stylish_rank|hp_boss|cockpit_n` — `.gui` layout files
  (ext-hash `0x22948394`), carry sprite rects + per-element RGBA tints.
- `gui\cockpit\main02_NOMIP` — `.tex` atlas (ext-hash `0x241F5DEB`).
  **Uncompressed RGBA8, 2048×1152, 20-byte header, byte3=alpha.** No DXT codec
  needed; channel order is R,G,B,A (detected from the warm/red HUD art).

## Recolor (current: black/red bloody)
`recolor.py` maps every atlas pixel through a luminance ramp: black art stays
black, bright art becomes blood red (green/blue crushed), alpha untouched. The
4 `.gui` blobs are copied verbatim; TOC layout preserved (first file @0x8000,
contiguous after).

```
python3 recolor.py <src Cookpit.arc> <out .arc>
```
Tunables at the top of `bloody()` / the `ng_k`,`nb_k` lines:
- red floor `40` and slope `*86//100` — raise floor for a brighter/oranger look.
- `ng_k`/`nb_k` (default 14/10) — small green/blue so highlights read as fresh
  blood; set both to 0 for pure crimson, raise green for a more orange "ember" HUD.

## Install / revert
Original is backed up next to it as `Cookpit.arc.orig`.
```
# revert:
cp Cookpit.arc.orig Cookpit.arc
```
`arc.py <file> [outdir]` lists a TOC, or extracts when given an output dir.
