#!/bin/bash
# DMC4SE-MOOD -- downgrade to the 2015 build (macOS). Put this + the .xdelta files
# into your Special Edition game folder, then double-click. Needs xdelta3:
#   brew install xdelta3
cd "$(dirname "$0")" || exit 1

EXE="DevilMayCry4SpecialEdition.exe"
API="steam_api.dll"

command -v xdelta3 >/dev/null 2>&1 || { echo "xdelta3 not found. Install it:  brew install xdelta3"; read -r _; exit 1; }
[ -f "$EXE" ] || { echo "ERROR: run this from your Special Edition game folder (no $EXE here)."; read -r _; exit 1; }

echo "Backing up current build to backup_2019/ ..."
mkdir -p backup_2019
cp -f "$EXE" "backup_2019/$EXE"
cp -f "$API" "backup_2019/$API"

echo "Applying downgrade..."
if ! xdelta3 -d -f -s "backup_2019/$EXE" "DMC4SE_2019to2015.xdelta" "$EXE.new"; then
  echo "FAILED: not the expected 2019 build. Nothing changed."; rm -f "$EXE.new"; read -r _; exit 1
fi
if ! xdelta3 -d -f -s "backup_2019/$API" "steam_api_2019to2015.xdelta" "$API.new"; then
  echo "FAILED on steam_api. Nothing changed."; rm -f "$EXE.new" "$API.new"; read -r _; exit 1
fi
mv -f "$EXE.new" "$EXE"
mv -f "$API.new" "$API"

echo
echo "DONE -- now on the 2015 build. Launch the game."
echo "Do NOT 'Verify integrity of game files' in Steam (it undoes this)."
echo "To revert: run 'Restore 2019.command'."
read -r _
