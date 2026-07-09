#!/bin/bash
# DMC4SE-MOOD -- restore the 2019 build from backup_2019/ (macOS).
cd "$(dirname "$0")" || exit 1
if [ ! -f "backup_2019/DevilMayCry4SpecialEdition.exe" ]; then
  echo "No backup found. (Or use Steam: Verify integrity of game files.)"; read -r _; exit 1
fi
cp -f "backup_2019/DevilMayCry4SpecialEdition.exe" "DevilMayCry4SpecialEdition.exe"
cp -f "backup_2019/steam_api.dll" "steam_api.dll"
echo "Restored the 2019 build."
read -r _
