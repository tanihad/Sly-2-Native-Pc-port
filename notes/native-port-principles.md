# Native port principles

Goal (from the project owner): Sly 2 should run on PC **as if it had been released on PC**, not
inside an emulator. Every subsystem decision follows from that.

## What "native" means here

1. **Game code is compiled, never interpreted.** The EE code is statically recompiled to C and
   compiled for x86-64 (done). The same applies to VU microcode: the game's VU1/VU0 microprograms
   are statically recompiled to native code at build time. An interpreter may exist only as a
   reference/fallback for verifying the recompiled programs.
2. **PS2 hardware requests are translated to PC APIs, not simulated.** The game's code still
   produces PS2-shaped requests (GS command packets, DMA lists, movie bitstreams, sound commands).
   Those are translated at a high level into the PC equivalent:
   - Graphics: GS primitives -> Direct3D 11 draw calls on the GPU (textures decoded from the
     game's uploads, render targets on the GPU). Native resolution scaling, widescreen,
     anti-aliasing and high frame rates are goals, as in other recompiled PC ports.
   - Movies: replace the PS2 movie library (sceMpeg/IPU usage) with native playback of the same
     video data through a PC decoder.
   - Audio: replace the sound library's IOP side (989snd/libsd) with a native mixer feeding
     WASAPI, driven by the game's sound commands.
   - Input: XInput + keyboard. Saves: plain files. Disc: game data read from the user's own disc
     image or extracted files.
3. **No PS2 BIOS, no cycle timing, no pixel-exact GS emulation.** Timing comes from the PC
   (frame pacing, vsync). Where the game depends on a PS2 quirk, emulate only that quirk.
4. **Library-level replacement is preferred.** The recompiler owns every function, so a Sony
   library function (or a game function) can be replaced wholesale by native C when that is
   simpler and more faithful than translating its hardware traffic.

## Reference projects for the approach

Zelda 64: Recompiled (N64 -> PC: recompiled CPU code, RT64 renderer translating graphics
commands to modern GPU APIs) and Unleashed Recompiled (Xbox 360 -> PC). Not PCSX2-style emulation.
