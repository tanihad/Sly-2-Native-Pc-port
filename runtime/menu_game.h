/* The Options menu in the game (menu_game.c): the "Options" and "Quit to Desktop" entries of the game's pause menu and
 * of its Options page (instruction patches, recomp/patches.py "pcmenu_*"), opening the menu from Esc / the PS button
 * through the game's own pause, the settings the game itself must apply (music / effects volumes through its sound
 * manager; its own options Vibrations, Speakers, Invert Binocucom / Turret through its getter / setter, on the EE thread),
 * the renderer switch, Quit to desktop, and the menu's host interface (menu.h). notes/pc-menu.md. */
#pragma once
#include <stdint.h>

struct ps2_ctx;

#define PCMENU_TEXT_ID 0x10000u          /* "Options" (ours): the entry's text id (the game's own ids are < 0xCB3) */
#define PCMENU_TEXT_QUIT 0x10001u        /* (pc-polish) "Quit to Desktop" */
#define PCMENU_LIST 0x000FF000u          /* EE RAM the game never uses (zeros): the pause list with the entries */
#define PCMENU_STRING 0x000FF200u        /* their texts */
#define PCMENU_STRING_QUIT 0x000FF240u
#define PCMENU_ITEM 0x000FF400u          /* (pc-polish) a scratch Options item for the game's getter / setter */

extern volatile int pcmenu_ee_pending;   /* menu_ee_frame has work (volumes, a pause to open) */
extern volatile int pcmenu_pause_req;    /* Esc / PS button: open the game's pause menu as START would */
extern volatile long pcmenu_open_pending; /* the menu opens once the pause menu is up (or after 0.5 s) */

/* patches */
void pcmenu_pause_list(struct ps2_ctx* ctx);     /* f_001B0290 before 0x1B0490: the pause list (Save / Load, Options, Quit) */
int  pcmenu_text(struct ps2_ctx* ctx);           /* f_0013E368 entry: 1 = our text id ($v0 set, return) */
void pcmenu_select(struct ps2_ctx* ctx);         /* f_001AEEE0 before 0x1AF154: Cross on an entry ($s0) */
void pcmenu_start(struct ps2_ctx* ctx);          /* f_001D9648 before 0x1D96EC: START pressed? ($v0) */
void pcmenu_ee_frame(struct ps2_ctx* ctx);       /* main loop 0x1A75A8: volumes / game options into the game */
/* (pc-polish) */
void pcmenu_options_list(struct ps2_ctx* ctx);   /* f_001B04E0 before 0x1B05E4: Options / Quit to Desktop on its page */
int  pcmenu_option_hidden(uint32_t kind);        /* f_001B04E0's item filter: the items our Options menu has */
void pcmenu_quit_request(void);                  /* Quit to desktop? (the entries, the window's close button) */
void menu_game_quit(void);                       /* quit confirmed: clean shutdown (exit code 0) */

void menu_game_init(int headless);               /* ps2_runtime.c main, after the window */
void menu_script(int k);                         /* window.c PS2_INPUT menu tokens */
void menu_focus(int active);                     /* window.c: the window became (in)active (sound in the background) */
