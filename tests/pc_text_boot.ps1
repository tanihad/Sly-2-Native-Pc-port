# Boot-flow test for the PC wording (runtime/pc_text.c, notes/pc-text.md): the game on a fresh, empty
# save folder must reach the title screen with no pad input at all - no "create a save file?" prompt -
# and leave the save data behind. Headless, about a minute.
#   .\tests\pc_text_boot.ps1 [-Out build\rel] [-Seconds 60]
# Checks: the log says the save data was created without a prompt; the memory-card dialog (0x2E6550
# +0x40, peeked every 15 VBlanks) is never the prompt (0x15); the title screen's START check
# (0x187D80 from 0x1DD968: an entry hook, so any build) is called, i.e. the title accepts input
# (the "@...:none" token waits for it and presses nothing); the folder holds save1..3.
param([string]$Out = "", [int]$Seconds = 60)
$root = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Join-Path $root 'build\rel' }
$exe = Join-Path $Out 'sly2.exe'
if (-not (Test-Path $exe)) { "no $exe (build it first: scripts\build_game.py --release)"; exit 1 }
$dir = Join-Path $root 'build\pc_text_boot'
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $exe (Join-Path $dir 'sly2_pc_text_boot.exe')
$card = Join-Path $dir 'card'

$env:PS2_PEEK = "2E6590:1"
$env:PS2_PEEK_EVERY = "15"
Remove-Item Env:\PS2_PC_TEXT -ErrorAction SilentlyContinue
$run = & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\run_game.ps1') -Out $dir -Exe 'sly2_pc_text_boot.exe' `
    -Card $card -Seconds $Seconds -Pad '@187D80/1DD96C+0:none'
Remove-Item Env:\PS2_PEEK, Env:\PS2_PEEK_EVERY
$log = Get-Content (Join-Path $dir 'run.log')

$fails = @()
if (-not ($log | Select-String -SimpleMatch '[pc_text] no save data yet: creating it (no prompt)')) { $fails += 'no silent creation in the log' }
if (-not ($log | Select-String -SimpleMatch '[pc_text] save data created')) { $fails += 'save data not created' }
if ($log | Select-String -SimpleMatch 'creating the save data failed') { $fails += 'creation failed' }
if (-not ($log | Select-String -Pattern '\[pc_text\] text bank loaded \(\d+ texts\): 19 in PC wording')) { $fails += 'not all 19 texts replaced' }
$dialogs = $log | Select-String -Pattern '\[peek\] v\d+ 002E6590: ([0-9A-F]{8})' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
if (-not $dialogs) { $fails += 'no PS2_PEEK lines' }
if ($dialogs -contains '00000015') { $fails += 'the create-a-save-file prompt (dialog 0x15) was shown' }
$shown = $dialogs | Where-Object { $_ -ne 'FFFFFFFF' -and $_ -ne '00000000' }
if ($shown) { $fails += "a memory-card dialog was shown: $($shown -join ', ')" }
if (-not ($log | Select-String -Pattern 'function 00187D80 called')) { $fails += 'the title screen was not reached' }
foreach ($f in 'save1', 'save2', 'save3', 'icon.sys') {
    if (-not (Test-Path -LiteralPath (Join-Path $card "BASCUS-97316YAOTWTD!\$f"))) { $fails += "missing $f" }
}
if ($fails) { "pc_text boot test FAILED:"; $fails | ForEach-Object { "  $_" }; "log: $(Join-Path $dir 'run.log')"; exit 1 }
"pc_text boot test passed: title reached with no input and no prompt; save data created in $card"
Remove-Item -Recurse -Force $dir
exit 0
