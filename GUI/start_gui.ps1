param([switch]$Stop, [switch]$NoBrowser)
$ErrorActionPreference = 'Stop'
$guiRoot = $PSScriptRoot
$metadataPath = Join-Path $guiRoot 'data\server.json'
$pythonPath = Join-Path $guiRoot '.venv\Scripts\python.exe'

function Find-RunningGui {
    if (Test-Path -LiteralPath $metadataPath) {
        try {
            $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
            if ($metadata.url -notmatch '^http://127\.0\.0\.1:\d+$') { return $null }
            $health = Invoke-RestMethod -Uri ($metadata.url + '/api/health') -TimeoutSec 2
            if ($health.app -eq 'dm-mc02-gesture-gui' -and $health.pid -eq $metadata.pid) { return $metadata.url }
        } catch { }
    }
    return $null
}

$url = Find-RunningGui
if ($Stop) {
    if ($url) {
        $session = Invoke-RestMethod -Uri ($url + '/api/session.json')
        Invoke-RestMethod -Uri ($url + '/api/shutdown') -Method Post -ContentType 'application/json' -Body '{}' -Headers @{'X-Gesture-Token'=$session.token} | Out-Null
        Write-Output 'Gesture GUI stopped.'
    } else { Write-Output 'Gesture GUI is not running.' }
    exit 0
}

if (-not $url) {
    if (-not (Test-Path -LiteralPath $pythonPath)) {
        $candidates = @('python.exe', (Join-Path $env:USERPROFILE 'miniforge3\python.exe'), (Join-Path $env:USERPROFILE 'AppData\Local\Programs\Python\Python313\python.exe'))
        $bootstrap = $null
        foreach ($candidate in $candidates) {
            $found = Get-Command $candidate -ErrorAction SilentlyContinue
            if ($found -and $found.Source -notlike '*\WindowsApps\*') {
                & $found.Source -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
                if ($LASTEXITCODE -eq 0) { $bootstrap = $found.Source; break }
            }
        }
        if (-not $bootstrap) { throw 'Python 3.10+ was not found. Install Python and run this launcher again.' }
        & $bootstrap -m venv (Join-Path $guiRoot '.venv')
        if ($LASTEXITCODE -ne 0) { throw 'Could not create the local Python environment.' }
    }
    & $pythonPath -c "import importlib.util,sys; sys.exit(0 if all(importlib.util.find_spec(name) for name in ['serial','numpy','imufusion']) else 1)"
    if ($LASTEXITCODE -ne 0) {
        & $pythonPath -m pip install -r (Join-Path $guiRoot 'requirements.txt')
        if ($LASTEXITCODE -ne 0) { throw 'Could not install GUI dependencies. Check the network and retry.' }
    }
    $pythonWindowless = Join-Path $guiRoot '.venv\Scripts\pythonw.exe'
    $process = Start-Process -FilePath $pythonWindowless -ArgumentList @(('"{0}"' -f (Join-Path $guiRoot 'server.py'))) -WorkingDirectory $guiRoot -WindowStyle Hidden -PassThru
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        Start-Sleep -Milliseconds 250
        $url = Find-RunningGui
        if ($url) { break }
        $process.Refresh()
        if ($process.HasExited) { throw 'The local server stopped. See GUI\data\gui.log.' }
    }
    if (-not $url) { throw 'The local server did not become ready. See GUI\data\gui.log.' }
}
Write-Output $url
if (-not $NoBrowser) { Start-Process $url }
