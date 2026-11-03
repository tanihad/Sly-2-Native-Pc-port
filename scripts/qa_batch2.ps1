param([string[]]$Scenes, [string[]]$Configs, [int]$Fps = 60, [string]$Out = 'build\qa\batch2.txt', [string]$Ini = '', [string]$Tag = '', [int]$Frames = 60, [string]$Extra = '')
$Scenes = @($Scenes | % { $_ -split ',' } | ? { $_ }); $Configs = @($Configs | % { $_ -split ',' } | ? { $_ })
$root = Split-Path $PSScriptRoot -Parent
foreach ($s in $Scenes) { foreach ($c in $Configs) {
    $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'qa_scene.ps1'),'-Scene',$s,'-Config',$c,'-Fps',"$Fps",'-Frames',"$Frames")
    if ($Ini) { $a += @('-Ini',$Ini) }; if ($Tag) { $a += @('-Tag',$Tag) }; if ($Extra) { $a += @('-Extra',$Extra) }
    $r = & powershell @a
    $r | Add-Content (Join-Path $root $Out)
} }
