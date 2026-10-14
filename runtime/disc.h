/* The game's disc image as a file: find the boot executable through the ISO 9660 file system
 * (SYSTEM.CNF names it, as the PS2's boot ROM reads it) and check that it is the executable this
 * build was recompiled from. Used at start-up when no ELF is given on the command line
 * (runtime/launch.c); pure file reading, no global state, unit tested in tests/unit/disc. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define DISC_SECTOR 2048u

typedef struct {
    char boot_name[64];          /* "SCUS_973.16": the BOOT2 file's name without directory or ";1" */
    uint32_t lbn, size;          /* where the boot executable lies in the image, and its length */
} disc_boot;

/* Results; disc_why() has a sentence for each one. */
enum {
    DISC_OK = 0,
    DISC_ERR_OPEN,               /* the file cannot be opened */
    DISC_ERR_NOT_ISO,            /* no ISO 9660 volume (not a 2048-byte-sector DVD image) */
    DISC_ERR_NO_CNF,             /* no SYSTEM.CNF: not a PS2 game disc */
    DISC_ERR_NO_BOOT,            /* SYSTEM.CNF names no BOOT2 executable, or it is missing */
    DISC_ERR_WRONG_GAME,         /* another game (boot executable name differs) */
    DISC_ERR_WRONG_VERSION,      /* the right name, but not the executable this build was made from */
    DISC_ERR_READ,               /* a read error or a truncated image */
};
const char* disc_why(int err);

/* Locate the boot executable (SYSTEM.CNF BOOT2) of the image at `path`. */
int disc_find_boot(const wchar_t* path, disc_boot* out);

/* disc_find_boot, then check name, size and SHA-256 (lower-case hex) of the boot executable. On
 * DISC_OK and elf_out != NULL, *elf_out is a malloc'ed copy of the executable (*elf_size bytes). */
int disc_check(const wchar_t* path, const char* want_name, uint32_t want_size, const char* want_sha256,
               uint8_t** elf_out, size_t* elf_size);

/* SHA-256 of a buffer as 64 lower-case hex digits plus NUL. */
void disc_sha256_hex(const void* data, size_t n, char out[65]);
