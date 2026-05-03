@echo off
setlocal enabledelayedexpansion
title KenshiMP Installer
color 0A

:: ============================================
::  Kenshi-Online Installer
:: ============================================
::  Most players should use KenshiMP.Injector.exe instead — it auto-detects
::  Kenshi, installs the DLL/mod, edits Plugins_x64.cfg, and launches the game
::  in one click. This script exists for two cases the Injector does not yet
::  fully cover:
::    1. GUI layout install (MainMenu / MultiplayerPanel / MultiplayerHUD).
::    2. Backups of Plugins_x64.cfg, __mods.list, and Kenshi_MainMenu.layout
::       before mutation, plus a clean uninstall path.
::  Until the Injector reaches parity, run this script ONCE on first install
::  (or after an update that changes the layouts), then use the Injector for
::  day-to-day launching.
::
::  Single canonical install script. Works for:
::   (A) Developer flow  - sources from .\build\bin\Release\ and .\dist\
::   (B) End-user flow   - sources from same folder as this script (unzipped dist)
::  Each artifact is resolved independently so both flows work.
::
::  Kenshi install dir auto-detection order:
::    1. KENSHI_DIR environment variable (override)
::    2. Same folder as this script (script dropped in Kenshi dir)
::    3. Parent folder of this script
::    4. Common Steam / GOG locations across drives C-G
::    5. Prompt user
:: ============================================

echo.
echo  ============================================
echo   Kenshi-Online Installer
echo  ============================================
echo.

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "BUILD_DIR=%SCRIPT_DIR%\build\bin\Release"
set "DIST_DIR=%SCRIPT_DIR%\dist"

:: ---------------------------------------------
:: Resolve source artifacts
:: ---------------------------------------------
set "SRC_DLL="
if exist "%BUILD_DIR%\KenshiMP.Core.dll"   set "SRC_DLL=%BUILD_DIR%\KenshiMP.Core.dll"
if not defined SRC_DLL if exist "%SCRIPT_DIR%\KenshiMP.Core.dll" set "SRC_DLL=%SCRIPT_DIR%\KenshiMP.Core.dll"

set "SRC_SERVER="
if exist "%BUILD_DIR%\KenshiMP.Server.exe" set "SRC_SERVER=%BUILD_DIR%\KenshiMP.Server.exe"
if not defined SRC_SERVER if exist "%SCRIPT_DIR%\KenshiMP.Server.exe" set "SRC_SERVER=%SCRIPT_DIR%\KenshiMP.Server.exe"

set "SRC_MOD="
if exist "%DIST_DIR%\kenshi-online.mod"    set "SRC_MOD=%DIST_DIR%\kenshi-online.mod"
if not defined SRC_MOD if exist "%SCRIPT_DIR%\kenshi-online.mod" set "SRC_MOD=%SCRIPT_DIR%\kenshi-online.mod"

set "SRC_MAINMENU="
if exist "%DIST_DIR%\Kenshi_MainMenu.layout"      set "SRC_MAINMENU=%DIST_DIR%\Kenshi_MainMenu.layout"
if not defined SRC_MAINMENU if exist "%SCRIPT_DIR%\Kenshi_MainMenu.layout" set "SRC_MAINMENU=%SCRIPT_DIR%\Kenshi_MainMenu.layout"

set "SRC_MPPANEL="
if exist "%DIST_DIR%\Kenshi_MultiplayerPanel.layout"      set "SRC_MPPANEL=%DIST_DIR%\Kenshi_MultiplayerPanel.layout"
if not defined SRC_MPPANEL if exist "%SCRIPT_DIR%\Kenshi_MultiplayerPanel.layout" set "SRC_MPPANEL=%SCRIPT_DIR%\Kenshi_MultiplayerPanel.layout"

set "SRC_MPHUD="
if exist "%DIST_DIR%\Kenshi_MultiplayerHUD.layout"      set "SRC_MPHUD=%DIST_DIR%\Kenshi_MultiplayerHUD.layout"
if not defined SRC_MPHUD if exist "%SCRIPT_DIR%\Kenshi_MultiplayerHUD.layout" set "SRC_MPHUD=%SCRIPT_DIR%\Kenshi_MultiplayerHUD.layout"

:: ---------------------------------------------
:: Resolve Kenshi install dir
:: ---------------------------------------------
:: Honor env override; KENSHI_DIR survives setlocal because it was set before.
if defined KENSHI_DIR (
    if exist "%KENSHI_DIR%\kenshi_x64.exe" goto :kenshi_found
    echo  [WARN] KENSHI_DIR=%KENSHI_DIR% has no kenshi_x64.exe -- ignoring.
    set "KENSHI_DIR="
)

