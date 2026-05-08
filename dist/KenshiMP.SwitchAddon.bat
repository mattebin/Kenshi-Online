@echo off
setlocal enabledelayedexpansion

set "CFG=%~dp0Plugins_x64.cfg"
if not exist "%CFG%" (
    echo [ERROR] Plugins_x64.cfg not found next to this script.
    pause
    exit /b 1
)

if not exist "%CFG%.before-switch" copy /Y "%CFG%" "%CFG%.before-switch" >nul

set "MODE=unknown"
findstr /B /L /C:"Plugin=KenshiMP.Core" "%CFG%" >nul && set "MODE=core"
findstr /B /L /C:"Plugin=KenshiMP.SafeAddon" "%CFG%" >nul && set "MODE=safe"

if "%MODE%"=="core" (
    echo [SWITCH] KenshiMP.Core -^> KenshiMP.SafeAddon
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$cfg=$env:CFG; (Get-Content -LiteralPath $cfg) -replace '^Plugin=KenshiMP\.Core\s*$','Plugin=KenshiMP.SafeAddon' | Set-Content -LiteralPath $cfg -Encoding ASCII"
    set "MODE=safe"
) else if "%MODE%"=="safe" (
    echo [SWITCH] KenshiMP.SafeAddon -^> KenshiMP.Core
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$cfg=$env:CFG; (Get-Content -LiteralPath $cfg) -replace '^Plugin=KenshiMP\.SafeAddon\s*$','Plugin=KenshiMP.Core' | Set-Content -LiteralPath $cfg -Encoding ASCII"
    set "MODE=core"
) else (
    echo [ERROR] Could not detect a KenshiMP plugin line in Plugins_x64.cfg.
    echo Add Plugin=KenshiMP.Core manually, then rerun this script.
    pause
    exit /b 1
)

echo.
echo Current mode: %MODE%
echo Plugin file: %CFG%
echo.
pause
