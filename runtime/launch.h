/* Start-up and shut-down of the game as a PC program (launch.c): where the log goes, which disc
 * image is used and where the game executable comes from, the folders for settings and saves, and
 * a clean exit. notes/install.md describes it from the player's side. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define LAUNCH_BOOT_ELF "SCUS_973.16"     /* Sly 2: Band of Thieves (USA) */

/* First thing in main: resolve the user data folder; without a console (the GUI build started
 * from Explorer) stderr goes to <user data>\sly2.log (the previous run's log: sly2.previous.log). */
void launch_init(void);
int  launch_has_console(void);            /* stderr goes somewhere a person or script reads */

/* The game executable to run: argv[1] if it is an ELF (the disc image then comes from PS2_ISO or
 * sly2.ini, unchecked, as before); otherwise from a disc image - argv[1] if it is one, else PS2_ISO,
 * else [game] disc in sly2.ini, else (no console, not headless) a file-open dialog - checked to hold
 * exactly the executable this build was made from. Hands the image to cdvd.c. Exits with a message
 * if there is no usable executable. Returns a malloc'ed copy. */
uint8_t* launch_load_game(int argc, char** argv, int headless, size_t* size);

/* Fatal start-up error: logged, and shown in a message box when there is no console. Exits(1). */
void launch_fatal(const char* fmt, ...);

/* The window was closed (window.c's exit hook): no new memory card operation, flush the log, exit. */
void launch_exit(int code);

/* ---- for the unit tests (tests/unit/disc) ---- */
/* disc_check against this build's executable (LAUNCH_BOOT_ELF, ps2_elf_size, ps2_elf_sha256). */
int launch_check_disc(const wchar_t* path, uint8_t** elf, size_t* size);
/* Ask for a disc image until a good one is chosen (stored in sly2.ini, 1) or the player gives up (0).
 * prompt: fills path, 0 = cancelled; retry: told why a choice is unusable, 0 = give up. */
typedef int (*launch_prompt_fn)(wchar_t* path, size_t cap);
typedef int (*launch_retry_fn)(const wchar_t* path, const char* why);
int launch_choose_disc(launch_prompt_fn prompt, launch_retry_fn retry, wchar_t* path, size_t cap, uint8_t** elf,
                       size_t* size);