if exist "%SCRIPT_DIR%\kenshi_x64.exe"    ( set "KENSHI_DIR=%SCRIPT_DIR%"    & goto :kenshi_found )
if exist "%SCRIPT_DIR%\..\kenshi_x64.exe" ( set "KENSHI_DIR=%SCRIPT_DIR%\.." & goto :kenshi_found )

call :probe "C:\Program Files (x86)\Steam\steamapps\common\Kenshi" && goto :kenshi_found
call :probe "C:\Program Files\Steam\steamapps\common\Kenshi"        && goto :kenshi_found
call :probe "D:\SteamLibrary\steamapps\common\Kenshi"               && goto :kenshi_found
call :probe "D:\Steam\steamapps\common\Kenshi"                      && goto :kenshi_found
call :probe "E:\SteamLibrary\steamapps\common\Kenshi"               && goto :kenshi_found
call :probe "F:\SteamLibrary\steamapps\common\Kenshi"               && goto :kenshi_found
call :probe "G:\SteamLibrary\steamapps\common\Kenshi"               && goto :kenshi_found
call :probe "C:\GOG Games\Kenshi"                                   && goto :kenshi_found
call :probe "C:\Program Files\GOG Galaxy\Games\Kenshi"              && goto :kenshi_found
call :probe "D:\GOG Games\Kenshi"                                   && goto :kenshi_found

echo  Could not auto-detect Kenshi.
echo  Tip: re-run with KENSHI_DIR set, e.g.
echo       set "KENSHI_DIR=D:\SteamLibrary\steamapps\common\Kenshi" ^&^& install.bat
echo.
echo  Or enter the path manually (folder containing kenshi_x64.exe):
set /p "KENSHI_DIR=Path: "

:kenshi_found
if "%KENSHI_DIR:~-1%"=="\" set "KENSHI_DIR=%KENSHI_DIR:~0,-1%"
if not exist "%KENSHI_DIR%\kenshi_x64.exe" (
    echo  [ERROR] kenshi_x64.exe not found at: %KENSHI_DIR%
    pause
    exit /b 1
)

echo  Kenshi dir : %KENSHI_DIR%
echo  Sources:
echo    DLL      : !SRC_DLL!
echo    Server   : !SRC_SERVER!
echo    Mod      : !SRC_MOD!
echo    MainMenu : !SRC_MAINMENU!
echo    MP Panel : !SRC_MPPANEL!
echo    MP HUD   : !SRC_MPHUD!
echo.

:: ---------------------------------------------
:: Pre-flight: Kenshi must not be running
:: ---------------------------------------------
tasklist /FI "IMAGENAME eq kenshi_x64.exe" 2>NUL | find /I "kenshi_x64.exe" >NUL
if %errorlevel% equ 0 (
    echo  [ERROR] Kenshi is running. Close it before installing.
    pause
    exit /b 1
)

:: ---------------------------------------------
:: Step 1 - Backups
:: ---------------------------------------------
echo  [1/6] Backups...
set "BACKUP_DIR=%KENSHI_DIR%\KenshiMP_backup"
if not exist "%BACKUP_DIR%" mkdir "%BACKUP_DIR%"
if exist "%KENSHI_DIR%\Plugins_x64.cfg" if not exist "%BACKUP_DIR%\Plugins_x64.cfg.bak" (
    copy /Y "%KENSHI_DIR%\Plugins_x64.cfg" "%BACKUP_DIR%\Plugins_x64.cfg.bak" >nul
    echo         Backed up Plugins_x64.cfg
)
if exist "%KENSHI_DIR%\data\__mods.list" if not exist "%BACKUP_DIR%\__mods.list.bak" (
    copy /Y "%KENSHI_DIR%\data\__mods.list" "%BACKUP_DIR%\__mods.list.bak" >nul
    echo         Backed up __mods.list
)
if exist "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" if not exist "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" (
    copy /Y "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" >nul
    echo         Backed up Kenshi_MainMenu.layout
)

:: ---------------------------------------------
:: Step 2 - Core DLL
:: ---------------------------------------------
echo  [2/6] KenshiMP.Core.dll...
if not defined SRC_DLL (
    echo  [ERROR] DLL not found.
    echo          Run build.bat first, or place KenshiMP.Core.dll next to this script.
    pause
    exit /b 1
)
copy /Y "!SRC_DLL!" "%KENSHI_DIR%\KenshiMP.Core.dll" >nul
if errorlevel 1 ( echo  [ERROR] Copy failed. & pause & exit /b 1 )
echo         Copied

