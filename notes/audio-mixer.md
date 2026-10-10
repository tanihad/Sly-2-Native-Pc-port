# Audio: the native sound backend (libsd replacement + WASAPI output)

Part of approach C (`notes/audio-native-plan.md`): the recompiled 989snd/spiop call Sony's libsd;
libsd is not run, its API (`runtime/sd.h`, frozen) is implemented natively on a PC voice mixer.
This document describes that mixer. Status (2026-10-10): implemented and unit-tested; not yet
called by the game (`runtime/snd.c` stays the active path until the IOP side is integrated).

## Files

| File | Contents |
|------|----------|
| `runtime/sd.h` | frozen interface (libsd functions, `iop_sd_transfer_done`, `iop_mem`, `audio_init`) |
| `runtime/sd_internal.h` | `sd_render` (pulled by the output thread), `sd_frames_rendered`, reverb types, test hooks |
| `runtime/sd.c` | libsd API, sound memory, 2 x 24 voices, envelopes, noise, pitch modulation, core mixing, AutoDMA input, transfers |
| `runtime/sd_reverb.c` | reverb presets, reverb algorithm, resampling filter |
| `runtime/audio_out.c/.h` | output thread: WASAPI shared/event mode, real-time null sink, WAV dump, test tone |
| `tests/unit/sd/` | `run.ps1` builds and runs `test_sd` (mixer) and `test_audio_out` (output path) |

No extra link libraries: `audio_out.c` loads `ole32.dll` (CoInitializeEx/CoCreateInstance) and
`avrt.dll` (Pro Audio thread class) with LoadLibrary and defines the four WASAPI GUIDs itself, so
the game links with the libraries it already uses. `sd.c` uses SRW locks (kernel32).

## Sources and how values were checked

- Constant names and encodings: ps2sdk `libsd-common.h`. Every encoding, default and table that
  libsd itself applies was checked against **the copy of libsd 3.3 inside the game** (`.irx` module
  "Sound_Device_Library", `scripts/irx_tool.py dis sound <addr>`); the details are in
  `notes/wip/audio-mixer.md` (Findings). Highlights:
  - entry = `param << 8 | core | voice << 1`; bit 0x80 only selects the shared-volume register
    block; 989snd's extra 0x40 bit on address entries is ignored, as libsd does.
  - SetAddr stores `hi = v >> 17`, `lo = (v >> 1) & 0xFFF8` (16-byte granularity); EEA keeps only
    `hi` and reads back as `hi << 17 | 0x1FFFF`.
  - sceSdInit(0) defaults: ATTR C000h/C001h, MMIX FF0h/FFCh, every VMIX bit on, MVOL/EVOL/BVOL 0,
    AVOL core 1 = 7FFFh, EEA hi Eh/Fh, all voices at a silent looping block it writes at 0x5000.
  - Note2Pitch: same arithmetic as libsd (tables `floor(8000h * 2^(n/12))` and
    `floor(8000h * 2^(k/1536))`, verified entry by entry; truncating divisions; rounding right shift;
    no left shift above two octaves, so +3 octaves returns 4000h like the original).
  - VoiceTrans rounds sizes up to 64 bytes and returns the rounded size (989snd treats a smaller
    return value as an error).
