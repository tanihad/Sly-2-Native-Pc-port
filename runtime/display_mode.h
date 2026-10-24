/* Fullscreen refresh rate (pc-pacing, notes/pc-experience.md "Interfaces: pacing and resolution").
 *
 * The game's pictures come at the PS2's 59.94 Hz. A display refreshing at a multiple of that (60, 120, 240 Hz) shows each
 * picture for the same number of refreshes (smooth); 144 / 165 Hz cannot (165 / 59.94 = 2.75: pictures last 2 or 3
 * refreshes, the judder the owner saw as "shaking") unless variable refresh (G-Sync / FreeSync) is on. In fullscreen the
 * game can switch the display to a multiple for as long as it is fullscreen and in front:
 * [display] fullscreen_refresh = desktop (default: never change the display) | auto (the highest multiple of 59.94 Hz the
 * display offers at the desktop's resolution, when the desktop's rate is not one: 120 Hz on a 165 Hz display) | <Hz>.
 * window.c calls display_mode_fullscreen(hwnd, 1) when the window goes fullscreen or is activated fullscreen, and (0) when
 * it leaves fullscreen or is deactivated. The mode is temporary (CDS_FULLSCREEN): Windows restores the desktop's when the
 * game ends. */
#pragma once

/* 1 if the display mode was changed (on) or restored (off) */
int display_mode_fullscreen(void* hwnd, int on);
/* the rate to switch to: 0 = none (keep the desktop's). rates: the display's rates at the desktop's resolution; want:
 * 0 desktop, -1 auto, > 0 a rate. Pure (unit tests). */
int display_mode_pick(const int* rates, int n, int desktop_hz, int want);
/* (pc-polish) the rate for another resolution (Fullscreen's [display] resolution): display_mode_pick's choice, else the
 * desktop's rate if that mode has it, else the fastest not above it, else the fastest (never 0 when n > 0). Pure. */
int display_mode_rate_at(const int* rates, int n, int desktop_hz, int want);
/* exclusive fullscreen (the Options menu's Display mode, menu_ext.h pacing_exclusive_fullscreen): while on, fullscreen uses
 * fullscreen_refresh = auto unless a rate is set (the desktop's rate when it is a multiple of 59.94 Hz already). The window
 * stays a borderless popup over the monitor (DXGI flip model: independent flip, the same as exclusive for latency). */
int display_mode_exclusive(void* hwnd, int on);
