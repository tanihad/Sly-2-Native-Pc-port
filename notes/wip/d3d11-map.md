# WIP: renderer-map (map of the current GS pipeline for a GPU backend)

Role: renderer-map. Branch next/d3d11, worktree C:\Users\tanih\Projects\sly2-worktrees\d3d11.
Status: COMPLETE. All runtime/gs*.c, gif.c/vif.c entry points, ps2_runtime.c (vblank/present), window.c,
gs_record.c, scripts/gs_replay.*, compare_frames.py and tests/unit/{gs-draw,gs-mem,window,gif} read.
runtime/ is unchanged since c6cf838 (git diff --stat empty), so every line number below is current.

## Progress
- [x] 1 data flow EE -> GS
- [x] 2 software renderer structure (gs_draw.c, gs_draw_pixel.c, gs_draw_tex.c, gs_draw_swz.c, gs_mem*.c)
- [x] 3 display compose (gs.c) + window.c D3D11
- [x] 4 seams for a GPU backend, tests, gs_replay

## Findings (file:line, worktree d3d11 @ 96075f8, runtime identical to c6cf838)

### 1 Data flow
- PATH1: VU1 XGKICK -> vu.c:298 vu_xgkick -> gif.c:247 gif_path1_kick (wraps at 16 KB, 2048 qw cap).
- PATH2: VIF1 DIRECT/DIRECTHL -> vif.c:340 path2_send / :354 direct_data -> gif.c:243 gif_path2.
- PATH3: DMA ch2 dma.c:174 -> gif.c:236 gif_path3_dma; EE FIFO stores 0x10006000 gif.c:221 fifo_write.
  Masks: GIF_MODE.M3R, VIF1 MSKPATH3 (vif.c:431 -> gif_set_path3_mask gif.c:270), GIF_CTRL.PSE; stall
  only at packet boundary (gif.c:168). GIF_STAT gif.c:279 (FIFO always drained; APATH only mid-packet).
- GIFtag decode gif.c:93 read_tag (PRE->PRIM write, Q reset), PACKED gif.c:121, REGLIST :186, IMAGE :174
  -> gs_image_write; A+D to HWREG -> image data (gif.c:116).
- All produce gs_write_reg(reg,v) / gs_image_write(data,bytes) (hw.h:100-101), implemented in
  gs_thread.c:102/:120. Mirror gs_reg_mirror + recording (gs_record.c) happen there.
- Queue: gs_thread.c SPSC ring 2^18 x 16-byte slots (:28), kinds reg / K_IMAGE / K_SKIP / K_VSYNC / K_SYNC
  (:34); publish every 256 slots (:117), after IMAGE (:135), sync, vsync. Consumer gs_thread_main :216
  executes gs_exec_write_reg / gs_exec_image_write; on idle gs_draw_kick() (:227) then WaitOnAddress.
- SIGNAL/FINISH/LABEL executed on the EE thread at queue time (gs_thread.c:106) -> gs.c:34-45 CSR/SIGLBLID,
  INTC_GS if unmasked by IMR (gs.c:28). CSR read gs.c:65: FIFO always "empty", FIELD toggles per vsync
  (gs.c:49 gs_vsync, called from ps2_runtime.c:394), VSINT|HSINT set every vsync.
- CSR.RESET (gs.c:67) -> gs_sync() + gs_draw_reset(). Priv regs PMODE..BGCOLOR stored raw in priv[] (gs.c:21),
  written on EE thread, snapshotted per VBlank (gs_priv_snapshot gs.c:184) into the K_VSYNC slot.
- Readback: TRXDIR=1 -> gs_mem_xfer_start dir 1 (on GS thread, after gs_draw_sync XFER gs_draw.c:913);
  EE gets data via VIF1 FIFO read: dma.c:183 (VIF1 dir from mem) / vif.c:636 -> vif1_read vif.c:574 ->
  gs_image_read gs_thread.c:155 -> gs_sync() (EE blocks until queue drained) -> gs_exec_image_read ->
  gs_mem_xfer_read. BUSDIR is only stored (gs.c:101), no behaviour.
