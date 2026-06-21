@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo   DMC4SE-MOOD  --  Downgrade to the 2015 launch build
echo ============================================================
echo.

if not exist "DevilMayCry4SpecialEdition.exe" (
  echo  ERROR: I don't see DevilMayCry4SpecialEdition.exe next to me.
  echo  Put ALL files from this folder into your game folder:
  echo     ...\steamapps\common\Special Edition\
  echo  then run this again.
  echo.
  pause
  exit /b 1
)

echo  Backing up your current build to  backup_2019\  ...
if not exist "backup_2019" mkdir "backup_2019"
copy /Y "DevilMayCry4SpecialEdition.exe" "backup_2019\DevilMayCry4SpecialEdition.exe" >nul
copy /Y "steam_api.dll"                   "backup_2019\steam_api.dll"                   >nul

echo  Applying downgrade patch (this can take a few seconds) ...
xdelta3.exe -d -f -s "backup_2019\DevilMayCry4SpecialEdition.exe" "DMC4SE_2019to2015.xdelta" "DevilMayCry4SpecialEdition.exe.new"
if errorlevel 1 (
  echo.
  echo  FAILED: your exe is not the expected latest ^(2019^) Steam build.
  echo  Nothing was changed. If you are already on 2015, you're done.
  del "DevilMayCry4SpecialEdition.exe.new" 2>nul
  pause
  exit /b 1
)
xdelta3.exe -d -f -s "backup_2019\steam_api.dll" "steam_api_2019to2015.xdelta" "steam_api.dll.new"
if errorlevel 1 (
  echo.
  echo  FAILED on steam_api.dll. Nothing was changed.
  del "steam_api.dll.new" 2>nul
  del "DevilMayCry4SpecialEdition.exe.new" 2>nul
  pause
  exit /b 1
)

move /Y "DevilMayCry4SpecialEdition.exe.new" "DevilMayCry4SpecialEdition.exe" >nul
move /Y "steam_api.dll.new"                   "steam_api.dll"                   >nul

echo.
echo  DONE -- you are now on the 2015 build. Launch the game.
echo.
echo  IMPORTANT: do NOT use Steam's "Verify integrity of game files" --
echo  it will re-download the 2019 build and undo this. To go back on
echo  purpose, run  "Restore 2019.bat".
echo.
pause
