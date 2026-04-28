@echo off
setlocal enabledelayedexpansion
title KenshiMP Uninstaller
color 0C

echo.
echo  ============================================
echo   Kenshi-Online (KenshiMP) Uninstaller
echo  ============================================
echo.

:: ── Auto-detect Kenshi directory (same logic as installer) ──
set "KENSHI_DIR="

if exist "%~dp0kenshi_x64.exe"     ( set "KENSHI_DIR=%~dp0"     & goto :found )
if exist "%~dp0..\kenshi_x64.exe"  ( set "KENSHI_DIR=%~dp0..\"  & goto :found )

set "P=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
if exist "%P%\kenshi_x64.exe" ( set "KENSHI_DIR=%P%" & goto :found )

set "P=C:\GOG Games\Kenshi"
if exist "%P%\kenshi_x64.exe" ( set "KENSHI_DIR=%P%" & goto :found )

set "VDF=C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf"
if exist "%VDF%" (
    for /f "tokens=2 delims=^"" %%A in ('findstr /C:"\"path\"" "%VDF%" 2^>nul') do (
        set "CAND=%%A\steamapps\common\Kenshi"
        set "CAND=!CAND:\\=\!"
        if exist "!CAND!\kenshi_x64.exe" (
            set "KENSHI_DIR=!CAND!"
            goto :found
        )
    )
)

echo  Could not find Kenshi. Enter the full path:
set /p "KENSHI_DIR=Path: "
if not exist "%KENSHI_DIR%\kenshi_x64.exe" (
    echo  [ERROR] kenshi_x64.exe not found at: %KENSHI_DIR%
    pause
    exit /b 1
)

:found
if "%KENSHI_DIR:~-1%"=="\" set "KENSHI_DIR=%KENSHI_DIR:~0,-1%"
echo  Found Kenshi at: %KENSHI_DIR%
echo.

tasklist /FI "IMAGENAME eq kenshi_x64.exe" 2>NUL | find /I "kenshi_x64.exe" >NUL
if %errorlevel% equ 0 (
    echo  [ERROR] Kenshi is currently running. Close it first.
    pause
    exit /b 1
)

:: ── Find newest KenshiMP backup folder (timestamped or legacy) ──
set "BACKUP_DIR="
for /f "delims=" %%D in ('dir /B /AD /O-D "%KENSHI_DIR%\KenshiMP_backup*" 2^>nul') do (
    if not defined BACKUP_DIR set "BACKUP_DIR=%KENSHI_DIR%\%%D"
)

echo  Removing KenshiMP installed files...
echo.

if exist "%KENSHI_DIR%\KenshiMP.Core.dll" (
    del /F "%KENSHI_DIR%\KenshiMP.Core.dll" && echo  [OK] KenshiMP.Core.dll
)
if exist "%KENSHI_DIR%\KenshiMP.Server.exe" (
    del /F "%KENSHI_DIR%\KenshiMP.Server.exe" && echo  [OK] KenshiMP.Server.exe
)
if exist "%KENSHI_DIR%\KenshiMP.MasterServer.exe" (
    del /F "%KENSHI_DIR%\KenshiMP.MasterServer.exe" && echo  [OK] KenshiMP.MasterServer.exe
)
if exist "%KENSHI_DIR%\KenshiMP.Injector.exe" (
    del /F "%KENSHI_DIR%\KenshiMP.Injector.exe" && echo  [OK] KenshiMP.Injector.exe
)
if exist "%KENSHI_DIR%\KenshiMP.TestClient.exe" (
    del /F "%KENSHI_DIR%\KenshiMP.TestClient.exe" && echo  [OK] KenshiMP.TestClient.exe
)
if exist "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerPanel.layout" (
    del /F "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerPanel.layout" && echo  [OK] Kenshi_MultiplayerPanel.layout
)
if exist "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerHUD.layout" (
    del /F "%KENSHI_DIR%\data\gui\layout\Kenshi_MultiplayerHUD.layout" && echo  [OK] Kenshi_MultiplayerHUD.layout
)
if exist "%KENSHI_DIR%\data\kenshi-online.mod" (
    del /F "%KENSHI_DIR%\data\kenshi-online.mod" && echo  [OK] data\kenshi-online.mod
)
if exist "%KENSHI_DIR%\mods\kenshi-online" (
    rmdir /S /Q "%KENSHI_DIR%\mods\kenshi-online" && echo  [OK] mods\kenshi-online\
)

echo.
if defined BACKUP_DIR (
    echo  Restoring originals from: %BACKUP_DIR%
    if exist "%BACKUP_DIR%\Plugins_x64.cfg.bak" (
        copy /Y "%BACKUP_DIR%\Plugins_x64.cfg.bak" "%KENSHI_DIR%\Plugins_x64.cfg" >nul && echo  [OK] Plugins_x64.cfg
    )
    if exist "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" (
        copy /Y "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" >nul && echo  [OK] Kenshi_MainMenu.layout
    )
    if exist "%BACKUP_DIR%\__mods.list.bak" (
        copy /Y "%BACKUP_DIR%\__mods.list.bak" "%KENSHI_DIR%\data\__mods.list" >nul && echo  [OK] __mods.list
    )
) else (
    echo  [INFO] No KenshiMP backup folder found. Skipping restores.
    echo         If your Plugins_x64.cfg / mods.list still mentions
    echo         "KenshiMP.Core" or "kenshi-online", remove those lines manually.
)

echo.
set "CONFIG_DIR=%APPDATA%\KenshiMP"
if exist "%CONFIG_DIR%" (
    set /p "DELCONFIG=Delete client config at %CONFIG_DIR%? [y/N]: "
    if /i "!DELCONFIG!"=="y" (
        rmdir /S /Q "%CONFIG_DIR%" && echo  [OK] Removed %CONFIG_DIR%
    )
)

echo.
echo  ============================================
echo   Uninstall complete. Game is back to vanilla.
echo  ============================================
echo.
pause
