/* PC wording for the game's PS2-specific texts, and the boot flow without the "create a save
 * file?" prompt. pc_text.h explains the mechanism; notes/pc-text.md lists every text. */
#include "pc_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The texts, as the game's text bank holds them (NTSC-U v1.00, English; ids are the bank's).
 * "\xAE" is the game's (R) sign. The memory-card dialog manager shows text id N as dialog N
 * (0x2E6550 +0x40, set by 0x1ACB58). Every PC text must be at most as long as the original. */
const pc_text_entry pc_text_table[] = {
    /* boot check: the card folder cannot be used (mc.c reports "no card": not creatable / not a
     * directory). One button (OK); the game then plays without saving. */
    { 17, 247,
      "There is no Memory Card (8MB) (for PlayStation\xAE""2) inserted in\nMEMORY CARD slot 1\n\nSly 2 requires 100 KB of free space\n\nPlease insert a Memory Card (8MB) (for PlayStation\xAE""2) containing sufficient space or you will not be able to save your progress\n",
      "The Sly 2 save folder cannot be used\n\nSly 2 requires 100 KB of free space\n\nCheck that the save folder can be written to and that the disk is not full, or you will not be able to save your progress\n" },
    /* boot check: the card is "unformatted" (only a %unformatted marker file does that, mc.c);
     * Yes formats = empties the save folder. */
    { 18, 175,
      "The Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1 is unformatted\n\nIf you do not format it then you will not be able to save your progress\n\nDo you want to format?",
      "The Sly 2 save folder is not set up\n\nIf you do not set it up then you will not be able to save your progress\n\nDo you want to set it up now?" },
    { 19, 165,
      "\nFormatting the Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1\n\nPlease do not turn off your console or remove the Memory Card (8MB) (for PlayStation\xAE""2)\n",
      "\nSetting up the save folder...\n\nPlease do not close the game\n" },
    { 20, 143,
      "An error occurred while formatting the Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1\n\nYou will not be able to save your progress\n",
      "The save folder could not be set up\n\nYou will not be able to save your progress\n" },
    /* boot check: no save data yet. The PC port does not ask (pc_save_boot_create); the text stays
     * for any other way the game reaches this dialog. */
    { 21, 261,
      "No Sly 2 Save File on the Memory Card (8MB) (for PlayStation\xAE""2) inserted in MEMORY CARD slot 1\n\nSly 2 requires 100 KB of free space\n\nIf you do not create a Sly 2 Save File then you will not be able to save your progress\n\nDo you want to create a Sly 2 Save File?",
      "There is no Sly 2 save data yet\n\nIf you do not create it then you will not be able to save your progress\n\nDo you want to create Sly 2 save data?" },
    { 22, 186,
      "\nCreating the Sly 2 Save File on the Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1\n\nPlease do not turn off your console or remove the Memory Card (8MB) (for PlayStation\xAE""2)\n",
      "\nCreating save data...\n\nPlease do not close the game\n" },
    { 23, 154,
      "An error occurred while creating the Sly 2 Save File on the Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1\n\nYour progress is not being saved\n",
      "The Sly 2 save data could not be created\n\nCheck that the disk is not full\n\nYour progress is not being saved\n" },
    /* boot check: less than ~100 KB left on the (8 MB) card the save folder stands for */
    { 24, 247,
      "The Memory Card (8MB) (for PlayStation\xAE""2) inserted in MEMORY CARD slot 1 is full\n\nSly 2 requires 100 KB of free space\n\nPlease insert a Memory Card (8MB) (for PlayStation\xAE""2) containing sufficient space or you will not be able to save your progress\n",
      "The Sly 2 save folder is full\n\nSly 2 requires 100 KB of free space\n\nRemove other files from the save folder or you will not be able to save your progress\n" },
    { 26, 100,
      "\nSaving...\n\nPlease do not turn off your console or remove the Memory Card (8MB) (for PlayStation\xAE""2)\n",
      "\nSaving...\n\nPlease do not close the game\n" },
    { 29, 179,
      "An error occurred while writing to the Sly 2 Save File on the Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1\n\nData may be corrupted\n\nYour progress is not being saved\n",
      "The Sly 2 save data could not be written\n\nCheck that the disk is not full\nData may be corrupted\n\nYour progress is not being saved\n" },
    { 33, 101,
      "\nErasing...\n\nPlease do not turn off your console or remove the Memory Card (8MB) (for PlayStation\xAE""2)\n",
      "\nErasing...\n\nPlease do not close the game\n" },
    /* shown once a new game has started: what the auto-save icon means */
    { 34, 188,
      "When the following icon appears,\nyour progress is being saved\n\n&2a&.\n\nWhile this icon is on screen, please\ndo not turn off your console or remove\nthe Memory Card (8MB) (for PlayStation\xAE""2)\n",
      "When the following icon appears,\nyour progress is being saved\n\n&2a&.\n\nWhile this icon is on screen, please\ndo not close the game\n" },
    { 35, 216,
      "Auto-save on the Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1 has failed\n\nAuto-save has been disabled\nData may be corrupted\n\nUse the save game option to manually save your game and re-enable auto-save\n",
      "Auto-save has failed\n\nAuto-save has been disabled\nData may be corrupted\n\nUse the save game option to manually save your game and re-enable auto-save\n" },
    /* the card was pulled out: on PC the save folder became unusable while playing */
    { 36, 182,
      "The Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1 was removed\n\nAuto-save has been disabled\n\nUse the save game option to manually save your game and re-enable auto-save\n",
      "The Sly 2 save folder cannot be reached\n\nAuto-save has been disabled\n\nUse the save game option to manually save your game and re-enable auto-save\n" },
    { 42, 145,
      "Some data in the Sly 2 Save File on the Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1 was corrupt\n\nCorrupted data has been cleared\n",
      "Some Sly 2 save data was corrupt\n\nCorrupted data has been cleared\n" },
    /* the disc tray: on PC the disc image could not be read */
    { 43, 72,
      "The disc was removed.\n\nPlease insert the Sly 2 disc to continue playing.",
      "The game data could not be read.\n\nCheck that the disc image is there." },
    { 46, 109,
      "\nPlease insert a Memory Card (8MB) (for PlayStation\xAE""2) in MEMORY CARD slot 1 that contains a Sly 2 Save File\n",
      "\nThere is no Sly 2 save data\n" },
    /* Options: the PS2's USB headset (Logitech) setting */
    { 79, 35, "USB Headset (for PlayStation\xAE""2): On",  "USB Headset: On" },
    { 80, 36, "USB Headset (for PlayStation\xAE""2): Off", "USB Headset: Off" },
};
const size_t pc_text_count = sizeof pc_text_table / sizeof pc_text_table[0];

