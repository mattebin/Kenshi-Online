@echo off
setlocal enabledelayedexpansion

echo.
echo  KenshiMP restore helper
echo  Restores the newest .before-* backup over each live KenshiMP binary.
echo.

set "DIR=%~dp0"
set RESTORED=0

for %%F in ("%DIR%KenshiMP.*.dll" "%DIR%KenshiMP.*.exe") do (
    set "LIVE=%%~F"
    set "BASE=%%~nxF"
    set "NEWEST="
    for /f "delims=" %%G in ('dir /b /od "%DIR%!BASE!.before-*" 2^>nul') do (
        set "NEWEST=%DIR%%%G"
    )
    if defined NEWEST (
        echo  - !BASE!
        echo      ^<- !NEWEST!
        copy /Y "!NEWEST!" "!LIVE!" >nul
        set /a RESTORED+=1
    ) else (
        echo  - !BASE! (no backup found, skipping)
    )
)

echo.
echo  Done. %RESTORED% file(s) restored.
echo  If the game is still broken, run uninstall.bat or verify Kenshi files in Steam.
echo.
pause
