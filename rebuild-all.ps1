$v = "BoltOS"
$d = "C:\Users\adria\Downloads\boltOS"
$vdi = "$d\iso\boltos.vdi"
$img = "$d\iso\os.img"
$pad = "$d\iso\os_padded.img"
$vb = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"

# Step 1: Build OS (compiles C/asm, links, creates os.img + boltos.iso)
bash $d/build.sh

# Step 2: Detach old VDI from VM
& $vb storageattach $v --storagectl IDE --port 0 --device 0 --type hdd --medium none

# Step 3: Remove old VDI from VirtualBox registry + disk
& $vb closemedium disk $vdi 2>$null
Remove-Item $vdi -Force -ErrorAction SilentlyContinue

# Step 4: Pad os.img to at least 1 MB (VDI minimum) and convert
Copy-Item $img $pad
$f = New-Object System.IO.FileStream($pad, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite)
if ($f.Length -lt 1048576) { $f.SetLength(1048576) }
$f.Close()
& $vb convertfromraw $pad $vdi --format VDI
Remove-Item $pad

# Step 5: Attach new VDI to VM
& $vb storageattach $v --storagectl IDE --port 0 --device 0 --type hdd --medium $vdi

Write-Host "Done. Run: & '$vb' startvm $v"
