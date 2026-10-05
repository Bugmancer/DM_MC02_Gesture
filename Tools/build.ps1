param([string]$CompilerBin = $env:ARMCC_BIN, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'sync-project.ps1')
if (-not $CompilerBin) {
    foreach ($candidate in @('E:\tools\keil\ARM\ARMCC\bin','C:\Keil_v5\ARM\ARMCC\bin')) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'armcc.exe')) { $CompilerBin = $candidate; break }
    }
}
if (-not $CompilerBin) { throw 'Set ARMCC_BIN or pass -CompilerBin pointing to Keil ARMCC 5 bin.' }
foreach ($name in @('armcc','armasm','armlink','fromelf')) {
    if (-not (Test-Path -LiteralPath (Join-Path $CompilerBin "$name.exe"))) { throw "Missing $name.exe" }
}
$project = Join-Path $projectRoot 'MDK-ARM\DM_MC02_Gesture.uvprojx'
[xml]$doc = Get-Content -LiteralPath $project -Raw
$target = $doc.Project.Targets.Target
$projectDir = Split-Path -Parent $project
$outputDir = [IO.Path]::GetFullPath((Join-Path $projectDir $target.TargetOption.TargetCommonOption.OutputDirectory))
[void](New-Item -ItemType Directory -Path $outputDir -Force)
$log = Join-Path $outputDir 'build.log'
$started = [DateTime]::UtcNow.ToString('o')
Set-Content -LiteralPath $log -Value "Full ARMCC build started $started" -Encoding utf8
$common = @('--cpu','Cortex-M7.fp.dp','--c99','-O2','-g','--apcs=interwork','--split_sections')
foreach ($define in $target.TargetOption.TargetArmAds.Cads.VariousControls.Define.Split(',')) {
    if ($define) { $common += '-D' + $define }
}
foreach ($include in $target.TargetOption.TargetArmAds.Cads.VariousControls.IncludePath.Split(';')) {
    if ($include) { $common += '-I' + [IO.Path]::GetFullPath((Join-Path $projectDir $include)) }
}
$objects = @()
foreach ($file in $target.Groups.Group.Files.File) {
    if ([int]$file.FileType -notin @(1,2)) { continue }
    $source = [IO.Path]::GetFullPath((Join-Path $projectDir $file.FilePath))
    $objectName = ($file.FilePath -replace '^\.\./','' -replace '[\\/:.]','_') + '.o'
    $objectPath = Join-Path $outputDir $objectName
    Write-Output ('Compiling ' + $file.FileName)
    if ([int]$file.FileType -eq 2) {
        & (Join-Path $CompilerBin 'armasm.exe') --cpu Cortex-M7.fp.dp -g --apcs=interwork -o $objectPath $source 2>&1 | Tee-Object -FilePath $log -Append
    } else {
        & (Join-Path $CompilerBin 'armcc.exe') @common -c $source -o $objectPath 2>&1 | Tee-Object -FilePath $log -Append
    }
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $source" }
    $objects += $objectPath
}
$axf = Join-Path $outputDir 'DM_MC02_Gesture.axf'
$map = Join-Path $outputDir 'DM_MC02_Gesture.map'
$link = @('--cpu','Cortex-M7.fp.dp','--strict','--scatter',(Join-Path $projectDir 'gesture.sct'),'--map','--symbols','--callgraph','--info','sizes,totals,unused,veneers','--list',$map,'-o',$axf) + $objects
& (Join-Path $CompilerBin 'armlink.exe') @link 2>&1 | Tee-Object -FilePath $log -Append
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
& (Join-Path $CompilerBin 'fromelf.exe') --i32combined --output (Join-Path $outputDir 'DM_MC02_Gesture.hex') $axf 2>&1 | Tee-Object -FilePath $log -Append
if ($LASTEXITCODE -ne 0) { throw 'HEX export failed.' }
& (Join-Path $CompilerBin 'fromelf.exe') --bin --output (Join-Path $outputDir 'DM_MC02_Gesture.bin') $axf 2>&1 | Tee-Object -FilePath $log -Append
if ($LASTEXITCODE -ne 0) { throw 'BIN export failed.' }
Write-Output "Build passed: $axf"
Write-Output ('Flash this HEX: ' + (Join-Path $outputDir 'DM_MC02_Gesture.hex'))
