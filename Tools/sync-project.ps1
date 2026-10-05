param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$projectFile = Join-Path $projectRoot 'MDK-ARM\DM_MC02_Gesture.uvprojx'
$doc = [System.Xml.XmlDocument]::new()
$doc.PreserveWhitespace = $false
$doc.Load($projectFile)
$target = $doc.SelectSingleNode('/Project/Targets/Target')
$common = $target.SelectSingleNode('TargetOption/TargetCommonOption')
# The IDE and command-line build must flash the same artifact.
$common.SelectSingleNode('OutputDirectory').InnerText = '.\Build\'
$common.SelectSingleNode('ListingPath').InnerText = '.\Build\'
$common.SelectSingleNode('CreateHexFile').InnerText = '1'
$includes = $target.SelectSingleNode('TargetOption/TargetArmAds/Cads/VariousControls/IncludePath')
$paths = @($includes.InnerText.Split(';') | Where-Object { $_ })
foreach ($relative in @('../App', '../Algorithm', '../Board', '../Display')) {
    if ($paths -notcontains $relative) { $paths += $relative }
}
$includes.InnerText = $paths -join ';'
$linker = $target.SelectSingleNode('TargetOption/TargetArmAds/LDads')
$linker.SelectSingleNode('umfTarg').InnerText = '0'
$linker.SelectSingleNode('useFile').InnerText = '1'
$linker.SelectSingleNode('ScatterFile').InnerText = '.\gesture.sct'
$groups = $target.SelectSingleNode('Groups')
foreach ($folder in @('App','Algorithm','Board','Display')) {
    $groupName = 'Gesture/' + $folder
    $group = $groups.SelectSingleNode("Group[GroupName='$groupName']")
    if (-not $group) {
        $group = $doc.CreateElement('Group')
        $name = $doc.CreateElement('GroupName')
        $name.InnerText = $groupName
        [void]$group.AppendChild($name)
        [void]$group.AppendChild($doc.CreateElement('Files'))
        [void]$groups.AppendChild($group)
    }
    $files = $group.SelectSingleNode('Files')
    foreach ($source in Get-ChildItem -LiteralPath (Join-Path $projectRoot $folder) -Filter '*.c') {
        $relative = '../' + $folder + '/' + $source.Name
        if (-not $files.SelectSingleNode("File[FilePath='$relative']")) {
            $file = $doc.CreateElement('File')
            foreach ($pair in @(@('FileName',$source.Name), @('FileType','1'), @('FilePath',$relative))) {
                $node = $doc.CreateElement($pair[0])
                $node.InnerText = $pair[1]
                [void]$file.AppendChild($node)
            }
            [void]$files.AppendChild($file)
        }
    }
}
# uVision requires conventional line-separated XML without a UTF-8 BOM.
$settings = [System.Xml.XmlWriterSettings]::new()
$settings.Encoding = [System.Text.UTF8Encoding]::new($false)
$settings.Indent = $true
$settings.IndentChars = '  '
$settings.NewLineChars = "`r`n"
$writer = [System.Xml.XmlWriter]::Create($projectFile, $settings)
try { $doc.Save($writer) }
finally { $writer.Dispose() }
Write-Output 'Keil source groups, includes, AXI DMA scatter file and Build output synchronized.'
