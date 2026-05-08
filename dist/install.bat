@echo off
setlocal enabledelayedexpansion
title KenshiMP Installer
color 0A

echo.
echo  ============================================
echo   Kenshi-Online / KenshiMP Installer
echo   Alpha multiplayer build for Kenshi 1.0.68
echo  ============================================
echo.

set "KENSHI_DIR="

rem Local checks first, so the package can be extracted into the game folder.
if exist "%~dp0kenshi_x64.exe"    ( set "KENSHI_DIR=%~dp0"    & goto :found_kenshi )
if exist "%~dp0..\kenshi_x64.exe" ( set "KENSHI_DIR=%~dp0..\" & goto :found_kenshi )

rem Common Steam/GOG locations.
for %%P in (
    "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
    "C:\Program Files\Steam\steamapps\common\Kenshi"
    "C:\SteamLibrary\Steam\steamapps\common\Kenshi"
    "C:\SteamLibrary\steamapps\common\Kenshi"
    "D:\SteamLibrary\Steam\steamapps\common\Kenshi"
    "D:\SteamLibrary\steamapps\common\Kenshi"
    "E:\SteamLibrary\Steam\steamapps\common\Kenshi"
    "E:\SteamLibrary\steamapps\common\Kenshi"
    "C:\GOG Games\Kenshi"
) do (
    if exist "%%~P\kenshi_x64.exe" (
        set "KENSHI_DIR=%%~P"
        goto :found_kenshi
    )
)

rem Parse Steam libraryfolders.vdf for non-default libraries.
for %%V in (
    "C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf"
    "C:\Program Files\Steam\steamapps\libraryfolders.vdf"
) do (
    if exist "%%~V" (
        for /f "tokens=2 delims=^"" %%A in ('findstr /C:"\"path\"" "%%~V" 2^>nul') do (
            set "CAND=%%A\steamapps\common\Kenshi"
            set "CAND=!CAND:\\=\!"
            if exist "!CAND!\kenshi_x64.exe" (
                set "KENSHI_DIR=!CAND!"
                goto :found_kenshi
            )
        )
    )
)

echo  Could not auto-detect Kenshi.
echo  Enter the full path to the folder containing kenshi_x64.exe:
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

tasklist /FI "IMAGENAME eq kenshi_x64.exe" 2>NUL | find /I "kenshi_x64.exe" >NUL
if %errorlevel% equ 0 (
    echo  [ERROR] Kenshi is currently running. Close it before installing.
    pause
    exit /b 1
)

for /f "delims=" %%T in ('powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Date -Format yyyyMMdd-HHmmss" 2^>nul') do set "STAMP=%%T"
if not defined STAMP set "STAMP=manual"
set "BACKUP_DIR=%KENSHI_DIR%\KenshiMP_backup_%STAMP%"
mkdir "%BACKUP_DIR%" 2>nul

echo  [1/9] Backing up files that may be changed...
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
for %%F in ("%KENSHI_DIR%\KenshiMP.*.dll" "%KENSHI_DIR%\KenshiMP.*.exe" "%KENSHI_DIR%\KenshiMP.*.bat") do (
    if exist "%%~F" copy /Y "%%~F" "%BACKUP_DIR%\%%~nxF.bak" >nul
)

echo  [2/9] Installing core plugin and tools...
if not exist "%~dp0KenshiMP.Core.dll" (
    echo  [ERROR] KenshiMP.Core.dll not found in installer folder.
    pause
    exit /b 1
)

for %%F in (
    "KenshiMP.Core.dll"
    "KenshiMP.SafeAddon.dll"
    "KenshiMP.Dashboard.exe"
    "KenshiMP.Server.exe"
    "KenshiMP.MasterServer.exe"
    "KenshiMP.Injector.exe"
    "KenshiMP.TestClient.exe"
    "KenshiMP.LogTail.exe"
    "KenshiMP.Cartographer.exe"
    "KenshiMP.CrashWatchdog.exe"
    "KenshiMP.Probe.exe"
    "KenshiMP.Restore.bat"
    "KenshiMP.SwitchAddon.bat"
) do (
    if exist "%~dp0%%~F" (
        set "SRC=%~dp0%%~F"
        set "DST=%KENSHI_DIR%\%%~F"
        if /I "!SRC!"=="!DST!" (
            echo         %%~F already in place
        ) else (
            copy /Y "!SRC!" "!DST!" >nul
            if errorlevel 1 (
                echo  [ERROR] Could not copy %%~F. Close Kenshi/KenshiMP tools and retry.
                pause
                exit /b 1
            )
            echo         %%~F
        )
    )
)

