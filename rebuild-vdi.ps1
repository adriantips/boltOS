$img = "C:\Users\adria\Downloads\boltOS\iso\os.img"
$padded = "C:\Users\adria\Downloads\boltOS\iso\os_padded.img"
$vdi = "C:\Users\adria\Downloads\boltOS\iso\boltos.vdi"
Copy-Item $img $padded
$f = [System.IO.File]::OpenWrite($padded)
$f.SetLength(1048576)
$f.Close()
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" convertfromraw $padded $vdi --format VDI
Remove-Item $padded