- EE waits on GS only at: gs_sync (image read, CSR.RESET), queue full (reserve :77), VBlank when previous
  picture not yet presented (gs_queue_vsync :160). SetGsCrt syscall writes SMODE2 (ps2_runtime.c:555);
  GsGetIMR/GsPutIMR (ps2_runtime.c:560-569).
- VBlank: ps2_runtime.c:389 vblank(): gs_vsync(); gs_async() ? gs_queue_vsync(vblank_count) :
  gs_display()+present_frame. present_frame ps2_runtime.c:362 -> window_present(frame,w,h,1024) +
  PS2_DUMP_FRAMES png. Workers: ncpu/2-1 capped 8 (ps2_runtime.c:684-690), PS2_GS_THREADS, PS2_GS_ASYNC=0.

### 2 Renderer (gs_draw.c)
- Register file reg[0x64] gs_draw.c:39; gs_exec_write_reg :983. Env regs set env_dirty when changed
  (:1008-1015); PRIM resets vcount, dirties env only if PRMODECONT.AC=1 and attr bits changed (:987).
  TEX0/TEX2 -> tex0_write :956 (CLUT load + wait on pending draws to CLUT pages, gs_clut_version++).
  TRXDIR -> trxdir :892; HWREG -> image data :1025; TEXFLUSH ignored (:1019).
- env_update :165 decodes context (PRMODECONT ? PRIM : PRMODE bit 9), FRAME/ZBUF/TEST/ALPHA/SCISSOR/XYOFFSET,
  PABE, FBA, COLCLAMP, DTHE, DIMX, FOGCOL, SCANMSK into gs_prim P (gs_draw.h:230); zbw = fbw (:191);
  records gs_fb_yfrac/psm per FBP (:225) for field detection; bind_textures :99 (TEX1 LOD mode -> texm
  0/1/2/3, cur_level, mip TBPs via MIPTBP1/2 or MTBA auto :147); gs_prim_finalize (gs_draw_pixel.c:1248);
  gs_prim_version++ (:232). prim_draws=0 when FBMSK all-ones and Z masked (:231).
- Vertex queue: vertex() :827 (XYZF/XYZ, kick vs no-kick, RGBAQ/ST/UV/FOG latched; ST low 8 mantissa bits
  dropped :836); strip/fan handling :843-866; draw() :800 dispatch. AA1 only for lines (:816 logs others).
- draw_sprite :386 (covers [X0,X1)x[Y0,Y1), flat v1 colour, U/V linear, 'local' in-place filter detection),
  draw_triangle :529 (exact int edge functions, top-left rule, double planes -> fixed rowstep, Z/colour/fog
  planes only when used, flat colour = v2), draw_line :593 (DDA, AA1 coverage = 2 pixels across minor axis,
  coverage replaces alpha), draw_point :650.
- tex_prepare :285: validates levels, texel bbox from vertices (UV, or STQ/q; mixed-sign q => whole
  texture) -> per level: in-place local filter (same tab/bp/bw as FRAME, :339: no snapshot) / direct
  sampling: read pages (gs_tex_read_pages) vs dest pages (frame+Z, :353) -> feedback => snapshot set,
  else reads set; cache levels: gs_tex_ensure + nowrap. Ends in gs_draw_set_reads(reads, snap) :372.
- mark_written :375 -> gs_mark_rect (texture cache invalidation).

### 2b Pixel pipeline (gs_draw_pixel.c)
- batch of <= 2048 px / 512 segs (:24-41); stages: colour_batch :307, sample (cache sample_batch :210,
  direct dsample_batch :262 per GS_TK kind), tfx_batch :330 (MODULATE/DECAL/HIGHLIGHT/2, >>7), fog :371,
  tests_batch :391 (ATE+AFAIL, DATE/DATM, ZTST NEVER/ALWAYS/GEQUAL/GREATER, Z write clamped to zmax),
  write_seg :455 (PABE, blend (A-B)*C>>7+D, DTHE only for 16-bit FB, COLCLAMP or wrap, FBA, FBMSK, RGB_ONLY).
  pipeline :540 orchestrates; Bilinear = 4-bit weights (:94). LOD: lod16_of :180; tex_general :189 mipmaps.
