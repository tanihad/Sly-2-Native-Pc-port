# Progress log

## Resume here (read first in a new session)

**2026-11-12: next/shadows2 merged (feecf9c); test build 0.1.1307 in dist\Sly2 = the 1.2 candidate (owner to playtest;
NOT released).** Branch gates (code identical after merge): reim_shadow_gate PASS on 3-run medians (ground flicker 0.68x
old, main before was 1.79x on the masked metric; stacked 2.30 -> 0.33 %; char 0.74x), gpu_diff_all 28/28, m9_exact
identical, r2_sweep 18/18, stability warm+cold PASS, gfx_stress -Cold PASS, +0.02 ms. run_tests green on main. Open owner
decisions: (1) PS2_LIGHT_SOFT_STABLE=1 for Faithful (fixed soft-edge pattern, agent recommends), (2) static-world
key-light shadows (PS2_REIM_SC_CAST=1). Release 1.2 after the owner's OK.

**2026-11-12: shadows (32a5c19), corpus (ccd9420), tex-opt (ef7c53c) merged; test build 0.1.1296 in dist\Sly2 (owner to
try; NOT a release yet).** Merged gates: gpu_diff_all 28/28 (re-recorded corpus in C:\Users\tanih\Projects\sly2-corpus),
m9_exact all identical, units 0 failed, stability PASS, gfx_stress PASS. reim_shadow_gate is noisy: world-shadow flicker
vs old 1.559 / 0.702 / 1.097 / 1.142 / 1.249x over 5 runs (median ~1.14x; also on the shadow branch's own exe), stacked
shadows and character-light flicker reliably better. Follow-up next/shadows2 (worktree sly2-worktrees\shadow2): median-of-N
gate, ground-shadow flicker below old, the game's soft shadow edge noise crawl. Release 1.2 after that + owner's playtest.
Open owner decision: static-world (walls / roofs / trees) key-light shadows (PS2_REIM_SC_CAST=1), off today.

**2026-11-11: both merged into main (82e0535 tex-prebuild, c0a7fb5 local-lights); test build 0.1.1267 packaged in
dist\Sly2 for the owner to try (not a release). Merged-tree gates running (units, gpu_diff_all, reim_stability_gate,
gfx_stress_gate -Cold). Next queued: lossless-looking optimisations (BC7 AI textures, fp16 AI builder, optional
one-time full texture build, dedupe check), each proven by picture comparison.**

**2026-11-11: next/tex-prebuild (worktree sly2-worktrees\ai, merged 82e0535) - Reimagined AI textures ready before first
sight.** Cached AI images go straight onto textures (no Enhanced image first), loader threads, per-level texture lists
preloaded at each level change (title: also the last-played level, so Resume Game starts ready), AI images never
swapped in while their texture is on screen (deferred until out of sight). ep2 hub, owner's settings: warm cache 354
Enhanced -> AI swaps after the level appeared (349 visible) -> 0; cold 39 visible -> 0; level change ep2 -> ep1 hub 321
visible -> 0; frame times and load time unchanged. Details + table: notes/reimagined.md "Ready before first sight".
Videos build\prebuild\videos (worktree). Switch PS2_AI_READY=0 = old flow.

**2026-11-11 (later): laptop fix confirmed by the owner. Started his next picks (1 then 2):** texture pre-build / no
pop-in (worktree sly2-worktrees\ai, branch next/tex-prebuild, checkpoint notes/wip/tex-prebuild.md) and local lights
(worktree sly2-worktrees\light, branch next/local-lights, notes/wip/local-lights.md), both from main e656907. Later
picks on the list: HD HUD/menus, water/wet reflections, HDR output, smoother characters, new character detail
(needs a download approval).

**2026-11-11: 1.1.1 RELEASED** - "Sly 2 PC 1.1.1", tag v0.1.1247 (190981d), Latest; repo renamed by the owner to
tanihad/Sly-2-Native-Pc-port (old URL redirects; launchers fetch 0.1.1247 through it). Published assets re-downloaded
and hash-checked. Waiting for the owner's laptop result (`[gpu] adapter:` line). TODO (low): point the launcher's
update URL and make_release's base URL at the new repo name in a later release.

**2026-11-11: 1.1.1 candidate 0.1.1247 (main 190981d) built.** Laptop fix: the game ran on the
integrated GPU of the owner's RTX 5070 laptop (slow + pixelated); now the high-performance GPU (notes/d3d11-renderer.md
2.1). Setup sha256 C8C37307..., bundle build\release\0.1.1247 signed, check_no_game_data 0 problems. Owner tests on the
laptop; publish (tag v0.1.1247 -> 190981d) only after his go. Ask him for the `[gpu] adapter:` line of the laptop's log.

**2026-11-11: 1.1 RELEASED** - "Sly 2 PC 1.1", tag v0.1.1237 (9328406), Latest, repo public. 5 assets verified by
SHA-256 (Setup f8aa9fb7..., bundle a21f2ff6...); releases/latest/download/sly2-release.txt and the bundle URL answer
200 unauthenticated, so installed launchers self-update. Next: the owner picks the next improvements (list in the
2026-11-08 entry below).

**2026-11-10: release candidate 0.1.1237 built (main 9328406), waiting for the owner's test.**
Look-switch crash fixed (next/reimagine-crash 605be08: the Reimagined cache list overflowed its buffer on every
re-enable; gfx_stress_gate 0 crashes in 6873 changes) + Setup/update carry the x4plus model (next/setup-model).
Gates on main: run_tests, 28 unit suites. Setup build\installer\Sly2-Setup.exe (--art, 62.9 MB, sha256 F8AA9FB7...),
check_no_game_data 0 problems (Setup and bundle); bundle build\release\0.1.1237 signed with the real key. After the
owner's go: GitHub release v0.1.1237 (name from the owner) via his Windows Chrome, Latest, attach Setup + SHA256SUMS.
The tag must point at 9328406 (the built commit), not the later progress commits.
Owner approved ("it works great, this is version 1.1"). Tag v0.1.1237 -> 9328406 pushed; DRAFT "Sly 2 PC 1.1" made in his
Windows Chrome with sly2-release.txt, .sig, SHA256SUMS.txt. Chrome uploads cap at 10 MB, so the owner drags in
Sly2-Setup.exe + sly2-source-0.1.1237.zip; then publish as Latest. 2026-11-11: the owner made the repo PUBLIC (history scanned: no keys/tokens), so launchers
self-update from releases/latest: the bundle zip MUST be attached before the release is published as Latest.

**2026-11-08 (paused by the owner until Tuesday 2026-11-10): owner build dist\Sly2 0.1.1226 = main 6bbf81a**
(next/reimagine2 merged: no fire shafts, tree-shadow world-matrix fix + static/alpha-tested casters, CAS sharpening,
deterministic reim_stability_gate). OPEN BUG: switching Look Faithful -> Reimagined + Apply sometimes crashes
(0xC0000005 in nvwgf2umx.dll / ntdll, right after "ai: Reimagined textures on", before "x4plus net ready"; his real
logs via \\localhost\C$\Users\tanih\AppData\Local\Sly2). Lead found before the pause: ra_init_net runs on the AI worker
thread (ra_thread, RJ_INIT) and uses gpu_rt.ctx, the D3D11 IMMEDIATE context, while the render thread draws. Work
branch next/reimagine-crash (worktree sly2-worktrees\light, no commits yet). Plan: move all context use to the render
thread, safe resource lifetimes on re-config, toggle stress gate (500 toggles cold+warm, 0 crashes).
Then the owner's next-step menu (he hasn't chosen yet): 1 HD HUD/menus, 2 smoother characters (tessellation),
3 local lights from lamps/windows/fires, 4 water/wet reflections, 5 pre-build AI textures (no pop-in),
6 new detail on characters (needs a download approval).

**2026-11-08: main d2de0e7 = next/reimagine merged (owner asked); dist\Sly2 0.1.1214 + models\x4plus.bin.** Owner
playtest: fires cast light shafts (wrong), tree shadows flicker when walking under them, Sly looks blurry (wants more
detail), Reimagined on High + 8x MSAA is ~9.3 ms (misses 120 fps). Fix agent on next/reimagine2 (worktree
sly2-worktrees\light), MP4 evidence in build\reimagine\videos2. NOTE: games launched from the agent shell run in the
Claude app's MSIX sandbox (AppData redirected -> wrong save/settings); the owner must start dist\Sly2\sly2.exe himself.

