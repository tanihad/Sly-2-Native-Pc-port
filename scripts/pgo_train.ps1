# (P6) Profile-guided optimisation: train an instrumented build on chaos-benchmark scenes and merge the profile
# (notes/perf-plan.md "P6 results").
#   1. .venv\Scripts\python.exe scripts\build_game.py --release --lto thin --pgo-gen --out build\pgogen
#   2. powershell -ExecutionPolicy Bypass -File scripts\pgo_train.ps1 [-Exe build\pgogen\sly2.exe] [-Scenes a,b] [-Ship]
#   3. .venv\Scripts\python.exe scripts\build_game.py --release [--pgo-use build\pgo\sly2.profdata]
# The instrumented game writes <Out dir>\raw\sly2-<pid>.profraw when its watchdog ends it (runtime/launch.h pgo_flush);
# llvm-profdata merges the runs into -Out (everything, dev only). -Ship also writes runtime\pgo\sly2.proftext, the
# profile release builds use by default: our own code only (runtime, runtime\gpu, runtime\r2) - the translated game,
# IOP and VU functions (f_*, iop_*, vuc_*, the generated files' statics) are left out, so it holds no names, hashes or
# counts of the game's code. Default scenes: hubs and bosses outside the P6 benchmark set (ep1 hub, m1 brawl, m2
# boardroom), so the benchmark measures the profile on scenes it was not trained on. One game at a time.
param([string]$Exe = "build\pgogen\sly2.exe", [string[]]$Scenes = @("ep3_hub", "ep5_hub", "m3_boss", "m5_boss", "m8_boss"),
      [string]$Out = "build\pgo\sly2.profdata", [int]$Secs = 0, [switch]$Ship)
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
$files = @(Get-ChildItem $raw -Filter *.profraw | Where-Object { $_.Length -gt 0 })
if (-not $files.Count) { "no .profraw written (an instrumented build? see build\chaos\bench\*_train_*\run.log)"; exit 1 }
$prof = Join-Path $MainRoot 'tools\llvm-mingw\bin\llvm-profdata.exe'
& $prof merge -sparse -o $Out @($files | ForEach-Object { $_.FullName })
if ($LASTEXITCODE) { exit 1 }
"{0}: {1} runs merged, {2:N1} MB" -f $Out, $files.Count, ((Get-Item $Out).Length / 1MB)
if ($Ship) {
    $ship = Join-Path $root 'runtime\pgo\sly2.proftext'
    New-Item -ItemType Directory -Force (Split-Path $ship) | Out-Null
    $game = '^(f_[0-9A-F]{8}$|iop_|vuc_|funcs_\d+\.c;|iop_funcs_\d+\.c;|func_table\.c;|iop_table\.c;|vuc_\w+\.c;)'
    & $prof merge -sparse --text "--no-function=$game" -o $ship $Out
    if ($LASTEXITCODE) { exit 1 }
    $left = @(Select-String -Path $ship -Pattern '^(f_[0-9A-F]{8}|iop_|vuc_|funcs_)' | Select-Object -First 1)
    if ($left.Count) { "runtime\pgo\sly2.proftext still names translated code: $($left[0].Line)"; exit 1 }
    "{0}: {1:N2} MB (our code only)" -f $ship, ((Get-Item $ship).Length / 1MB)
}
