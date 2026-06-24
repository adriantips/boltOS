Add-Type -AssemblyName System.Core
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "boltos", [System.IO.Pipes.PipeDirection]::In)
try {
    $pipe.Connect(2000)
    $reader = New-Object System.IO.StreamReader($pipe)
    for ($i = 0; $i -lt 50; $i++) {
        if (-not $pipe.IsConnected) { break }
        $line = $reader.ReadLine()
        if ($line -ne $null) { Write-Output $line }
        else { break }
        Start-Sleep -Milliseconds 100
    }
    $reader.Close()
    $pipe.Close()
} catch {
    Write-Output "ERROR: $_"
}
