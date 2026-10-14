/* Audio output: 48 kHz stereo from the native sound backend (sd.c) to the PC's speakers.
 * audio_init() is declared in sd.h (the frozen interface); these are extras for tests/tools. */
#pragma once
#include <stdint.h>

void audio_init(void);         /* start the output thread (idempotent) */
void audio_shutdown(void);     /* stop the thread and finalize the PS2_AUDIO_WAV file */

enum { AUDIO_OFF = 0, AUDIO_WASAPI = 1, AUDIO_NULL = 2 };
int      audio_backend(void);          /* which output is running */
uint64_t audio_frames_written(void);   /* frames handed to the device / null sink so far */
/* Queue text for stderr without blocking (a writer thread prints it): for real-time threads (the
 * output thread, the IOP thread), whose own stderr writes can block for 100 ms on a busy machine. */
void     host_log_async(const char* text);

/* Environment:
 *   PS2_AUDIO=0          no output thread at all (sd.c state then only changes through the API)
 *   PS2_AUDIO_NULL=1     no device: render in real time into a null sink (headless runs)
 *   PS2_AUDIO_WAV=<file> also write everything rendered to a 16-bit stereo 48 kHz WAV
 *   PS2_AUDIO_TEST=1     play a 440 Hz sine instead of the game's sound (checks the output path)
 *   PS2_AUDIO_LATENCY=<ms> target queued audio (default 40) */
