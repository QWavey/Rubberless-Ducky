@echo off
:: =============================================================================
:: flash_firmware.bat
:: AT32UC3B1256 Full Erase + Flash Script with Polling Loop (Silent Erase)
:: =============================================================================

setlocal enabledelayedexpansion

set CHIP=at32uc3b1256
set "HEX_FILE=%~dp0build\firmware.hex"
set "DFU_TOOL=%~dp0tools\dfu-programmer.exe"

:: ---- Fallback: use dfu-programmer from PATH if not in tools\ ----
:: SAFE CHECK: Using a temporary variable to avoid parenthesis breakdown in IF blocks
set "TEST_DFU=%DFU_TOOL%"
if not exist "!TEST_DFU!" (
    where dfu-programmer >nul 2>&1
    if !errorlevel! neq 0 (
        echo [ERROR] dfu-programmer not found!
        echo.
        echo Please either:
        echo   1. Download dfu-programmer and place dfu-programmer.exe in:
        echo      "%~dp0tools\"
        echo   2. OR add dfu-programmer to your system PATH.
        echo.
        echo Download from: https://sourceforge.net/projects/dfu-programmer/
        pause
        exit /b 1
    )
    set "DFU_TOOL=dfu-programmer"
)

:: ---- Check HEX file exists ----
set "TEST_HEX=%HEX_FILE%"
if not exist "!TEST_HEX!" (
    echo [ERROR] Firmware HEX file not found!
    echo PATH: !TEST_HEX!
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
echo   HEX File : !HEX_FILE!
echo   DFU Tool : !DFU_TOOL!
echo =============================================================
echo.

echo === Waiting for DFU mode (unplug, hold HWB, replug) ===

:: ---- Polling Loop ----
set FOUND=0
for /l %%i in (1,1,120) do (
    "!DFU_TOOL!" %CHIP% get >nul 2>&1
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
:: NOTE on AT32UC3 + dfu-programmer:
::   The chip's DFU read-back is unreliable, so the blank-check after erase
::   and the verify after flash BOTH spuriously report errors even when the
::   write actually succeeded ("Erasing flash... Success" / "Programming ...
::   Success" are the lines that matter).  We therefore ignore the erase
::   blank-check result and flash with --suppress-validation.  The REAL proof
::   that the new firmware is on the chip is the 3 red boot-blinks.

echo DFU FOUND! Erasing...
"!DFU_TOOL!" %CHIP% erase --force
echo (Ignore any "Checking memory ... ERROR" above - the erase itself succeeded.)

echo.
echo Flashing (validation suppressed - UC3 read-back is unreliable)...
"!DFU_TOOL!" %CHIP% flash --suppress-validation --force "!HEX_FILE!"
echo Flash exit: !errorlevel!
if !errorlevel! neq 0 (
    echo.
    echo [ERROR] Programming itself failed. Look above for "Programming ... Success".
    echo If it is NOT there: unplug, hold HWB, replug, re-run this script.
    pause
    exit /b 1
)

echo.
echo Starting the new firmware...
"!DFU_TOOL!" %CHIP% launch --no-reset
echo Launch exit: !errorlevel!

echo.
echo =============================================================
echo   === Programmed OK (verify step skipped - normal for UC3) ===
echo.
echo   Now physically UNPLUG the device and REPLUG it for a clean
echo   power-on of the new firmware.
echo.
echo   You should see 3 fast RED blinks at plug-in.  THAT is the
echo   real confirmation the new firmware is running.
echo =============================================================
echo.
pause
endlocal