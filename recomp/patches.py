"""Instruction patches: native C inserted at one instruction of a recompiled function.

Native overrides (recomp/overrides.py) replace a whole function; entry hooks (recomp/hooks.py) let a
debug tool see a call. A patch is the smallest change of the three: a few C statements the
recompiler emits right before (or right after) one instruction of the translated function, for
features the PC port adds to the game itself - widescreen (runtime/aspect.c) is the first user.
The game's code stays as it is; nothing in guest memory is modified by poking.

Each patch is guarded like the other tables, by a hash of the containing function's original
bytes [start, end): another executable (another region, the synthetic test ELF) gets no patch.
A patch must leave the game exactly as it was when its feature is off (e.g. 4:3 selected), so the
C code tests the runtime setting first and changes nothing otherwise.

Placement rules (checked by the recompiler, recomp/codegen.py):
- "before": the code runs on every path that reaches the instruction (after its label, before the
  instruction, and for a branch or jump before its delay slot too).
- "after": the instruction must be a plain one (no branch, jump or delay slot); the code runs after
  it, on the fall-through path only.
See notes/recompiler-design.md ("Instruction patches") and notes/widescreen.md.
"""
from dataclasses import dataclass

from .overrides import function_sha1


@dataclass(frozen=True)
class Patch:
    name: str
    func: int          # start of the function that contains `at`
    sha1: str          # of that function's original bytes [start, end)
    at: int            # address of the instruction
    when: str          # "before" or "after"
    code: str          # C statements; `ctx` is the guest context
    reason: str


# ---- widescreen (runtime/aspect.c, notes/widescreen.md); every patch is a no-op at 4:3 ------------

CM_RECALC = (0x0014DB70, "a5e0071f09d2ab6393d169c298376bc420f63da7")     # camera: frustum from FOV/aspect
FRAME_DRAW = (0x00157FA0, "3f04dc1f6453a3e67b0819ad931ccfd5f8c146ed")    # frame: sorted render list -> DMA chain
RENDER_ALLOC = (0x001CD998, "63ce148130315b99fedecd0f519809d70bbf643c")  # allocate a render-list entry
CM_PROJECT = (0x0014F308, "cc5768df6f4e07553979ba8ad0e9d47a88160e3f")    # world point -> screen [-1, 1]
CM_UNPROJECT = (0x0014F268, "a51f27e60bf66bc9db7b3c9a54ff5409f03b8db9")  # screen [-1, 1] -> world point
WORLD_LABEL = (0x001CD780, "ae9904c7f2fc8c1f1c4aef1f72d62aaf10898610")   # text label at a world point
MOVIE_PLAY = (0x001B3BD0, "7eee24abd101fcdd6690561742906d524523e157")    # play one movie (sceMpeg loop)
SCENE_DRAW = (0x00213300, "be7f4567e360dea5be27cff355abccd397b3d7f9")    # 2D scene: draw (vtable method)
DL_QUEUE = (0x0021A998, "67ef1d52df4bab0890b10adcafb8f699de8ba55a")      # 2D: queue a display list's items
ITEM_QUEUE = (0x00215960, "9e5c686306164bd9a4c1fdd5fe87f35fdffea152")    # 2D: queue one placed object
MAIN_LOOP = (0x001A7518, "a1bf2357cbfd44da199858e87f269865b5ea7ea0")     # the game's main loop
MOTION_BLUR = (0x00160F98, "fb6879611634e74a6c18e8c7bda1b2f7eeb43407")   # the motion blur post effect
TEXT_LOAD = (0x0013DF80, "8dda0003180328c95a6d2446ee5f52e2f2813477")     # read a level's text bank
CARD_UPDATE = (0x001AB1A0, "1d6ccadcbdefdd6b7e8f0f0d7473c824222ccccb")   # memory card manager, per frame
OPTIONS_BUILD = (0x001B04E0, "6dbf069bc55ffc47d8e36a035831e849f83a9ed9")  # build the Options menu's items
# (pc-menu) the pause menu (notes/pc-menu.md)
PAUSE_LIST = (0x001B0290, "ceb3e45c987b7505b4ec7388628233baf720d7cf")    # build the pause menu's entries, push page 1
TEXT_LOOKUP = (0x0013E368, "70da33a2bc009fbe743fb29e1ef891e60916a5fe")   # text id -> string (the level's text table)
MENU_UPDATE = (0x001AEEE0, "6f518491757212ff8aaf84c5197d1753ac1bbcaf")   # the menu object's per-frame update (input)
HUD_UPDATE = (0x001D9648, "d8e9a7bdb104955a41913d2a79b514ebb10cfce0")    # the HUD manager's update (START -> pause)
TITLE_UPDATE = (0x001DD920, "3de9752327f0ff069620a43a327f6aec75eb5ffb")  # (pc-menu2) the title screen's update (START / SELECT)
# (pc-fps) the frame rate (runtime/frame_rate.h, notes/frame-rate.md)
CLOCK_TICK = (0x0014D0B8, "d711437a42a7cf2bf2ab9622a903c33ff5744981")    # the game clock: dt from the COP0 Count
VBLANK_WAIT = (0x0016F560, "b591b01bb4c0034d517f71d94b8a12f816560883")   # render thread: wait for the next VBlank
RENDER_THREAD = (0x00160B70, "ae5d4b497429d76b28da3ae24690c6b58c68d8e4")  # (pc-fps2) render thread main loop
# (pc-smooth) the display module: per-frame flip after the VBlank, the 2D flip, display init
DISP_FLIP = (0x0016F590, "2377f5a7364cb5e93071322afcfc7a2ce183597a")     # render thread: VBlank wait, half offset, draw env
DISP_FLIP2 = (0x0016F6B8, "6bcb49757d6349e31a1774b8202cdae38bb7c228")    # the same for a second draw environment
DISP_INIT = (0x0016F788, "100efdaa85761c7612174dd9172f7cdf4d1f4417")     # display init (field parity word 0x2DEDB8)

