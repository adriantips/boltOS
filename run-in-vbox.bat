@echo off
REM Start BoltOS in VirtualBox (GUI mode)
REM The OS outputs to screen and serial COM1.
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm BoltOS
if errorlevel 1 (
    echo.
    echo VM not found. Register it first from the boltOS directory:
    echo   cd /d "%~dp0"
    echo   "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" createvm --name BoltOS --register
    echo   "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm BoltOS --memory 24576 --cpus 4 --pae on --longmode on --ioapic on --chipset piix3 --firmware bios --hwvirtex on
    echo   "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" storagectl BoltOS --name IDE --add ide --controller PIIX4 --bootable on
    echo   "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" storageattach BoltOS --storagectl IDE --port 0 --device 0 --type hdd --medium "%~dp0iso\boltos.vdi"
    echo   "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" storageattach BoltOS --storagectl IDE --port 1 --device 0 --type dvddrive --medium "%~dp0iso\boltos.iso"
    echo   REM NAT NIC the e1000 driver binds ^(8086:100E^) -- gives the browser internet:
    echo   "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm BoltOS --nic1 nat --nictype1 82540EM
    echo   "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm BoltOS --uart1 0x3F8 4 --uartmode1 server \\.\pipe\boltos
    pause
)
