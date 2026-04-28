@echo off
setlocal enabledelayedexpansion
title KenshiMP Installer
color 0A

echo.
echo  ============================================
echo   Kenshi-Online (KenshiMP) Installer
echo   Co-op alpha build
echo  ============================================
echo.

:: ── Auto-detect Kenshi directory ──

set "KENSHI_DIR="

:: Local checks first (installer dropped into / next to game folder)
if exist "%~dp0kenshi_x64.exe"     ( set "KENSHI_DIR=%~dp0"     & goto :found_kenshi )
if exist "%~dp0..\kenshi_x64.exe"  ( set "KENSHI_DIR=%~dp0..\"  & goto :found_kenshi )

:: Default Steam install path
set "P=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
if exist "%P%\kenshi_x64.exe" ( set "KENSHI_DIR=%P%" & goto :found_kenshi )

:: GOG default
set "P=C:\GOG Games\Kenshi"
if exist "%P%\kenshi_x64.exe" ( set "KENSHI_DIR=%P%" & goto :found_kenshi )

:: Parse Steam libraryfolders.vdf for non-default library locations
:: (Steam stores extra libraries like D:\SteamLibrary, C:\SteamLibrary, etc.)
set "VDF=C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf"
if exist "%VDF%" (
    for /f "tokens=2 delims=^"" %%A in ('findstr /C:"\"path\"" "%VDF%" 2^>nul') do (
        set "CAND=%%A\steamapps\common\Kenshi"
        :: Replace double backslashes (libraryfolders.vdf escapes them)
        set "CAND=!CAND:\\=\!"
        if exist "!CAND!\kenshi_x64.exe" (
            set "KENSHI_DIR=!CAND!"
            goto :found_kenshi
        )
    )
)

:: Last-resort manual prompt
echo  Could not auto-detect Kenshi.
echo  Enter the full path to your Kenshi folder
echo  ^(the folder containing kenshi_x64.exe^):
echo.
set /p "KENSHI_DIR=Path: "

if not exist "%KENSHI_DIR%\kenshi_x64.exe" (
    echo.
    echo  [ERROR] kenshi_x64.exe not found at: %KENSHI_DIR%
    pause
    exit /b 1
)

:found_kenshi
if "%KENSHI_DIR:~-1%"=="\" set "KENSHI_DIR=%KENSHI_DIR:~0,-1%"

echo  Found Kenshi at: %KENSHI_DIR%
echo.

:: ── Refuse to install while Kenshi is running ──
tasklist /FI "IMAGENAME eq kenshi_x64.exe" 2>NUL | find /I "kenshi_x64.exe" >NUL
if %errorlevel% equ 0 (
    echo  [ERROR] Kenshi is currently running. Close it before installing.
    pause
    exit /b 1
)

:: ── Timestamped backup directory ──
for /f "tokens=2 delims==" %%T in ('wmic os get localdatetime /value 2^>nul') do set "STAMP=%%T"
set "STAMP=%STAMP:~0,8%-%STAMP:~8,6%"
set "BACKUP_DIR=%KENSHI_DIR%\KenshiMP_backup_%STAMP%"
mkdir "%BACKUP_DIR%" 2>nul

echo  [1/7] Backing up files we are about to modify...
if exist "%KENSHI_DIR%\Plugins_x64.cfg" (
    copy /Y "%KENSHI_DIR%\Plugins_x64.cfg" "%BACKUP_DIR%\Plugins_x64.cfg.bak" >nul
    echo         Plugins_x64.cfg
)
if exist "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" (
    copy /Y "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" >nul
    echo         Kenshi_MainMenu.layout
)
if exist "%KENSHI_DIR%\data\__mods.list" (
    copy /Y "%KENSHI_DIR%\data\__mods.list" "%BACKUP_DIR%\__mods.list.bak" >nul
    echo         __mods.list
)

echo  [2/7] Installing KenshiMP.Core.dll...
if not exist "%~dp0KenshiMP.Core.dll" (
    echo  [ERROR] KenshiMP.Core.dll not found in installer folder.
    pause
    exit /b 1
)
copy /Y "%~dp0KenshiMP.Core.dll" "%KENSHI_DIR%\KenshiMP.Core.dll" >nul
if errorlevel 1 (
    echo  [ERROR] DLL copy failed. Is something locking the file?
    pause
    exit /b 1
)
echo         OK

echo  [3/7] Patching Plugins_x64.cfg...
findstr /C:"Plugin=KenshiMP.Core" "%KENSHI_DIR%\Plugins_x64.cfg" >nul 2>&1
if errorlevel 1 (
    echo Plugin=KenshiMP.Core>> "%KENSHI_DIR%\Plugins_x64.cfg"
    echo         Added Plugin=KenshiMP.Core
) else (
    echo         Already present, leaving as-is
)

