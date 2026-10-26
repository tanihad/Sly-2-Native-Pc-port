# r2-re: reverse-engineering the game's rendering for a modern high-level renderer

**Goal**: map Sly 2's rendering data and code (objects, meshes, materials, textures, animation,
VU1 programs, effects) with evidence, measure scenes, and propose where a modern renderer takes
over (summary in notes/modern-renderer-re.md).

**Status**: complete (2026-10-26). Summary: notes/modern-renderer-re.md.

## Done
- Read widescreen.md, vu-microcode.md, frame-rate.md (interp prototype!), hle-architecture.md, d3d11 census.
- Sly 1 decomp research (sub-agent, web): RP enum, RPL, GLOBSET/SHD/TEX/CLUT, CTK passes, SHADOW, blips (below).
- Tools (committed): scripts/r2_dis.py (annotated disassembly, callers, refs, vtable refs; `build` once ->
  build/r2/), runtime/r2_trace.c + patches r2_alloc/r2_frame (PS2_R2_TRACE=<vblank>:<frames>:<dir>, off by
  default; frame_<n>.txt = sorted render list + submitter regs, ram_<n>.bin), scripts/r2_frame.py (offline
  decode of a traced frame: chain walk, per entry/prio stats, --dump N), scripts/r2_vu_kicks.c (replay a
  PS2_VU_RECORD recording, decode every XGKICK GIF packet).
- ep3 hub traces: build/r2trace/ep3 (2 frames), ep3b (1 frame + 5000-program VU recording). NOT committed.

## In progress
- (none)

## Next
1. textures: where TEX/CLUT live, upload path (per-frame 1024x256 T8 upload at TBP 1080 etc.)
2. camera struct 0x2DDF40 fields used by frame globals (0x158120..), fog
3. class begin/end functions: shadows (prio 7/8, class 6/7 = clip program), post passes (direct PATH2 17k qw)
4. measure ep1/ep5 hubs, a boss, museum (warp_run + PS2_R2_TRACE), table
5. interception point design, write notes/modern-renderer-re.md

## Findings (evidence; addresses SCUS_973.16)
### Render list
- 16-byte entries at 0x3B3980 (max 0x2000 entries, end ptr 0x3D4A50): {+0 priority 0..25, +4 sort key
  (float, distance^2 to camera, or subglob+4 when flag 0x400), +8 DMA CALL #1 (per-frame header packet or a
  fully static packet), +0xC DMA CALL #2 (static mesh DMA, or 0)}. Allocator 0x1CD998 (memsets +8..+F).
- Writers: 0x1CD998 callers (glob 0x168638, attachments 0x1681F8, 0x13A1E8 [prio 12], 0x15DD48, portraits
  0x169198, 2D 0x2130C0/0x2163B0/0x216750/0x217298/0x2174C8/0x2177E0) + **inline in 0x1D5808 (static world
  tree)** which writes 0x3D4A50 itself (ep3: ~380 of ~600 entries).
- RenderSw 0x1CDA80: resets list, 0x1CD9E0 (objects of a set -> 0x1681F8), then for every object in
  SW+0x3F20 (flags&0x88==8) vtable+0x9C (render). 0x157FA0 sorts (0x157AE0) and emits: per class change
  end(prev)/begin(new) from 0x2BDD88, then CALL tags for +8 and +0xC.
- prio -> class (0x2BDD20): 0->4, 1->3, 2,3,5,10,11,13,14,16,19,23,25->0, 4,6->11, 7->6, 8->7, 9,17,20,24->1,
  12->2, 15->8, 18->9, 21->5, 22->10. Class begin/end (0x2BDD88): 0 0x16E1A8/0x16E228 (MPG main 376110651
  0x25E680, MSCAL 0 / 0x3AB0, zeroes VU 0xA..0x14), 1 0x16EA20 (MPG 188054331), 2 0x13A830 (MPG 376109499),
  3 0x16E298 (class 0 + CALL list 0x3DCFA8 count 0x3DCFA0; end + GS packet 0x2DF010), 4 0x167FC0 (MPG
  752221467), 5 0x211A80 (2D), 6/7 0x16DA48/0x16DD58 (MPG 188054619), 8/9 portraits 0x1BE990/0x1BE9E8,
  10 0x157CF0, 11 0x16E3C8/0x16E400 (class 0 + GS packets 0x2DF700/0x2DF720).