**2026-11-07: Reimagined test build 0.1.1207 (next/reimagine 85bba3d, not on main)** in dist\Sly2-Reimagined (+
models\x4plus.bin from sly2-worktrees\light\build\models, never committed). next/reimagine = next/ai merged (AI
x4plus textures + surface maps, background cache) + Reimagined lighting (moon shadow maps, warm grade, auto exposure,
glow/shafts/fog, normal-mapped relief/cavity) + menu always offers Look = Reimagined + smooth outlines (owner approved;
modern frames only, Retro/exact untouched). All gates green run alone; perf ep1 1440p Medium idle: Faithful 4.77 ms,
Reimagined 5.85 ms. Waiting for the owner's verdict; then merge to main, package dist\Sly2, push. No release without
the owner. Open: ep3 slightly darker, no daylight scene checked; decide whether to ship x4plus.bin in releases.

**2026-11-06 (later): owner build 0.1.1178 (81cde3d)** = Enhanced textures (de-dither) + Options overhaul (Look Retro /
Faithful / Reimagined [hidden], Quality Low..Ultra, Apply for everything; footer without a count). Owner direction:
Reimagined = bold reimagining; Faithful stays as is. Running: next/ai (worktree sly2-worktrees\ai: Reimagined textures,
RealESRGAN_x4plus approved + downloaded, sha256 4FA0D389..., plus generated normal maps, interface in
notes/reimagined.md) and next/reimagine (worktree sly2-worktrees\light: Reimagined lighting/grading/shadows). A menu
agent once wrote into the main checkout via a relative path; its stray edits were discarded with the owner's OK (patch
kept in the session scratchpad). Agents must use absolute paths.

**2026-11-06: three agents running** (owner approved all): next/tex dither-grain fix (worktree sly2-worktrees\light);
next/ai AI texture upscaler prototype (sly2-worktrees\ai; owner approved the one download: Real-ESRGAN anime 6B,
tools\models\realesrgan, sha256 F872D837..., BSD-3; load the .pth with a restricted unpickler only); next/menu settings
overhaul (sly2-worktrees\menu). Owner's menu decisions: Quality Original/Low/Medium/High/Ultra (no "PS2" wording), one
"Retro look" switch for the old-TV effects, Advanced sub-page, and Apply for every change (stage -> Apply/Discard, ask
on leave, display changes keep the 15 s Keep check). Weaker-PC test: owner says done (details not given). Enhanced
textures (next/tex e1a20a9) are not in the owner's build until the grain fix lands.

