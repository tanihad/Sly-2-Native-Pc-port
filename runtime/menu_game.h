/* The Options menu in the game (menu_game.c): the "PC Settings" entry of the game's pause menu (instruction patches,
 * recomp/patches.py "pcmenu_*"), opening the menu from Esc / the PS button through the game's own pause, the
 * settings the game itself must apply (music / effects volumes through its sound manager, on the EE thread), the
 * renderer switch, and the menu's host interface (menu.h). notes/pc-menu.md. */
#pragma once
#include <stdint.h>

struct ps2_ctx;

#define PCMENU_TEXT_ID 0x10000u          /* the entry's text id (the game's own ids are < 0xCB3) */
#define PCMENU_LIST 0x000FF000u          /* EE RAM the game never uses (zeros): the pause list with the entry */
#define PCMENU_STRING 0x000FF200u        /* its text */

extern volatile int pcmenu_ee_pending;   /* menu_ee_frame has work (volumes, a pause to open) */
extern volatile int pcmenu_pause_req;    /* Esc / PS button: open the game's pause menu as START would */
extern volatile long pcmenu_open_pending; /* the menu opens once the pause menu is up (or after 0.5 s) */

/* patches */
void pcmenu_pause_list(struct ps2_ctx* ctx);     /* f_001B0290 before 0x1B0490: the list with "PC Settings" */
int  pcmenu_text(struct ps2_ctx* ctx);           /* f_0013E368 entry: 1 = our text id ($v0 set, return) */
void pcmenu_select(struct ps2_ctx* ctx);         /* f_001AEEE0 before 0x1AF154: Cross on an entry ($s0) */
void pcmenu_start(struct ps2_ctx* ctx);          /* f_001D9648 before 0x1D96EC: START pressed? ($v0) */
void pcmenu_ee_frame(struct ps2_ctx* ctx);       /* main loop 0x1A75A8: volumes into the game's sound manager */

void menu_game_init(int headless);               /* ps2_runtime.c main, after the window */
void menu_script(int k);                         /* window.c PS2_INPUT menu tokens */
void menu_focus(int active);                     /* window.c: the window became (in)active (sound in the background) */