- solid fill fast path fill_span :632 (prim->solid, :1280).
- Deferred band-parallel: :661-1201. NBANDS 32, BIN_SEGS 4096, DB_PRIMS 8192, DB_STATES 256, DB_QUEUE 6.
  dbatch :703 (states/prims/bins, owner[512], written/readp/copyp/snap_hold page sets, job_fn), pool :744.
  Page ownership per batch; assign_band :1126 -> -1 (two bands) / -2 (WAR: writes a page read earlier in
  batch) -> db_submit :981. register_prim :1075: RAW (reads pages written in batch), SNAP, HOLD, LIMIT ->
  new batch; snapshots gs_prim when gs_prim_version changed; gs_tex_note_use pins texels. Batches drawn
  strictly in order; bands in parallel by workers (claim_band :845, process_band :814, run_bands :898).
  Shadow copy for feedback: copy_pages(copyp) at start_job :859 into gs_vram_shadow (:760).
  gs_draw_queue_job :1015 for the VBlank picture (JOB_BAND). gs_draw_pages_touched :1025 used by transfers
  and CLUT loads; gs_draw_pages_pending :1046; gs_draw_busy :1040. Syncs counted by reason (GS_SYNC_*),
  PS2_GS_STATS=1 report :781. gs_batch_begin/span/end :1205/:1219/:1242 are the rasterizer->pipeline API.

### 2c Textures, VRAM, CLUT (gs_draw_tex.c, gs_draw_swz.c, gs_mem*.c)
- VRAM: gs_mem.c:30 `gs_vram[4 MB]` (64-aligned), swizzle tables built in a constructor :56
  (ct32/z32 32x64, ct16/16s/z16/z16s 64x64, t8 64x128, t4 128x128). gs_mem_swizzle.h: gs_addr32/16/8/4
  :35-46 (bp*units + page*units_per_page + table), gs_px_read :66 / gs_px_write :90 per PSM (24-bit keeps
  top byte, 4HL/4HH nibble merge). Public gs_mem_read/write gs_mem.c:107/:111 (x,y & 2047).
- Second copy of the 32/16-bit layout in gs_draw_swz.c (:23-58, gs_fmt :96 of gs_draw.h, gs_swz_addr
  gs_draw.h:110) self-checked against gs_mem at gs_swz_init :95 (check_layout :75) -> gs_swz_fast; if 0 every
  frame/Z access uses gs_mem_read/write and deferred drawing is off (deferred() gs_draw_pixel.c:757).
- Page tracking: gs_page_dims :126, gs_rect_pages :135 (512-bit set), gs_mark_rect :157 bumps
  gs_page_gen[p]/gs_vram_serial only for pages some cached texture references (gs_page_texrefs).
- CLUT: gs_mem_tex.c clut[512] halves :17, clut_load :28 (CSM1 16x16 with bits 3/4 swapped / 8x2; CSM2
  line at COU*16,COV), CLD rules + CBP0/CBP1 gs_clut_on_tex0 :57, gs_clut_lookup :87, gs_tex_fetch :91
  (all PSMs, TEXA expand16/24 :70/:76). Replay restores the CLUT by a 16x16 CT32 upload + TEX0 CLD=1
  (gs_replay.c:53-66).
- Transfers gs_mem_xfer.c: state xf :17; host->local packed stream put_pixels :30 (CT32/T8 fast), split
  pixels carried across calls (:165), 4-bit nibble pairs; local->host get_pixels :55 / refill_out :194 /
  gs_mem_xfer_read :210 (zero-fills when idle); local->local :83 immediate, TRXPOS.DIR order, word copy for
  CT32/Z32. gs_mem_xfer_start :114 decodes BITBLTBUF/TRXPOS/TRXREG.
