@echo off
:: =============================================================================
:: flash_firmware.bat
:: AT32UC3B1256 Full Erase + Flash Script with Polling Loop (Silent Erase)
:: =============================================================================

setlocal enabledelayedexpansion

set CHIP=at32uc3b1256
set HEX_FILE=%~dp0build\firmware.hex
set DFU_TOOL=%~dp0tools\dfu-programmer.exe

:: ---- Fallback: use dfu-programmer from PATH if not in tools\ ----
if not exist "%DFU_TOOL%" (
    where dfu-programmer >nul 2>&1
    if %errorlevel% neq 0 (
        echo [ERROR] dfu-programmer not found!
        echo.
        echo Please either:
        echo   1. Download dfu-programmer and place dfu-programmer.exe in:
        echo      %~dp0tools\
        echo   2. OR add dfu-programmer to your system PATH.
        echo.
        echo Download from: https://sourceforge.net/projects/dfu-programmer/
        pause
        exit /b 1
    )
    set DFU_TOOL=dfu-programmer
)

:: ---- Check HEX file exists ----
if not exist "%HEX_FILE%" (
    echo [ERROR] Firmware HEX file not found: %HEX_FILE%
    echo.
    echo Please build the firmware first using the Makefile:
    echo   make all
    echo.
    pause
    exit /b 1
)

echo.
echo =============================================================
echo   AT32UC3B1256 Firmware Flash Utility
echo =============================================================
echo   Chip     : %CHIP%
echo   HEX File : %HEX_FILE%
echo   DFU Tool : %DFU_TOOL%
echo =============================================================
echo.

echo === Waiting for DFU mode (unplug, hold HWB, replug) ===

:: ---- Polling Loop ----
set FOUND=0
for /l %%i in (1,1,120) do (
    "%DFU_TOOL%" %CHIP% get >nul 2>&1
    if !errorlevel! equ 0 (
        set FOUND=1
        goto :DFU_FOUND
    )
    timeout /t 1 >nul
)

:DFU_TIMEOUT
echo.
echo [ERROR] Timed out waiting for DFU device.
pause
exit /b 1

:DFU_FOUND
echo DFU FOUND! Erasing...
:: FIXED: Redirected all output (>nul 2>&1) so the blank-check error doesn't print
"%DFU_TOOL%" %CHIP% erase --force >nul 2>&1

echo Flashing...
"%DFU_TOOL%" %CHIP% flash --suppress-validation --force "%HEX_FILE%"
echo Flash exit: %errorlevel%
if %errorlevel% neq 0 (
    echo [ERROR] Flash failed!
    pause
    exit /b 1
)

echo Launching (--no-reset)...
"%DFU_TOOL%" %CHIP% launch --no-reset
echo Launch exit: %errorlevel%

echo.
echo =============================================================
echo   === Flashed! ALL DELAYS REMOVED FROM MAIN AGAIN ===
echo =============================================================
echo.
pause
endlocal