### Objects / globs
- ALO: +0xC vtable (8-byte slots {s16 delta, fn}); render-all slot +0xA0, render-self +0x98/0x9C (base
  ALO: 0x1F3C70 self, 0x1F3CC8 all = lights 0x1A4478 into a 0x30 struct + 0x168638 + children +0x64..+0x70
  linked by +0x24); +0x2D render mask byte (& camera 0x2DDF54); +0x80 embedded glob set (below); +0x12C fade;
  +0x134 light set.
- Glob set (ALO+0x80) as used by 0x168638(a0=globset, a1=extra matrix, a2=light set, f12/f13 fades):
  +0 owner ALO (owner+0x60 -> transform, world matrix at +0x40); +0x4 last-drawn time; +0x10 bound centre;
  +0x20 radius; +0x24/+0x28 attached objects (vtable+0x40); +0x34 param; +0x38 subglob count; +0x3C 0x14-byte
  records (+4 sort key, +8 static DMA for attachments); +0x40 0x28-byte subglob records {+4 transform
  source object, +8 local matrix ptr, +0xC flags (bits0-2 kind: 2 = ?, 3 = clip-needed flag upload; bits 3-7
  priority class; 0x100 REFS light prefix; 0x200 extra param; 0x400 fixed sort key; 0x800 hidden; 0x1000
  matrix from +0x10), +0x10 static mesh DMA, +0x18 skeleton (bone palette, 0x1684D0), +0x1C warp record};
  +0x44 0x18-byte bounds {centre xyz, radius, +0x10 LOD/fade distance}; +0x4C frame stamp; +0x50 flags.
- Culling: VU0 micro routine 0x1C0 (vcallms 0x1C0): vf10-12 frustum planes (camera +0xA0..), vf13 camera
  pos (+0x90), vf14 cam+0x1C, vf15 sphere centre, vf16 radius, vf17 LOD distance -> vf19.x fade (0 = cull).
- Per-frame header packet (built in DMAS 0x2DEA28 buffer): [REFS light set (VU 0x15..0x20)] CNT {UNPACK
  V4-32 x4 world matrix -> VU 0+TOPS; V4-32 x1 -> VU 8 (fade a, fade b, globset+0x50, param); [S-32 x1 ->
  0x12 clip flag]; [warp matrices V4-32 x4 each from VU (warp+0x20)]; [bone palette, 0x1684D0]} RET.
- Static world 0x1D5808 (object vtable 0x2D42FC, called by 0x1B4478): 4-ary tree of 32-byte nodes {centre,
  radius, +0x10 bits: kind 0-3, mask, prio (bits 14-19), subglob index (>>19); +0x18 DMA or child}; leaves
  = prebuilt world-space DMA (entry c1 = node DMA, c2 = 0) when fully visible, else REFS + fade CNT.
### Mesh DMA (static, in level heap e.g. 0x156xxxx / 0xD3xxxx), main program 376110651
- Batch = REFS/CNT tags: VU 0x009 1 qw (GIF tag template / PRIM bits), 0x00A 11 x V4-16 strip descriptors
  {uv addr, ..., 0x8000|nverts (GIF NLOOP+EOP)}, {pass A packet addr 0x15, pass B 0x1E}; 0x015 and 0x01E: 9 qw
  each = 2 vertex GIF tags (REGS 4F12 / 41F2) + A+D tag + TEX0, TEX1, CLAMP, TEST, ALPHA, FOGCOL.
  Pass A: TEST 0x717EB (ATST GEQUAL 126 FB_ONLY, Z GREATER), ALPHA 0x44 (Cs-Cd)*As+Cd. Pass B: TEST 0x51001
  (ATST NEVER FB_ONLY, Z GEQUAL), ALPHA 0x48 (Cs-0)*As+Cd, **other CLUT (TEX0 CBP differs)**.
- Prelit vertex (world, props; routines 0x2E20/0x2E68/0x2EB0 set-up + 0x2EE8 next batch): STCYCL 4/1,
  4 qw/vertex: UV V2-16 (4.12 fixed, 0x1000 = 1.0), colour A V4-5 (RGBA5551) + S-8 into w, colour B V4-5 +
  S-8 into w (STMASK 3F3F3F3F), position V4-32 xyz + w = 2^23-biased flags (ADC). Output: one vertex
  buffer, two GIF tags (pass A uses colour A, pass B colour B: REGS 4F12 vs 41F2 place RGBAQ differently).
