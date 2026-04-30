param([string]$FilePath, [string]$NewIP)
$enc = [System.Text.UTF8Encoding]::new($false)
$txt = [System.IO.File]::ReadAllText($FilePath, $enc)
$txt = $txt -replace 'websocket: ws://[0-9.]+:8010', "websocket: ws://${NewIP}:8010"
[System.IO.File]::WriteAllText($FilePath, $txt, $enc)
Write-Host "  [OK] websocket IP -> $NewIP"