int pc_text_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS2_PC_TEXT");
        on = !(e && e[0] == '0' && !e[1]);
    }
    return on;
}

static uint32_t rd32(const uint8_t* ram, uint32_t ram_size, uint32_t a) {
    a &= 0x1FFFFFFFu;                                  /* kseg0/1 mirrors */
    if (a > ram_size - 4) return 0;
    uint32_t v;
    memcpy(&v, ram + a, 4);
    return v;
}

int pc_options_hidden(uint32_t kind) {
    return (kind == 0x50u || kind == 0x3Fu) && pc_text_enabled();
}

int pc_text_apply(uint8_t* ram, uint32_t ram_size, uint32_t table, uint32_t count) {
    if (!ram || ram_size < 8 || count > 0x100000u) return 0;
    int done = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = rd32(ram, ram_size, table + 8 * i);
        uint32_t p = rd32(ram, ram_size, table + 8 * i + 4) & 0x1FFFFFFFu;
        if (id < 17 || id > 80 || p == 0 || p >= ram_size) continue;      /* quick reject: ids of the table */
        for (size_t k = 0; k < pc_text_count; k++) {
            const pc_text_entry* e = &pc_text_table[k];
            if (e->id != id) continue;
            /* exact original: same length (NUL right after it) and the same bytes */
            if (p + e->len >= ram_size || ram[p + e->len] != 0 || memcmp(ram + p, e->orig, e->len) != 0) break;
            size_t n = strlen(e->pc);
            if (n > e->len) break;                                       /* (the unit test checks) */
            memcpy(ram + p, e->pc, n);
            memset(ram + p + n, 0, e->len - n);
            done++;
            break;
        }
    }
    return done;
}

