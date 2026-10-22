/* PC wording: the game's PS2-specific texts (memory card, console, disc) replaced with PC wording,
 * and the console-only step of the boot flow (the "create a save file?" prompt) skipped.
 * notes/pc-text.md lists every text and why.
 *
 * Texts: each level's text bank (the loader 0x13DF80 fills world+0x4640 = count, +0x4644 = a table
 * of {u32 id, char* text}) is fixed up right after it is loaded (instruction patch "pc_text_bank",
 * recomp/patches.py). A text is replaced only when its id AND its exact original bytes match an
 * entry of the table below, in place (every PC text is shorter): another game version or language
 * keeps its own texts.
 *
 * Boot: with no save data on the card the game asks "Do you want to create a Sly 2 Save File?";
 * the PC port creates it without asking (the game's own "create" job, as when the player answers
 * Yes) and shows a message only if that fails (patches "pc_save_boot_create", "pc_save_boot_poll").
 *
 * PS2_PC_TEXT=0 turns both off (the game's original texts and prompt, for comparison). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;           /* text id in the game's text bank */
    uint32_t len;          /* strlen of the original */
    const char* orig;      /* the original English text (NTSC-U v1.00), exact bytes */
    const char* pc;        /* PC wording; strlen(pc) <= len */
} pc_text_entry;

extern const pc_text_entry pc_text_table[];
extern const size_t pc_text_count;

int pc_text_enabled(void);  /* PS2_PC_TEXT != "0" */

/* Replace the texts of a loaded text bank: `ram` is EE RAM (`ram_size` bytes), `table` the guest
 * address of `count` {id, text pointer} pairs. Returns the number of texts replaced. Pure function
 * (no game state), so tests/unit/pc_text can run it on a synthetic bank. */
int pc_text_apply(uint8_t* ram, uint32_t ram_size, uint32_t table, uint32_t count);

struct ps2_ctx;
/* Patch "pc_text_bank": the text bank of `world` (0x3DD490) has just been loaded. */
void pc_text_bank_loaded(struct ps2_ctx* ctx, uint32_t world);
/* Patch "pc_save_boot_create": the boot check found no save data and is about to ask; returns 1 when
 * the save data is being created instead (the caller then skips the prompt). */
int pc_save_boot_create(struct ps2_ctx* ctx);
/* Patch "pc_save_boot_poll": every frame of the card manager; reports a failed creation with the
 * game's own error dialog. */
void pc_save_boot_poll(struct ps2_ctx* ctx);
extern int pc_save_boot_pending;