- Texture sampling (gs_draw_tex.c): two paths selected in gs_tex_bind :345.
  (a) DIRECT (kind>=0, normal): direct_setup :613 -> texels read from gs_vram (or gs_vram_shadow for feedback,
  gs_tex_use_snapshot :384) by gs_tex_raw/gs_tex_texel (gs_draw.h:187/:212) using raw_layout tables
  (t8_tab/t4_tab probed from gs_mem at startup probe_t8 :503 / probe_t4 :524, 32/16-bit from gs_draw_swz),
  verified per (PSM, TBW parity) with random data verify_raw :545 in gs_tex_fast_init :586 (raw_ok[64][2]).
  Indexed kinds use palette snapshots: palette_update :632, PAL_SLOTS 2048 ring with per-slot batch serial
  (pal_serial) so deferred drawing keeps sampling the right CLUT; identical consecutive palettes share a
  slot. Pages read: gs_tex_read_pages :660 (wrap modes via axis_ranges :722).
  (b) CACHE (kind -1: unverified layout, TBW 0, or !gs_swz_fast): tex_entry :29 (512 entries, 384 MB budget),
  lazily decoded 8x8 tiles decode_tile :675 into linear 0xAABBGGRR; indexed entries keep raw indices + a
  palette probed per index position (pos[256]); invalidation per page rectangle reset_written :111 on
  gs_page_gen; TILE_STALE + graves :250 keep buffers alive while deferred batches sample them.
  CLAMP decode axis_params :217 (REPEAT/CLAMP/REGION_CLAMP/REGION_REPEAT -> lo/hi/msk/fix).
  gs_tex_validate :363 re-checks per primitive; gs_tex_note_use :736 pins entries/palettes to a batch serial.

### 3 Display and presentation
- Privileged regs gs.c:18 enum P_PMODE..P_BGCOLOR (16-byte spaced, raw u64), gs_priv_access :84 (CSR :99,
  IMR :100, BUSDIR :101, SIGLBLID :102; DISPFB change counts a flip for PS2_PERF :121; EXTWRITE logged only).
- Snapshot: gs_priv_snapshot :184 = priv[] + gs_vsync_count (S_VSYNC). gs_display_snap (gs_draw.h:38) adds
  gs_fb_yfrac/psm (field state) taken in stream order by gs_display_snapshot :259.
- Compose gs_compose :289: PMODE EN1/EN2/MMOD/SLBG/ALP; decode_circuit :138 (DISPFB FBP/FBW/PSM/DBX/DBY,
  DISPLAY DX/DY/MAGH/MAGV/DW/DH); union of both circuits' DISPLAY rects divided by ref circuit MAGH+1/MAGV+1
  -> W x H (<=1024). read_row :150 reads VRAM rows (fast swizzle or gs_mem_read) as 0xAARRGGBB; rows reused
  for line doubling/MAGV; per-pixel alpha blend mix :173 (MMOD: ALP or circuit-1 alpha, 0x80=1.0) over
  circuit 2 or BGCOLOR. SMODE2 INT|FFMD==3 => field_mode: row index >>1 (:347) then deinterlace :211:
  weave with previous field of the other parity where static (px_close +-6), average where moving; uses
  gs_fb_yfrac (0 or 8 = XYOFFSET.Y half-pixel) to decide parity. Static state => pictures composed strictly
  in order (one in flight, gs_thread.c:183-190).
- Entry points: gs_display :250 (sync path: gs_sync + snapshot + gs_display_regs :280 which does
  gs_draw_sync_why(DISPLAY)); async: K_VSYNC -> vblank_picture gs_thread.c:206 -> gs_draw_queue_job(picture_run)
  with gs_display_pages :266 as its read set; picture_run :197 composes into a static 1024x1024 frame and calls
  present(frame, ok, w, h, vblank) then picture_done (vsync_done++ wakes EE).
