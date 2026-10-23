/* Unit tests for the PC wording table (runtime/pc_text.c, built with PC_TEXT_NO_GAME): the table's
 * own invariants, and pc_text_apply on synthetic text banks laid out like the game's
 * ({u32 id, u32 text pointer} pairs in EE RAM, NUL-terminated texts). */
#include "pc_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define RAM_SIZE 0x100000u
static uint8_t ram[RAM_SIZE];

static void w32(uint32_t a, uint32_t v) { memcpy(ram + a, &v, 4); }

/* a text bank at `table` with the given texts, strings from `heap` on; returns the string addresses */
static void make_bank(uint32_t table, uint32_t heap, int n, const uint32_t* ids, const char* const* texts,
                      uint32_t* where) {
    for (int i = 0; i < n; i++) {
        size_t len = strlen(texts[i]);
        memcpy(ram + heap, texts[i], len + 1);
        w32(table + 8 * i, ids[i]);
        w32(table + 8 * i + 4, heap);
        where[i] = heap;
        heap += (uint32_t)len + 1;
    }
}

static const pc_text_entry* entry(uint32_t id) {
    for (size_t k = 0; k < pc_text_count; k++) if (pc_text_table[k].id == id) return &pc_text_table[k];
    return NULL;
}

static int has_ps2_words(const char* s) {
    static const char* words[] = { "Memory Card", "MEMORY CARD", "PlayStation", "console", "slot 1", "insert",
                                   "Insert", "disc was removed" };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) if (strstr(s, words[i])) return 1;
    return 0;
}

static void test_table(void) {
    CHECK(pc_text_count >= 19);
    for (size_t k = 0; k < pc_text_count; k++) {
        const pc_text_entry* e = &pc_text_table[k];
        CHECK(strlen(e->orig) == e->len);               /* the recorded length is the original's */
        CHECK(strlen(e->pc) <= e->len);                 /* fits in place */
        CHECK(strlen(e->pc) > 0);
        CHECK(!has_ps2_words(e->pc));                   /* no console wording left */
        CHECK(e->id >= 17 && e->id <= 80);              /* pc_text_apply's quick-reject range */
        for (size_t j = 0; j < k; j++) CHECK(pc_text_table[j].id != e->id);   /* one entry per id */
        /* the game's inline-icon escapes "&2x&." the original has must survive */
        const char* esc = strstr(e->orig, "&2");
        if (esc) CHECK(strstr(e->pc, "&2a&.") != NULL);
        /* plain ASCII: the game's font has no glyphs we could not check */
        for (const char* c = e->pc; *c; c++) CHECK((unsigned char)*c >= 32 || *c == '\n');
    }
}

static void test_apply(void) {
    memset(ram, 0xCC, sizeof ram);
    const pc_text_entry* saving = entry(26);
    const pc_text_entry* headset = entry(79);
    CHECK(saving && headset);
    uint32_t ids[] = { 0, 26, 25, 79, 300 };
    const char* texts[] = { "Sly 2: Band of Thieves", saving->orig, "Save Game", headset->orig, "Too far" };
    uint32_t at[5];
    make_bank(0x10000, 0x20000, 5, ids, texts, at);
    int n = pc_text_apply(ram, RAM_SIZE, 0x10000, 5);
    CHECK(n == 2);
    CHECK(strcmp((char*)ram + at[1], saving->pc) == 0);
    CHECK(strcmp((char*)ram + at[3], headset->pc) == 0);
    CHECK(strcmp((char*)ram + at[0], "Sly 2: Band of Thieves") == 0);   /* others untouched */
    CHECK(strcmp((char*)ram + at[2], "Save Game") == 0);
    CHECK(strcmp((char*)ram + at[4], "Too far") == 0);
    /* the rest of the old string is cleared, the byte after it (the next string) is not touched */
    for (uint32_t i = (uint32_t)strlen(saving->pc); i <= saving->len; i++) CHECK(ram[at[1] + i] == 0);
    CHECK(ram[at[1] + saving->len + 1] == (uint8_t)texts[2][0]);
    /* applying again changes nothing (the texts no longer match their originals) */
    CHECK(pc_text_apply(ram, RAM_SIZE, 0x10000, 5) == 0);
    CHECK(strcmp((char*)ram + at[1], saving->pc) == 0);
}

static void test_other_versions_left_alone(void) {
    memset(ram, 0, sizeof ram);
    const pc_text_entry* saving = entry(26);
    /* another language / version: same id, different text */
    char french[] = "\nSauvegarde...\n\nN'eteignez pas votre console\n";
    /* the same text one byte longer or shorter (a different build): not ours */
    char longer[256], shorter[256];
    snprintf(longer, sizeof longer, "%s!", saving->orig);
    snprintf(shorter, sizeof shorter, "%.*s", (int)saving->len - 1, saving->orig);
    /* the right text under another id */
    uint32_t ids[] = { 26, 26, 26, 27 };
    const char* texts[] = { french, longer, shorter, saving->orig };
    uint32_t at[4];
    make_bank(0x10000, 0x20000, 4, ids, texts, at);
    CHECK(pc_text_apply(ram, RAM_SIZE, 0x10000, 4) == 0);
    for (int i = 0; i < 4; i++) CHECK(strcmp((char*)ram + at[i], texts[i]) == 0);
}

static void test_bad_banks(void) {
    memset(ram, 0, sizeof ram);
    /* pointers outside RAM, kseg mirrors, a text running to the end of RAM, a huge count */
    const pc_text_entry* e = entry(43);
    w32(0x100, 43); w32(0x104, 0x7FFFFFF0u);                       /* outside */
    w32(0x108, 43); w32(0x10C, 0x80000000u | 0x30000u);            /* kseg0 mirror of 0x30000 */
    memcpy(ram + 0x30000, e->orig, e->len + 1);
    uint32_t tail = RAM_SIZE - (uint32_t)e->len;                    /* no room for the NUL */
    memcpy(ram + tail, e->orig, e->len);
    w32(0x110, 43); w32(0x114, tail);
    CHECK(pc_text_apply(ram, RAM_SIZE, 0x100, 3) == 1);
    CHECK(strcmp((char*)ram + 0x30000, e->pc) == 0);
    CHECK(memcmp(ram + tail, e->orig, e->len) == 0);
    CHECK(pc_text_apply(ram, RAM_SIZE, 0x100, 0x7FFFFFFFu) == 0);   /* a garbage count is refused */
    CHECK(pc_text_apply(ram, RAM_SIZE, RAM_SIZE - 4, 4) == 0);      /* a table past the end */
}

static void test_options(void) {
    CHECK(pc_options_hidden(0x50));                  /* USB Headset */
    CHECK(pc_options_hidden(0x3F));                  /* Center Screen... */
    static const uint32_t kept[] = { 0x3C, 0x3D, 0x3E, 0x4C, 0x67, 0x68, 0x51, 0x53, 0x48, 0x4F, 0 };
    for (int i = 0; kept[i]; i++) CHECK(!pc_options_hidden(kept[i]));
}

int main(void) {
    test_options();
    test_table();
    test_apply();
    test_other_versions_left_alone();
    test_bad_banks();
    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("pc_text: %zu texts, all checks passed\n", pc_text_count);
    return 0;
}
