/* Disc image reading for start-up: ISO 9660 directory lookup, SYSTEM.CNF, SHA-256. See disc.h. */
#include "disc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* disc_why(int err) {
    switch (err) {
    case DISC_OK:                return "ok";
    case DISC_ERR_OPEN:          return "the file cannot be opened";
    case DISC_ERR_NOT_ISO:       return "it is not a DVD disc image (.iso with an ISO 9660 file system)";
    case DISC_ERR_NO_CNF:        return "it is not a PlayStation 2 game disc (no SYSTEM.CNF)";
    case DISC_ERR_NO_BOOT:       return "its SYSTEM.CNF names no boot executable, or the executable is missing";
    case DISC_ERR_WRONG_GAME:    return "it is a different game";
    case DISC_ERR_WRONG_VERSION: return "it is a different version of the game than this build was made from";
    case DISC_ERR_READ:          return "the image cannot be read completely (damaged or truncated?)";
    default:                     return "unknown error";
    }
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(FILE* f, uint64_t off, void* dst, size_t n) {
    return _fseeki64(f, (int64_t)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

/* Find `name` (case-insensitive, ";version" ignored) in the directory whose extent is lbn/size.
 * Returns 1 and the entry's extent, length and directory flag. */
static int dir_lookup(FILE* f, uint32_t lbn, uint32_t size, const char* name, uint32_t* elbn, uint32_t* esize,
                      int* is_dir) {
    if (size > 4u << 20) return 0;                       /* no PS2 directory is anywhere near this big */
    uint8_t sec[DISC_SECTOR];
    size_t want = strlen(name);
    for (uint32_t s = 0; s * DISC_SECTOR < size; s++) {
        if (!read_at(f, (uint64_t)(lbn + s) * DISC_SECTOR, sec, DISC_SECTOR)) return 0;
        for (uint32_t o = 0; o + 33 < DISC_SECTOR;) {
            uint8_t len = sec[o];
            if (len == 0) break;                         /* records never span sectors: rest is padding */
            if (len < 33 || o + len > DISC_SECTOR) break;
            uint8_t nlen = sec[o + 32];
            const char* n = (const char*)sec + o + 33;
            if (33u + nlen <= len) {
                size_t k = 0;
                while (k < nlen && n[k] != ';') k++;     /* "NAME.EXT;1" */
                if (k > 0 && n[k - 1] == '.') k--;        /* "NAME.;1" (no extension) */
                if (k == want && !_strnicmp(n, name, k)) {
                    *elbn = le32(sec + o + 2);
                    *esize = le32(sec + o + 10);
                    *is_dir = (sec[o + 25] & 2) != 0;
                    return 1;
                }
            }
            o += len;
        }
    }
    return 0;
}

/* Walk a path like "\\SCUS_973.16" or "\\DIR\\FILE.ELF" from the root directory. */
static int path_lookup(FILE* f, uint32_t root_lbn, uint32_t root_size, const char* path, uint32_t* lbn,
                       uint32_t* size) {
    uint32_t dl = root_lbn, ds = root_size;
    char part[64];
    while (*path) {
        while (*path == '\\' || *path == '/') path++;
        size_t k = 0;
        while (path[k] && path[k] != '\\' && path[k] != '/') k++;
        if (!k) break;
        if (k >= sizeof part) return 0;
        memcpy(part, path, k);
        part[k] = 0;
        path += k;
        int last = !*path || !path[strspn(path, "\\/")];
        uint32_t el, es;
        int dir;
        if (!dir_lookup(f, dl, ds, part, &el, &es, &dir)) return 0;
        if (last) {
            if (dir) return 0;
            *lbn = el;
            *size = es;
            return 1;
        }
        if (!dir) return 0;
        dl = el;
        ds = es;
    }
    return 0;
}

/* "BOOT2 = cdrom0:\SCUS_973.16;1" -> "\SCUS_973.16" (path on the disc, version dropped). */
static int parse_boot2(const char* cnf, char* path, size_t cap) {
    for (const char* line = cnf; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "BOOT2", 5)) continue;
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != '=') continue;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "cdrom0:", 7)) return 0;
        p += 7;
        size_t n = 0;
        while (p[n] && p[n] != ';' && p[n] != '\r' && p[n] != '\n' && p[n] != ' ' && p[n] != '\t') n++;
        if (!n || n >= cap) return 0;
        memcpy(path, p, n);
        path[n] = 0;
        return 1;
    }
    return 0;
}

