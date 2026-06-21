@echo off
setlocal
cd /d "%~dp0"

echo  Restoring the latest (2019) Steam build from backup_2019\ ...
if not exist "backup_2019\DevilMayCry4SpecialEdition.exe" (
  echo  No backup found. Nothing to restore.
  echo  ^(Or just use Steam: Verify integrity of game files.^)
  pause
  exit /b 1
)
copy /Y "backup_2019\DevilMayCry4SpecialEdition.exe" "DevilMayCry4SpecialEdition.exe" >nul
copy /Y "backup_2019\steam_api.dll"                   "steam_api.dll"                   >nul
echo  Restored. You are back on the 2019 build.
pause
