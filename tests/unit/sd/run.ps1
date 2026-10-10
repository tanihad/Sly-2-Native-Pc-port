# Build and run the native sound backend tests:  powershell -ExecutionPolicy Bypass -File tests\unit\sd\run.ps1 [-Audible]
#   test_sd:        runtime/sd.c + sd_reverb.c (ADPCM, ADSR, pitch, loops/ENDX/NAX, transfers, ADMA, routing,
#                   reverb presets, Note2Pitch, performance)
#   test_audio_out: runtime/audio_out.c on top (test tone and voice to WAV through the null sink, WASAPI start)
# -Audible plays the 440 Hz test tone through the default output device for two seconds.
# Output: build\unit\sd\ (tone.wav, voice.wav, device.wav).
param([switch]$Audible)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$main = $root
if (-not (Test-Path (Join-Path $root 'tools\llvm-mingw'))) {      # worktree: tools live in the main checkout
    $gitfile = Join-Path $root '.git'
    if (Test-Path $gitfile -PathType Leaf) {
        $gitdir = ((Get-Content $gitfile -Raw) -replace '^gitdir:\s*', '').Trim()
        $main = Split-Path (Split-Path (Split-Path $gitdir -Parent) -Parent) -Parent
    }
}
$clang = Join-Path $main 'tools\llvm-mingw\bin\clang.exe'
$out = Join-Path $root 'build\unit\sd'
New-Item -ItemType Directory -Force $out | Out-Null
$rt = Join-Path $root 'runtime'
$flags = @('-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', $rt)

& $clang @flags (Join-Path $PSScriptRoot 'test_sd.c') (Join-Path $rt 'sd.c') (Join-Path $rt 'sd_reverb.c') `
    -o (Join-Path $out 'test_sd.exe')
if ($LASTEXITCODE -ne 0) { Write-Host 'sd tests: build failed'; exit 1 }
# audio_out.c must link without extra libraries (COM is loaded at run time), like the game does
& $clang @flags (Join-Path $PSScriptRoot 'test_audio_out.c') (Join-Path $rt 'audio_out.c') (Join-Path $rt 'sd.c') `
    (Join-Path $rt 'sd_reverb.c') -o (Join-Path $out 'test_audio_out.exe')
if ($LASTEXITCODE -ne 0) { Write-Host 'audio_out tests: build failed'; exit 1 }

& (Join-Path $out 'test_sd.exe')
if ($LASTEXITCODE -ne 0) { Write-Host 'sd tests: FAILED'; exit 1 }
$args2 = @($out)
if ($Audible) { $args2 += '--audible' }
& (Join-Path $out 'test_audio_out.exe') @args2
if ($LASTEXITCODE -ne 0) { Write-Host 'audio_out tests: FAILED'; exit 1 }
Write-Host 'sd tests: all passed'
exit 0
