@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM aducm355-cv-firmware - GCC ARM build script
REM
REM Requires arm-none-eabi-gcc on PATH. External dependencies
REM are pointed at via environment variables:
REM
REM   AD5940LIB_DIR   — path to ad5940lib/ (contains ad5940.h/.c)
REM   ADUCM355_SDK_DIR — path to EVAL-ADuCM355QSPZ/ root
REM
REM CMSIS headers must be at C:\CMSIS (core_cm3.h etc.)
REM ============================================================

set THISDIR=%~dp0
if "%THISDIR:~-1%"=="\" set "THISDIR=%THISDIR:~0,-1%"

REM --- External dependency roots --------------------------------
if "%AD5940LIB_DIR%"=="" set "AD5940LIB_DIR=%THISDIR%\..\ad5940lib"
if "%ADUCM355_SDK_DIR%"=="" set "ADUCM355_SDK_DIR=%THISDIR%\..\EVAL-ADuCM355QSPZ"

set "ADI_COMMON=%ADUCM355_SDK_DIR%\firmware\aducm355-examples\common"
set "ADI_INC=%ADUCM355_SDK_DIR%\firmware\aducm355-examples\inc"
set "ADI_GCC=%ADUCM355_SDK_DIR%\firmware\aducm355-examples\gcc_min_uart"
set "CMSIS_INC=C:\CMSIS"

set "BUILD=%THISDIR%\build"
if not exist "%BUILD%" mkdir "%BUILD%"

set "CC=arm-none-eabi-gcc"
set "OBJCOPY=arm-none-eabi-objcopy"
set "SIZE=arm-none-eabi-size"

REM --- Common flags for all files --------------------------------
set "CFLAGS_BASE=-mcpu=cortex-m3 -mthumb -Os -g -ffunction-sections -fdata-sections -std=c11"
set "CFLAGS_BASE=%CFLAGS_BASE% -DCHIPSEL_M355"
set "CFLAGS_BASE=%CFLAGS_BASE% -I%THISDIR%\src"
set "CFLAGS_BASE=%CFLAGS_BASE% -I%AD5940LIB_DIR%"
set "CFLAGS_BASE=%CFLAGS_BASE% -I%ADI_COMMON%"
set "CFLAGS_BASE=%CFLAGS_BASE% -I%ADI_INC%"
set "CFLAGS_BASE=%CFLAGS_BASE% -I%CMSIS_INC%"

REM External files: suppress warnings that clutter the output
set "CFLAGS_EXT=%CFLAGS_BASE% -w"

REM Our files: full warnings
set "CFLAGS_OUR=%CFLAGS_BASE% -Wall -Wextra"

REM --- Linker flags ----------------------------------------------
REM -lm must come after object files in the link command to resolve
REM sqrt/atan2 references from ad5940.c. It is appended after %OBJS%.
REM nano.specs + _printf_float uses a function-pointer dispatch that is null
REM on GCC 15.x (iprintf != printf, %f causes a null BLX). Use nosys.specs
REM only (full libc) for correct float printf.  Binary grows ~20 KB but
REM stays comfortably under the 256 KB flash limit.
set "LDFLAGS=-mcpu=cortex-m3 -mthumb --specs=nano.specs --specs=nosys.specs"
set "LDFLAGS=%LDFLAGS% -nostartfiles -Wl,--gc-sections"
set "LDFLAGS=%LDFLAGS% -T%THISDIR%\linker.ld"

REM --- External sources (SDK + ad5940lib + GCC startup) ----------
set "EXT_SRCS="
set "EXT_SRCS=%EXT_SRCS% %ADI_GCC%\startup_adi_gcc.c"
set "EXT_SRCS=%EXT_SRCS% %ADI_COMMON%\system_ADuCM355.c"
set "EXT_SRCS=%EXT_SRCS% %ADI_COMMON%\ClkLib.c"
set "EXT_SRCS=%EXT_SRCS% %ADI_COMMON%\DioLib.c"
set "EXT_SRCS=%EXT_SRCS% %ADI_COMMON%\UrtLib.c"
set "EXT_SRCS=%EXT_SRCS% %ADI_COMMON%\IntLib.c"
set "EXT_SRCS=%EXT_SRCS% %ADI_COMMON%\SpiLib.c"
set "EXT_SRCS=%EXT_SRCS% %ADI_COMMON%\ADuCM355Port.c"
set "EXT_SRCS=%EXT_SRCS% %AD5940LIB_DIR%\ad5940.c"

REM --- Our sources -----------------------------------------------
set "OUR_SRCS="
set "OUR_SRCS=%OUR_SRCS% %THISDIR%\src\main.c"
set "OUR_SRCS=%OUR_SRCS% %THISDIR%\src\cv_sweep.c"
set "OUR_SRCS=%OUR_SRCS% %THISDIR%\src\retarget_uart.c"

REM --- Compile external sources ----------------------------------
set "OBJS="
for %%F in (%EXT_SRCS%) do (
    set "NAME=%%~nF"
    echo [EXT] %%~nxF
    %CC% %CFLAGS_EXT% -c "%%F" -o "%BUILD%\!NAME!.o" || goto :err
    set "OBJS=!OBJS! "%BUILD%\!NAME!.o""
)

REM --- Compile our sources ---------------------------------------
for %%F in (%OUR_SRCS%) do (
    set "NAME=%%~nF"
    echo [OUR] %%~nxF
    %CC% %CFLAGS_OUR% -c "%%F" -o "%BUILD%\!NAME!.o" || goto :err
    set "OBJS=!OBJS! "%BUILD%\!NAME!.o""
)

REM --- Link ------------------------------------------------------
echo [LINK]
%CC% %LDFLAGS% %OBJS% -lm -o "%BUILD%\cv_firmware.elf" || goto :err
%OBJCOPY% -O ihex --gap-fill 0xFF "%BUILD%\cv_firmware.elf" "%BUILD%\cv_firmware.hex" || goto :err
%OBJCOPY% -O binary --gap-fill 0xFF "%BUILD%\cv_firmware.elf" "%BUILD%\cv_firmware.bin" || goto :err
%SIZE% "%BUILD%\cv_firmware.elf"

REM --- Patch page-0 CRC32 ----------------------------------------
REM The ADuCM355 boot ROM verifies a CRC32 over flash page 0 before
REM jumping to ResetISR. The GCC startup file reserves a 0xFFFFFFFF
REM placeholder at 0x7F8; tools\patch_checksum.py computes the CRC
REM over 0x000-0x7F7 and patches both the .bin and the .hex so the
REM two outputs stay consistent.
echo [CRC]
python "%THISDIR%\tools\patch_checksum.py" "%BUILD%\cv_firmware.bin" "%BUILD%\cv_firmware.hex" || goto :err

echo.
echo Build OK: %BUILD%\cv_firmware.{elf,hex,bin}
endlocal
exit /b 0

:err
echo.
echo BUILD FAILED
endlocal
exit /b 1
