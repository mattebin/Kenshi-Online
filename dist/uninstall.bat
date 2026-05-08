@echo off
setlocal enabledelayedexpansion
title KenshiMP Uninstaller
color 0C

echo.
echo  ============================================
echo   Kenshi-Online / KenshiMP Uninstaller
echo  ============================================
echo.

set "KENSHI_DIR="

if exist "%~dp0kenshi_x64.exe"    ( set "KENSHI_DIR=%~dp0"    & goto :found )
if exist "%~dp0..\kenshi_x64.exe" ( set "KENSHI_DIR=%~dp0..\" & goto :found )

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
        goto :found
    )
)

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
                goto :found
            )
        )
    )
)

echo  Could not find Kenshi. Enter the full path containing kenshi_x64.exe:
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

set "BACKUP_DIR="
for /f "delims=" %%D in ('dir /B /AD /O-D "%KENSHI_DIR%\KenshiMP_backup*" 2^>nul') do (
    if not defined BACKUP_DIR set "BACKUP_DIR=%KENSHI_DIR%\%%D"
)

echo  Removing KenshiMP files...
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
    if exist "%KENSHI_DIR%\%%~F" (
        del /F "%KENSHI_DIR%\%%~F" >nul
        echo         %%~F
    )
)

for %%F in (
    "Kenshi_MultiplayerPanel.layout"
    "Kenshi_MultiplayerHUD.layout"
) do (
    if exist "%KENSHI_DIR%\data\gui\layout\%%~F" (
        del /F "%KENSHI_DIR%\data\gui\layout\%%~F" >nul
        echo         data\gui\layout\%%~F
    )
)

if exist "%KENSHI_DIR%\data\kenshi-online.mod" (
    del /F "%KENSHI_DIR%\data\kenshi-online.mod" >nul
    echo         data\kenshi-online.mod
)
if exist "%KENSHI_DIR%\mods\kenshi-online" (
    rmdir /S /Q "%KENSHI_DIR%\mods\kenshi-online" >nul
    echo         mods\kenshi-online
)

echo.
if defined BACKUP_DIR (
    echo  Restoring originals from: %BACKUP_DIR%
    if exist "%BACKUP_DIR%\Plugins_x64.cfg.bak" (
        copy /Y "%BACKUP_DIR%\Plugins_x64.cfg.bak" "%KENSHI_DIR%\Plugins_x64.cfg" >nul
        echo         Plugins_x64.cfg
    )
    if exist "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" (
        copy /Y "%BACKUP_DIR%\Kenshi_MainMenu.layout.bak" "%KENSHI_DIR%\data\gui\layout\Kenshi_MainMenu.layout" >nul
        echo         Kenshi_MainMenu.layout
    )
    if exist "%BACKUP_DIR%\__mods.list.bak" (
        copy /Y "%BACKUP_DIR%\__mods.list.bak" "%KENSHI_DIR%\data\__mods.list" >nul
        echo         __mods.list
    )
) else (
    echo  [INFO] No KenshiMP backup folder found. Cleaning plugin/mod lines directly.
    set "CFG=%KENSHI_DIR%\Plugins_x64.cfg"
    if exist "%CFG%" powershell -NoProfile -ExecutionPolicy Bypass -Command "$cfg=$env:CFG; $lines=Get-Content -LiteralPath $cfg; $lines=@($lines | Where-Object { $_ -notmatch '^Plugin=KenshiMP\.(Core|SafeAddon)\s*$' }); Set-Content -LiteralPath $cfg -Value $lines -Encoding ASCII"
    set "MODS=%KENSHI_DIR%\data\__mods.list"
    if exist "%MODS%" powershell -NoProfile -ExecutionPolicy Bypass -Command "$mods=$env:MODS; $lines=Get-Content -LiteralPath $mods; $lines=@($lines | Where-Object { $_ -ine 'kenshi-online' }); Set-Content -LiteralPath $mods -Value $lines -Encoding ASCII"
)

echo.
set "CLIENT_CFG_DIR=%APPDATA%\KenshiMP"
if exist "%CLIENT_CFG_DIR%" (
    set /p "DELCONFIG=Delete client config at %CLIENT_CFG_DIR%? [y/N]: "
    if /i "!DELCONFIG!"=="y" (
        rmdir /S /Q "%CLIENT_CFG_DIR%" >nul
        echo         removed client config
    )
)

echo.
echo  ============================================
echo   Uninstall complete.
echo  ============================================
echo.
pause