- Output picture: 32-bit BGRX, stride 1024, typically 512x448 or 640x448 (window.c:35 FRAME_MAX 1024).
- window.c: window_present :421 copies into a triple buffer (fb_pix[3] :399, index swap under SRWLOCK,
  fb_event). Window thread window_thread :986 (own message loop, per-monitor DPI, ES_DISPLAY_REQUIRED),
  MsgWaitForMultipleObjectsEx loop :1026 -> pump_frame :876 -> render :850. D3D11: d3d_init :667 (d3d11.dll +
  d3dcompiler_47.dll loaded at runtime, FL 11_1..10_0, HW then WARP), shaders :549 (fullscreen triangle from
  SV_VertexID, PS swizzles .bgr), sampler linear/clamp :707, swap chain d3d_create_swapchain :599
  (FLIP_DISCARD -> FLIP_SEQUENTIAL -> blit, 2 buffers, R8G8B8A8, SetMaximumFrameLatency 1, NO_ALT_ENTER),
  d3d_upload :741 (DYNAMIC texture Map/WRITE_DISCARD, recreated on size change), d3d_render :775 (resize on
  client change d3d_resize :728, clear black, letterbox :282 viewport 4:3, Draw(3), Present(1,0)).
  Device-lost recovery :864 (3 tries then GDI). GDI fallback gdi_render :809 (StretchDIBits HALFTONE +
  DwmFlush). Fullscreen = borderless toggle_fullscreen :884 (Alt+Enter/F11/Esc). Default client 1280x960
  place_window :965. Aspect is hard-wired 4:3 in letterbox(); the frame is always stretched to it (no
  pixel-aspect handling for 512-wide frames beyond the stretch). Title shows fps from window_present count.
- Threads summary: EE thread (game, VIF/VU, GIF, queue producer) | GS thread (gs_exec_*, binning) |
  N pixel workers (bands, compose+present job) | window thread (D3D present) | host timer | audio.

### 4 Seams for a GPU backend, tests, replay harness
- Frozen: hw.h (gs_priv_access, gs_write_reg, gs_image_write, gs_image_read, gs_vsync, gs_display,
  gs_thread_start/gs_async/gs_queue_vsync, window_present/window_init/window_get_input) and gs_internal.h
  (gs_vram, gs_mem_read/write, gs_tex0/texa/texclut, gs_clut_on_tex0, gs_tex_fetch, gs_clut_lookup,
  gs_mem_xfer_*). gs_draw.h is the gs-draw owner's internal header (not frozen) and gs_replay.c + the
  unit tests use only: GS_INIT, gs_exec_write_reg, gs_exec_image_write, gs_tex_flush_all, gs_thread_start,
  gs_write_reg, gs_image_write, gs_priv_access, gs_vsync, gs_queue_vsync, gs_display, gs_sync, GS_PRIV_SNAPSHOT.
- Seam A (lowest risk, "GPU rasterizer behind the same stream"): keep gs_thread.c + gs_draw.c register
  decoding/vertex queue (reg[], env_update, vertex, draw dispatch) and replace the rasterizer+pipeline
  (draw_sprite/triangle/line/point -> gs_batch_* in gs_draw_pixel.c) and the texture binder (gs_draw_tex.c)
  with a backend that records primitives with the gs_prim snapshot (already a self-contained, versioned
  state struct: FRAME/ZBUF/TEST/ALPHA/fog/tex levels/scissor/dimx) into GPU vertex/draw lists. The
  existing per-primitive page-set machinery (tex_prepare -> gs_draw_set_reads: reads vs feedback snapshot
  pages; mark_written; trxdir page waits; tex0_write CLUT page waits) is exactly the hazard information a
  GPU backend needs to decide render-target/texture barriers and copies. VRAM would stay authoritative in
  gs_vram for transfers/CLUT/readback; a GPU backend needs (1) upload of host->local transfer rects
  (gs_exec_image_write/HWREG, local->local), (2) download for local->host (TRXDIR 1, gs_exec_image_read)
  and for gs_compose if compose stays on the CPU, (3) a VRAM-page residency model so the CPU view and GPU
  view stay coherent (page sets already exist: gs_rect_pages/gs_tex_read_pages/gs_display_pages).