:: ---------------------------------------------
:: Step 3 - Plugins_x64.cfg
:: ---------------------------------------------
echo  [3/6] Plugins_x64.cfg...
findstr /C:"Plugin=KenshiMP.Core" "%KENSHI_DIR%\Plugins_x64.cfg" >nul 2>&1
if errorlevel 1 (
    echo Plugin=KenshiMP.Core>> "%KENSHI_DIR%\Plugins_x64.cfg"
    echo         Added Plugin=KenshiMP.Core
) else (
    echo         Already present
)

:: ---------------------------------------------
:: Step 4 - Layouts
:: ---------------------------------------------
echo  [4/6] GUI layouts...
if not exist "%KENSHI_DIR%\data\gui\layout" mkdir "%KENSHI_DIR%\data\gui\layout"
if defined SRC_MPPANEL (
    copy /Y "!SRC_MPPANEL!" "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerPanel.layout" >nul
    echo         Kenshi_MultiplayerPanel.layout
) else (
    echo  [WARN] Kenshi_MultiplayerPanel.layout not found - in-game MP panel will be missing.
)
if defined SRC_MPHUD (
    copy /Y "!SRC_MPHUD!" "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerHUD.layout" >nul
    echo         Kenshi_MultiplayerHUD.layout
)
if defined SRC_MAINMENU (
    copy /Y "!SRC_MAINMENU!" "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" >nul
    echo         Kenshi_MainMenu.layout (pre-patched with MULTIPLAYER button)
) else (
    findstr /C:"MultiplayerButton" "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" >nul 2>&1
    if errorlevel 1 (
        echo  [WARN] No pre-patched Kenshi_MainMenu.layout, and current layout lacks the
        echo         MULTIPLAYER button. F1 menu still works in-game.
    )
)

:: ---------------------------------------------
:: Step 5 - Mod (REQUIRED - game crashes without it)
:: ---------------------------------------------
echo  [5/6] kenshi-online.mod (required for player spawning)...
if not defined SRC_MOD (
    echo  [ERROR] kenshi-online.mod not found.
    echo          Looked in: %DIST_DIR%\ and %SCRIPT_DIR%\
    echo          Without this mod, Kenshi crashes during character creation.
    pause
    exit /b 1
)
copy /Y "!SRC_MOD!" "%KENSHI_DIR%\data\kenshi-online.mod" >nul
echo         Copied to data\
if not exist "%KENSHI_DIR%\mods\kenshi-online" mkdir "%KENSHI_DIR%\mods\kenshi-online"
copy /Y "!SRC_MOD!" "%KENSHI_DIR%\mods\kenshi-online\kenshi-online.mod" >nul
echo         Copied to mods\kenshi-online\

if not exist "%KENSHI_DIR%\data\__mods.list" type nul > "%KENSHI_DIR%\data\__mods.list"
findstr /X /C:"kenshi-online" "%KENSHI_DIR%\data\__mods.list" >nul 2>&1
if errorlevel 1 (
    echo kenshi-online>> "%KENSHI_DIR%\data\__mods.list"
    echo         Added kenshi-online to __mods.list
) else (
    echo         Already in __mods.list
)

:: ---------------------------------------------
:: Step 6 - Server (optional)
:: ---------------------------------------------
echo  [6/6] KenshiMP.Server.exe...
if defined SRC_SERVER (
    copy /Y "!SRC_SERVER!" "%KENSHI_DIR%\KenshiMP.Server.exe" >nul
    if errorlevel 1 (
        echo         [WARN] Copy failed (server running?)
    ) else (
        echo         Copied
    )
) else (
    echo         [INFO] Server exe not found (optional - skip if you only join games)
)

echo.
echo  ============================================
echo   Install complete.
echo  ============================================
echo   Kenshi : %KENSHI_DIR%
echo   Backup : %BACKUP_DIR%
echo.
echo   Launch Kenshi - F1 in-game opens the MP menu.
echo   To start a local server: run KenshiMP.Server.exe in the Kenshi dir.
echo.
pause
exit /b 0

:: ---------------------------------------------
:: :probe <path>  -- sets KENSHI_DIR and returns 0 if path has kenshi_x64.exe
:: ---------------------------------------------
:probe
if exist "%~1\kenshi_x64.exe" (
    set "KENSHI_DIR=%~1"
    exit /b 0
)
exit /b 1