- Lit/skinned vertex (characters; routines 0x2EF8/0x2F80/0x2FC8/0x3358): V3-16 (UV+?), V4-8 signed normal,
  V4-8 colour (7F7F7F80), V4-32 position (some -1,-1,-1), + separate STCYCL 1/1 V4-8 weights (000000FF,
  00807F00 ...). Lights from REFS (VU 0x15-0x20, 4 lights: direction + colour), bones at VU 0x21+ (24 x 4 qw
  in Sly's case, EE-computed by 0x1684D0, see "Skinning" below).
### VU1 programs by function (ep3 hub, 1 frame, r2_vu_kicks)
- 376110651 main: prelit + lit/skinned meshes, two passes; tristrips TME FGE ABE; 160k vertices/frame total.
  0x3628/0x37A8 (prio 13): untextured ABE strips ALPHA 0x6A (Cd: colour masked), TEST Z GREATER(+DATE):
  Z/stencil-only volumes. 0x3AB0 class-end flush.
- 188054331 (class 1, prio 9/17/20/24): 0x0738 setup + 0x07F0 = **cel outlines**: lines, pairs (Z-only TEST
  0x73001, then AA1+ABE RGB TEST 0x72001), 536+536 per frame; inputs V3-32 + V4-8.
- 188054619 (class 6/7, prio 7/8, "clip"): 0x1910/0x1A68 = **shadow volumes**: untextured strips ALPHA
  0x8000000062 = (0-Cs)*128+Cd, TEST 0x75001 (DATE, Z GREATER), inputs V3-32/V4-8/V4-32 + near clip.
- 376109499 (class 2, prio 12, submitter 0x13A1E8): 0x0390/0x0870 textured strips regs 412E, TEST 0x7180B:
  sprite/particle-like (ESIN/ESQRT in program).
- 188055835 2D (class 5): 0x0140 AA1 line strips + flat strips/fans (vector art), 0x00C0/0x0100 text.
- 752221467 (class 4, prio 0, 0x1681F8 attachments): not seen in ep3 frame.
- Per frame (ep3 hub): 606 entries; static mesh DMA 2.9 MB referenced, per-frame packets 67 KB;
  MSCAL 3782; main program 2EE8 x964, 2E20 x464, 2EB0 x313, 3358 x225 ...; ~6000 GIF primitives.

### Skinning, warps (corrected)
- Skinned subglobs (subglob +0x18 != 0): 0x1684D0 builds the bone palette on the EE: per bone (0x50-byte
  records) local 4x4 (+0x10..+0x40) x the node's world matrix (node +0x40..+0x70) with VU0 macro 0x60, all
  bones in one UNPACK V4-32 (count x 4 qw) to the VU address in the record (Sly: 24 bones at VU 0x21); the
  entry's world matrix is then identity (0x2A2D30). VU1 blends with the per-vertex V4-8 weights.
- Subglob +0x1C record = procedural **warp** (not bones): matrices at owner +0x6F0 + i*0x40 (and +0x7F0)
  written by 0x22C028 (vtable 0x2DCF84; sin(t*f*2pi+phase) per element = Sly 1 WR circle/bend), uploaded
  inverse-world-relative, V4-32 x4 each.
### Frame globals (0x157FA0 before the list)
- VU1 0..1: depth/screen constants from camera +0x4C/+0x50 (near/far) with templates 0x2C1F00/0x2C1F10;
  VU 0xD flag 0x2DE5A4; VU 2..5 camera +0x140 (world -> clip, row vectors); VU 6..9 camera +0x60 (camera ->
  world: translation = camera position); VU 0xA = SW +0x3E28 (fog/brightness params). Camera CM 0x2DDF40:
  +0x20 fov, +0x3C aspect, +0x40/+0x44 x/y range, +0x48 near-clip radius, +0x4C near, +0x50 far, +0x90
  position, +0xA0.. frustum planes (VU0 cull), +0x14 render mask.
- Chain order (ep3): CALL 0x2DF800 (GS setup) / MPG main / CALL 0x168F820 = per-frame texture uploads from
  level RAM 0x164C020.. (CLUT 1 KB + 2 small, 256 KB T8, 4 KB + 8 KB T4 -> TBP 0x1000.. = inside the Z buffer
  pages) / prio 1 sky (class 3) / class 3 end GS packet 0x2DF010 / prio 3.. / ... / 2D / END.
- Shadow pipeline post passes are XGKICKed by the shadow program 188054619 itself: routines 0x1AA0 (class 6
  begin: CTX2 FRAME FBMSK 3FFF CT16 shuffle sprites, G stencil init FBMSK FFFF00FF) and 0x1C48 (end: shuffles
  back, alpha bits); REGLIST sprites. Motion blur: render thread 0x160B70 -> 0x160F98 when frame +0x30 bit 0.
- GIF chain (frame +0x4) = 12-qw context setup only; everything else is the VIF1 chain.
### Cel outlines (188054331)
- Static per subglob: V3-32 vertex positions + V4-8 edge records (v0, v1, va, vb) = edge + the opposite
  vertices of its two triangles (winged edge): silhouette test on VU1, lines drawn twice (Z-only, then AA1).
### Particles (376109499, submitter 0x13A1E8, prio 12)
- Per-frame CNT with emitter params (positions, velocity, gravity, dt 0.01667) + REFS of per-particle
  records (6 qw, STCYCL 10/1) from level RAM 0x1D4xxxx: the VU computes particle positions (Sly 1 "blips").
### Measurements (release build, one game frame each; scripts/r2_scene.ps1 + r2_frame.py + r2_texstats.py)
| scene | entries | prelit ent / verts | lit+skinned ent / verts | outline | shadow vol | particles | 2D | GS prims | textures (KB) / CLUTs | static/unchanged (prelit) |
|---|---|---|---|---|---|---|---|---|---|---|
| ep1 hub (Paris roofs) | 894 | 755 / 63.9k | 63 / 8.4k | 13 | 41 | 8 | 12 | 54.3k | 210 (1164) / 406 | - |
| ep3 hub (run 1, open view) | 606 | 464 / 65.8k | 95 / 12.3k | 11 | 10 | 8 | 12 | (VU out 160k verts) | - | 447 of 464 |
| ep3 hub (run 2, at a wall) | 193 | 91 / 16.2k | 55 / 9.4k | 18 | 9 | 4 | 12 | 20.7k | 91 (802) / 176 | 77 of 91 |
| ep5 hub | 561 | 460 / 51.3k | 46 / 4.6k | 11 | 22 | 4 | 12 | 41.5k | 150 (932) / 292 | - |
| m8 boss (airship) | 195 | 110 / 18.2k | 24 / 3.4k | 11 | 6 | 10 | 34 | 24.0k | 124 (534) / 226 | 13 of 110 (moving ship) |
| museum (prologue) | 393 | 297 / 30.9k | 30 / 3.9k | 11 | 33 | 2 | 12 | 28.0k | 91 (797) / 174 | 279 of 297 |
- "verts" = source vertices (V4-32 positions); the GS sees ~2x (pass A + B). Static mesh DMA referenced per
  frame 0.9-2.9 MB; per-frame packets 32-67 KB (+ texture uploads 42-269 KB).
- VRAM: FB 0x00-0x37, 0x38-0x6F, Z32 0x70-0xA7 (pages); textures resident 0xA8-0x1FF (+ per-frame set at
  0x80..); per frame 91-210 distinct textures (T4 65-80%, T8), 0.5-1.2 MB; 170-410 CLUTs (each texture with
  ~2: pass A / pass B palettes). No uploads of resident textures during gameplay.

## Sly 1 decomp (TheOnlyZac/sly1) facts likely carrying over
- RP enum (Sly 1, 18): DynamicTexture, Background, BlotContext, Opaque, Cutout, CelBorder, ProjVolume,
  OpaqueAfterProjVolume, CutoutAfterProjVolume, CelBorderAfterProjVolume, MurkClear, MurkOpaque, MurkFill,
  Translucent, TranslucentCelBorder, Blip, Foreground, WorldMap. Sly 2 has 26 priorities.
- SHD kinds ThreeWay/Prelit/Shadow/SpotLight/ProjectedVolume/...; CTK passes (ambient/diffuse/saturation =
  different CLUTs per pass) -> matches Sly 2's pass A/B with different CBP.
- Blips (VU particle batches of 44), SHADOW (projected), ALO XF (matWorld at +0x110 in Sly 1), MRD fades.
