/* Interfaces the Options menu (menu.c) needs from the pc-pacing stream (notes/pc-experience.md "Interfaces:
 * menu"). menu_stubs.c has weak stand-ins, so this branch links and works on its own; pc-pacing provides the
 * real (strong) definitions and the stand-ins drop out at link time - no edit of the menu is needed at the merge.
 * (Input - navigation, bindings, the [controls] options - is runtime/input.h, already merged.) */
#pragma once

/* graphics.pacing (SETTINGS_PACING_*) changed: 1 if it is in effect now, 0 if it applies at the next start;
 * pacing = -1 only asks (1: changes apply while the game runs). Stand-in: 0 (at the next start). */
int  pacing_apply_mode(int pacing);
/* exclusive fullscreen on / off on the GPU renderer's swap chain: 1 if done (0: not supported; the window then uses
 * borderless fullscreen). Stand-in: 0. */
int  pacing_exclusive_fullscreen(int on);