- Voice algorithms (inherited from the PS1 SPU): psx-spx. ADPCM (shift/filter, rounding, reserved
  shift 13-15 = 9), loop flags (end+mute / end+repeat, loop start copies the address to LSAX, ENDX),
  pitch counter (bits 12+ sample, 4-11 interpolation index, step clamped at 4000h), pitch modulation
  (factor = previous voice's output + 8000h), ADSR/sweep envelope operation, noise generator, reverb
  formula.
- SPU2 specifics: ps2tek (AutoDMA: 1024-byte blocks = 512 bytes left + 512 right, double buffered).
  The MMIX bit assignment was derived from what 989snd writes: core 0 F00h and core 1 F0Ch (voices
  dry+wet, core 1 also takes core 0 through its external input), movie sound sets C0h together with
  BVOL (sound-data input). Bits: 11/10 voice dry L/R, 9/8 voice wet, 7/6 sound-data dry, 5/4
  sound-data wet, 3/2 external dry, 1/0 external wet.
- No emulator source code was used.

## Design

**Rendering on demand.** The output thread asks `sd_render(frames)` for exactly what the device
needs (about 40 ms queued). Rendering advances everything (pitch counters, NAX, ENDX, envelopes,
AutoDMA position), so voice positions move with real playback, which 989snd's stream code polls
(it reads NAX three times until two reads agree; our reads are consistent under the lock). Without
a device the same thread renders against the performance counter (null sink), so headless runs
keep the same timing. If the device disappears it keeps rendering and retries every 3 s.

**Locking.** One SRW lock protects all state. `sd_render` takes it per 64-frame slice (about 30 us
of CPU), so a sceSd* call from the emulation thread never waits long. `iop_sd_transfer_done` is
always called without the lock: from the emulation thread inside sceSdVoiceTrans, and from the
audio thread for AutoDMA half-buffers.

**Voices** (per 48 kHz sample, voice-major in 64-sample slices so pitch modulation can read the
previous voice's output): sample = 4-tap Gaussian interpolation of the decoded ADPCM (or the core's
noise for NON voices); OUTX = sample * ENVX; L/R = OUTX * VOLX, added to the dry and/or wet buses
per VMIXL/R and VMIXEL/ER; then envelope, volume sweeps and the pitch counter step. When a block
ends its loop flags apply (ENDX on loop end, jump to LSAX, end without repeat = release at level 0;
the voice keeps running from LSAX like the hardware). A voice released at level 0 takes a fast path
that only follows block headers (NAX/ENDX/LSAX still advance); KON resets its decoder anyway.
Voice and sweep volumes: bit 15 clear = fixed (bits 0-14 = volume/2), set = sweep (linear or
exponential, up or down, phase) with the envelope rules. ENVX, VOLX and MVOLX read back live values.

**Gaussian table.** psx-spx documents the 512-entry hardware table and how it is indexed. We do not
copy the ROM table; we generate it from a kernel fitted to the table's documented shape:
`g[n] = K((512-n)/256)`, `K(x) = A*(exp(-x^2/(2s^2)) - exp(-2/s^2))`, `A = 0.70224`,
`s^2 = 0.32407`, which gives `g[000h] = 0`, `g[100h] = 1307h`, `g[1FFh] = 59B2h`; the four taps of
every phase sum to 7EA8h..7F88h (within 0.07 dB of unity). The audible character (the soft PS2
treble) is that of the hardware; individual entries can differ from the ROM by a few units.

**Cores.** Core 0 mixes its voices, its sound-data input (x BVOL) and nothing on the external input;
core 1 takes core 0's output as external input (x AVOL). Each core: dry and wet buses per MMIX,
reverb on the wet bus (when ATTR bit 7 is set), `out = (dry + reverb * EVOL) * MVOLX`, silent unless
ATTR bits 15 (on) and 14 (unmute; sceSdInit sets it and 989snd never clears it) are set. Core 1's
output is the 48 kHz stereo output. Saturation to 16 bits at each bus.

**Reverb** (`sd_reverb.c`). The PS1 reverb formula from psx-spx (same-side and different-side IIR
reflections, four comb taps, two all-pass stages; every product `SAR 15`, every store saturated),
with the SPU2's per-core work area ESA..EEA and the addresses as halfword offsets from the moving
buffer position. It runs at 24 kHz (half rate, as the PS1 runs at 22.05 kHz): the wet input is
decimated and the output interpolated with a 39-tap half-band FIR (Blackman-windowed sinc, centre
tap 4000h, odd taps zero, DC gain 1; psx-spx describes a 39-tap filter of the same structure).
Presets: libsd 3.3's `sceSdSetEffectAttr` copies one of ten 32-halfword sets (data at libsd+4AC0h)
whose address entries it writes x4 into the SPU2; they are the classic PS1 presets (Room, Studio
A/B/C, Hall, Space, Echo, Delay, Pipe; psx-spx lists the same numbers), so the PS2 plays them ~8%
shorter than a PS1 would. We embed the table in PS1 register order (dAPF1..vRIN) and apply it the
same way: work area sizes from libsd+4A98h (in 8-byte units there), ESA = EEA - size + 1, EVOL =
depth, and for Echo/Delay `d = (delay+1)*32`: mLSAME = 2d - dAPF1, mRSAME = d - dAPF2, dLSAME =
dRSAME + d, mLCOMB1 = mRCOMB1 + d, mLAPF1/mRAPF1 = mLAPF2/mRAPF2 + d, vWALL = feedback * 258. Mode
bit 100h clears the work area. Because of the flooring arithmetic the IIRs can settle on a few LSB
of DC after a long silence (as the documented formula implies); it is inaudible.

**Sound-data input (AutoDMA, movie sound).** `sceSdBlockTrans(ch, WRITE[_FROM][|LOOP], buf, size,
start)` makes core `ch & 1` play 16-bit stereo PCM from IOP RAM in 1024-byte blocks. A block is
fetched (copied) whenever one of the core's two MEMIN halves is free, so the "DMA" position runs one
to two blocks ahead of playback, as on the hardware. Loop transfers treat the buffer as two halves
and announce each finished half (`iop_sd_transfer_done(ch)`), wrapping at the end; one-shot
transfers announce the end and stop. `sceSdBlockTransStatus` returns `half << 24 | IOP address`
(989snd masks the address with FFFFFFh); STOP returns the same and silences the input.

**Transfers.** `sceSdVoiceTrans` copies immediately (with the 64-byte rounding, wrapping at 2 MB),
sets TSA after the data, announces completion and returns the rounded size; the status calls
report "done".

## Output (`audio_out.c`)

WASAPI shared mode, event driven, 48 kHz / 2 ch / 16-bit with `AUTOCONVERTPCM |
SRC_DEFAULT_QUALITY` (Windows converts to the mix format), buffer = target latency, refilled on
every event to keep the target queued. Environment:

| Variable | Effect |
|----------|--------|
| `PS2_AUDIO=0` | no output thread; nothing renders, so NAX/ENDX don't move (only for the `snd.c` fallback) |
| `PS2_AUDIO_NULL=1` | no device: render in real time into a null sink (headless test runs) |
| `PS2_AUDIO_WAV=<file>` | also write everything rendered to a WAV (header refreshed every 250 ms, since the game may leave with ExitProcess) |
| `PS2_AUDIO_TEST=1` | 440 Hz sine at -12 dBFS instead of the game's sound |
| `PS2_AUDIO_LATENCY=<ms>` | target queued audio, 10-500 (default 40) |
| `PS2_LOG_SD=1` | log sceSdInit, KON (voice, SSA, pitch, ADSR, volume), KOFF, transfers, BlockTrans, SetEffectAttr |

## Tests (`tests/unit/sd/run.ps1 [-Audible]`)

`test_sd` (674 checks): ADPCM against an independent reference decoder (4096 random blocks, every
shift x filter); Gaussian table shape; Note2Pitch (hand-derived values, a grid against an independent
formulation and against 4096*2^(semitones/12)); init defaults and address encodings; ADSR levels and
timing (linear/exponential attack incl. the 6000h slowdown, exponential decay to the sustain level,
sustain hold and increase, linear release, rate 7Fh) and volume sweeps; pitch accuracy of an encoded
1 kHz sine at 0x0155..0x3FFF (zero crossings, 0.2%) and its level through core 0 -> core 1; loops,
ENDX, NAX advance (also for silent voices and at double pitch), software LSAX; VoiceTrans write,
read, rounding, wrap, TSA and completion; AutoDMA loop/write-from/one-shot playback, half-buffer
announcements and status; routing (AVOL, VMIX, mute), noise, pitch modulation; every reverb preset
(produces output, bounded, decays, never writes outside its work area) and full-feedback echo
stability; performance (48 voices + hall reverb: about 24 ms of CPU per second of audio).

`test_audio_out` (12 checks): the test tone through the null sink is a clean 440.000 Hz sine at
the right level and real-time pace; a voice rendered by `sd.c` reaches the WAV frame-exact; the
WASAPI path starts and is paced by the device (48000 frames per second).

## Integration notes (for the integrator / IOP side)

- Call `audio_init()` once at start-up (e.g. in `ps2_runtime.c` next to the other subsystem inits);
  `audio_shutdown()` is registered with atexit and is optional.
- `iop_sd_transfer_done(int channel)`: `sd.c` has a weak no-op default so the game links before
  integration; the IOP side's definition replaces it. It is called (lock released) synchronously
  inside `sceSdVoiceTrans` on the emulation thread, and on the audio thread for AutoDMA halves; it
  must only queue the guest's transfer handler. libsd's `VoiceTransStatus(ch, WAIT)` returns after
  the handler has run on hardware; if a caller depends on that, run pending handlers before
  returning from a waiting status call (989snd's movie-sound code at 0xB5C0 waits this way right
  after its VoiceTrans).
- `sd_effect_attr` has the same field offsets as the guest's struct (core at 0, mode 4, depth 8/10,
  delay 12, feedback 16; 989snd stores core as a word), so the guest bytes can be converted field by
  field from those offsets.
- `sceSdClearEffectWorkArea` and the clear inside `sceSdSetEffectAttr` do not announce a transfer
  (libsd waits for that DMA internally).
- `sd_frames_rendered()` (sd_internal.h) is a monotonic 48 kHz clock of rendered audio, usable if
  the IOP's 240 Hz tick should follow the audio clock.
- `scripts/run_game.ps1` runs headless but would still open the audio device; consider setting
  `PS2_AUDIO_NULL=1` there unless `-Window`, and a `-Wav <file>` switch for `PS2_AUDIO_WAV`.

## Known deviations

- Gaussian table generated from a fitted kernel (see above), not the ROM values.
- Reverb resampling filter is our own half-band design; left and right are computed in the same
  24 kHz step. When the effect bit is off the reverb neither reads nor writes its work area.
- Not modelled (989snd does not use them): IRQA/SPU2 interrupts, writes to NAX, AutoDMA reads,
  the SPU2's capture areas at the start of sound memory, SPDIF output modes, the KON start delay,
  transfer durations. ATTR bit 0 (external input enable) does not gate core 1's external input.
- Writing ENDX clears all its bits. ADPCM filter values 5-7 decode as filter 0.
