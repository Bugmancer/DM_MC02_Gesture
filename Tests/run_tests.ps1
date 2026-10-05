param([string]$GccPath = 'gcc', [string]$PythonPath = 'python')
$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot
if (-not (Get-Command $GccPath -ErrorAction SilentlyContinue)) {
    $GccPath = 'D:\c++\MinGW64\bin\gcc.exe'
}
if (-not (Get-Command $PythonPath -ErrorAction SilentlyContinue)) {
    $PythonPath = 'C:\Users\Acer\miniforge3\python.exe'
}
function Invoke-CTest([string]$Name, [string[]]$Includes, [string[]]$Sources) {
    $outputPath = Join-Path $env:TEMP ('dm_mc02_' + $Name + '_tests.exe')
    $arguments = @('-std=c99','-O2','-Wall','-Wextra','-Werror')
    foreach ($include in $Includes) { $arguments += '-I' + (Join-Path $projectDirectory $include) }
    foreach ($source in $Sources) { $arguments += Join-Path $projectDirectory $source }
    & $GccPath @arguments -lm -o $outputPath
    if ($LASTEXITCODE -ne 0) { throw "$Name compilation failed." }
    & $outputPath
    if ($LASTEXITCODE -ne 0) { throw "$Name tests failed." }
}
Invoke-CTest 'engine' @('Algorithm') @('Algorithm/gesture_engine.c','Tests/test_gesture_engine.c')
Invoke-CTest 'config' @('App','Algorithm') @('App/gesture_config.c','Algorithm/gesture_engine.c','Tests/test_gesture_config.c')
Invoke-CTest 'store' @('Tests/board_mock','Board') @('Board/board_store.c','Tests/board_store_test.c')
Invoke-CTest 'display' @('Display/tests') @('Display/tests/test_display.c')
Invoke-CTest 'keys' @('Tests/key_mock','Board') @('Tests/test_gesture_keys.c')
Invoke-CTest 'app' @('Tests/app_mock','App','Algorithm','Board','Display') @('Tests/test_gesture_app.c','App/gesture_config.c','Algorithm/gesture_engine.c')
Invoke-CTest 'usb' @('Tests/app_mock','App','USB_DEVICE/App') @('Tests/test_gesture_usb.c')
& $PythonPath -m unittest discover -s (Join-Path $projectDirectory 'Tests') -p 'test_gesture_host.py' -v
if ($LASTEXITCODE -ne 0) { throw 'Host protocol tests failed.' }
$guiPython = Join-Path $projectDirectory 'GUI\.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $guiPython)) { $guiPython = $PythonPath }
& $guiPython -m unittest discover -s (Join-Path $projectDirectory 'GUI') -p 'test_*.py' -v
if ($LASTEXITCODE -ne 0) { throw 'GUI backend and HTTP tests failed.' }
Write-Output 'All host checks passed. These do not replace board validation.'
