@echo off
setlocal EnableExtensions DisableDelayedExpansion
title OpenXR Toolkit PSVR2 v1.1 - Uninstall
echo OpenXR Toolkit PSVR2 v1.1
echo Uninstalling OpenXR API layer...
echo.

fltmc >nul 2>&1
if errorlevel 1 (
    if /i "%~1"=="--elevated" goto elevation_failed
    set "BAT_PATH=%~f0"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $p=Start-Process -FilePath $env:ComSpec -ArgumentList ('/d /c ""' + $env:BAT_PATH + '" --elevated"') -Verb RunAs -Wait -PassThru; exit $p.ExitCode } catch { Write-Host ('Administrator access was not granted: ' + $_.Exception.Message); Read-Host 'Press Enter to close' | Out-Null; exit 1 }"
    goto return_elevated_result
)

if not exist "%~dp0Uninstall-Layer.ps1" (
    echo ERROR: Uninstall-Layer.ps1 was not found next to Uninstall.bat.
    echo Extract the entire ZIP to one permanent folder and try again.
    pause
    exit /b 2
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-Layer.ps1"
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
    echo.
    echo ERROR: Uninstallation failed. PowerShell exit code: %RESULT%.
    echo Check the error above and try again.
    pause
    exit /b %RESULT%
)

echo.
echo Uninstallation completed successfully.
echo Restart SteamVR before launching a game.
pause
exit /b 0

:elevation_failed
echo ERROR: Administrator access is required to uninstall the OpenXR API layer.
pause
exit /b 1

:return_elevated_result
exit /b %ERRORLEVEL%
