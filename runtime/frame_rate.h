/* Frame rate (pc-fps; notes/frame-rate.md): the game loop runs once per presented frame at the chosen rate.
 *
 * Sly 2 already steps its world by real time: its clock (0x14D0B8, called once per main-loop frame) takes dt from the
 * COP0 Count and clamps it to [1/60, 1/30] s; its main loop never waits for a VBlank itself - the render thread
 * (0x160B70) flips at the next VBlank (0x16F560 waits for the semaphore the VBlank handler 0x16F468 signals). So the
 * game's frame rate is the VBlank rate it gets, and above 60 frames per second only the clamp's lower bound is in the
 * way (it would make the game run faster than real time). A higher rate therefore needs two things:
 *  - the clamp's lower bound (patch frame_dt_min) lowered: ps2_frame_dt_min (0 = the game's own 1/60, 60 mode);
 *  - VBlanks when the game is ready for its next flip, at most `rate` per second (ps2_runtime.c host timer: the frame
 *    limiter - patch frame_vblank_wait calls frame_vblank_request), and the PS2's 59.94 Hz whenever the game is not
 *    waiting (movies, loading, a frame that takes longer): everything that is not the game's frame stays at 59.94 Hz.
 * At 60 (the default) nothing changes: the 59.94 Hz VBlank timer, the game's own clamp. */
#pragma once
#include <stdint.h>

/* [display] frame_rate values: a rate in Hz (60 = the PS2's 59.94), or these */
#define FRAME_RATE_UNLIMITED (-1)        /* as fast as the game and the GPU go (at most FRAME_RATE_MAX_HZ) */
#define FRAME_RATE_DISPLAY (-2)          /* the display's refresh rate */
#define FRAME_RATE_MAX_HZ 500.0          /* unlimited: the limiter's minimum gap (and the dt clamp's floor) */
#define FRAME_RATE_NTSC_HZ (60000.0 / 1001.0)

extern volatile float ps2_frame_dt_min;  /* (patch frame_dt_min) the game clock's lower clamp; 0 = the game's own 1/60 */
/* (reimagine2, tests only) PS2_FRAME_DT_FIXED=1: every game frame steps the clock by exactly 1 / the frame rate (patch
 * frame_dt_min), whatever real time passed - a scripted run then plays the same in game time even when frame dumps
 * (PS2_DUMP_WAIT) stall it: the same pictures every run. 0 = off (always in play) */
extern volatile float ps2_frame_dt_fixed;

/* the setting (frame_rate = 60 | 120 | 144 | 165 | 200 | unlimited | display), live from any thread */
void   frame_rate_init(void);            /* ps2_settings.frame_rate, and its applier (settings SET_APPLY_FRAME_RATE) */
void   frame_rate_set(int setting);
int    frame_rate_setting(void);
/* the display's refresh rate (Hz) as the presenter / display clock measured it (FRAME_RATE_DISPLAY follows it) */
void   frame_rate_note_display(double hz);
double frame_rate_display_hz(void);
/* the target now: FRAME_RATE_NTSC_HZ for 60, the chosen rate, the display's, 0 = unlimited; the display lock's rate */
double frame_rate_target_hz(void);
/* (pc-smooth) the setting's rate without the display lock (FRAME_RATE_NTSC_HZ for 60, 0 = unlimited) */
double frame_rate_base_hz(void);
/* (pc-smooth) display-locked cadence on a fixed-refresh display whose refresh is not a multiple of the rate (165 Hz
 * without VRR): the display's own VBlanks drive the game at refresh / k (gpu_hw.c clock thread, k = the smallest whole
 * number of refreshes per frame at or below the setting, at most 60: 60 -> 55 frames/s on 165 Hz), so every picture stays
 * on screen the same number of refreshes instead of alternating 2 and 3 (judder). 0 = off. Movies and the RC chopper
 * keep the PS2's 59.94 Hz (frame_rate_lock_hz then reads 0). */
void   frame_rate_set_lock(double hz);
double frame_rate_lock_hz(void);
/* 1: above the PS2's rate (the limiter runs the VBlanks); 0: the 59.94 Hz timer */
int    frame_rate_high(void);
/* the pictures' cadence for pacing and the GPU governor (ms): 1000 / target; unlimited: the display's period (at most
 * 1000 / FRAME_RATE_MAX_HZ) */
double frame_rate_content_ms(void);
/* the limiter's minimum gap between VBlanks (QPC ticks for qpf), 0 when not high */
int64_t frame_rate_min_gap(int64_t qpf);
/* (patches frame_movie_begin / _end) a movie plays: the PS2's 59.94 Hz and the game's own dt clamp until it ends */
void   frame_rate_movie(int on);
/* (patch frame_vblank_wait) the game's render thread waits for the next VBlank: its frame is kicked. target_vt: the vtable of the
 * camera's target object (an RC chopper keeps 60 fps while it flies: frame_rate.c keeps_60) */
void   frame_vblank_request(uint32_t target_vt);
/* ps2_runtime.c: the limiter's request, taken by the host timer (QPC of the request, 0 = none) */
int64_t frame_vblank_take_request(void);
void*  frame_vblank_event(void);         /* auto-reset event set by a request (HANDLE) */
/* a text for the log / menu ("120 fps", "display (165 Hz)", "unlimited") */
const char* frame_rate_describe(char* buf, int cap);
