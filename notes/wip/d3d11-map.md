# WIP: renderer-map (map of the current GS pipeline for a GPU backend)

Role: renderer-map. Branch next/d3d11, worktree C:\Users\tanih\Projects\sly2-worktrees\d3d11.
Status: reading done for gs.c, gs_thread.c, gs_draw.h, gs_draw.c, gs_draw_pixel.c, gif.c, vif.c (DIRECT,
vif1_read), ps2_runtime.c (vblank/present, gs_thread_start). Next: gs_draw_tex.c, gs_draw_swz.c, gs_mem*.c,
gs_record.c, window.c, scripts/gs_replay*, compare_frames.py, tests/unit/gs-*.

## Progress
- [x] 1 data flow EE -> GS (notes below)
- [~] 2 software renderer structure (gs_draw.c + gs_draw_pixel.c done; tex/mem/swz pending)
- [x] 3 display compose (gs.c) / [ ] window.c D3D11
- [ ] 4 seams for a GPU backend, tests, gs_replay

## Findings (file:line, worktree d3d11 @ c6cf838)

### 1 Data flow
- PATH1: VU1 XGKICK -> vu.c:298 vu_xgkick -> gif.c:247 gif_path1_kick (wraps at 16 KB, 2048 qw cap).
- PATH2: VIF1 DIRECT/DIRECTHL -> vif.c:340 path2_send / :354 direct_data -> gif.c:243 gif_path2.
- PATH3: DMA ch2 dma.c:174 -> gif.c:236 gif_path3_dma; EE FIFO stores 0x10006000 gif.c:221 fifo_write.
  Masks: GIF_MODE.M3R, VIF1 MSKPATH3 (vif.c:431 -> gif_set_path3_mask), GIF_CTRL.PSE; stall only at packet
  boundary (gif.c:168). GIF_STAT gif.c:279 (FIFO always drained; APATH only mid-packet).
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
  TRXDIR -> trxdir :892; HWREG -> image data :1025; TEXFLUSH ignored.
- env_update :165 decodes context (PRMODECONT ? PRIM : PRMODE bit 9), FRAME/ZBUF/TEST/ALPHA/SCISSOR/XYOFFSET,
  PABE, FBA, COLCLAMP, DTHE, DIMX, FOGCOL, SCANMSK into gs_prim P; zbw = fbw; records gs_fb_yfrac/psm per
  FBP (:225) for field detection; bind_textures :99 (TEX1 LOD mode -> texm 0/1/2/3, cur_level, mip TBPs via
  MIPTBP1/2 or MTBA auto); gs_prim_finalize (gs_draw_pixel.c:1248); gs_prim_version++.
- Vertex queue: vertex() :827 (XYZF/XYZ, kick vs no-kick, RGBAQ/ST/UV/FOG latched; ST low 8 mantissa bits
  dropped :836); strip/fan handling :843-866; draw() :800 dispatch. AA1 only for lines (:816 logs others).
- draw_sprite :386 (covers [X0,X1)x[Y0,Y1), flat v1 colour, U/V linear, 'local' in-place filter detection),
  draw_triangle :529 (exact int edge functions, top-left rule, double planes -> fixed rowstep, Z/colour/fog
  planes only when used, flat colour = v2), draw_line :593 (DDA, AA1 coverage = 2 pixels across minor axis,
  coverage replaces alpha), draw_point :650.
- tex_prepare :285: texel bbox -> per level: in-place local filter (no snapshot) / direct sampling reads set
  or feedback snapshot set (if texel pages intersect dest pages) / decoded cache fallback; gs_draw_set_reads.
- mark_written :375 -> gs_mark_rect (texture cache invalidation).

### 2b Pixel pipeline (gs_draw_pixel.c)
- batch of <= 2048 px / 512 segs (:24-41); stages: colour_batch :307, sample (cache sample_table :254,
  direct dsample_table :296 per GS_TK kind), tfx_batch :330 (MODULATE/DECAL/HIGHLIGHT/2, >>7), fog :371,
  tests_batch :391 (ATE+AFAIL, DATE/DATM, ZTST NEVER/ALWAYS/GEQUAL/GREATER, Z write clamped to zmax),
  write_seg :455 (PABE, blend (A-B)*C>>7+D, DTHE only for 16-bit FB, COLCLAMP or wrap, FBA, FBMSK, RGB_ONLY).
  Bilinear = 4-bit weights (:94). LOD: lod16_of :180; tex_general :189 for mipmaps.
- solid fill fast path fill_span :632 (prim->solid, :1280).
- Deferred band-parallel: :661-1201. NBANDS 32, BIN_SEGS 4096, DB_PRIMS 8192, DB_STATES 256, DB_QUEUE 6.
  Page ownership per batch (owner[512]); assign_band :1126 -> -1 (two bands) / -2 (WAR: writes a page read
  earlier in batch) -> db_submit. register_prim :1075: RAW (reads pages written in batch), SNAP, HOLD,
  LIMIT -> new batch. Batches drawn strictly in order; bands in parallel by workers (claim_band :845).
  Shadow copy for feedback: copy_pages(copyp) at start_job :862 into gs_vram_shadow.
  gs_draw_queue_job :1015 for the VBlank picture (JOB_BAND). gs_draw_pages_touched :1025 used by transfers
  and CLUT loads. Syncs counted by reason (GS_SYNC_*), PS2_GS_STATS=1 report :781.
