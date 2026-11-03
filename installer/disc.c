/* Sly2-Setup: checks the player's disc image the way the game does (runtime/disc.c) and the build does
 * (scripts/extract_elf.py): ISO 9660 volume -> SYSTEM.CNF -> BOOT2 = cdrom0:\SCUS_973.16;1 -> that
 * file's size and SHA-256 must be Sly 2: Band of Thieves (USA, SCUS-97316, v1.00). Nothing is copied. */
#include "setup.h"

#include <bcrypt.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048

static int rd(HANDLE h, uint32_t lba, void* buf, uint32_t size) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)lba * SECTOR;
    DWORD got = 0;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, buf, size, &got, NULL) && got == size;
}

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Find "DIR\NAME;1" (case-insensitive, version optional) from the root directory. 1 = found. */
static int find_file(HANDLE h, uint32_t root_lba, uint32_t root_size, const char* path, uint32_t* lba, uint32_t* size,
                     int* err) {
    uint32_t cur_lba = root_lba, cur_size = root_size;
    char parts[8][64];
    int np = 0;
    const char* p = path;
    while (*p && np < 8) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != '\\' && *p != '/' && k < 63) parts[np][k++] = *p++;
        parts[np++][k] = 0;
    }
    for (int i = 0; i < np; i++) {
        if (cur_size > (16u << 20)) { *err = DISC_NOT_ISO; return 0; }
        uint8_t* d = malloc(cur_size ? cur_size : 1);
        if (!d || !rd(h, cur_lba, d, cur_size)) { free(d); *err = DISC_SHORT; return 0; }
        char want[64];
        strcpy(want, parts[i]);
        char* semi = strchr(want, ';');
        if (semi) *semi = 0;
        int found = 0;
        for (uint32_t pos = 0; pos < cur_size;) {
            uint8_t n = d[pos];
            if (!n) { pos = (pos / SECTOR + 1) * SECTOR; continue; }   /* records never cross a sector */
            if (pos + n > cur_size || n < 34) break;
            const uint8_t* rec = d + pos;
            char name[256];
            int nl = rec[32];
            if (33 + nl > n) break;
            memcpy(name, rec + 33, (size_t)nl);
            name[nl] = 0;
            char* s2 = strchr(name, ';');
            if (s2) *s2 = 0;
            int is_dir = rec[25] & 2;
            if (!_stricmp(name, want) && (i == np - 1 || is_dir)) {
                cur_lba = le32(rec + 2);
                cur_size = le32(rec + 10);
                found = 1;
                break;
            }
            pos += n;
        }
        free(d);
        if (!found) return 0;
    }
    *lba = cur_lba;
    *size = cur_size;
    return 1;
}