**2026-11-05 (late night): Enhanced lighting merged** (941dd02 = next/light d421d57; dist 0.1.1150): AO + bloom,
per-pixel lighting + rim, soft shadow edges; "Lighting: Enhanced / Original" live, Enhanced default in Balanced/Quality;
+0.63 ms GPU at 1440p Balanced ep1. Design: notes/enhanced-lighting.md. ep1 tfo "blinking seams" were NOT a lighting
regression: the letterbox bars slide in when the scripted job-start press lands early (timing varies per run; 393ea64
fails the same way with a longer window); flicker_check now counts sliding/fading edges as motion (a6ca7d4). All gates
pass in both modes. Waiting for the owner to try it; no lighting release without the owner. Package builds run from
scripts must set PS2_USER_DIR (else they use the owner's %LOCALAPPDATA%\Sly2).

**2026-11-05 (night): 1.0.2 RELEASED** - "Sly 2 PC 1.0.2", tag v0.1.1136 (d1d7328 = merge of next/setup-art d6eb131),
Latest. Setup built with `--art C:\Users\tanih\AppData\Local\Sly2\art` (owner's decision: releases carry the owner's
official art; the art is never committed): backdrop + logo + mask-badge exe icon; check_no_game_data with the allow-list:
0 problems; bundle clean. Setup sha256 306e4a77... Running overnight (owner asleep, "run till done"): next/light
(Enhanced lighting, worktree sly2-worktrees\light; owner: ON by default in Balanced/Quality, live switch, Original =
bit-exact). Merge each verified lighting step into the owner's dist build; no release of lighting without the owner.
- Sweep on 0.1.1121+: NO DATA results were load stalls (all pass alone on a quiet machine). tfo (owner settings, 120
  fps, MFT 4x, thin) intermittently flags 1 picture in ep4/ep5/ep6/ep7: worst pictures are HUD text (ep4 scrolling hint
  ticker; ep7 cutscene fade-in with fading hint text) = detector noise on moving/fading 2D text, not 3D flicker. TODO
  (low): let flicker_check ignore the HUD layer / text bands, or lower tfo's sensitivity there.

**2026-11-05: 1.0.1 RELEASED** (owner's go): "Sly 2 PC 1.0.1", tag v0.1.1130 (8cc3725 = merge of next/launcher-art
4f4c9ae), Latest. New launcher look, owner art override (%LOCALAPPDATA%\Sly2\art, local only) > disc art (save icon
.ico, intro backdrop capture) > own theme; publisher "tanihad"; failed update checks no longer shown;
check_no_game_data.py: 0 problems in Setup and bundle. Setup sha256 9671dae1... Open: sweep NO DATA re-checks (ep4,
ep6, ep7, m2_boardroom tfo/hc) once the machine is quiet; committed game-frame screenshots exist in notes/pc-menu,
notes/pc-text, notes/m10 (private repo; owner to decide).

**2026-11-05: 1.0 RELEASED** on the owner's go: GitHub release "Sly 2 PC 1.0", tag v0.1.1121 (18e74e4), Latest, private
repo. Assets: Sly2-Setup.exe (sha256 e5b03805...), SHA256SUMS.txt, sly2-release.txt + .sig (Ed25519, key matches
installer/release_key.h; owner backed the key up), sly2-source-0.1.1121.zip. Published through the owner's Windows
Chrome (no gh CLI). While the repo is private launchers cannot self-update ("Couldn't check for updates"): the release
notes say so; owner decision pending on a public release-only host. Full r2_sweep (ab,hc,tf,tfo,mf, -Parallel 2) was
started on 18e74e4 before publishing; failures go out as 1.0.x.
- Owner roadmap after 1.0 (notes/future-ideas.md): launcher-art (running: owner's own art in %LOCALAPPDATA%\Sly2\art,
  local only, never committed) -> sharper textures -> better lighting -> settings menu overhaul. European release
  support: dropped by the owner.

**2026-11-04 (later): OWNER TEST HOLD on dist 0.1.1118 (main 0906f31).** No agents running.
- Merged since 0.1.1064: mft-cover, shot-key (F12 screenshots), p6-pgo (ThinLTO + PGO, VU v3), stray-tri (a99bef8:
  degenerate-w clipper fans kept exact, reason 6 "Q"; fixes the owner's one-frame "random lines"), r8-perf (0906f31:
  exact R2 draw plan, ~6% GPU at Balanced; 300 fps hub target not reached, remaining cost listed in modern-renderer.md).
- Combined build checked: run_tests 16/16, ep1 mf (0 pops, 0 stray triangles) + tfo pass. Full gate batch still to run.
- Open, seen by stray-tri: character meshes near the camera alternate frame to frame in close-ups (compare with Exact).
- Owner confirmed on 0.1.1118: the flicker is fine (thin-outline flicker and stray triangles both gone).
- 1.0 checklist: Modern default (D2) approved and already the default (Balanced) - done; DualSense tested by the owner,
  works well - done; weaker PC - the owner tests it; full playthrough check; back up the release key; first signed
  release only on the owner's go.

**2026-11-04: OWNER TEST HOLD.** The thin-outline flicker fix (r2-flicker) is merged as 111d8eb and shipped to the owner as dist 0.1.1064.
- Final gates: all pass except 9 sweep checks in ep7, m8_boss and ep8 (no frames during the level load), all at -Parallel 4.
  ep7 and m8_boss re-run alone pass tf and mf, so it was load stalls on an overloaded machine.
- mft-cover (2f62d02), r8-perf (837a1a1) and p6-pgo (2c52282) are stopped and pushed. Resume them on "continue".
- mft-cover found that the remaining grid stretches are mostly briefing pages and cutscenes, so it is close to done.
- If the owner still sees the flicker: ship the keep-all-chains safe mode first (+2.5 ms in ep1), then fix the cheap rule.
- Brainstorm list: notes/future-ideas.md.

**2026-11-03 (late): PAUSED by the owner.** All agents stopped. WIP commits are pushed on each branch.
- **Merged into main this round:**
  - test-flake.
  - mft-pop: 0 gameplay grid pops. The mf sweep check fixes 4 causes, probably the owner's "line that flicks".
  - qa-a and qa-b: harness and reports, no renderer bugs found.
  - Main is NOT yet re-gated or packaged. The run_tests + package.py run was interrupted, so the owner's dist is still 0.1.1032.
- **On "continue":**
  1. Run run_tests and package.py on main, deploy, and tell the owner the pop fix is in their game.
  2. Resume these branches, each from its notes/wip/<task>.md:
     - r2-flicker (dcde97a): the thin-outline flicker, the owner's main bug. It was replacing the outline length threshold with a native prediction from the previous frame. It branched before mft-pop, so merge main.
     - mft-cover (1b5049c): MFT coverage for the "target view" / ALIAS16 / m7_boss grid stretches qa-b found, plus the m7_thermals water band. It had just started. Merge main first, because mft-pop already fixed some ALIAS16 cases.
     - r8-perf (16e631f): gpu_diff_all + m9_exact pass, result table in notes.
     - p6-pgo (a581c14): gates green, was running the window/warp unit suites.
- **Owner decision pending:** Faithful filters in Modern. The flicker filter could run as a post pass. Interlacing on MFT would only imitate it and lose resolution. See notes/modern-renderer.md "MFT pops".

**2026-11-03 ~11:50: PARALLEL PUSH. The owner asked for many agents (usage until 15:00).** All worktrees branched from main 38a38df. Running:
- **r2-flicker:** the flicker remains with outlines=thin; fixing the outline path's VIF state.
- **mft-pop:** the one-frame MFT grid fallback ("other target").
- **r8-perf:** Balanced (MFT 100% MSAA 4x) GPU time, aiming at 300 fps in the hubs.
- **p6-pgo:** CPU overhead (LTO/PGO, hot paths).
- **qa-a / qa-b** (Sonnet, report-only): Modern vs Exact visual QA across episodes 1-4 and 5-8, presets and setting combos.
- **test-flake** (Sonnet): window suite F11/Alt+Enter made deterministic.

Merge in batches, with one gate run per batch.

**2026-11-03: FLICKER FIX MERGED (57c8877), dist 0.1.1032 — HOLD POINT REACHED: the owner is testing.**
- **Cause:** native world chains that were dropped no longer left the VIF1 state the exact shadow class needs (STMASK, flip parity, leftover lanes).
- **Fix:** the owed registers are written, and native chains are kept, with drawing off, only in the class run before a switch. Cost about +0.03 ms per frame.
- **New check:** the temporal flicker check (`tf`) is now part of r2_sweep.
- **Open:** MFT falls back to the grid for a single frame about once per 27k frames ("other target", a full-screen blended draw into another target). That gives a one-frame AA pop and may be the owner's "random line". Fix it if the owner still sees the line.

Do not start new work until the owner says so.

**2026-11-03: LAUNCHER MERGED (2245f36).** build\installer\Sly2-Setup.exe is now the launcher.
- **What it does:** drag-and-drop ISO; imports a 2.9 GB copy to `data\sly2.img`, so the ISO is no longer needed; Play / Repair / Previous / Uninstall.
- **Updates:** Ed25519-signed updates from the GitHub latest release, with staged swap and rollback.
- **Release key:** private key at `%USERPROFILE%\.sly2-release\release-ed25519.key`, outside the repo. The OWNER MUST BACK IT UP.
- **Release process:** notes/install.md, "Publishing an update".
- **Running:** the flicker fix (r2-flicker). The flips broke ep6, so it now keeps native meshes' chains with XGKICKs dropped; it is measuring the CPU cost, then the gates.

**PAUSED by the owner (2026-10-30).** Nothing is running.

The flicker fix is on branch next/r2-flicker (c61586e, pushed; main's R8 is already merged in).
- **Cause:** native (dropped) world meshes no longer leave the VIF1 state the shadow-class VU program depends on (STMASK etc. plus the MSCAL double-buffer flip parity). So Sly's lamp shadow went missing in Modern, and it flickered whenever entries switched between native and exact.
- **Fix:** owe the registers before the next exact chain, and apply the flips at the class switch.
- **State:** it was re-verifying (ALT heat at 3x grid, class-begin VIF state). The full gates are not yet run on the final version.
- **On "continue":**
  1. Resume the agent (or start a new one reading notes/wip/r2-flicker.md) to verify on the grid path and on MFT 100%/MSAA 4x, then run the gates.
  2. Merge, package, push.
  3. HOLD POINT: tell the owner it is ready to try.

**2026-10-30: R8 MERGED (35bfeef), dist 0.1.1001.** Modern frames: free render scale (25-200%) + MSAA off/2x/4x/8x, HUD after resolve, motion vectors (off). Presets: PS2 (=Original: Reference, integer grid, faithful filters, 4:3), Performance (Modern 75%, no MSAA), Balanced (DEFAULT: Modern 100% + MSAA 4x), Quality (100%, 8x). NOTE: this makes Modern the default for new installs (D2), pending the owner's approval. 1440p ep1: R8 100% MSAA 4x 258 fps vs 4x grid 221 / 3x grid 319. The owner reported a Modern-only shadow flicker plus a random flicking line in the ep1 hub outdoors (owner ini: render_scale auto, 120 fps, fullscreen). Running: fix agent in worktree r2-flicker (branched before R8; it must merge main). After that comes the HOLD POINT.

**2026-10-30: R7 part 3 (HUD) MERGED (8a19e4d), dist 0.1.986.** Profiling showed a native HUD would not pay off: its draws cost about 0.1 ms; the overhead is in clears. So the branch makes partial Z clears lazy instead (-0.2..-0.3 ms GPU per frame). The window suite's F11/Alt+Enter checks fail on main too; that is environmental (the owner's focused apps), not a regression. Running: R8 (worktree r2-m8); it has been told to merge main. After R8 comes the HOLD POINT below.

**2026-10-29: R5 part 2 MERGED (a3a5752), dist 0.1.977:** guard-band meshes native through the game's own VU clipper on a private VU1 (r2_clip.c). ep1 3x 285->325 fps (1% low 127->243), m2 190->228. Native vertex coverage: ep1 91%, ep3 95%, ep5 94%, m1 89%, m2 74%. Left (each <=1%): billboards, lit sub-batch variants, 0x0E30/0x16B0/0x13F0, sky/warps; 4x chaos not measured (owner was gaming). Running: native gameplay HUD (worktree r2-hud, Sonnet). Then R8 (D6 decided: free scale + MSAA, PS2 mode = preset), then the hold point below.

**OWNER HOLD POINT (2026-10-29):** once the renderer is done (R5 part 2, the native gameplay HUD, and R8 with free scale + MSAA + the PS2-mode preset), STOP: tell the owner it's ready to try on their PC and wait. Do NOT start the launcher, 1.0 items or anything else until they say so.

**2026-10-29: R6 MERGED into main.** Running: R5 part 2 (worktree r2-m5b: VU clipper for guard-band crossing triangles, remaining mesh variants). Ask the owner about D6 before R8 (MSAA, the big lever).

**2026-10-29: R6 particles done on next/r2-m6:** profiled first - particles cost GPU fill of their exact triangles (m2 steam 1.15 ms/frame, m8 0.69), not VU simulation (already recompiled on the CPU). Particle-class triangles now go through a native pipeline (hardware-interpolated attributes and Z, R2H_PART span hints): their cost halves, pictures identical in replays; plus m2's G bit-plane shadow sprites as compute passes. m2 boardroom 3x 188->205 fps (GPU 5.23->4.80 ms), 4x 120->128; m8 3x 397->431. Gates pass (sweep 22/22, m9_exact identical). Left: guard-band exact meshes, shadow volumes, R2 draw overhead; supersampling itself (R8 MSAA) is the big GPU lever.

**2026-10-29: R7 part 2 MERGED (60f8998), dist 0.1.952:** decoded texture cache + one-pass shadow pipeline (both pixel-identical): ep1 hub 3x 222->286 fps, 4x 161->195; ep3 3x 238->321; ep5 3x 261->324; m1 3x 246->284. Next: R6 particles (worktree r2-m6; fire/rain/fight scenes, m2 boardroom ~90 fps), then R5 part 2 (VU clipper, remaining variants), then R8 (ask owner D6 first).

**2026-10-29: R7 part 1 MERGED (ba01fe2), dist 0.1.944:** modern mode shadows (order-independent G counter) + compute darken/motion blur: ep1 hub 3x 209->242 fps, 4x 128->146; ep5 3x 221->261. Running: R7 part 2 (worktree r2-m7b): sky, shuffles+darken merge, R2 world fill, costly exact-path triangles. Modern mode still opt-in (Options > Graphics > Rendering or F10) until the owner approves it as default.

**2026-10-27 (night): r2-m5 MERGED (ef64cef):** hybrid tree bug fixed (stale mesh decodes during level streaming; byte-compare cache), hybrid-vs-reference check (r2_hcheck) in r2_sweep, R5 part 1 (82-90% native vertices). R5 part 2 (VU clipper for crossing triangles, remaining variants) deferred: CPU-side only, scenes are GPU-bound. Running: R7 (worktree r2-m7): profile GPU in modern mode, native shadows / motion blur / cheap fill wins to cut GPU time (~4.4-5 ms at 3x).

**2026-10-27 (later): R3+R4 MERGED (1db240e):** characters (GPU skinning + the game's lighting, oracle bit-exact) and outlines (VU outline program in C, identical packets) native; CPU stages 1.3-2 ms, every scene GPU-bound at 4-5 ms (~200 fps at 3x). Known bug on main (modern mode only, opt-in): ep1 hub tree vanishes / sign-texture shards in hybrid mode. Running: r2-m5 (worktree r2-m5): fix that hybrid bug + add a hybrid-vs-reference check, then R5 (sky, warps, remaining variants, the PS2 clipper). Then R6 particles, R7 shadows/motion blur/HUD (GPU wins). Owner (2026-10-27): keep going after usage limits reset.

**2026-10-27: modern renderer R2 MERGED (opt-in: Options > Graphics > Rendering: Modern, or F10; default Reference until the owner approves).** ep1 hub 114 -> 203 fps, ep5 154 -> 216; GPU-bound scenes unchanged; m3 boss 1% low slightly worse. Guard-band clipping and draw consolidation exist but are off (fail A/B gates). Next per plan: R3 lit/skinned characters, R4 outlines, R5 sky/warps/clipper, R6 particles, R7 shadows/motion blur/HUD (moves the remaining exact GPU work, needed for GPU-bound scenes). Waiting for the owner to test modern mode and say continue.

**2026-10-26: modern renderer BUILD started** (owner: start; be economical; Windows first, Linux/macOS last). Decisions taken (recommended defaults, owner said start): D1 'Original' = visually faithful within tolerances (pixel-exact = the existing renderer as 'Reference'); D2 modern renderer becomes default per milestone once proven (from R2); D3 perf P4/P5 dropped (P6, P7 later). Running: R0+R1 in worktree r2-m1 (branch next/r2-m1, notes/wip/r2-m1.md). Plan: notes/modern-renderer.md.

**2026-10-26: owner chose to start the MODERN RENDERER now** (before the remaining CPU perf steps). Phase 1 (research + design, worktree r2-design, branch next/r2-design): r2 RE survey (notes/wip/r2-re.md) + r2 prior art / API (notes/wip/r2-api.md) -> design doc notes/modern-renderer.md with milestones; owner reviews before building. Hybrid: new renderer for what it understands, the exact GS renderer for the rest.

**PAUSED for the owner's test (2026-10-24).** Batch merged and pushed (main 1bee7c3+): pc-small (movies straight from the 640x480 IPU picture; pause 'Controls...' -> our Controls page), perf P2 (GPU -30..35% at 3x, exact), perf P3 (frontend exact speedups 3-8%, chaos benchmark scripts\chaos_bench.ps1; parallel prepare = no-go). Gates: 25 unit suites, gpu_diff_all 28/28, pc_text_boot, m9_exact identical at S=1/S=3 vs the pre-batch baseline. dist 0.1.886 + build\installer\Sly2-Setup.exe. On 'continue': P6 in-game overhead + compiler speedups (PGO, LTO, AVX2 dispatch), P4 GS decode fast path, P5 VU1 pool, P7 Reflex, then import-once; later the modern high-level renderer (cross-platform API). Owner decisions pending: 5x6 grid at render scale 100% (not exact, -25% 4x GPU work); installer release questions (online vs offline default, where to publish, code signing, licence notices); optional 'Button layout' help page.

**OWNER DECISION (2026-10-24): later, the BIG PROJECT = a modern high-level renderer** (draw the game's own models/levels with a modern renderer instead of translating the GS stream; RT64 / Ship of Harkinian style; also the base for better graphics/mods). First finish the current batch (perf P2, P3, pc-small), pause for the owner's test, then the queued items. FSR1/CAS not wanted now.

**2026-10-24: pc-menu2 MERGED (95e3147: Esc/PS -> the game's own menus, Sly-styled Options in the game font, defaults borderless native + 60 fps).** Running: perf P3 (frontend split + chaos benchmark, worktree perf-p3), perf P2 (GPU pass costs, worktree perf-p2), installer (worktree installer). OWNER INSTRUCTION: when these three are done -> merge, test, rebuild dist, then PAUSE and let the owner test; do not start new work until they say continue. Queued for after that (owner agreed): P6 in-game overhead (stages cost 1.5-2x their offline time) + compiler speedups (PGO + LTO, an AVX2 build of the hot code with runtime dispatch), then P4 GS decode fast path, P5 VU1 pool, P7 Reflex. ALSO QUEUED (owner said yes): 'import once' - the installer (and the game) copy the game data the runtime needs from the player's disc image into the install folder (a packed data file, checked against the known disc), so the ISO can be deleted or moved afterwards; still requires the player's own copy (Ship of Harkinian style). Running in this batch too: pc-small (sharper FMV movies presented directly from the IPU output; pause-menu 'Controls...' opens our Options > Controls).

**2026-10-24: owner likes the build ('really good'); running pc-menu2** (worktree pc-menu2, branch next/pc-menu2): Esc/PS -> the game's own pause menu, its Options -> a Sly-styled settings menu (game font/colours/layout via the user's disc assets at run time), smooth menu/mouse at display refresh, default launch fullscreen at native resolution + 60 fps (exclusive 120 Hz on non-VRR 165 Hz). A desktop shortcut 'Sly 2' was created for the owner (OneDrive\Desktop). Still waiting: perf plan P2-P7 go-ahead.

**2026-10-24 (late): ALL CURRENT WORK MERGED into main (cd7c914) and pushed; dist 0.1.830; nothing running.** Merged since the last note: perf P1 (early depth, GPU -20..35%, bit-identical), pc-smooth (field-offset patches fix the menu shake + edge/shadow flicker in progressive mode; display-locked cadence 55 fps = 3 refreshes on the owner's fixed 165 Hz display; governor fixes). Waiting for the owner: go-ahead for perf plan P2-P7 (notes/perf-plan.md), VRR on their monitor, DualSense hardware test, the two open questions (gas-cloud drift, Faithful distant scenery).

**2026-10-24: pc-polish MERGED (4e2158e: Quit to Desktop, one Options menu, Resolution + Render scale, Apply/Revert). pc-research DONE (branch next/pc-research, notes/perf-plan.md):** VU1-on-GPU dropped; recommended plan H = P1 early depth (prototype: GPU -20..33%) -> P2 GPU pass costs + P3 frontend split -> P4 GS decode fast path -> P5 VU1 program pool -> P6 in-game overhead -> P7 Reflex; ~200 fps at 3x in the heaviest hub, 2-3 months; owner asked to approve the full plan. P1 running in worktree perf-p1 (branch next/perf-p1, from main + next/pc-research). pc-smooth still running.

**PAUSED by the user (2026-10-23 15:05).** Nothing is running. pc-polish, pc-smooth, pc-research were stopped early (1-2 commits each; mid-edit work saved as 'PAUSED ... unverified' WIP commits, pushed). On 'continue': resume each from notes/wip/<name>.md.

**2026-10-23 (night): owner feedback round 3** - stutter, menu shaking, minor shadow flicker, no quit, duplicate pause menu/Options, confusing internal vs actual resolution, settings not applying instantly (wants Apply), wants easy 200 fps with modern optimisation (research first: 'make sure it is the best plan'). Running: pc-polish (Quit to Desktop, one Options menu, Resolution + Render scale, Apply/Revert), pc-smooth (root causes of stutter, menu shake, shadow flicker; short batched real-monitor runs), pc-research (decision doc notes/perf-plan.md: research RT64/Unleashed/PCSX2/Dolphin/RPCS3/NVIDIA Reflex/DLSS, cost breakdown at 200 fps, prototype VU1-on-GPU compute and frontend parallelisation). Worktrees pc-polish/pc-smooth/pc-research, checkpoints notes/wip/<name>.md. Big performance work waits for the research verdict + owner OK.

**2026-10-23 (latest): pc-fps2 MERGED into main (4a0f536):** high frame rates via (a) a graphics worker thread above 60 fps + whole GIF packets to the GS thread + fast VIF unpack (EE ~2 ms/frame); the owner's interpolation idea (b) was prototyped and lost on the numbers (each in-between picture costs a full frame of VU1/GIF/GS/GPU work; game tick fell below 60) - kept behind PS2_INTERP. Results at 165: ep1 hub 3x 72->87, 2x 69->97; ep3 2x 121->160; ep5 2x 92->124; m8 boss 2x 111->145. Gates green; dist 0.1.784. Next candidates: split the GPU frontend thread's per-primitive work across cores (ep1 hub limit ~9 ms), RC chopper frame-rate independence, Intel double-precision emulation, owner's DualSense + real-monitor test.

**2026-10-23 (later): pc-fps MERGED into main (ff40358):** frame-rate setting 60/120/144/165/200/display/unlimited (real-time dt, movies at 59.94, RC chopper kept at 60), VRR status from NVAPI in the log and PC Settings (driver reports VRR off for the owner's Dell S2722DGM: monitor OSD FreeSync + per-display NVIDIA tick). Gates green, dist 0.1.774. Running: pc-fps2 (worktree pc-fps2, branch next/pc-fps2): profile + prototype and compare (a) pipelining/parallelising the per-frame graphics preparation (VIF/VU1/GIF ~54% of the EE thread at 120 fps) vs (b) the owner's idea, RT64-style frame interpolation; build the winner or both. Queued: double-precision emulation for Intel GPUs; owner's DualSense hardware test.

**2026-10-23: all five PC-experience streams MERGED into main (c352273)**: in-game PC Settings menu + presets (pc-menu), DualSense/DS4/Xbox/HID controllers (pc-input; real-hardware check by the owner pending: dist\Sly2\Controller test.cmd), no PS2 wording/memory-card prompt (pc-text), fresh-clone build + portability (pc-portable), game speed independent of the GPU + pacing options (pc-pacing). Gates: run_tests, 24 unit suites, gpu_diff_all 28/28, pc_text boot test; dist rebuilt (0.1.759). Running now: pc-fps (worktree pc-fps, branch next/pc-fps): why G-Sync does not engage for our game on the owner's DELL S2722DGM (owner says G-Sync IS on: our presentation's fault) + a frame-rate setting 60/120/144/165/200/unlimited (game loop at higher rates if Sly 2's timing allows). Queued: double-precision emulation for Intel GPUs.

**2026-10-23: pc-portable MERGED into main (ca6ee02):** fresh clone -> setup.ps1 -> own .iso in orig\disc -> build.ps1 -> dist\Sly2\sly2.exe (tested twice from clean clones, offline via -From); any x86-64 CPU, Windows 10 1809+, system DLLs only; README rewritten. Follow-up queued: GPUs without double-precision shaders (Intel Iris Xe / Arc) fall back to the software renderer - emulate doubles in the shaders (renderer task) after the current streams. Still running: pc-pacing, pc-menu, pc-input, pc-text.

**PAUSED by the user (2026-10-22, shortly after starting the PC-experience streams).** Nothing is running. All five worktrees (pc-pacing, pc-menu, pc-input, pc-text, pc-portable; branches next/pc-*) are committed and pushed; uncommitted edits were saved as 'PAUSED ... unverified' WIP commits (pc-input 8 files, pc-text 4, pc-portable 1). pc-menu had only read code and started a baseline build (no commits yet). On 'continue': resume each agent from its notes/wip/<stream>.md (they are told to run headless; the owner did not like game windows popping up while using the PC).

**2026-10-22 (later): OWNER REJECTED the PS2-style experience** (see notes/pc-experience.md). Four agents running from main e66b3a5, each with notes/wip/<stream>.md checkpoints: pc-pacing (game slows down at high internal scale + judder on the owner's 2560x1440 165 Hz monitor; auto scale dropped 3x->2x in their session), pc-menu (in-game Options menu + graphics presets Original/Performance/Balanced/Quality/Custom + settings model), pc-input (DualSense/DS4 HID USB+BT, rumble, remapping, menu input API; owner uses a PS5 controller, none connected during development - real-hardware check pending with the owner), pc-text (no PS2 memory-card prompts/wording). Merge all four into main when done, then rebuild dist and have the owner test (incl. DualSense).

**2026-10-22: THE GPU RENDERER IS COMPLETE AND THE DEFAULT (M0-M11 merged into main).** The game
starts on the Direct3D 11 renderer with internal_scale = auto (3x on the RTX 4060 Ti), 16:9 native,
ps2 outlines, Modern preset; software renderer selectable ([graphics] renderer = software) and
the automatic fallback when the GPU path cannot start (one log line). GPUs without ROVs run an
exact no-ROV path (slower). Docs: notes/gpu-renderer.md (as-built map), notes/d3d11-renderer.md
(design + appendices A-M), notes/settings.md, notes/controls.md, notes/install.md.
Final M11 results: all gates green on 28 recordings (bit-exact at S=1; lockstep 28343 comparisons
0 differ); 55-scene in-game sweep 13,260 verifier comparisons, 3 beyond tolerance (dual-source
deep-layering drift, owner question (a)); 200 s default regression 59.9-60 flips/s, memory flat
555-618 MB; 22-minute long session through every episode: no growth, 0 underruns. M11 also fixed
an M4-era feedback-copy staleness bug (gpu_own.c; ep6 RC) and made tests/unit/iop_kernel robust.
Open owner questions: (a) accept the gas-cloud +-3..16 colour drift under deep translucent
layering (exact costs 4-5x GPU); (b) Faithful preset keeping distant hub scenery PS2-blocky
(needs a Sly-specific rule). Possible follow-ups: faster no-ROV path (layering instead of one draw
per overlapping primitive); splitting the repo into a reusable PS2 recomp core + per-game folders
(for Sly 3), which the owner deferred until Sly 2 is finished.

**2026-10-22: THE GPU RENDERER IS COMPLETE AND THE DEFAULT (M0-M11 merged into main).** The game
starts on the Direct3D 11 renderer with internal_scale = auto (3x on the RTX 4060 Ti), 16:9 native,
ps2 outlines, Modern preset; software renderer selectable ([graphics] renderer = software) and
the automatic fallback when the GPU path cannot start (one log line). GPUs without ROVs run an
exact no-ROV path (slower). Docs: notes/gpu-renderer.md (as-built map), notes/d3d11-renderer.md
(design + appendices A-M), notes/settings.md, notes/controls.md, notes/install.md.
Final M11 results: all gates green on 28 recordings (bit-exact at S=1; lockstep 28343 comparisons
0 differ); 55-scene in-game sweep 13,260 verifier comparisons, 3 beyond tolerance (dual-source
deep-layering drift, owner question (a)); 200 s default regression 59.9-60 flips/s, memory flat
555-618 MB; 22-minute long session through every episode: no growth, 0 underruns. M11 also fixed
an M4-era feedback-copy staleness bug (gpu_own.c; ep6 RC) and made tests/unit/iop_kernel robust.
Open owner questions: (a) accept the gas-cloud +-3..16 colour drift under deep translucent
layering (exact costs 4-5x GPU); (b) Faithful preset keeping distant hub scenery PS2-blocky
(needs a Sly-specific rule). Possible follow-ups: faster no-ROV path (layering instead of one draw
per overlapping primitive); splitting the repo into a reusable PS2 recomp core + per-game folders
(for Sly 3), which the owner deferred until Sly 2 is finished.

**2026-10-19: M9 COMPLETE, merged into next/gpu and main (7f661f4): at 3x 16:9 in game GPU 7.3-10.9 ms (was 10-16), frontend -30%, GS thread <= 6 ms except ep3 hub 6.07, peak memory 1.6 GB -> 0.8 GB, adaptive frames in flight (2, up to 6 after slowdowns), latency queue->vsync 14 ms median; one recorded exactness trade (palette-bounded source alpha: affected primitives join the +-1 FF blend class, PS2_GPU_NO_TEX_ALPHA=1 reverts). A stray M9 experiment (conservative depth in runtime/gpu/shaders/gs.hlsl) had been written into the main checkout: saved to the scratchpad and discarded. M11 (default switch, auto internal scale, no-ROV path, long-session memory, flaky iop_kernel test, docs) running in worktree gpu-M11 (branch gpu/M11).** Earlier: **PAUSED by the user (2026-10-18).** Nothing is running. M9 (worktree gpu-M9, branch gpu/M9, pushed) was stopped mid-edit again (memory stats line; last commit unverified): verify it first, then continue from notes/wip/gpu-M9.md, then M11. **2026-10-17 (later): M8 (54-scene sweep: 0 divergence, 0 PUNT) and M10 (native widescreen on the GPU) COMPLETE, merged into next/gpu and main (405ce80); all tests pass; dist rebuilt. M9 (performance) running in worktree gpu-M9 (branch gpu/M9). Open owner questions: accept the ep5 tank gas-cloud +-3..5 colour drift (proposed: accept); distant hub scenery sharper at 3x (proposed: sharp in Modern, PS2-blocky in Faithful). Then M11.** **2026-10-17: M7 COMPLETE, merged into next/gpu and main (2207bb4): 3x default, outlines ps2|thin, Modern/Faithful + toggles, hotkeys F2-F6 (GPU renderer only; software stays default). Disk was full (0.5 GB): deleted 245 GB of regenerable frame dumps/exes/WAVs/RAM dumps from worktree build dirs (corpus .gsrec, screenshots kept); every agent prompt now carries disk-hygiene rules. Running in parallel: M8 (episode sweep, worktree gpu-M8) and M10 (widescreen GPU side, worktree gpu-M10). Then M9 (performance: ep3/ep5 frontend 11-15 ms and GPU 9.5-10.6 ms at 3x over budget), M11. Flaky: tests/unit/iop_kernel failed once under heavy load, passes 3/3 on rerun - make it robust in M11.** Earlier: **PAUSED by the user (2026-10-16).** Nothing is running. M7 (worktree gpu-M7, branch gpu/M7, pushed) was stopped mid-edit (latency measurement; last commit unverified): verify it first, then continue from notes/wip/gpu-M7.md, then M8..M11. **2026-10-16: M6 COMPLETE; next/gpu (M0-M6) MERGED INTO MAIN: GPU renderer opt-in via sly2.ini [graphics] renderer = gpu (or PS2_GS_BACKEND=hw); software stays the default. M7 (3x scaling + outline/filter settings) running in worktree gpu-M7 (branch gpu/M7). For M9: on main in a loaded run (M7 agent busy) the museum had frontend thread 17-21 ms per game frame vs 4-6 ms M6 measured in hubs — re-measure on a quiet machine. Also M6: up to 6 frames in flight (latency) and ~850 MB committed in hw mode.** **2026-10-16: M5 COMPLETE and merged into next/gpu (ee7e27b): GPU 3-6 ms per game frame at S=1 (REF 8-15). M6 (in-game integration, opt-in PS2_GS_BACKEND=hw) running in worktree gpu-M6 (branch gpu/M6, notes/wip/gpu-M6.md).** Earlier: **PAUSED by the user (2026-10-15 11:15).** Nothing is running. M5 (worktree gpu-M5, branch gpu/M5, pushed) was stopped mid-edit (gs_replay options/config wiring; last commit unverified): verify it first, then continue from notes/wip/gpu-M5.md, then M6..M11. **2026-10-15: M3 and M4 COMPLETE and merged into next/gpu (06b9bc5; M4: lockstep 24/24 recordings exact on hw + WARP). M5 (FF fast path) running in worktree gpu-M5 (branch gpu/M5, notes/wip/gpu-M5.md).** Earlier: **PAUSED by the user (2026-10-14, 22:25).** Nothing is running. Unfinished: GPU M3 in worktree `sly2-worktrees\gpu-M3` (branch gpu/M3, pushed). Its last commit 2200666 holds an unverified mid-edit (a PABE shortcut fix in the frontend: A==B, D==Cd is not 'colour unchanged' when PABE is on and As < 0x80). Verify that first, then continue from notes/wip/gpu-M3.md. On "continue": finish M3, then M4..M11 (merge each into next/gpu from `sly2-worktrees\gpu-integ`), sending the owner short progress updates.

**GPU: M2 = GO, merged; main merged into next/gpu (0f3fa1b). M3 (exact REF tier at S=1) running in worktree gpu-M3 (branch gpu/M3, notes/wip/gpu-M3.md).** Earlier: M2 resumed in gpu-M2. HUD edge
anchoring (hud = edges|centered), live F7 aspect / F8 HUD switch and the Binocucom side fill are
MERGED into main (next/hud; notes/widescreen.md 'HUD anchors'). Then M3..M11 one agent per milestone.

**PAUSED by the user (2026-10-14).** Nothing is running. Only unfinished task: GPU milestone M2
(worktree `sly2-worktrees\gpu-M2`, branch gpu/M2, pushed). Done there: baseline refresh (b5a6253),
render-thread executor of the frontend stream + gs_replay -Backend hwsubmit + bench script
(080ca43); the last commit fb62a15 is an unverified mid-edit (compact prim/state record rewrite):
verify or finish it first, then continue from notes/wip/gpu-M2.md. On "continue" the owner wants
EVERYTHING finished: M2..M11 (notes/d3d11-renderer.md section 11), merging each milestone into
next/gpu from `sly2-worktrees\gpu-integ`, then the GPU-dependent settings, HUD edge anchoring and
the open items below.

**PC packaging merged into main (2026-10-14):** `python scripts\package.py` -> dist\Sly2\sly2.exe
(GUI, icon, version info; reads SCUS_973.16 straight from the validated disc image; first run asks
for the .iso; data in %LOCALAPPDATA%\Sly2 or next to the exe with portable.txt; window state
remembered). Dev/console builds and all scripts behave as before. notes/install.md.

**Widescreen merged into main (2026-10-14):** 16:9 by default (sly2.ini `[display] aspect`,
PS2_ASPECT override; notes/widescreen.md, notes/settings.md). Instruction patches
(recomp/patches.py) widen the camera aspect/frustum and keep screen-space projections 4:3; HUD and
2D get GS register 0x7F side-band hints (runtime/gs_hint.h) that the sw renderer applies at kick;
movies are pillarboxed. 4:3 is pixel-identical to before. Regression: 60.1 VBlanks/s avg, 0
underruns. Limits: HUD is centre-anchored (sits in the 4:3 safe area), aspect read at start-up.
The `[graphics]` settings (internal_scale, outlines, filters + toggles) are parsed but only take
effect with the GPU renderer. The GPU renderer's M10 must implement the same hint contract.

**GPU renderer status (2026-10-14, later):** M0, C, M1 merged into next/gpu (coordinator worktree
`sly2-worktrees\gpu-integ`), and main merged into next/gpu (4b5b1da; window.c 'rec' token now -9).
M2 (go/no-go submission benchmark + baseline refresh for the PCRTC fix) running in worktree
gpu-M2 (branch gpu/M2; checkpoint notes/wip/gpu-M2.md). Widescreen (EE patch + sw HUD hints +
settings file) running in worktree `widescreen` (branch next/widescreen; notes/wip/widescreen.md).

**Main 60f11e6 (2026-10-14): whole-game sweep COMPLETE and merged.** Prologue and Episodes 1-8 were
each played end to end by a tester (every job started, completion flows + autosaves checked,
every episode transition through the Episode 8 credits). Merged: next/integrate (all ep branches,
VU review fix 11a6c10, IOP on its own host thread with MMCSS + mixer hold PS2_AUDIO_IOP_LIMIT,
async host logging, PCRTC field-mode fix, pixel-worker start-race fix, sd.c noise-voice fix, debug
tools PS2_TASK_SET/FILE, PS2_POKE variants, live 'pause'/'release'/'calls'/'watch' tokens).
Regression on main (release, quiet-ish machine): boot -> museum 200 s, 60.0 VBlanks/s average
(min 58), 0 underruns, 153 MB committed flat; all 17 unit suites + run_tests pass.
Open (from notes/wip/integrate.md on next/integrate): one 5.07 s EE stall seen once under load at
the briefing->museum load (not reproduced); music-stop-under-heavy-load fix merged but not
re-verified with ~10 games running; ep6 moose "polygon shards" when the camera is inside the
model (unverified, probably game-side); ep7 one-frame blue band at an RC camera cut (not
reproduced). Performance limit: the software GS thread (~16 ms/VBlank) -> the GPU renderer.

**GPU renderer (2026-10-14):** design finished: `notes/d3d11-renderer.md` on branch next/gpu
(= next/d3d11). Design 1 (GS draws -> D3D11 draws at an integer internal scale, exact integer depth,
ROV paths for what fixed-function blending can't express; software renderer stays the default and
the reference until M11). Milestones C (corpus extension, worktree gpu-C, branch gpu/C) and M0
(harness, render thread, all-fallback backend; worktree gpu-M0, branch gpu/M0) are running as
background agents with checkpoints notes/wip/gpu-C.md / gpu-M0.md. UPDATE: M0 and C are complete
and merged into next/gpu (M0: 0 differing pixels on all 13 recordings, conformance exact on the
4060 Ti and WARP, both support ROV; C: 11 more recordings, 24 total in tests/gpu/corpus.txt, and
no GS readbacks anywhere in the game). M1 (frontend dry run) is running in worktree gpu-M1
(branch gpu/M1); merge it into next/gpu from the coordinator worktree `sly2-worktrees\gpu-integ`
when it passes. Then M2
(go/no-go benchmark, needs a quiet machine), M3... per section 11 of the design.
The sweep continuation (wf_1522fb31-777) was resumed again on 2026-10-14; ep4 is COMPLETE.

**RESUMED 2026-10-11 (after the pause below):** continuation workflow `whole-game-sweep-continue`
(run wf_1522fb31-777) runs only the unfinished testers ep1, ep2, ep3, ep4, ep7, then the VU
review, then integrate (finished reports passed in as args; full copies in
build\sweep-finished.json). The D3D11 design workflow resumed as wf_45705d77-396. Resume either
with its `resumeFromRunId`.

**PAUSED by the user on 2026-10-11** (to use the computer). Both workflows were stopped,
all agent game processes were killed, and every worktree is clean (unfinished edits committed as
"PAUSED ... unverified" WIP in ep3 and d3d11), and all next/* branches are pushed. Finished agents:
warp, vu-recomp, ep5, ep6, ep8 (results in the run journal of wf_1f8b6c38-938). Unfinished:
ep1, ep2, ep3, ep4, ep7 (continue from notes/wip/epN.md), then the VU review and the integrate
step; and the D3D11 design workflow (wf_45705d77-396; its understand agents had just started).
Note: on the last resume, ep5/ep6 started a new session ("session 6 start") although they had
finished, so resuming by run id may re-run finished testers. Better: a continuation script that
runs only the unfinished testers and passes the finished results from the journal to integrate.

**Main 81186bf (2026-10-11), episode testers: merge main into your branch.** It has the EE thread
fix from next/ep5 (runtime/threads.c): thread churn leaked ~1 MB/s of fiber stacks, and a restarted
thread resumed its old fiber (StartThread after a normal return spun forever; after ExitThread or
TerminateThread it resumed inside the old syscall). Hangs or "stuck" scripts that involve thread
restarts, and growing memory in long runs, may be this bug. ep5, ep6, ep8 are complete; the
workflow was resumed again (ep1-4, ep7, VU review, integrate pending).

**Direct3D 11 renderer design (2026-10-11), running in parallel:** workflow
`d3d11-renderer-design` (run wf_45705d77-396; worktree `sly2-worktrees\d3d11`, branch
next/d3d11): renderer map + census of Sly 2's GS usage over the .gsrec corpus + prior art, then
3 competing designs (high-level translation / GPU-exact compute / Sly-specialised hybrid), a
3-lens judge panel, and the final design in `notes/d3d11-renderer.md` on next/d3d11. Each agent
checkpoints to `notes/wip/d3d11-*.md` there. Resume: `Workflow({scriptPath: <session>\workflows\
scripts\d3d11-renderer-design-wf_45705d77-396.js, resumeFromRunId: "wf_45705d77-396"})`.

**Main 125390a (2026-10-11):** the VU microcode recompiler and the warp tool's release-build
direct starts are merged. Release regression: 60 fps, VU 100% recompiled (2.2 ns/pair), EE thread
7.7 ms/frame (was ~14), GS thread ~16.4 ms/frame (now the busiest: the game draws more detail when
it has time; the Direct3D 11 renderer is the fix), audio 0 underruns. Still unmerged: next/ep1..8
(episode testers; ep7 verified the Episode 7 -> 8 transition; ep6 investigates Neyla chopper
stages ending in "Job Failed"); the VU review and the integrate step are pending in the
resumed workflow.

**RESUMED on 2026-10-11:** workflow `whole-game-sweep` (run wf_1f8b6c38-938) is running again:
episode testers continue from their checkpoints, then the VU recompiler review and the integrate
step. The state below is the pause point.

**PAUSED by the user on 2026-10-11** (to use the computer). Nothing is running. Main (78c51f8+)
has everything up to and including the episode warp tool and is playable (`scripts\play.ps1`).
Unmerged work, all committed and pushed to GitHub, each with `notes/wip/<task>.md` checkpoints:
- `next/vu-recomp` (worktree `sly2-worktrees\vu-recomp`): VU microcode recompiler, COMPLETE per its
  checkpoint (exact vs the interpreter on recordings + fuzzing). Its independent review was
  interrupted; run the review (prompt in the `whole-game-sweep` workflow script), then merge.
- `next/ep1`..`next/ep8`: per-episode playtesting/fixes. Every job type tested starts in every
  episode; no runtime blockers reported. ep1/ep2/ep3 end with a "PAUSED mid-edit" commit holding
  unfinished, untested fixes (ep1: audio/IOP timing in sd.c/iop_kernel.c; ep2/ep3: GS thread /
  pixel workers) — finish and verify those before merging. Notable open finding (ep2): menu
  music stream can stop when the IOP runs late while the EE waits for the GS.
- To resume: same session -> `Workflow({scriptPath: <whole-game-sweep script>, resumeFromRunId:
  "wf_1f8b6c38-938"})` (finished agents replay from cache; the others continue from their
  checkpoints), then the `integrate` step merges everything and runs the full regression.
  New session -> start one agent per unfinished branch with its checkpoint, then integrate.
Next after that: Direct3D 11 renderer (resolution scaling), widescreen, 60 Hz-synced pacing.

**warp (2026-10-10):** start the game at any episode or job without playing up to it:
`scripts\warp_run.ps1 -Episode N [-Job m3_heist] [-Direct] -Play [-Release]`, or
`scripts\warp.py make --episode N` / `--job <job>` then `scripts\play.ps1`. Memory-card saves built
from the reverse-engineered progress blob (schema + task graph read from the game), plus a
debug-only hook that starts a job directly (runtime/warp.c, PS2_WARP_TASK). All 9 episode starts
and 10 sampled jobs verified, no runtime blockers found. Guide and format: `notes/warp.md`.
**next/vu-recomp (2026-10-10):** the game's VU microcode is statically recompiled to C at build
time (`recomp/vu_recomp.py`: all 8 overlays of `.vutext`, the VU1 renderers and the VU0 library);
`runtime/vu.c` runs the compiled program whenever micro memory holds a compiled image, the
interpreter otherwise (logged once; `PS2_VU_INTERP=1` forces it). Exact against the interpreter
(registers, flags, Q/P, data memory, XGKICK addresses and cycles, pair/cycle counts): recordings of
intro, title, briefing and museum gameplay replay with 0 differences, in-game `PS2_VU_VERIFY`
found 0 mismatches in 15.7M programs, and `tests\unit\vu_recomp` runs random programs differentially.
VU cost per pair 4-7x lower; the VU share of the EE thread drops from ~60% to ~10-15%. Numbers:
`notes/performance.md`; design: `notes/vu-microcode.md` (Recompiler).

**fix/fix-perf (2026-10-10):** the release build (and the -O1 default build) runs at full speed,
59.94 VBlanks/s and ~60 game flips/s, on the movie, memory-card prompt, intro cutscene, title and
Binocucom briefing (main before: 22-32 VBlanks/s in the 3D scenes). GS on its own thread with
deferred band-parallel pixel workers, direct texture sampling from VRAM, EcoQoS opt-out, timer
pacing. Headroom 2-6 ms per VBlank on the EE thread (the intro cutscene is the tightest: 2 ms) and
3-6 ms on the GS thread. The EE thread is now ~60% VU interpreter: the VU recompiler is the next
big step. Tools and numbers: `notes/performance.md` (PS2_PERF, PS2_GS_STATS, profilers, headroom,
GS and VU record/replay).

**Newest (main c8cc6ef):** perf, gameplay and native audio are merged. The release build runs at
60 fps with sound from boot to gameplay in the Cairo Museum prologue (`scripts\play.ps1` builds
and launches it). Workflow `whole-game-sweep` is running: `next/warp` (episode warp tool),
`next/vu-recomp` (static VU microcode recompiler), `next/ep1`..`next/ep8` (one playtest/fix agent
per episode, after the warp tool), then `next/integrate` (merge all + full regression). Worktrees
under `C:\Users\tanih\Projects\sly2-worktrees\`; each keeps `notes/wip/<task>.md`.

**Previous:** main (eb96e20) reaches the title screen and the Bentley/Sly Binocucom briefing
(SQUARE answers "Yes" on the memory-card prompt: PS2_INPUT="600:square:12,1500:start:10").
fix-render and fix-input are merged. Workflow `continue-to-playable` is running:
`fix/fix-perf` (60/60 fps in release on menus already; merging main, continuing its checkpoint),
`audio/audio-iop` (recompiled 989snd/spiop + native mixer; finishing integration),
`play/play-1` (pushing into gameplay). Each has `notes/wip/<task>.md`; resume from those.

**Earlier (2026-10-10, later):** all `hw/*` branches are merged into main. The game boots, plays
the intro movie, shows the mask logo and renders the memory-card prompt ("Do you want to create
a Sly 2 Save File?"), where it currently stops reacting to input (presses do reach the game's
joystick object). Running in parallel, each in a worktree under
`C:\Users\tanih\Projects\sly2-worktrees\`:
- workflow `playtest-fixes`: `fix/fix-input` (why the prompt ignores input, then push further
  into the game), `fix/fix-perf` (game runs ~77% speed; profiler + optimisations),
  `fix/fix-render` (grey button glyph, accuracy).
- workflow `native-audio`: `audio/audio-iop` (recompile 989snd/spiop + native IOP libraries) and
  `audio/audio-mixer` (native libsd mixer + WASAPI, interface `runtime/sd.h`), then integration
  on `audio/audio-iop`.
If interrupted: each branch has commits and `notes/wip/<task>.md` checkpoints; restart an agent in
that worktree with the task text from the workflow scripts plus the checkpoint protocol.
Debug tools: PS2_INPUT (scripted presses), PS2_PEEK (memory), PS2_LOG_PAD/SIF/GS, frame dumps.

Earlier state on 2026-10-10:
- `main`: recompiler + runtime boot the game to its intro movie; hardware layer architecture with
  frozen interfaces (`runtime/hw.h`, `runtime/gs_internal.h`) and stubs for every subsystem.
- Design rules: `notes/native-port-principles.md` (native port, not an emulator),
  `notes/hle-architecture.md` (file ownership, build/run from worktrees),
  `notes/audio-native-plan.md` (audio: recompile 989snd/spiop, native sceSd mixer + WASAPI).
- Subsystem implementations were built in parallel on branches `hw/<name>`, one git worktree each
  under `C:\Users\tanih\Projects\sly2-worktrees\<name>` (also pushed to GitHub):
  gs-mem, gs-draw, gif, dma, vif, vu, ipu, timer, window, pad, mc, audio (research notes only).
  Each has unit tests in `tests/unit/<name>/run.ps1`. Check each branch's state with
  `git -C <worktree> log main..HEAD` and `git -C <worktree> status` (an agent may have stopped
  with uncommitted work).

If the hardware workflow (`ps2-hardware-layer`) was interrupted: do NOT rerun it from the top.
For each `hw/<name>` worktree whose work is unfinished (no review commits, or uncommitted files),
start one agent in that worktree with the original requirements (in the session's workflow script
`ps2-hardware-layer-*.js`) plus the checkpoint protocol (`notes/agent-checkpoints.md`): it reads the
branch's commits and any `notes/wip/<name>.md`, finishes implementation, then the adversarial
review. Same session: `Workflow({scriptPath, resumeFromRunId})` returns finished agents instantly.

Next steps, in order:
1. Merge the `hw/*` branches into main (file ownership means few conflicts), apply the
   integrator requests the agents listed (e.g. extra link libraries for window.c's D3D11), build,
   run `scripts\run_game.ps1 -Seconds 20 -Dump build\frames`, look at the frames.
2. Phase 2 (native pieces): Direct3D 11 renderer replacing gs-draw's software rasterizer; static
   recompilation of the VU microcode (the vu branch's interpreter becomes the reference);
   native audio per `notes/audio-native-plan.md`.
3. Play-test and fix until the game is playable start to finish; then performance and polish
   (resolution scaling, widescreen).

## Target build

- Game: Sly 2: Band of Thieves, NTSC-U, `SCUS_973.16`, VER 1.00 (SYSTEM.CNF)
- Executable SHA-1: `de2af92a54de71e33edc8a160c31c0337a95e6f5`
- Disc filesystem holds only SYSTEM.CNF, SCUS_973.16, IOPRP300.IMG, Z6TAIL.DUP (32 MB padding).
  The ~2.8 GB of game data is outside the filesystem, so the game reads it by raw sector (LBA).

## Executable layout

| Section   | Range                   | Notes                                                    |
|-----------|-------------------------|----------------------------------------------------------|
| .text     | 0x00100000-0x00259DF0   | EE code, recompiled                                      |
| .vutext   | 0x00259DF0-0x00262330   | VU microcode, different ISA, not EE code                 |
| .rodata   | 0x00262380-0x002AC864   | includes switch jump tables                              |
| .data     | 0x002AC880-0x002DD230   | vtables, init/ctor tables, libkernel patch blobs         |
| .bss      | 0x002DD380-0x00452200   |                                                          |
| .irx      | 0x00472200-0x004E5E40   | embedded IOP modules (second segment)                    |

No symbol table.

## Recompiler status (2026-10-10)

- ~6,900 functions found; 371k EE instructions, all but 14 translated (12 `vcallms`, 2 `eret`).
- VU0 macro mode and the MMI instructions the game uses are implemented and tested.
- Function discovery: jal/j targets, code constants, data pointers (vtables, ctor tables),
  after-`jr $ra` boundaries, with switch jump tables protected and a merge pass for bad guesses.

## Boot progress (what runs before it stops)

1. crt0: SetupThread, SetupHeap, FlushCache.
2. libkernel kernel patches: SetSyscall + FindAddress scan of kernel RAM to locate the syscall
   table (runtime emulates a real table at phys 0x10000), alarm patch copied from .data to
   0x80076000 and a second patch at 0x80075000 (not recompiled; runtime HLE stands in).
3. Timer 2 setup, AddIntcHandler/EnableIntc, CreateThread, ChangeThreadPriority,
   GetOsdConfigParam, CreateSema.
4. All C++ static constructors and init tables.
5. SIF setup: AddDmacHandler(5 = SIF0), EnableDmac, polls SifGetReg(4) for IOP boot.
6. SIF RPC init (sreg 0 handshake), then CD/DVD init. Game halts unless disc type == 0x14
   (PS2 DVD); runtime answers from the ISO.
7. IOP reboot with IOPRP300.IMG, LOADFILE version check ("3000"), modules loaded:
   sio2man, padman, mcman, mcserv, Sound_Device_Library (libsd), USB_driver, lgAud,
   989snd_Library, spiop (Sucker Punch).
8. GS reset + display mode (sceGsResetGraph, NTSC interlaced), pad, memory card, fileio and
   989snd servers bound. 989snd batches need 0xFFFFFFFF sentinels around results.
9. Worker thread (f_00160B70) started; requires real threads (fibers in runtime/threads.c).
10. VSync interrupt at 59.94 Hz, serviced at loop heads.
11. CD streaming starts (lbn ~959782, 128-sector chunks, double-buffered at 0x01BE0400 /
    0x01C20400). **Now at the intro movie: sceMpeg drives the IPU (0x10002000) and waits.**

## IOP services (runtime stand-ins)

| SID        | Service                     | Status                                  |
|------------|-----------------------------|-----------------------------------------|
| 0x80000592 | CD init                     | ok                                      |
| 0x80000593 | CD S-commands               | fn 3 disc type, fn 4 error; fn 34 TODO  |
| 0x80000595 | CD N-commands               | fn 1 read (from ISO), 5-8, 14 ready     |
| 0x8000059C | CD (callback?)              | zeros                                   |
| 0x80000003 | IOP heap                    | alloc/free                              |
| 0x80000006 | LOADFILE                    | version, module loads (logged)          |
| 0x80000100/101 | padman                  | zeros (no controller yet)               |
| 0x80000400 | mcserv (memory card)        | zeros                                   |
| 0x80000001 | fileio                      | zeros                                   |
| 0x00123456/57 | 989snd                   | sentinel replies, silent                |

## Bugs found and fixed

- RAM fast-path mask `0xCFFFFFFF` also matched 0x1xxxxxxx, so all hardware registers
  (DMA, IPU, VIF, INTC, VU memory) silently read/wrote RAM. Covered by a test now.

## Blockers

- (resolved) Windows Smart App Control blocked fresh builds; user turned it off.

## Open questions

- [ ] Which IOP modules does the game load (from .irx and IOPRP300.IMG), and what RPCs does it use?
- [ ] How does the game locate its data on disc (sector table in the executable?)

## Matched functions (decomp work, paused)

| Function | Address | Source file | Date |
|----------|---------|-------------|------|