PATCHES: list[Patch] = [
    Patch("ws_projection", *CM_RECALC, 0x0014DBA4, "after",
          "if (ps2_aspect.wide) ctx->f[12].f *= ps2_aspect.proj_x;",
          "lwc1 $f12, 0x3C($s0) loads the camera's aspect (1.2987, set by the camera init at 0x14D750) "
          "for the X range (tan(fov/2) * aspect -> +0x40), the near-clip radius (+0x48) and the "
          "projection matrix (0x11C448) - which also drive the view frustum planes (0x14DC58). Widened "
          "by A/(4/3) here, the stored aspect stays the game's own"),
    Patch("ws_screen_project", *CM_PROJECT, 0x0014F36C, "after",
          "if (ps2_aspect.wide) ctx->f[4].f *= ps2_aspect.hud_x;",
          "lwc1 $f4, 0x40($s0) loads the camera's X range for 'world point -> screen position in "
          "[-1, 1]' (x = -y / (xrange * depth)). Its callers place 2D things at world points (labels "
          "0x1CD780, markers 0x202D38/0x218220, flying pick-ups 0x1D04E8) and test whether something is "
          "on screen (0x173768 via 0x173588, 0x14AA30). With the 4:3 X range the answer is the game's "
          "own: 2D content is laid out on the 4:3 screen and drawn with the HUD transform, which puts "
          "it on the world point of the wider view; on-screen tests keep the 4:3 area (the community "
          "16:9 patch's 'widescreen bug fix' at 0x14F370 does the same)"),
    Patch("ws_screen_unproject", *CM_UNPROJECT, 0x0014F2B4, "after",
          "if (ps2_aspect.wide) ctx->f[2].f *= ps2_aspect.hud_x;",
          "the inverse of 0x14F308 (screen position -> world point; 0x1D05A0 and 0x2236E8 use the two "
          "together): the same 4:3 X range, so the round trip stays exact"),
    Patch("ws_label_size", *WORLD_LABEL, 0x001CD7E0, "after",
          "if (ps2_aspect.wide) ctx->f[0].f *= ps2_aspect.hud_x;",
          "a text label at a world point is scaled by 1 / (xrange * depth): with the 4:3 X range it keeps "
          "its size next to the 3D object once the HUD transform draws it at 4:3 proportions"),
    Patch("ws_movie_begin", *MOVIE_PLAY, 0x001B3BD0, "before",
          "if (ps2_aspect.wide) aspect_hint_now(GS_HINT_ASPECT_4_3_BEGIN, 0);",
          "entry of the movie player (one movie: sceMpeg decode loop with its own GS output, called "
          "by the playlist 0x1B3B08 from the main loop's top): the pictures until it returns are 4:3 "
          "video, shown pillarboxed. Outside the frame's DMA chain, so the hint goes to the GS directly"),
    Patch("ws_movie_end", *MOVIE_PLAY, 0x001B3EB4, "before",
          "if (ps2_aspect.wide) aspect_hint_now(GS_HINT_ASPECT_4_3_END, 0);",
          "the movie player's only return"),
    Patch("ws_render_alloc", *RENDER_ALLOC, 0x001CD9D4, "before",
          "if (ps2_aspect.wide) aspect_render_entry_alloc(ctx);",
          "jr $ra of the render-list entry allocator: $v0 = the new entry, $ra = who draws it. "
          "aspect.c remembers which 2D entries are scene set-ups (drawn as is) and which are 2D "
          "content (drawn with the HUD transform)"),
    Patch("ws_render_entry", *FRAME_DRAW, 0x00158378, "before",
          "if (ps2_aspect.wide) aspect_render_entry(ctx, 0x002DEA28u, ctx->r[18].uw[0], ctx->r[16].uw[0]);",
          "in the loop over the sorted render list ($s2 = entry, $s0 = its render priority), after the "
          "render class switch and before the entry's DMA CALLs are written into the frame's chain "
          "(DMAS at 0x2DEA28): HUD_BEGIN/HUD_END hints around the 2D entries (priority 21)"),
    Patch("ws_render_done", *FRAME_DRAW, 0x001583FC, "before",
          "if (ps2_aspect.wide) aspect_render_done(ctx, 0x002DEA28u);",
          "after the last render class's end function: close an open HUD_BEGIN"),
    Patch("ws_scene_begin", *SCENE_DRAW, 0x00213300, "before",
          "if (ps2_aspect.wide) aspect_scene_begin(ctx);",
          "entry of a 2D scene's draw method ($a0 = the scene): the render-list entries allocated until "
          "it returns belong to that scene (aspect.c picks the scene's HUD anchor)"),
    Patch("ws_scene_end", *SCENE_DRAW, 0x002137E8, "before",
          "if (ps2_aspect.wide) aspect_scene_end(ctx);",
          "the scene draw method's only return"),
    Patch("ws_item_draw", *SCENE_DRAW, 0x00213408, "before",
          "if (ps2_aspect.wide) aspect_item_draw(ctx);",
          "head of the scene's loop over its queued items ($s0 = the item): its render-list entries get "
          "the anchor of the root-level object the item was queued under"),
    Patch("ws_dl_begin", *DL_QUEUE, 0x0021A998, "before",
          "if (ps2_aspect.wide) aspect_dl_begin(ctx);",
          "entry of 'queue a display list's placed objects' (recursive through nested sprites): nesting "
          "depth, so aspect.c knows the root-level objects (one per HUD widget)"),
    Patch("ws_dl_end", *DL_QUEUE, 0x0021AA54, "before",
          "if (ps2_aspect.wide) aspect_dl_end(ctx);",
          "its only return"),
    Patch("ws_dl_object", *DL_QUEUE, 0x0021A9F8, "before",
          "if (ps2_aspect.wide) aspect_dl_object(ctx);",
          "head of its loop over the placed objects ($s0 = the object)"),
    Patch("ws_dl_queue_mask", *DL_QUEUE, 0x0021AA24, "before",
          "if (ps2_aspect.wide) aspect_item_queued(ctx);",
          "a mask-end object is queued ($a1 = the object)"),
    Patch("ws_item_queue", *ITEM_QUEUE, 0x002159CC, "before",
          "if (ps2_aspect.wide) aspect_item_queued(ctx);",
          "a shape or text object is queued into the scene's item list ($a1 = the object)"),
    # Graphics settings (runtime/gfx.c, notes/settings.md): motion_blur = off is the game's own switch.
    Patch("motion_blur_off", *MOTION_BLUR, 0x00160FA0, "after",
          "if (ps2_motion_blur_off) ctx->r[3].ud[0] = 1;",
          "lw $v1, -0x14DC($v0) loads the word at 0x2DEB24, which the motion blur function (f_00160F98: "
          "previous frame blended into the new one, FIX 48, the 16 32x222 sprites of notes/wip/d3d11-census.md) "
          "tests first: nonzero returns at once (bne -> 0x161218), before any GS work. Nothing in the "
          "game's code writes that word by address (a debug switch); the community 'No Motion Blur' "
          "patch nops this load instead, which leaves $v1 at whatever the caller had. motion_blur = off "
          "makes the load read 1: the game's own off path, nothing else changes"),
    # (pc-smooth, notes/wip/pc-smooth.md) field rendering in progressive output: the "shaking" of every still picture
    # (menus, the safehouse, the pause screen) and the 30 Hz flicker of horizontal edges and shadows. Sly 2 draws each
    # frame for the field it will be shown in: after the VBlank wait the render thread stores half = !CSR.FIELD in
    # 0x2DEDB4 and passes it to sceGsSetHalfOffset (0x240AE0: draw environment, 0x800, 0x800, half), which puts the
    # frame half a native line higher (XYOFFSET.Y + 8/16) on odd fields - right on a TV, whose odd field is half a line
    # lower. A progressive picture has no fields: at 60 fps every other frame was half a native line (one output line)
    # higher - a one-line bob at 60 Hz (measured: 20-31% of the pixels of a still scene changing every VBlank, the
    # buffers' contents one sample row apart at 1x..3x). At 30 fps all frames got the same field and nothing moved,
    # which is why it showed in menus and still scenes more than in play.
    Patch("field_offset_flip", *DISP_FLIP, 0x0016F63C, "after",
          "if (ps2_progressive) ctx->r[7].ud[0] = 0;",
          "lh $a3, -0x124C($s1): half (0x2DEDB4) for sceGsSetHalfOffset of the next frame's draw environment: 0 in "
          "progressive output (every frame drawn as the even field)"),
    Patch("field_offset_flip2", *DISP_FLIP2, 0x0016F734, "after",
          "if (ps2_progressive) ctx->r[7].ud[0] = 0;",
          "lh $a3, -0x124C($a2): the same for the second draw environment (0x2DEBE0)"),
    Patch("field_offset_init", *DISP_INIT, 0x0016F824, "after",
          "if (ps2_progressive) ctx->r[7].ud[0] = 0;",
          "lh $a3, -0x124C($t1): the same at display init"),
    Patch("motion_blur_progressive", *MOTION_BLUR, 0x00161134, "after",
          "if (ps2_progressive) ctx->r[18].ud[0] = 8;",
          "movn $s2, $zero, $v0: $s2 is the V of the blur sprites' top edge (0x10 = 1.0 texel; 0 when the field "
          "parity word 0x2DEDB8 is odd), U is +0.5 (texel centre). The GS samples at the pixel's top-left corner, so "
          "V 1.0 / 0.0 is half way between two rows: the previous picture (drawn for the other field) is blended in "
          "half a native line up or down - in place on a TV, where the two fields are half a line apart. With every "
          "frame drawn for the same field (field_offset_*) the previous picture belongs in place: V 0.5, the texel "
          "centre, for both buffers. Interlaced output keeps the game's offsets"),
    # Not a ws_ patch: it must also run at 4:3 (to switch to a wide aspect); it does nothing unless
    # the player asked for a switch (F7/F8), and a switch to 4:3 leaves the game's own numbers.
    Patch("aspect_switch", *MAIN_LOOP, 0x001A75A8, "before",
          "if (ps2_aspect_pending) aspect_apply(ctx);",
          "main loop, right after the camera's per-frame update (jal 0x14D470) and before anything of "
          "the frame is drawn (0x1CDA80, 0x157FA0): a run-time aspect / HUD switch asked for by "
          "window.c takes effect here, for the whole frame; aspect_apply recomputes the camera's "
          "frustum with the game's 0x14DB70"),
    # PC wording (runtime/pc_text.c, notes/pc-text.md): no PS2 memory-card texts, no boot prompt.
    Patch("pc_text_bank", *TEXT_LOAD, 0x0013E0A4, "before",
          "pc_text_bank_loaded(ctx, ctx->r[21].uw[0]);",
          "the level's text bank has just been read ($s5 = the world 0x3DD490: +0x4640 count, +0x4644 "
          "{id, text} table; every path of the loader joins here, before its last call 0x210BE0): the "
          "PS2-specific texts (memory card, console, disc) are replaced in place with PC wording, each "
          "only when its id and exact original bytes match"),
    Patch("pc_save_boot_poll", *CARD_UPDATE, 0x001AB1DC, "before",
          "if (pc_save_boot_pending) pc_save_boot_poll(ctx);",
          "the card manager's per-frame update ($s0 = 0x2E65E0), past its 'world loading' / 'paused' "
          "returns and before the 'card job running' and dialog-mode returns: when the boot's silent "
          "save-data creation (below) has finished with an error, the game's own 'could not create' "
          "dialog (0x17) is shown, as after a failed Yes"),
    Patch("pc_save_boot_create", *CARD_UPDATE, 0x001AB360, "before",
          "if (pc_save_boot_create(ctx)) goto L_001AB380;",
          "the boot check (dialog mode 1) found a card without the game's save data (state 3) and is "
          "about to ask 'Do you want to create a Sly 2 Save File?' (0x1ACB58 dialog 0x15). On PC the "
          "save data is created without asking: pc_save_boot_create starts the game's own create job "
          "(0x1AAF90 with the PMF at 0x2A6D88, what Yes does in 0x1AD168) and the prompt is skipped "
          "(L_001AB380 = this function's epilogue). Once per run; otherwise the game asks as before"),
    Patch("pc_options_hide", *OPTIONS_BUILD, 0x001B058C, "before",
          "{ uint32_t k_ = ps2_r32(ctx, ctx->r[16].uw[0]); if (pc_options_hidden(k_) || pcmenu_option_hidden(k_)) ctx->r[4].ud[0] = 0; }",
          "the Options menu (title SELECT menu and pause menu: both build it here) copies the items of "
          "the static table 0x2BFEC8 ($s0 = the item; word 0 = its kind = text id) that pass a test "
          "($a0 != 0, tested by the beq here). PS2 hardware options are left out on PC: 'USB Headset' "
          "(0x50, the PS2 USB headset) and 'Center Screen...' (0x3F, moves the picture on a CRT TV); (pc-polish) and the "
          "items the PC Options menu has (menu_game.c pcmenu_option_hidden: Speakers, Music / Effects Volume, Invert "
          "Binocucom / Turret, Vibrations - set there through the game's own getter / setter)"),
    Patch("pcmenu_options_list", *OPTIONS_BUILD, 0x001B05E4, "before",
          "pcmenu_options_list(ctx);",
          "(pc-polish) after the item loop ($s1 = the end of the copied list at 0x2F6470, room for 11): 'Options' (the PC "
          "Options menu) and 'Quit to Desktop' are appended to the page (the title screen's SELECT menu), before it is "
          "pushed with the count $s1 - 0x2F6470"),
    # The PC Options menu (runtime/menu_game.c, notes/pc-menu.md). Always on: the pause menu gets a "PC Settings"
    # entry; the other patches do nothing unless the menu asks for something.
    Patch("pcmenu_list", *PAUSE_LIST, 0x001B0490, "before",
          "pcmenu_pause_list(ctx);",
          "jal f_001AF4D8 (push the menu's page 1) with $a1 = the entry count and $a2 = the list the function just "
          "filtered from its 7 templates into 0x2F65D0 (full at 7, read by nothing else: the page keeps the pointer). "
          "menu_game.c copies it to unused EE RAM (0xFF000) with a 'PC Settings' entry after 'Options' (text id "
          "0x10000, no callbacks, flags 2 = stay open) and passes the copy"),
    Patch("pcmenu_text", *TEXT_LOOKUP, 0x0013E368, "before",
          "if (pcmenu_text(ctx)) return;",
          "entry of the text lookup (a leaf, $a1 = text id, $v0 = the string): the entry's id 0x10000 (the game's own "
          "ids are below 0xCB3) returns 'PC Settings' (EE 0xFF200); the menu draws it in the game's own font"),
    Patch("pcmenu_select", *MENU_UPDATE, 0x001AF154, "before",
          "pcmenu_select(ctx);",
          "Cross on the highlighted entry of an idle page ($s0 = the entry, before its select callback is loaded): "
          "ours opens the PC Options menu; its flags keep the pause menu open underneath"),
    Patch("pcmenu_start", *HUD_UPDATE, 0x001D96EC, "before",
          "if (pcmenu_pause_req) pcmenu_start(ctx);",
          "after f_00187D80(pad, START) returned whether START was pressed ($v0), where the HUD manager opens the "
          "pause menu (jal 0x1B0290): Esc / the PS button pause the game exactly as START does, only where START would"),
    Patch("pcmenu_title", *TITLE_UPDATE, 0x001DD964, "before",
          "pcmenu_title_frame(ctx);",
          "(pc-menu2) the title screen's update, before it asks f_00187D80(pad, START): the title is up (Esc there opens its "
          "menu: the game's Options page f_001B04E0(0), exactly what its SELECT check below does)"),
    Patch("pcmenu_frame", *MAIN_LOOP, 0x001A75A8, "before",
          "pcmenu_ee_frame(ctx);",
          "main loop (as aspect_switch): the music / effects volumes chosen in the menu go into the game's sound "
          "manager with the game's own 0x1FEA60, as its Options page does (and again when a loaded save resets them); a menu "
          "waiting for the pause menu opens"),
    # The frame rate (runtime/frame_rate.h, notes/frame-rate.md): both do nothing at frame_rate = 60 (the default).
    Patch("frame_dt_min", *CLOCK_TICK, 0x0014D120, "after",
          "if (ps2_frame_dt_min > 0.0f) ctx->f[2].f = ps2_frame_dt_min;",
          "the game clock (f_0014D0B8, once per main-loop frame: a0 = the clock at 0x2DDED0) turns the COP0 Count ticks "
          "since its last call into seconds (x 1/294.912 MHz) and clamps them to [$f2, $f1] = [1/60, 1/30] (mtc1 here "
          "loads 1/60 = 0x3C888889 into $f2, used only by the clamp at 0x14D134/0x14D140). Above 60 frames per second "
          "the lower bound would make every frame a 1/60 s step (120 fps = double speed): the frame limiter's rates "
          "need it lower (frame_rate.c: 1/625 s; real steps come from the Count). The upper bound stays"),
    Patch("frame_vblank_wait", *VBLANK_WAIT, 0x0016F560, "before",
          "{ uint32_t t_ = ps2_r32(ctx, 0x002DE1A0u); frame_vblank_request(t_ >= 0x100000u && t_ < 0x1FF0000u ? ps2_r32(ctx, t_ + 0xCu) : 0u); }",
          "entry of 'wait for the next VBlank' (sets 0x2DFB58 and waits on the semaphore 0x2DFB4C that the VBlank "
          "handler 0x16F468 signals): called by the render thread (0x160B70 via 0x16F590) right after it kicked a "
          "frame. Above 60 fps the host timer raises the VBlank now (at most one per 1 / rate s); at 60 it returns at once. "
          "It is passed the vtable of the camera's target (camera 0x2DDF40 +0x260): the RC chopper class (0x2D5898) keeps 60 fps"),
    Patch("interp_frame_done", *RENDER_THREAD, 0x00160C68, "before",
          "{ void interp_frame_done(ps2_ctx*); interp_frame_done(ctx); }",
          "(pc-fps2 PROTOTYPE, runtime/interp.c, PS2_INTERP only) the render thread before it frees the frame's buffer "
          "(SignalSema 0x2DEB1C): the frame's DMA is done, its chains can run again with interpolated matrices"),
    Patch("frame_movie_begin", *MOVIE_PLAY, 0x001B3BD0, "before",
          "frame_rate_movie(1);",
          "entry of the movie player (one movie: its own decode / display loop, paced by the VBlanks it waits for - at "
          "120 VBlanks per second a movie played twice as fast, picture and sound apart): movies keep the PS2's 59.94 Hz"),
    Patch("frame_movie_end", *MOVIE_PLAY, 0x001B3EB4, "before",
          "frame_rate_movie(0);",
          "the movie player's only return: the frame rate's VBlanks again"),
    # (pc-small) movies from their decoded 640x480 picture (runtime/movie_layer.h, notes/movies.md)
    Patch("movie_layer_begin", *MOVIE_PLAY, 0x001B3BD0, "before",
          "movie_layer_ee(1);",
          "entry of the movie player: MOVIE_BEGIN into the GS stream - the GS thread takes the movie sprite's source "
          "picture for the presenters (shown at the output's resolution instead of through the 512x448 frame)"),
    Patch("movie_layer_end", *MOVIE_PLAY, 0x001B3EB4, "before",
          "movie_layer_ee(0);",
          "the movie player's only return: MOVIE_END"),
]


def patches_for(prog, start: int, end: int) -> dict[int, list[Patch]]:
    """The patches of the function at start whose bytes are the expected ones, by address."""
    out: dict[int, list[Patch]] = {}
    sha = None
    for p in PATCHES:
        if p.func == start and start <= p.at < end:
            sha = sha or function_sha1(prog, start, end)
            if sha == p.sha1:
                out.setdefault(p.at, []).append(p)
    return out