echo  [4/7] Installing UI layouts...
if exist "%~dp0Kenshi_MultiplayerPanel.layout" (
    copy /Y "%~dp0Kenshi_MultiplayerPanel.layout" "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerPanel.layout" >nul
    echo         Kenshi_MultiplayerPanel.layout
)
if exist "%~dp0Kenshi_MultiplayerHUD.layout" (
    copy /Y "%~dp0Kenshi_MultiplayerHUD.layout" "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerHUD.layout" >nul
    echo         Kenshi_MultiplayerHUD.layout
)
if exist "%~dp0Kenshi_MainMenu.layout" (
    copy /Y "%~dp0Kenshi_MainMenu.layout" "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" >nul
    echo         Kenshi_MainMenu.layout (with MULTIPLAYER button)
)

echo  [5/7] Installing kenshi-online.mod...
if exist "%~dp0kenshi-online.mod" (
    copy /Y "%~dp0kenshi-online.mod" "%KENSHI_DIR%\data\kenshi-online.mod" >nul
    if not exist "%KENSHI_DIR%\mods\kenshi-online" mkdir "%KENSHI_DIR%\mods\kenshi-online"
    copy /Y "%~dp0kenshi-online.mod" "%KENSHI_DIR%\mods\kenshi-online\kenshi-online.mod" >nul
    findstr /C:"kenshi-online" "%KENSHI_DIR%\data\__mods.list" >nul 2>&1
    if errorlevel 1 (
        echo kenshi-online>> "%KENSHI_DIR%\data\__mods.list"
        echo         Activated kenshi-online in __mods.list
    ) else (
        echo         kenshi-online already in __mods.list
    )
) else (
    echo         [INFO] kenshi-online.mod not in package, skipping
)

echo  [6/7] Installing dedicated server + master server...
if exist "%~dp0KenshiMP.Server.exe" (
    copy /Y "%~dp0KenshiMP.Server.exe" "%KENSHI_DIR%\KenshiMP.Server.exe" >nul
    echo         KenshiMP.Server.exe
)
if exist "%~dp0KenshiMP.MasterServer.exe" (
    copy /Y "%~dp0KenshiMP.MasterServer.exe" "%KENSHI_DIR%\KenshiMP.MasterServer.exe" >nul
    echo         KenshiMP.MasterServer.exe
)
if exist "%~dp0server.json" (
    if not exist "%KENSHI_DIR%\server.json" (
        copy /Y "%~dp0server.json" "%KENSHI_DIR%\server.json" >nul
        echo         server.json (default config)
    ) else (
        echo         server.json already exists, leaving as-is
    )
)

echo  [7/7] Writing default client config...
:: Default client config — points at localhost so a self-host setup
:: (run KenshiMP.Server.exe on the same machine) "just works."
:: Edit %APPDATA%\KenshiMP\client.json afterwards to point elsewhere.
set "CFG_DIR=%APPDATA%\KenshiMP"
if not exist "%CFG_DIR%" mkdir "%CFG_DIR%"
if not exist "%CFG_DIR%\client.json" (
    > "%CFG_DIR%\client.json" (
        echo {
        echo   "autoConnect": true,
        echo   "favoriteServers": ["127.0.0.1:27800"],
        echo   "lastPort": 27800,
        echo   "lastServer": "127.0.0.1",
        echo   "masterPort": 27801,
        echo   "masterServer": "127.0.0.1",
        echo   "overlayScale": 1.0,
        echo   "playerName": "Player",
        echo   "useSyncOrchestrator": false
        echo }
    )
    echo         Created %CFG_DIR%\client.json
) else (
    echo         %CFG_DIR%\client.json already exists, leaving as-is
)

echo.
echo  ============================================
echo   Installation complete.
echo  ============================================
echo.
echo   ALPHA STATUS — please read:
echo.
echo   What works:
echo    - Mod loads, in-game F1 menu, MULTIPLAYER button
echo    - Server connect, name/ping display, chat HUD
echo    - Auto-connect on save load
echo.
echo   What doesn't yet:
echo    - Kenshi can terminate when the first NPC spawns
echo      after connecting. Reproducible across builds.
echo      Tracked in KNOWN_ISSUES.md on the fork.
echo.
echo   TO HOST + JOIN LOCALLY (recommended first test):
echo    1. Run KenshiMP.Server.exe ^(stays in console window^)
echo    2. Launch Kenshi from Steam
echo    3. New Game ^> pick the "Singleplayer" start
echo    4. Auto-connect fires ~2s after world loads
echo.
echo   TO PLAY WITH ANOTHER PERSON:
echo    The host runs KenshiMP.Server.exe and forwards
echo    UDP port 27800. Joiner edits
echo    %APPDATA%\KenshiMP\client.json -> set
echo    "lastServer" to the host's public IP.
echo.
echo   Backup of original files: %BACKUP_DIR%
echo   Uninstall: run uninstall.bat
echo.
pause