int disc_find_boot(const wchar_t* path, disc_boot* out) {
    memset(out, 0, sizeof *out);
    FILE* f = _wfopen(path, L"rb");
    if (!f) return DISC_ERR_OPEN;
    int err = DISC_OK;
    uint8_t pvd[DISC_SECTOR];
    uint64_t file_size = 0;
    if (_fseeki64(f, 0, SEEK_END) == 0) file_size = (uint64_t)_ftelli64(f);
    if (!read_at(f, 16ull * DISC_SECTOR, pvd, sizeof pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) {
        err = DISC_ERR_NOT_ISO;
        goto done;
    }
    uint32_t root_lbn = le32(pvd + 156 + 2), root_size = le32(pvd + 156 + 10);
    uint32_t cl, cs;
    if (!path_lookup(f, root_lbn, root_size, "SYSTEM.CNF", &cl, &cs)) { err = DISC_ERR_NO_CNF; goto done; }
    char cnf[1024];
    if (cs >= sizeof cnf) cs = sizeof cnf - 1;
    if (!read_at(f, (uint64_t)cl * DISC_SECTOR, cnf, cs)) { err = DISC_ERR_READ; goto done; }
    cnf[cs] = 0;
    char boot[128];
    if (!parse_boot2(cnf, boot, sizeof boot)) { err = DISC_ERR_NO_BOOT; goto done; }
    const char* base = boot + strlen(boot);
    while (base > boot && base[-1] != '\\' && base[-1] != '/') base--;
    if (strlen(base) >= sizeof out->boot_name) { err = DISC_ERR_NO_BOOT; goto done; }
    strcpy(out->boot_name, base);
    if (!path_lookup(f, root_lbn, root_size, boot, &out->lbn, &out->size)) { err = DISC_ERR_NO_BOOT; goto done; }
    if ((uint64_t)out->lbn * DISC_SECTOR + out->size > file_size) err = DISC_ERR_READ;
done:
    fclose(f);
    return err;
}

int disc_check(const wchar_t* path, const char* want_name, uint32_t want_size, const char* want_sha256,
               uint8_t** elf_out, size_t* elf_size) {
    disc_boot b;
    int err = disc_find_boot(path, &b);
    if (err) return err;
    if (_stricmp(b.boot_name, want_name)) return DISC_ERR_WRONG_GAME;
    if (b.size != want_size) return DISC_ERR_WRONG_VERSION;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return DISC_ERR_OPEN;
    uint8_t* buf = malloc(b.size ? b.size : 1);
    int ok = buf && read_at(f, (uint64_t)b.lbn * DISC_SECTOR, buf, b.size);
    fclose(f);
    if (!ok) { free(buf); return DISC_ERR_READ; }
    char hex[65];
    disc_sha256_hex(buf, b.size, hex);
    if (_stricmp(hex, want_sha256)) { free(buf); return DISC_ERR_WRONG_VERSION; }
    if (elf_out) { *elf_out = buf; if (elf_size) *elf_size = b.size; }
    else free(buf);
    return DISC_OK;
}

/* ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void disc_sha256_hex(const void* data, size_t n, char out[65]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t* p = data;
    size_t left = n;
    for (; left >= 64; left -= 64, p += 64) sha_block(h, p);
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tn = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tn - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(h, tail);
    if (tn == 128) sha_block(h, tail + 64);
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
}