echo  [3/9] Enabling KenshiMP.Core in Plugins_x64.cfg...
set "CFG=%KENSHI_DIR%\Plugins_x64.cfg"
if not exist "%CFG%" type nul > "%CFG%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$cfg=$env:CFG; $lines=@(); if(Test-Path -LiteralPath $cfg){ $lines=Get-Content -LiteralPath $cfg }; $lines=@($lines | Where-Object { $_ -notmatch '^Plugin=KenshiMP\.(Core|SafeAddon)\s*$' }); $lines += 'Plugin=KenshiMP.Core'; Set-Content -LiteralPath $cfg -Value $lines -Encoding ASCII"
if errorlevel 1 (
    echo  [ERROR] Failed to patch Plugins_x64.cfg
    pause
    exit /b 1
)
echo         Plugin=KenshiMP.Core

echo  [4/9] Installing UI layouts...
if not exist "%KENSHI_DIR%\data\gui\layout" mkdir "%KENSHI_DIR%\data\gui\layout"
for %%F in (
    "Kenshi_MainMenu.layout"
    "Kenshi_MultiplayerPanel.layout"
    "Kenshi_MultiplayerHUD.layout"
) do (
    if exist "%~dp0%%~F" (
        copy /Y "%~dp0%%~F" "%KENSHI_DIR%\data\gui\layout\%%~F" >nul
        echo         %%~F
    )
)

echo  [5/9] Installing and enabling kenshi-online.mod...
if exist "%~dp0kenshi-online.mod" (
    if not exist "%KENSHI_DIR%\data" mkdir "%KENSHI_DIR%\data"
    if not exist "%KENSHI_DIR%\mods\kenshi-online" mkdir "%KENSHI_DIR%\mods\kenshi-online"
    copy /Y "%~dp0kenshi-online.mod" "%KENSHI_DIR%\data\kenshi-online.mod" >nul
    copy /Y "%~dp0kenshi-online.mod" "%KENSHI_DIR%\mods\kenshi-online\kenshi-online.mod" >nul
    if not exist "%KENSHI_DIR%\data\__mods.list" type nul > "%KENSHI_DIR%\data\__mods.list"
    findstr /I /X /C:"kenshi-online" "%KENSHI_DIR%\data\__mods.list" >nul 2>&1
    if errorlevel 1 (
        echo kenshi-online>> "%KENSHI_DIR%\data\__mods.list"
        echo         enabled in data\__mods.list
    ) else (
        echo         already enabled in data\__mods.list
    )
) else (
    echo         [WARN] kenshi-online.mod not found in installer folder.
)

echo  [6/9] Installing bundled shared test save...
set "SAVE_DIR=%LOCALAPPDATA%\kenshi\save"
set "BUNDLED_SAVE=%~dp0saves\123"
if exist "%BUNDLED_SAVE%\quick.save" (
    if not exist "%SAVE_DIR%" mkdir "%SAVE_DIR%"
    if exist "%SAVE_DIR%\123" (
        move /Y "%SAVE_DIR%\123" "%SAVE_DIR%\123.before-kenshimp-%STAMP%" >nul
        echo         backed up existing save 123
    )
    xcopy /E /I /Y "%BUNDLED_SAVE%" "%SAVE_DIR%\123" >nul
    if errorlevel 1 (
        echo  [ERROR] Failed to install bundled save 123.
        pause
        exit /b 1
    )
    echo         %SAVE_DIR%\123
) else (
    echo         bundled save 123 not present, skipping
)

echo  [7/9] Installing default server config...
if exist "%~dp0server.json" (
    if not exist "%KENSHI_DIR%\server.json" (
        copy /Y "%~dp0server.json" "%KENSHI_DIR%\server.json" >nul
        echo         server.json
    ) else (
        echo         server.json already exists, leaving as-is
    )
)

echo  [8/9] Writing default client config...
set "CLIENT_CFG_DIR=%APPDATA%\KenshiMP"
if not exist "%CLIENT_CFG_DIR%" mkdir "%CLIENT_CFG_DIR%"
if not exist "%CLIENT_CFG_DIR%\client.json" (
    > "%CLIENT_CFG_DIR%\client.json" (
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
    echo         %CLIENT_CFG_DIR%\client.json
) else (
    echo         client.json already exists, leaving as-is
)

echo  [9/9] Final checks...
if exist "%KENSHI_DIR%\KenshiMP.Core.dll" ( echo         Core DLL installed )
if exist "%KENSHI_DIR%\KenshiMP.Server.exe" ( echo         Server installed )
if exist "%KENSHI_DIR%\KenshiMP.TestClient.exe" ( echo         Test client installed )

echo.
echo  ============================================
echo   Installation complete.
echo  ============================================
echo.
echo   Read README.md and JOINING.md in this package.
echo.
echo   Quick local test:
echo    1. Run KenshiMP.Server.exe
echo    2. Launch Kenshi from Steam
echo    3. Load Game: 123
echo    4. Join 127.0.0.1:27800 from the multiplayer menu
echo.
echo   Alpha warning:
echo    - Passive fake-client relay is safe for testing.
echo    - Real remote player spawning is experimental and may crash.
echo.
echo   Backups: %BACKUP_DIR%
echo   Uninstall: run uninstall.bat from this package.
echo.
pause