#ifndef PC_TEXT_NO_GAME                  /* the unit test builds the table and pc_text_apply alone */
#include "ps2_internal.h"

/* ---- text banks ------------------------------------------------------------------------------- */

void pc_text_bank_loaded(ps2_ctx* ctx, uint32_t world) {
    (void)ctx;
    if (!pc_text_enabled()) return;
    uint32_t count = rd32(ps2_ram, PS2_RAM_SIZE, world + 0x4640);
    uint32_t table = rd32(ps2_ram, PS2_RAM_SIZE, world + 0x4644);
    int n = pc_text_apply(ps2_ram, PS2_RAM_SIZE, table, count);
    static int last = -1;
    if (n != last) {
        fprintf(stderr, "[pc_text] text bank loaded (%u texts): %d in PC wording\n", count, n);
        last = n;
    }
}

/* ---- boot: create the save data without asking ----------------------------------------------- */

/* Sly 2 NTSC-U v1.00 (the patches that call these are guarded by the functions' SHA-1s). */
#define DIALOG_MGR      0x002E6550u    /* memory-card dialogs: +0x40 dialog (= text id), +0x44 mode */
#define CARD_WORKER     0x002F633Cu    /* card manager 0x2E65E0 + 0xFD5C: +4 thread (-1 idle), +0x10 result */
#define PMF_CREATE      0x002A6D88u    /* {0xFFFF0000, 0x1AB868}: the "create the save file" job */
#define FN_START_JOB    0x001AAF90u    /* (worker, job): run a card job in the worker thread */
#define FN_SET_DIALOG   0x001ACB58u    /* (dialog manager, dialog id) */
#define DIALOG_CREATE_FAILED 0x17u     /* text 23 */

int pc_save_boot_pending;

int pc_save_boot_create(ps2_ctx* ctx) {
    static int done;
    if (!pc_text_enabled() || done) return 0;
    done = 1;                          /* once per run: a second "no save data" gets the game's prompt */
    if (rd32(ps2_ram, PS2_RAM_SIZE, PMF_CREATE) != 0xFFFF0000u ||
        rd32(ps2_ram, PS2_RAM_SIZE, PMF_CREATE + 4) != 0x001AB868u ||
        rd32(ps2_ram, PS2_RAM_SIZE, CARD_WORKER + 4) != 0xFFFFFFFFu)
        return 0;
    /* what the prompt's Yes does (0x1AD168 for dialog 0x15), without the dialogs */
    ps2_ctx saved = *ctx;
    ctx->r[REG_A0].ud[0] = CARD_WORKER;
    uint64_t pmf;
    memcpy(&pmf, ps2_ram + PMF_CREATE, 8);
    ctx->r[REG_A1].ud[0] = pmf;
    ctx->r[REG_RA].ud[0] = 0x001AB36Cu;               /* (the prompt's call site) */
    ps2_call(ctx, FN_START_JOB);
    *ctx = saved;
    pc_save_boot_pending = 1;
    fprintf(stderr, "[pc_text] no save data yet: creating it (no prompt)\n");
    return 1;
}

void pc_save_boot_poll(ps2_ctx* ctx) {
    if (!pc_save_boot_pending || rd32(ps2_ram, PS2_RAM_SIZE, CARD_WORKER + 4) != 0xFFFFFFFFu) return;
    pc_save_boot_pending = 0;
    uint32_t result = rd32(ps2_ram, PS2_RAM_SIZE, CARD_WORKER + 0x10);
    if (result == 0) {
        fprintf(stderr, "[pc_text] save data created\n");
        return;
    }
    fprintf(stderr, "[pc_text] creating the save data failed (%d): showing the game's error message\n", (int)result);
    ps2_ctx saved = *ctx;                /* the dialog the game shows after a failed "Yes" */
    ctx->r[REG_A0].ud[0] = DIALOG_MGR;
    ctx->r[REG_A1].ud[0] = DIALOG_CREATE_FAILED;
    ctx->r[REG_RA].ud[0] = 0x001AD218u;
    ps2_call(ctx, FN_SET_DIALOG);
    *ctx = saved;
}
#endif