int disc_check_iso(const wchar_t* path, wchar_t* why, size_t cap) {
    why[0] = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wfmt(why, cap, L"The file cannot be opened (error %lu).", GetLastError());
        return DISC_OPEN;
    }
    int r = DISC_OK, err = DISC_NOT_ISO;
    uint8_t pvd[SECTOR];
    uint32_t lba, size;
    char* cnf = NULL;
    uint8_t* elf = NULL;
    if (!rd(h, 16, pvd, SECTOR) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) {
        wfmt(why, cap, L"This is not an ISO 9660 disc image (.iso). Other formats (.bin/.cue, .chd, .cso, .zip) must be "
                       L"converted to .iso first.");
        r = DISC_NOT_ISO;
        goto out;
    }
    uint32_t root_lba = le32(pvd + 156 + 2), root_size = le32(pvd + 156 + 10);
    if (!find_file(h, root_lba, root_size, "SYSTEM.CNF;1", &lba, &size, &err) || size > 4096) {
        wfmt(why, cap, err == DISC_SHORT ? L"The image ends early: it is incomplete or damaged."
                                         : L"This image is not a game disc (it has no SYSTEM.CNF). Choose the image of your Sly 2 disc.");
        r = err == DISC_SHORT ? DISC_SHORT : DISC_NO_CNF;
        goto out;
    }
    cnf = calloc(1, size + 1);
    if (!rd(h, lba, cnf, size)) { wfmt(why, cap, L"The image ends early: it is incomplete or damaged."); r = DISC_SHORT; goto out; }
    char boot[128] = "";
    for (char* line = cnf; line && *line;) {
        char* next = strpbrk(line, "\r\n");
        if (next) *next++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!_strnicmp(line, "BOOT2", 5)) {
            char* eq = strchr(line, '=');
            if (eq) {
                eq++;
                while (*eq == ' ' || *eq == '\t') eq++;
                if (!_strnicmp(eq, "cdrom0:", 7)) {
                    strncpy(boot, eq + 7, sizeof boot - 1);
                    size_t bl = strlen(boot);
                    while (bl && (boot[bl - 1] == ' ' || boot[bl - 1] == '\t')) boot[--bl] = 0;
                }
            }
            break;
        }
        line = next;
    }
    if (!boot[0]) {
        wfmt(why, cap, L"This is not the right kind of game disc (a disc for an older console, or a data disc?). "
                       L"Choose the image of your Sly 2 disc.");
        r = DISC_NOT_PS2;
        goto out;
    }
    char name[64] = "";
    {
        const char* b = boot;
        while (*b == '\\' || *b == '/') b++;
        strncpy(name, b, sizeof name - 1);
        char* semi = strchr(name, ';');
        if (semi) *semi = 0;
    }
    if (_stricmp(name, "SCUS_973.16")) {
        wchar_t wn[64];
        MultiByteToWideChar(CP_ACP, 0, name, -1, wn, 64);
        const wchar_t* hint = !_strnicmp(name, "SCES_", 5) || !_strnicmp(name, "SLES_", 5) ? L" (a European / PAL disc)"
                            : !_strnicmp(name, "SCPS_", 5) || !_strnicmp(name, "SLPS_", 5) || !_strnicmp(name, "SCAJ_", 5)
                                ? L" (a Japanese / Asian disc)"
                                : L"";
        wfmt(why, cap, L"This disc is %ls%ls: another game or another region. The PC version needs Sly 2: Band of "
                       L"Thieves, USA release (SCUS-97316).", wn, hint);
        r = DISC_OTHER_GAME;
        goto out;
    }
    if (!find_file(h, root_lba, root_size, boot, &lba, &size, &err)) {
        wfmt(why, cap, err == DISC_SHORT ? L"The image ends early: it is incomplete or damaged."
                                         : L"The boot file SCUS_973.16 is missing from the image: it is damaged.");
        r = DISC_SHORT;
        goto out;
    }
    if (size != SETUP_ELF_SIZE) {
        wfmt(why, cap, L"This is a different version of Sly 2 (SCUS_973.16 is %lu bytes, the USA v1.00 release has %lu): "
                       L"a modified or patched image cannot be used.", (unsigned long)size, (unsigned long)SETUP_ELF_SIZE);
        r = DISC_OTHER_VERSION;
        goto out;
    }
    elf = malloc(size);
    if (!elf || !rd(h, lba, elf, size)) { wfmt(why, cap, L"The image ends early: it is incomplete or damaged."); r = DISC_SHORT; goto out; }
    uint8_t d[32];
    BCRYPT_ALG_HANDLE alg;
    int hashed = 0;
    if (!BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) {
        hashed = BCryptHash(alg, NULL, 0, elf, size, d, 32) == 0;
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (!hashed || !sha256_equal_hex(d, SETUP_ELF_SHA256)) {
        wfmt(why, cap, L"This is a different version of Sly 2 or a damaged image (SCUS_973.16 does not match the USA "
                       L"v1.00 release): make a fresh, unmodified image of your disc.");
        r = DISC_OTHER_VERSION;
        goto out;
    }
    wfmt(why, cap, L"Sly 2: Band of Thieves (USA, SCUS-97316): OK.");
out:
    free(cnf);
    free(elf);
    CloseHandle(h);
    return r;
}
