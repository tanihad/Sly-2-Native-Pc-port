/* Interfaces the Options menu (menu.c) needs from the other PC-experience streams (notes/pc-experience.md
 * "Interfaces"). Each has a weak stand-in in menu_stubs.c, so this branch links and works on its own; the stream
 * that owns the subsystem provides the real (strong) definition and the stand-in drops out at link time - no
 * edit of the menu is needed when the branches merge.
 *
 *   pc-input  (runtime/input*.c, pad.c): bindings, the [controls] options, the controller kind, the game-input gate
 *   pc-pacing (gpu_present.c, ps2_runtime.c timing): pacing mode and exclusive fullscreen applied while running
 */
#pragma once
#include <stddef.h>

/* ---- pc-input ------------------------------------------------------------------------------------------------ */
/* device: 0 = keyboard, 1 = controller. Bindable actions are numbered 0..count-1 (the PS2 pad's buttons and stick
 * directions: Cross, Circle, ... as the input stream defines them). */
int  input_bind_count(int device);
const char* input_bind_name(int device, int action);         /* "Jump (Cross)", "Camera left (right stick)" */
void input_bind_text(int device, int action, char* buf, size_t cap);   /* the key / button now: "Space", "Cross" */
/* bind action to code (keyboard: a Windows virtual-key code; controller: the input stream's button code, as
 * reported to menu_capture_button). Persisted to sly2.ini [controls] and applied by the input stream. 1 = done. */
int  input_bind_set(int device, int action, int code);
void input_bind_reset(int device);                          /* the device's default bindings (persisted) */
/* ps2_settings [controls] changed (vibration, vibration_strength, invert_camera_x/y, stick_deadzone): apply them */
void input_apply_settings(void);
/* the controller in use, for the button hints: MENU_DEV_PLAYSTATION (DualSense, DualShock 4), MENU_DEV_XBOX, or
 * MENU_DEV_KEYBOARD when none is connected */
int  input_controller_kind(void);
/* a short controller rumble to feel the strength being set (0..100); may do nothing */
void input_rumble_test(int strength);

/* ---- pc-pacing ----------------------------------------------------------------------------------------------- */
/* graphics.pacing (SETTINGS_PACING_*) changed: 1 if it is in effect now, 0 if it applies at the next start;
 * pacing = -1 only asks (1: changes apply while the game runs) */
int  pacing_apply_mode(int pacing);
/* exclusive fullscreen on / off on the GPU renderer's swap chain: 1 if done (0: not supported; the window then uses
 * borderless fullscreen) */
int  pacing_exclusive_fullscreen(int on);
