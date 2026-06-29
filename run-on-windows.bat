@echo off
REM ===========================================================================
REM BoltOS: Run in QEMU on Windows (with display window + serial output)
REM ===========================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"

if not exist %QEMU% (
    echo ERROR: QEMU not found at C:\Program Files\qemu
    exit /b 1
)

REM Build first (skip if already built)
if not exist "%ROOT%iso\os.img" (
    echo [*] OS image not found, building first...
    call "%ROOT%build-and-run-vbox.bat" 2>nul
    REM Find bash and build
    set BASH=
    if exist "C:\Program Files\Git\bin\bash.exe" (
        set BASH="C:\Program Files\Git\bin\bash.exe"
    ) else if exist "C:\msys64\usr\bin\bash.exe" (
        set BASH="C:\msys64\usr\bin\bash.exe"
    )
    if not "!BASH!"=="" (
        cd /d "%ROOT%"
        !BASH! -c "./build.sh"
    )
)

echo [*] Starting BoltOS in QEMU...
echo [*] Serial output will appear in this window (Ctrl+C to exit)
echo.

REM Use SDL display if available, otherwise fall back to GTK
%QEMU% ^
    -drive id=boot,file="%ROOT%iso\os.img",format=raw,if=none ^
    -device ide-hd,drive=boot,bus=ide.0,unit=0 ^
    -drive id=cd,file="%ROOT%iso\boltos.iso",format=raw,if=none ^
    -device ide-cd,drive=cd,bus=ide.0,unit=1 ^
    -boot order=c ^
    -m 512M ^
    -rtc base=utc ^
    -vga std -global VGA.vgamem_mb=64 ^
    -display sdl ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0 ^
    -audiodev none,id=snd0 -device AC97,audiodev=snd0 ^
    -serial stdio ^
    -no-reboot -no-shutdown

if errorlevel 1 (
    echo.
    echo [*] SDL display not available, trying GTK...
    %QEMU% ^
        -drive id=boot,file="%ROOT%iso\os.img",format=raw,if=none ^
        -device ide-hd,drive=boot,bus=ide.0,unit=0 ^
        -drive id=cd,file="%ROOT%iso\boltos.iso",format=raw,if=none ^
        -device ide-cd,drive=cd,bus=ide.0,unit=1 ^
        -boot order=c ^
        -m 512M ^
        -rtc base=utc ^
        -vga std -global VGA.vgamem_mb=64 ^
        -display gtk ^
        -netdev user,id=net0 ^
        -device e1000,netdev=net0 ^
        -audiodev none,id=snd0 -device AC97,audiodev=snd0 ^
    -audiodev none,id=snd0 -device AC97,audiodev=snd0 ^
        -serial stdio ^
        -no-reboot -no-shutdown
)

echo.
echo [*] QEMU exited.
pause
