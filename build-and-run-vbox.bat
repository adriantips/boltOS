@echo off
REM ===========================================================================
REM BoltOS: Build + VirtualBox setup + run
REM ===========================================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set VBOX="C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
set VMNAME=BoltOS

REM Check VirtualBox installed
if not exist %VBOX% (
    echo ERROR: VirtualBox not found at "C:\Program Files\Oracle\VirtualBox"
    exit /b 1
)

REM Find bash (Git Bash or msys64)
set BASH=
if exist "C:\Program Files\Git\bin\bash.exe" (
    set BASH="C:\Program Files\Git\bin\bash.exe"
) else if exist "C:\msys64\usr\bin\bash.exe" (
    set BASH="C:\msys64\usr\bin\bash.exe"
) else (
    echo ERROR: bash not found. Install Git for Windows or msys64.
    exit /b 1
)

echo [*] Building BoltOS...
cd /d "%ROOT%"
%BASH% -c "./build.sh"
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)
echo [OK] Build complete

REM Delete existing VM to avoid UUID conflicts
echo [*] Setting up VirtualBox VM...
%VBOX% list vms | findstr /i "%VMNAME%" >nul 2>&1
if not errorlevel 1 (
    echo [*] Removing old VM...
    %VBOX% controlvm %VMNAME% poweroff 2>nul
    %VBOX% unregistervm %VMNAME% --delete 2>nul
)

echo [*] Creating %VMNAME%...
%VBOX% createvm --name %VMNAME% --register
if errorlevel 1 (
    echo ERROR: Failed to create VM
    exit /b 1
)

echo [*] Configuring %VMNAME%...
%VBOX% modifyvm %VMNAME% --memory 512 --cpus 4 --pae on --longmode on --ioapic on --chipset piix3 --firmware bios --hwvirtex off --nestedpaging off --vram 128 --graphicscontroller vboxvga --boot1 disk --boot2 dvd --triple-fault-reset on
if errorlevel 1 (
    echo ERROR: Failed to configure VM
    exit /b 1
)

echo [*] Setting up storage...
%VBOX% storagectl %VMNAME% --name IDE --add ide --controller PIIX4 --bootable on
if errorlevel 1 (
    echo ERROR: Failed to add storage controller
    exit /b 1
)

echo [*] Converting to VDI...
set VDI=%ROOT%iso\boltos.vdi
if exist "%VDI%" del "%VDI%"
set PADDED=%ROOT%iso\os_padded.img
copy "%ROOT%iso\os.img" "%PADDED%" >nul
powershell -Command "$f=[System.IO.File]::Open('%PADDED%', [System.IO.FileMode]::Open); $l=$f.Length; if ($l -lt 1048576) { $f.SetLength(1048576) }; $f.Close()" >nul
%VBOX% convertfromraw "%PADDED%" "%VDI%" --format VDI >nul 2>&1
del "%PADDED%" 2>nul
if not exist "%VDI%" (
    echo ERROR: Failed to create VDI
    exit /b 1
)
echo [*] Attaching HDD...
%VBOX% storageattach %VMNAME% --storagectl IDE --port 0 --device 0 --type hdd --medium "%VDI%"
if errorlevel 1 (
    echo ERROR: Failed to attach HDD
    exit /b 1
)

echo [*] Attaching ISO...
%VBOX% storageattach %VMNAME% --storagectl IDE --port 1 --device 0 --type dvddrive --medium "%ROOT%iso\boltos.iso"
if errorlevel 1 (
    echo ERROR: Failed to attach ISO
    exit /b 1
)

echo [*] Configuring serial...
%VBOX% modifyvm %VMNAME% --uart1 0x3F8 4 --uartmode1 server \\.\pipe\boltos

echo [*] Configuring NIC...
%VBOX% modifyvm %VMNAME% --nic1 nat --nictype1 82540EM
if errorlevel 1 (
    echo ERROR: Failed to configure NIC
    exit /b 1
)

echo [OK] VM created and configured

echo [*] Starting %VMNAME%...
%VBOX% startvm %VMNAME%
if errorlevel 1 (
    echo ERROR: Failed to start VM
    exit /b 1
)

echo [OK] VM started
exit /b 0