- Seam B (whole GS): replace gs_exec_write_reg/gs_exec_image_write/gs_exec_image_read/gs_draw_reset,
  gs_display_regs/gs_compose and gs_draw_* (keep gs.c priv regs/CSR and gs_thread.c queue as is). gs.c's
  gs_display_regs calls gs_draw_sync_why and gs_compose reads gs_vram directly (:159), so a GPU compose
  must either produce the 32-bit BGRX frame for window_present or hand window.c a GPU texture (new seam C).
- Seam C (presentation): window_present(bgrx,w,h,stride) is the only input to window.c; d3d.dev/ctx are
  static in window.c (:565) and the window lives on its own thread, so a GPU renderer must either share
  the device (create it earlier, before window_thread) or present via a shared texture; letterbox()
  (4:3) and d3d_upload are the places for native-resolution/widescreen output.
- Tests: tests/unit/gs-draw/run.ps1 builds gs.c+gs_draw*.c+gs_thread.c+gs_record.c+debug.c with (1) fake
  gs_mem (documented layout, fast paths on), (2) same with -DTEST_ASYNC (GS thread + 3 workers, synced
  reads), (3) -DFAKE_LINEAR (forces slow gs_mem path), (4) real gs_mem*.c; test_gs_draw.c scenes
  (t_sprite_coverage :184, t_fill_rule :206, t_gouraud :253, t_clut_textures :277, t_bilinear :352,
  t_blend :378, t_scissor_offset :470, t_alpha_tests :499, t_fog_tfx :542, t_frame16 :588, t_perspective
  :632, t_mipmap :657, t_texture_cache :700, t_lines_points :758, t_csr :810, t_display :861,
  t_deferred_hazards :927, t_partial_invalidation_clut :968, t_direct_order :1002, t_display_weave :1132,
  bench :1214, t_region_clamp :1328, t_formats_contexts :1363, t_stq_negative_coords :1430,
  t_coverage_reference :1469, t_texture_cache_random :1518; main :1560) write PNGs to build/unit/gs-draw.
  tests/unit/gs-mem (layout, transfers, CLUT; gs_mem*.c only), tests/unit/window (letterbox, pad mapping,
  triple buffer, D3D resize recovery, interactive PrintWindow check), tests/unit/gif (tag parsing, paths).
- Recording/replay: PS2_GS_RECORD=<file> (_FROM default 600, _FRAMES 60) gs_record.c:44: header "GSREC001"
  + 4 MB VRAM + 1 KB CLUT + 0x64 regs (state at first VBlank after gs_sync), then records {u32 tag,u32 len}:
  1 reg write (u64), 2 IMAGE bytes (pad 4), 3 VBlank + 16 u64 priv snapshot. SIGNAL/FINISH/LABEL not
  recorded (:34). scripts/gs_replay.ps1 compiles runtime/gs*.c (+gs_mem*.c, debug.c) of any -Runtime dir
  with scripts/gs_replay.c -> gs_replay.exe <rec> <outdir|-> [workers]; gs_replay.c restores CLUT (:53),
  regs without side effects (:70), VRAM, then feeds gs_write_reg/gs_image_write and at tag 3 writes the
  priv regs via gs_priv_access(0x12000000+16*i) :112, gs_vsync, then gs_queue_vsync (workers>0, present
  callback save -> frame_<vblank>.png via debug_write_png) or gs_display synchronously. Prints ms/VBlank.
  scripts/compare_frames.py <dirA> <dirB> [--tol N]: pure-python PNG decode, per-frame differing-pixel
  count + bbox, "N/M identical". => a GPU backend can be run offline on the 13 recordings and diffed
  pixel-exactly (or with tolerance) against the software renderer with no game/EE involved.
- Recordings (read in place): fix-perf/build/{rv_prompt,rv_intro,title,brief}.gsrec, ep3/build/gsrec/
  {hub,recon_int,rope,brief}.gsrec, ep5/build/gsrec/hub_street.gsrec, ep6/build/gsrec/rc_battle.gsrec,
  ep7/build/boss.gsrec, ep8/build/gsrec/{hub,boss}.gsrec.
