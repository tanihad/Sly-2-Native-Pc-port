# (P6) Profile-guided optimisation: train an instrumented build on chaos-benchmark scenes and merge the profile.
#   1. .venv\Scripts\python.exe scripts\build_game.py --release --lto thin --pgo-gen --out build\pgogen
#   2. powershell -ExecutionPolicy Bypass -File scripts\pgo_train.ps1 [-Exe build\pgogen\sly2.exe] [-Scenes a,b] [-Out f]
#   3. .venv\Scripts\python.exe scripts\build_game.py --release --lto thin --pgo-use build\pgo\sly2.profdata --out build\rel
# The instrumented game writes <Out dir>\raw\sly2-<pid>.profraw when its watchdog ends it (runtime/launch.h pgo_flush);
# llvm-profdata merges the runs. Default scenes: the hubs and bosses that are not in the P6 benchmark set (ep1 hub, m1
# brawl, m2 boardroom), so the benchmark measures the profile on scenes it was not trained on. One game at a time.
param([string]$Exe = "build\pgogen\sly2.exe", [string[]]$Scenes = @("ep3_hub", "ep5_hub", "m3_boss", "m5_boss", "m8_boss"),
      [string]$Out = "build\pgo\sly2.profdata", [int]$Secs = 0)
$Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'project.ps1')
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
$raw = Join-Path (Split-Path $Out) 'raw'
Remove-Item -Recurse -Force $raw -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $raw | Out-Null
$args2 = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'chaos_bench.ps1'), '-Tag', 'pgo_train',
           '-Exes', "train=$Exe", '-Scenes', ($Scenes -join ','), '-Extra', "LLVM_PROFILE_FILE=$raw\sly2-%p.profraw")
if ($Secs -gt 0) { $args2 += @('-Secs', "$Secs") }
& powershell @args2 | Out-Host
$files = @(Get-ChildItem $raw -Filter *.profraw)
if (-not $files.Count) { "no .profraw written (an instrumented build? see build\chaos\bench\*_train_*\run.log)"; exit 1 }
$prof = Join-Path $MainRoot 'tools\llvm-mingw\bin\llvm-profdata.exe'
& $prof merge -sparse -o $Out @($files | ForEach-Object { $_.FullName })
if ($LASTEXITCODE) { exit 1 }
"{0}: {1} runs merged, {2:N1} MB" -f $Out, $files.Count, ((Get-Item $Out).Length / 1MB)
