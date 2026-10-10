/* IOP memory card service (mcserv) backed by host files.
 *
 * High-level emulation of the two IOP modules the game ships in its .irx section and loads at
 * boot: XMCSERV 2.10 (SIF RPC server 0x80000400) and XMCMAN 2.26 (the card file system). Port 0
 * slot 0 holds a formatted 8 MB PS2 card whose files live in a host directory (PS2_MEMCARD_DIR,
 * else memcard\slot0 next to the executable); port 1 is empty.
 *
 * Everything here follows the game's own code: libmc on the EE side and the mcserv/mcman IRX
 * images that run on the IOP. notes/memcard-protocol.md documents the protocol (function numbers,
 * argument and reply layouts, how file data moves between EE and IOP) and the mcman behaviour
 * reproduced below (result codes, open flags, directory listing and pattern matching rules).
 *
 * Host layout: a card directory is a host directory and a card file a host file. Card names are
 * case-sensitive byte strings; bytes Windows can't keep in a name are escaped as %XX. Each host
 * directory has a sidecar "%meta" holding the directory's own entry (".") and, in card order, its
 * entries with each file's mode, attribute word and timestamps (PS2 clock time, JST). Escaping
 * never produces a name starting with '%' followed by non-hex characters, so sidecars can't clash
 * with card files.
 *
 * All calls complete synchronously inside the RPC; sif.c delivers the RPC end packet, whose
 * callback (libmc's) signals the semaphore sceMcSync waits on.
 */
#include "hw.h"

#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <wchar.h>
#include <windows.h>

#define MC_SID             0x80000400u
#define MCSERV_VERSION     0x210u            /* XMCSERV 2.10 (mcserv .data) */
#define MCMAN_VERSION      0x226u            /* XMCMAN 2.26 (mcman module info) */

#define MC_CLUSTER         1024u             /* bytes per cluster: 2 pages of 512 */
#define MC_ALLOC_CLUSTERS  8001u             /* allocatable clusters on an 8 MB card; the root uses 1 */
#define MC_MAX_FD          3                 /* mcman's file handle table */
#define MC_NAME_MAX        32                /* bytes in a directory entry's name field */
#define MC_SUBDIR_ENTRIES  20                /* entries (with . and ..) a subdirectory may hold */
#define MC_MAX_ENT         1024              /* host-side limit on entries per directory */
#define MC_PATH_MAX        1024
#define HOST_MAX           1024              /* wchar_t */
#define IO_CHUNK           0x2000u           /* mcserv moves file data in 8 KB pieces */
#define ESC_MAX            (3 * MC_NAME_MAX + 1)

/* sceMcRes* result codes */
enum { MC_OK = 0, MC_NEWCARD = -1, MC_NOFORMAT = -2, MC_FULL = -3, MC_NOENTRY = -4, MC_DENIED = -5,
       MC_NOTEMPTY = -6, MC_UPLIMIT = -7, MC_NOCARD = -10, MC_REPLACE_OPEN = -43 };

/* directory entry mode bits (sceMcFileAttr*) and open flags */
enum { M_READ = 0x0001, M_WRITE = 0x0002, M_EXEC = 0x0004, M_FILE = 0x0010, M_SUBDIR = 0x0020,
       M_CLOSED = 0x0080, M_HIDDEN = 0x2000, M_EXISTS = 0x8000 };
enum { O_RD = 0x0001, O_WR = 0x0002, O_MKDIR = 0x0040, O_CREATE = 0x0200, O_KEEP = 0x1800 };
#define MODE_NEW_FILE  0x8417u               /* exists | file | rwx; M_CLOSED is added on close */
#define MODE_FILE      0x8497u
#define MODE_DIR       0x8427u               /* exists | 0x400 | subdir | rwx */
#define ATTR_SET_MASK  0x180Fu               /* mode bits sceMcSetFileInfo may change */

/* mcserv function numbers (they equal libmc's sceMcFuncNo* command codes) */
enum { FN_GETINFO = 0x01, FN_OPEN = 0x02, FN_CLOSE = 0x03, FN_SEEK = 0x04, FN_READ = 0x05,
       FN_WRITE = 0x06, FN_FLUSH = 0x0A, FN_CHDIR = 0x0C, FN_GETDIR = 0x0D, FN_SETFILEINFO = 0x0E,
       FN_DELETE = 0x0F, FN_FORMAT = 0x10, FN_UNFORMAT = 0x11, FN_GETENTSPACE = 0x12,
       FN_CHGPRITY = 0x14, FN_SLOTMAX = 0x15, FN_X33 = 0x33, FN_INIT = 0xFE };

/* ---- small helpers -------------------------------------------------------------------------- */

static uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t get16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }

/* IOP -> EE transfers are SIF DMAs into main RAM; addresses wrap inside RAM like the DMAC's. */
static void ee_write(uint32_t addr, const void* src, uint32_t n) {
    const uint8_t* s = src;
    while (n) {
        uint32_t off = addr & (PS2_RAM_SIZE - 1), k = PS2_RAM_SIZE - off;
        if (k > n) k = n;
        memcpy(ps2_ram + off, s, k);
        addr += k; s += k; n -= k;
    }
}
static void ee_read(uint32_t addr, void* dst, uint32_t n) {
    uint8_t* d = dst;
    while (n) {
        uint32_t off = addr & (PS2_RAM_SIZE - 1), k = PS2_RAM_SIZE - off;
        if (k > n) k = n;
        memcpy(d, ps2_ram + off, k);
        addr += k; d += k; n -= k;
    }
}

static void log_once(const char* what, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_once(const char* what, const char* fmt, ...) {
    static const char* seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == what) return;
    if (n < 64) seen[n++] = what;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[mc] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---- timestamps: sceMcStDateTime {resv, sec, min, hour, day, month, year16}, PS2 clock (JST) -- */

typedef struct { uint8_t b[8]; } mc_time;

static mc_time time_from_unix(int64_t t) {
    mc_time m = {{0}};
    t += 9 * 3600;                           /* the PS2's clock keeps Japan time */
    if (t < 0) t = 0;
    time_t tt = (time_t)t;
    struct tm* g = gmtime(&tt);
    if (!g) return m;
    m.b[1] = (uint8_t)g->tm_sec; m.b[2] = (uint8_t)g->tm_min; m.b[3] = (uint8_t)g->tm_hour;
    m.b[4] = (uint8_t)g->tm_mday; m.b[5] = (uint8_t)(g->tm_mon + 1);
    put16(m.b + 6, (uint16_t)(g->tm_year + 1900));
    return m;
}
static mc_time time_now(void) { return time_from_unix((int64_t)time(NULL)); }
static mc_time time_from_filetime(FILETIME ft) {
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return time_from_unix((int64_t)(v / 10000000u) - 11644473600LL);
}

static void time_text(const mc_time* t, char* out) {
    out += sprintf(out, "%04u-%02u-%02uT%02u:%02u:%02u", get16(t->b + 6), t->b[5], t->b[4], t->b[3],
                   t->b[2], t->b[1]);
    if (t->b[0]) sprintf(out, "+%02X", t->b[0]);
}
static int time_parse(const char* s, mc_time* t) {
    unsigned y, mo, d, h, mi, se, r = 0;
    if (sscanf(s, "%u-%u-%uT%u:%u:%u", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    const char* plus = strchr(s, '+');
    if (plus) sscanf(plus + 1, "%x", &r);
    t->b[0] = (uint8_t)r; t->b[1] = (uint8_t)se; t->b[2] = (uint8_t)mi; t->b[3] = (uint8_t)h;
    t->b[4] = (uint8_t)d; t->b[5] = (uint8_t)mo; put16(t->b + 6, (uint16_t)y);
    return 1;
}

/* ---- card names <-> host names ----------------------------------------------------------------- */

static int plain_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c && strchr("!#$&'()+,-.;=@[]^_`{}~", c));
}

/* Windows device names (CON, COM1, ...) can't be file names, whatever the extension. */
static int device_name(const char* n) {
    size_t len = strcspn(n, ".");
    static const char* const dev3[] = { "CON", "PRN", "AUX", "NUL" };
    if (len == 3)
        for (int i = 0; i < 4; i++) if (_strnicmp(n, dev3[i], 3) == 0) return 1;
    if (len == 4 && (_strnicmp(n, "COM", 3) == 0 || _strnicmp(n, "LPT", 3) == 0) && n[3] >= '1' && n[3] <= '9')
        return 1;
    return 0;
}

/* Card name (up to 32 bytes) -> host name: unsafe bytes, a device name's first byte and a trailing
 * '.' become %XX. The mapping is one-to-one; escape_name(unescape_name(h)) == h is enforced. */
static void escape_name(const char* name, char* out) {
    char n[MC_NAME_MAX + 1];
    size_t len = strnlen(name, MC_NAME_MAX);
    memcpy(n, name, len);
    n[len] = 0;
    int dev = device_name(n);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (!plain_char(c) || (i == 0 && dev) || (i == len - 1 && c == '.')) out += sprintf(out, "%%%02X", c);
        else *out++ = (char)c;
    }
    *out = 0;
}

static int hexval(wchar_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Host name -> card name. 0 if the host name isn't one escape_name produces (sidecars, foreign
 * files, "." and ".."), so those never show up on the card. */
static int unescape_name(const wchar_t* h, char* out) {
    char host[ESC_MAX];
    size_t k = 0, n = 0;
    for (; h[n]; n++) {
        if (h[n] >= 0x7F || h[n] < 0x20 || n >= ESC_MAX - 1) return 0;
        host[n] = (char)h[n];
    }
    host[n] = 0;
    for (size_t i = 0; i < n; ) {
        if (k == MC_NAME_MAX) return 0;
        if (host[i] == '%') {
            if (i + 2 >= n) return 0;
            int hi = hexval(host[i + 1]), lo = hexval(host[i + 2]);
            if (hi < 0 || lo < 0) return 0;
            out[k++] = (char)(hi * 16 + lo);
            i += 3;
        } else out[k++] = host[i++];
    }
    out[k] = 0;
    if (k == 0 || strlen(out) != k || !strcmp(out, ".") || !strcmp(out, "..")) return 0;
    char back[ESC_MAX];
    escape_name(out, back);
    return strcmp(back, host) == 0;
}

/* Is this host directory entry a card entry? Hidden/system host files (desktop.ini, Thumbs.db)
 * and names that aren't escape_name output are not. */
static int card_entry(const WIN32_FIND_DATAW* fd, char* name) {
    if (fd->dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return 0;
    return unescape_name(fd->cFileName, name);
}

/* ---- card state --------------------------------------------------------------------------------- */

static struct {
    int      init, ok, formatted;
    int      seen;                       /* GetInfo has reported the card (first report: new card) */
    wchar_t  root[HOST_MAX];
    char     cwd[MC_PATH_MAX];           /* current directory: canonical absolute card path */
    uint32_t used;                       /* allocated clusters (cache) */
    int      used_valid;
    uint64_t used_tick;
} card;

typedef struct {
    int      open, rd, wr, is_dir, dirty;
    uint32_t pos;
    char     path[MC_PATH_MAX];          /* canonical card path */
} mc_fd;
static mc_fd fds[MC_MAX_FD];

static int path_is_root(const char* canon) { return canon[0] == '/' && canon[1] == 0; }

/* Host path of a canonical card path ("/" or "/A/B"), optionally with `extra` (a raw host name)
 * appended. 0 if it would be too long. */
static int host_path(const char* canon, const wchar_t* extra, wchar_t* out) {
    size_t len = wcslen(card.root);
    if (len >= HOST_MAX) return 0;
    wmemcpy(out, card.root, len + 1);
    for (const char* p = canon; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[MC_NAME_MAX + 1], esc[ESC_MAX];
        size_t k = 0;
        while (*p && *p != '/') { if (k < MC_NAME_MAX) comp[k++] = *p; p++; }
        comp[k] = 0;
        escape_name(comp, esc);
        size_t el = strlen(esc);
        if (len + 1 + el >= HOST_MAX) return 0;
        out[len++] = L'\\';
        for (size_t i = 0; i < el; i++) out[len++] = (wchar_t)(unsigned char)esc[i];
        out[len] = 0;
    }
    if (extra) {
        size_t el = wcslen(extra);
        if (len + 1 + el >= HOST_MAX) return 0;
        out[len++] = L'\\';
        wmemcpy(out + len, extra, el + 1);
    }
    return 1;
}

/* canonical path of `name` inside directory `dir` */
static int path_join(const char* dir, const char* name, char* out) {
    size_t dl = strlen(dir), nl = strnlen(name, MC_NAME_MAX);
    if (dl + 1 + nl >= MC_PATH_MAX) return 0;
    if (out != dir) memmove(out, dir, dl);
    if (!path_is_root(dir)) out[dl++] = '/';
    memcpy(out + dl, name, nl);
    out[dl + nl] = 0;
    return 1;
}
static void path_up(char* canon) {
    char* s = strrchr(canon, '/');
    if (!s || s == canon) { canon[0] = '/'; canon[1] = 0; } else *s = 0;
}

static int is_dir_w(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void mkdir_p(wchar_t* p) {
    for (wchar_t* s = p; *s; s++) {
        if ((*s == L'\\' || *s == L'/') && s > p && s[-1] != L':') {
            wchar_t c = *s; *s = 0;
            CreateDirectoryW(p, NULL);
            *s = c;
        }
    }
    CreateDirectoryW(p, NULL);
}

/* Files and directories copied onto the host read-only (from a CD, an archive) are still writable
 * card entries: the card's own mode decides. Clear the host attribute before the host refuses a
 * change the card allows. */
static void host_writable(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_READONLY)) return;
    a &= FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_SYSTEM;
    SetFileAttributesW(path, a ? a : FILE_ATTRIBUTE_NORMAL);
}

/* CreateFileW for writing an existing host file, read-only or not. */
static HANDLE open_existing_w(const wchar_t* path, DWORD access, DWORD share, DWORD disposition) {
    HANDLE h = CreateFileW(path, access, share, NULL, disposition, 0, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        host_writable(path);
        h = CreateFileW(path, access, share, NULL, disposition, 0, NULL);
    }
    return h;
}

static int write_file_w(const wchar_t* path, const void* data, uint32_t n) {
    host_writable(path);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, n, &w, NULL) && w == n;
    CloseHandle(h);
    return ok;
}

/* ---- directory listings and sidecars --------------------------------------------------------- */

typedef struct {
    char     name[MC_NAME_MAX + 1];
    uint8_t  is_dir;
    uint32_t size;                        /* files: length in bytes */
    uint16_t mode;
    uint32_t attr;                        /* the entry's word at 0x20 (sceMcTblGetDir.Reserve2) */
    mc_time  ctime, mtime;
} mc_ent;

typedef struct {
    mc_ent self;                          /* the directory's own entry (its "." line) */
    int    n;
    mc_ent e[MC_MAX_ENT];
} mc_dir;

static mc_dir dir_a, dir_b;               /* scratch listings for one operation */

static void ent_fix_type(mc_ent* e) {
    e->mode |= M_EXISTS;
    if (e->is_dir) e->mode = (uint16_t)((e->mode & ~M_FILE) | M_SUBDIR);
    else e->mode = (uint16_t)((e->mode & ~M_SUBDIR) | M_FILE);
}

static int find_ent(const mc_dir* d, const char* name) {
    for (int i = 0; i < d->n; i++) if (strcmp(d->e[i].name, name) == 0) return i;
    return -1;
}

/* Read a sidecar into `buf` (NUL-terminated). Returns its length (0 if absent). */
static char sidecar_buf[256 * 1024];
static size_t read_sidecar(const wchar_t* dir) {
    wchar_t p[HOST_MAX];
    sidecar_buf[0] = 0;
    if (wcslen(dir) + 7 >= HOST_MAX) return 0;
    wcscpy(p, dir); wcscat(p, L"\\%meta");
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(h, sidecar_buf, sizeof sidecar_buf - 1, &got, NULL);
    CloseHandle(h);
    sidecar_buf[got] = 0;
    return got;
}

/* Parse one sidecar line "<host name> <mode> <attr> <ctime> <mtime>". */
static int parse_meta_line(char* line, char* hostname, mc_ent* e) {
    char ct[48], mt[48];
    unsigned mode, attr;
    if (sscanf(line, "%96s %x %x %47s %47s", hostname, &mode, &attr, ct, mt) != 5) return 0;
    e->mode = (uint16_t)mode;
    e->attr = attr;
    return time_parse(ct, &e->ctime) && time_parse(mt, &e->mtime);
}

static void default_self(const wchar_t* dir, mc_ent* self) {
    memset(self, 0, sizeof *self);
    self->is_dir = 1;
    self->mode = MODE_DIR;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(dir, GetFileExInfoStandard, &fa)) {
        self->ctime = time_from_filetime(fa.ftCreationTime);
        self->mtime = time_from_filetime(fa.ftLastWriteTime);
    } else self->ctime = self->mtime = time_now();
}

/* The "." line of directory `dir`'s sidecar (its own entry), or defaults from the host. */
static void load_self(const wchar_t* dir, mc_ent* self) {
    default_self(dir, self);
    if (!read_sidecar(dir)) return;
    for (char* line = strtok(sidecar_buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char hn[ESC_MAX + 1];
        mc_ent e;
        if (parse_meta_line(line, hn, &e) && strcmp(hn, ".") == 0) {
            self->mode = e.mode; self->attr = e.attr; self->ctime = e.ctime; self->mtime = e.mtime;
            break;
        }
    }
    strcpy(self->name, ".");
    ent_fix_type(self);
}

/* List directory `canon`: host entries in sidecar order (then any others the sidecar doesn't know
 * about), with each file's metadata from the sidecar and each subdirectory's from its own. */
static int dir_load(const char* canon, mc_dir* d) {
    static wchar_t host[HOST_MAX], pat[HOST_MAX], child[HOST_MAX];
    static mc_ent found[MC_MAX_ENT];
    static uint8_t taken[MC_MAX_ENT];
    int nf = 0;
    d->n = 0;
    if (!host_path(canon, NULL, host) || !host_path(canon, L"*", pat)) return 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        mc_ent* e = &found[nf];
        if (!card_entry(&fd, e->name)) {
            if (fd.cFileName[0] != L'.' && fd.cFileName[0] != L'%' &&
                !(fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                log_once("foreign", "ignoring host files that aren't card names (e.g. %ls)", fd.cFileName);
            continue;
        }
        if (nf == MC_MAX_ENT) { log_once("toomany", "more than %d entries in a directory; rest ignored", MC_MAX_ENT); break; }
        e->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
        e->mode = e->is_dir ? MODE_DIR : MODE_FILE;
        e->attr = 0;
        e->ctime = time_from_filetime(fd.ftCreationTime);
        e->mtime = time_from_filetime(fd.ftLastWriteTime);
        taken[nf++] = 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    default_self(host, &d->self);
    if (read_sidecar(host)) {
        for (char* line = strtok(sidecar_buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            char hn[ESC_MAX + 2], name[MC_NAME_MAX + 1];
            mc_ent m;
            wchar_t wn[ESC_MAX + 2];
            int meta = parse_meta_line(line, hn, &m);
            if (!meta) {                                   /* "<host name>/": a subdirectory */
                if (sscanf(line, "%97s", hn) != 1) continue;
                size_t l = strlen(hn);
                if (l < 2 || hn[l - 1] != '/') continue;
                hn[l - 1] = 0;
            } else if (strcmp(hn, ".") == 0) {
                d->self.mode = m.mode; d->self.attr = m.attr; d->self.ctime = m.ctime; d->self.mtime = m.mtime;
                continue;
            }
            size_t l = strlen(hn);
            for (size_t i = 0; i <= l; i++) wn[i] = (wchar_t)(unsigned char)hn[i];
            if (!unescape_name(wn, name)) continue;
            for (int i = 0; i < nf; i++) {
                if (taken[i] || strcmp(found[i].name, name) != 0) continue;
                taken[i] = 1;
                mc_ent* e = &d->e[d->n++];
                *e = found[i];
                if (meta && !e->is_dir) { e->mode = m.mode; e->attr = m.attr; e->ctime = m.ctime; e->mtime = m.mtime; }
                break;
            }
        }
    }
    for (int i = 0; i < nf; i++) if (!taken[i]) d->e[d->n++] = found[i];
    for (int i = 0; i < d->n; i++) {
        mc_ent* e = &d->e[i];
        if (e->is_dir) {                                   /* a directory's entry lives in its own sidecar */
            char name[MC_NAME_MAX + 1];
            strcpy(name, e->name);
            wchar_t esc_w[ESC_MAX];
            char esc[ESC_MAX];
            escape_name(name, esc);
            for (size_t k = 0; k <= strlen(esc); k++) esc_w[k] = (wchar_t)(unsigned char)esc[k];
            if (!host_path(canon, esc_w, child)) continue;
            load_self(child, e);
            strcpy(e->name, name);
            e->size = 0;
        }
        ent_fix_type(e);
    }
    strcpy(d->self.name, ".");
    ent_fix_type(&d->self);
    return 1;
}

/* Write directory `canon`'s sidecar (atomically: temp file + rename). */
static int dir_save(const char* canon, const mc_dir* d) {
    static char text[MC_MAX_ENT * 170 + 256];
    static wchar_t tmp[HOST_MAX], fin[HOST_MAX];
    if (!host_path(canon, L"%meta.tmp", tmp) || !host_path(canon, L"%meta", fin)) return 0;
    char ct[32], mt[32], esc[ESC_MAX];
    size_t n = (size_t)sprintf(text, "# memory card directory metadata: name mode attr created modified (JST)\n");
    time_text(&d->self.ctime, ct); time_text(&d->self.mtime, mt);
    n += (size_t)sprintf(text + n, ". %04X %08X %s %s\n", d->self.mode, d->self.attr, ct, mt);
    for (int i = 0; i < d->n; i++) {
        const mc_ent* e = &d->e[i];
        escape_name(e->name, esc);
        if (e->is_dir) { n += (size_t)sprintf(text + n, "%s/\n", esc); continue; }
        time_text(&e->ctime, ct); time_text(&e->mtime, mt);
        n += (size_t)sprintf(text + n, "%s %04X %08X %s %s\n", esc, e->mode, e->attr, ct, mt);
    }
    if (!write_file_w(tmp, text, (uint32_t)n)) {
        log_once("sidecar", "cannot write %ls", fin);
        return 0;
    }
    host_writable(fin);
    if (!MoveFileExW(tmp, fin, MOVEFILE_REPLACE_EXISTING)) {
        log_once("sidecar", "cannot write %ls", fin);
        return 0;
    }
    return 1;
}

/* ---- card setup, space accounting ---------------------------------------------------------------- */

/* card.root = `dir` as an absolute \\?\ path, so card paths aren't limited to MAX_PATH (260
 * characters, 248 for directories; escaped card names alone can take 96 per level). A drive root
 * stays as it is. */
static int set_root(const wchar_t* dir) {
    wchar_t full[HOST_MAX];
    DWORD n = GetFullPathNameW(dir, HOST_MAX, full, NULL);
    if (n == 0 || n >= HOST_MAX) return 0;
    size_t len = wcslen(full);
    while (len > 3 && (full[len - 1] == L'\\' || full[len - 1] == L'/')) full[--len] = 0;
    const wchar_t* prefix = L"\\\\?\\";
    const wchar_t* rest = full;
    if (!wcsncmp(full, L"\\\\?\\", 4) || !wcsncmp(full, L"\\\\.\\", 4) || len <= 3) prefix = L"";
    else if (full[0] == L'\\' && full[1] == L'\\') { prefix = L"\\\\?\\UNC\\"; rest = full + 2; }
    if (wcslen(prefix) + wcslen(rest) >= HOST_MAX - 64) return 0;
    wcscpy(card.root, prefix);
    wcscat(card.root, rest);
    return 1;
}

static void card_setup(void) {
    if (card.init) return;
    card.init = 1;
    strcpy(card.cwd, "/");
    wchar_t dir[HOST_MAX];
    const wchar_t* env = _wgetenv(L"PS2_MEMCARD_DIR");
    if (env && *env && wcslen(env) < HOST_MAX - 64) wcscpy(dir, env);
    else {
        DWORD n = GetModuleFileNameW(NULL, dir, HOST_MAX - 64);
        if (n == 0 || n >= HOST_MAX - 64) { fprintf(stderr, "[mc] cannot locate the executable: no memory card\n"); return; }
        wchar_t* s = wcsrchr(dir, L'\\');
        if (s) s[1] = 0; else dir[0] = 0;
        wcscat(dir, L"memcard\\slot0");
    }
    if (!set_root(dir)) { fprintf(stderr, "[mc] cannot use %ls: no memory card\n", dir); return; }
    mkdir_p(card.root);
    if (!is_dir_w(card.root)) { fprintf(stderr, "[mc] cannot use %ls: no memory card\n", dir); return; }
    card.ok = 1;
    wchar_t p[HOST_MAX];
    host_path("/", L"%unformatted", p);
    card.formatted = GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES;
    host_path("/", L"%meta", p);
    if (card.formatted && GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
        dir_load("/", &dir_a);                    /* fresh card: record the root's creation time */
        dir_a.self.ctime = dir_a.self.mtime = time_now();
        dir_save("/", &dir_a);
    }
    fprintf(stderr, "[mc] memory card (port 0 slot 0, 8 MB%s) in %ls\n", card.formatted ? "" : ", unformatted", dir);
}

static uint32_t count_used(const wchar_t* dir, int depth) {
    wchar_t pat[HOST_MAX];
    if (wcslen(dir) + 3 >= HOST_MAX) return 0;
    wcscpy(pat, dir); wcscat(pat, L"\\*");
    uint32_t used = 0, n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char name[MC_NAME_MAX + 1];
            if (!card_entry(&fd, name)) continue;
            n++;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (depth < 8 && wcslen(dir) + 1 + wcslen(fd.cFileName) < HOST_MAX) {
                    wcscpy(pat, dir); wcscat(pat, L"\\"); wcscat(pat, fd.cFileName);
                    used += count_used(pat, depth + 1);
                }
            } else {
                uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                used += (uint32_t)((size + MC_CLUSTER - 1) / MC_CLUSTER);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return used + (n + 2 + 1) / 2;               /* the directory's own clusters: 2 entries each */
}

/* Free clusters. Recounted when stale (our own changes invalidate it; host-side edits are picked
 * up within a second). */
static uint32_t card_free(void) {
    uint64_t now = GetTickCount64();
    if (!card.used_valid || now - card.used_tick > 1000) {
        card.used = count_used(card.root, 0);
        card.used_valid = 1;
        card.used_tick = now;
    }
    return card.used < MC_ALLOC_CLUSTERS ? MC_ALLOC_CLUSTERS - card.used : 0;
}
static void card_changed(void) { card.used_valid = 0; }

static int card_check(int port, int slot) {
    card_setup();
    if (port != 0 || slot != 0 || !card.ok) return MC_NOCARD;
    if (!card.formatted) return MC_NOFORMAT;
    return MC_OK;
}

/* ---- path resolution ------------------------------------------------------------------------------ */

typedef struct {
    int      status;                      /* 0 found, 1 missing (its directory exists), 2 directory missing */
    int      conflict;                    /* a host name differing only in case exists */
    int      is_root;
    int      is_dir;
    uint32_t size;
    char     path[MC_PATH_MAX];           /* canonical path of the target */
    char     parent[MC_PATH_MAX];         /* canonical path of the directory holding it */
    char     name[MC_NAME_MAX + 1];
} mc_res;

/* 1 = entry `name` exists in directory `dir` (canonical), 0 = no, -1 = only in another case */
static int lookup(const char* dir, const char* name, int* is_dir, uint32_t* size) {
    static wchar_t p[HOST_MAX];
    char esc[ESC_MAX];
    wchar_t esc_w[ESC_MAX];
    escape_name(name, esc);
    for (size_t k = 0; k <= strlen(esc); k++) esc_w[k] = (wchar_t)(unsigned char)esc[k];
    if (!host_path(dir, esc_w, p)) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    char got[MC_NAME_MAX + 1];
    if (!card_entry(&fd, got) || strcmp(got, name) != 0) return -1;
    if (is_dir) *is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (size) *size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
    return 1;
}

/* Walk `path` (absolute, or relative to the current directory) the way mcman does: every
 * component but the last must be an existing directory; "." and ".." are understood. */
static void resolve(const char* path, mc_res* r) {
    char cur[MC_PATH_MAX];
    memset(r, 0, sizeof *r);
    r->status = 2;
    strcpy(cur, path[0] == '/' ? "/" : card.cwd);
    if (!path_is_root(cur)) {                    /* the current directory may have gone */
        wchar_t p[HOST_MAX];
        if (!host_path(cur, NULL, p) || !is_dir_w(p)) return;
    }
    const char* p = path;
    for (;;) {
        while (*p == '/') p++;
        char comp[MC_NAME_MAX + 1];
        size_t k = 0;
        const char* q = p;
        while (*q && *q != '/') { if (k < MC_NAME_MAX) comp[k++] = *q; q++; }
        comp[k] = 0;
        const char* s = q;
        while (*s == '/') s++;
        int last = *s == 0;
        p = q;
        if (k == 0) break;                       /* end of path: the target is `cur` itself */
        if (!strcmp(comp, ".")) { if (last) break; continue; }
        if (!strcmp(comp, "..")) { path_up(cur); if (last) break; continue; }
        int is_dir = 0, lk = lookup(cur, comp, &is_dir, &r->size);
        if (last) {
            strcpy(r->parent, cur);
            strcpy(r->name, comp);
            if (!path_join(cur, comp, r->path)) return;
            r->status = lk == 1 ? 0 : 1;
            r->conflict = lk == -1;
            r->is_dir = lk == 1 && is_dir;
            return;
        }
        if (lk != 1 || !is_dir || !path_join(cur, comp, cur)) return;
    }
    strcpy(r->path, cur);
    r->is_dir = 1;
    r->size = 0;
    if (path_is_root(cur)) { r->is_root = 1; r->status = 0; strcpy(r->parent, "/"); return; }
    strcpy(r->parent, cur);
    path_up(r->parent);
    strcpy(r->name, strrchr(cur, '/') + 1);
    r->status = 0;
}

/* mcman refuses names with wildcards or control characters. */
static int valid_name(const char* s) {
    for (; *s; s++) if (*s == '?' || *s == '*' || (unsigned char)*s < 0x20) return 0;
    return 1;
}
/* Is the last path component "." or ".."? */
static int last_is_dot(const char* path) {
    const char* s = strrchr(path, '/');
    s = s ? s + 1 : path;
    return !strcmp(s, ".") || !strcmp(s, "..");
}

/* The target's own entry: files from their directory's sidecar, directories from their own. */
static int load_entry(const mc_res* r, mc_ent* e) {
    if (r->is_dir) {
        wchar_t p[HOST_MAX];
        if (!host_path(r->path, NULL, p)) return 0;
        load_self(p, e);
        strcpy(e->name, r->is_root ? "." : r->name);
        return 1;
    }
    if (!dir_load(r->parent, &dir_b)) return 0;
    int i = find_ent(&dir_b, r->name);
    if (i < 0) return 0;
    *e = dir_b.e[i];
    return 1;
}
static int store_entry(const mc_res* r, const mc_ent* e) {
    if (r->is_dir) {
        if (!dir_load(r->path, &dir_b)) return 0;
        dir_b.self.mode = e->mode; dir_b.self.attr = e->attr; dir_b.self.ctime = e->ctime; dir_b.self.mtime = e->mtime;
        return dir_save(r->path, &dir_b);
    }
    if (!dir_load(r->parent, &dir_b)) return 0;
    int i = find_ent(&dir_b, r->name);
    if (i < 0) return 0;
    dir_b.e[i] = *e;
    return dir_save(r->parent, &dir_b);
}

static int any_fd_on(const char* path, int skip, int writers_only) {
    for (int i = 0; i < MC_MAX_FD; i++)
        if (i != skip && fds[i].open && !strcmp(fds[i].path, path) && (!writers_only || fds[i].wr)) return 1;
    return 0;
}

/* ---- mcman operations -------------------------------------------------------------------------- */

static mc_res res_a, res_b;

static int32_t mc_open(int port, int slot, const char* name, uint32_t flags) {
    int r = card_check(port, slot);
    if (r) return r;
    if (!name[0]) return MC_NOENTRY;
    flags &= ~(uint32_t)M_HIDDEN;
    if (flags & O_CREATE) flags |= O_WR;
    if (!valid_name(name)) return MC_NOENTRY;
    int fd = 0;
    while (fd < MC_MAX_FD && fds[fd].open) fd++;
    if (fd == MC_MAX_FD) return MC_UPLIMIT;
    mc_res* res = &res_a;
    resolve(name, res);
    if (res->status == 2) return MC_NOENTRY;
    mc_time now = time_now();

    if (res->status == 1) {                                  /* create a file or a directory */
        if ((flags & O_MKDIR) && !path_is_root(res->parent)) return MC_DENIED;   /* only in the root */
        if (!(flags & (O_CREATE | O_MKDIR))) return MC_NOENTRY;
        if (!dir_load(res->parent, &dir_a)) return MC_DENIED;
        int entries = 2 + dir_a.n;
        if (!path_is_root(res->parent) && entries >= MC_SUBDIR_ENTRIES) return MC_DENIED;
        if (dir_a.n == MC_MAX_ENT) return MC_FULL;
        if (res->conflict) {
            log_once("case", "cannot create %s: the host already has a name differing only in case", res->path);
            return MC_DENIED;
        }
        uint32_t need = (entries % 2 == 0) + ((flags & O_MKDIR) ? 1 : 0);
        if (need > card_free()) return MC_FULL;
        wchar_t p[HOST_MAX];
        if (!host_path(res->path, NULL, p)) return MC_DENIED;
        mc_ent* e = &dir_a.e[dir_a.n];
        memset(e, 0, sizeof *e);
        strcpy(e->name, res->name);
        e->ctime = e->mtime = now;
        e->is_dir = (flags & O_MKDIR) != 0;
        if (e->is_dir) {
            if (!CreateDirectoryW(p, NULL)) { log_once("mkdir", "cannot create directory %ls", p); return MC_DENIED; }
            static mc_dir fresh;
            fresh.n = 0;
            fresh.self = *e;
            fresh.self.mode = (uint16_t)(MODE_DIR | (flags & O_KEEP));
            strcpy(fresh.self.name, ".");
            dir_save(res->path, &fresh);
        } else {
            HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) { log_once("create", "cannot create file %ls", p); return MC_DENIED; }
            CloseHandle(h);
            e->mode = (uint16_t)(MODE_NEW_FILE | (flags & O_KEEP));
        }
        ent_fix_type(e);
        dir_a.n++;
        dir_a.self.mtime = now;
        dir_save(res->parent, &dir_a);
        card_changed();
        if (e->is_dir) return MC_OK;                       /* sceMcMkdir */
        fds[fd] = (mc_fd){ 1, (flags & O_RD) != 0, (flags & O_WR) != 0, 0, 1, 0, {0} };
        strcpy(fds[fd].path, res->path);
        return fd;
    }

    if (flags & O_MKDIR) return MC_NOENTRY;                  /* already exists */
    mc_ent e;
    if (!load_entry(res, &e)) return MC_NOENTRY;
    if ((flags & O_WR) && !(e.mode & M_WRITE)) return MC_DENIED;
    if ((flags & O_RD) && !(e.mode & M_READ)) return MC_DENIED;
    if (res->is_dir) {
        if ((e.mode & (M_READ | M_EXEC)) != (M_READ | M_EXEC)) return MC_DENIED;
        fds[fd] = (mc_fd){ 1, 0, 0, 1, 0, 0, {0} };
        strcpy(fds[fd].path, res->path);
        return fd;
    }
    if ((flags & O_WR) && any_fd_on(res->path, -1, 1)) return MC_DENIED;
    int dirty = 0;
    if (flags & O_CREATE) {                                  /* the old entry is deleted and made anew */
        if (any_fd_on(res->path, -1, 0)) return MC_REPLACE_OPEN;
        wchar_t p[HOST_MAX];
        if (!host_path(res->path, NULL, p)) return MC_DENIED;
        HANDLE h = open_existing_w(p, GENERIC_WRITE, 0, TRUNCATE_EXISTING);
        if (h == INVALID_HANDLE_VALUE) { log_once("trunc", "cannot truncate %ls", p); return MC_DENIED; }
        CloseHandle(h);
        e.mode = (uint16_t)(MODE_NEW_FILE | (flags & O_KEEP));
        e.attr = 0;
        e.ctime = e.mtime = now;
        e.size = 0;
        store_entry(res, &e);
        card_changed();
        dirty = 1;
    }
    fds[fd] = (mc_fd){ 1, (flags & O_RD) != 0, (flags & O_WR) != 0, 0, dirty, 0, {0} };
    strcpy(fds[fd].path, res->path);
    return fd;
}

static mc_fd* get_fd(int fd) { return fd >= 0 && fd < MC_MAX_FD && fds[fd].open ? &fds[fd] : NULL; }

/* mcman's entry update for an open file: the "closed" bit set (close/flush) or cleared (first write
 * after an open or flush), and the time; its directory gets the time too. */
static void mark_entry(const mc_fd* f, int closed) {
    mc_res* res = &res_b;
    resolve(f->path, res);
    if (res->status != 0 || res->is_dir || !dir_load(res->parent, &dir_a)) return;
    int i = find_ent(&dir_a, res->name);
    if (i < 0) return;
    mc_time now = time_now();
    if (closed) dir_a.e[i].mode |= M_CLOSED;
    else dir_a.e[i].mode &= (uint16_t)~M_CLOSED;
    dir_a.e[i].mtime = now;
    dir_a.self.mtime = now;
    dir_save(res->parent, &dir_a);
}

/* Close/flush of a written file. */
static void finalize(mc_fd* f) {
    if (!f->dirty || f->is_dir) return;
    f->dirty = 0;
    mark_entry(f, 1);
}

static int32_t mc_close(int fd) {
    mc_fd* f = get_fd(fd);
    if (!f) return MC_DENIED;
    f->open = 0;
    int r = card_check(0, 0);
    if (r) return r;
    finalize(f);
    return MC_OK;
}

static int32_t mc_flush(int fd) {
    mc_fd* f = get_fd(fd);
    if (!f) return MC_DENIED;
    int r = card_check(0, 0);
    if (r) return r;
    finalize(f);
    return MC_OK;
}

static int32_t mc_seek(int fd, int32_t offset, int origin) {
    mc_fd* f = get_fd(fd);
    if (!f) return MC_DENIED;
    int r = card_check(0, 0);
    if (r) return r;
    int64_t pos;
    if (origin == 0) pos = offset;
    else if (origin == 2) {
        mc_res* res = &res_b;
        resolve(f->path, res);
        pos = (int64_t)(res->status == 0 && !res->is_dir ? res->size : 0) + offset;
    } else pos = (int64_t)f->pos + offset;                  /* SEEK_CUR, and anything else */
    if (pos < 0) pos = 0;
    f->pos = (uint32_t)pos;
    return (int32_t)f->pos;
}

/* mcman McRead: up to n bytes at the file position. Like McWrite, it frees the handle when the read
 * itself fails (past the handle and card checks). */
static int32_t mc_read(int fd, uint8_t* dst, int32_t n) {
    mc_fd* f = get_fd(fd);
    if (!f || !f->rd) return MC_DENIED;
    int r = card_check(0, 0);
    if (r) return r;
    if (n <= 0) return 0;
    wchar_t p[HOST_MAX];
    if (!host_path(f->path, NULL, p)) { f->open = 0; return MC_DENIED; }
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { f->open = 0; return MC_NOENTRY; }
    LARGE_INTEGER li;
    li.QuadPart = f->pos;
    DWORD got = 0;
    if (SetFilePointerEx(h, li, NULL, FILE_BEGIN)) ReadFile(h, dst, (DWORD)n, &got, NULL);
    CloseHandle(h);
    f->pos += got;
    return (int32_t)got;
}

/* The host side of a write: all n bytes at the file position, or MC_FULL (nothing written) when
 * the card hasn't the clusters for them. mcman allocates clusters as it goes and gives up with -3
 * at the first one it can't get; what it wrote of that call lies past the entry's length, so it
 * never shows. */
static int32_t write_data(mc_fd* f, const uint8_t* src, int32_t n) {
    wchar_t p[HOST_MAX];
    if (!host_path(f->path, NULL, p)) return MC_DENIED;
    HANDLE h = open_existing_w(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return MC_NOENTRY;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) size.QuadPart = 0;
    uint64_t have = ((uint64_t)size.QuadPart + MC_CLUSTER - 1) / MC_CLUSTER;
    uint64_t end = (uint64_t)f->pos + (uint32_t)n;
    uint64_t want = (end > (uint64_t)size.QuadPart ? end : (uint64_t)size.QuadPart);
    want = (want + MC_CLUSTER - 1) / MC_CLUSTER;
    if (want > have + card_free()) { CloseHandle(h); return MC_FULL; }
    LARGE_INTEGER li;
    li.QuadPart = f->pos;
    DWORD w = 0;
    BOOL ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) && WriteFile(h, src, (DWORD)n, &w, NULL) && w == (DWORD)n;
    CloseHandle(h);
    if (!ok) { log_once("write", "cannot write %ls", p); return MC_DENIED; }
    f->pos += w;
    if ((uint64_t)f->pos > (uint64_t)size.QuadPart)          /* keep the cluster count current */
        card.used += (uint32_t)(((uint64_t)f->pos + MC_CLUSTER - 1) / MC_CLUSTER - have);
    return (int32_t)w;
}

/* mcman McWrite: n bytes at the file position. The first write after an open or flush marks the
 * entry "not closed"; a failed write frees the handle (so the entry is never closed). */
static int32_t mc_write(int fd, const uint8_t* src, int32_t n) {
    mc_fd* f = get_fd(fd);
    if (!f || !f->wr) return MC_DENIED;
    int r = card_check(0, 0);
    if (r) return r;
    if (n <= 0) return 0;
    if (!f->dirty) {
        f->dirty = 1;
        mark_entry(f, 0);
    }
    r = write_data(f, src, n);
    if (r < 0) f->open = 0;
    return r;
}

/* mcman's name pattern match: '?' matches one character (or the end of the name), '*' any run;
 * wildcards right after a '*' are skipped. */
static int match(const char* s, const char* pat) {
    for (;;) {
        size_t q = strcspn(pat, "?"), st = strcspn(pat, "*");
        if (!pat[q] && !pat[st]) return strcmp(s, pat) == 0;
        size_t first = q < st ? q : st;
        if (strncmp(pat, s, first) != 0) return 0;
        pat += first; s += first;
        while (*pat == '?') {
            if (!*s) return 1;
            pat++; s++;
        }
        if (*pat != '*') continue;
        while (*pat == '*' || *pat == '?') pat++;
        if (!*pat) return 1;
        for (;;) {
            const char* hit = strchr(s, *pat);
            if (!hit) return 0;
            s = hit;
            if (match(s, pat)) return 1;
            s++;
        }
    }
}

/* sceMcGetDir search state: one at a time, like mcman's. */
static struct {
    int    valid, index;
    char   pattern[MC_PATH_MAX];
    mc_dir list;                          /* snapshot of the directory, with "." and ".." for subdirectories */
} search;

static int32_t search_start(const char* name) {
    search.valid = 0;
    char dir[MC_PATH_MAX];
    const char* slash = strrchr(name, '/');
    if (!slash) { dir[0] = 0; strcpy(search.pattern, name); }
    else {
        size_t dl = slash == name ? 1 : (size_t)(slash - name);
        memcpy(dir, name, dl);
        dir[dl] = 0;
        strcpy(search.pattern, slash + 1);
    }
    mc_res* res = &res_b;
    resolve(dir, res);
    if (res->status != 0 || !res->is_dir) return MC_NOENTRY;
    if (!dir_load(res->path, &search.list)) return MC_NOENTRY;
    if (!res->is_root) {                                     /* subdirectories list "." and ".." first */
        mc_dir* l = &search.list;
        if (l->n > MC_MAX_ENT - 2) l->n = MC_MAX_ENT - 2;
        memmove(&l->e[2], &l->e[0], (size_t)l->n * sizeof l->e[0]);
        l->e[0] = l->self;
        strcpy(l->e[0].name, ".");
        char up[MC_PATH_MAX];
        strcpy(up, res->path);
        path_up(up);
        wchar_t p[HOST_MAX];
        if (host_path(up, NULL, p)) load_self(p, &l->e[1]);
        else l->e[1] = l->self;
        strcpy(l->e[1].name, "..");
        l->n += 2;
    }
    search.index = 0;
    search.valid = 1;
    return MC_OK;
}

static int search_next(mc_ent* out) {
    while (search.index < search.list.n) {
        const mc_ent* e = &search.list.e[search.index++];
        if (!(e->mode & M_EXISTS) || (e->mode & M_HIDDEN)) continue;
        if (!match(e->name, search.pattern)) continue;
        if (out) *out = *e;
        return 1;
    }
    return 0;
}

/* sceMcTblGetDir */
static void fill_tbl(uint8_t t[64], const mc_ent* e) {
    memset(t, 0, 64);
    memcpy(t + 0x00, e->ctime.b, 8);
    memcpy(t + 0x08, e->mtime.b, 8);
    put32(t + 0x10, e->is_dir ? 0 : e->size);
    put16(t + 0x14, e->mode);
    put32(t + 0x18, e->attr);
    memcpy(t + 0x20, e->name, strnlen(e->name, MC_NAME_MAX));
}

/* mcserv's GetDir: mcman McGetDir once per entry (first with the caller's flags, then 1 =
 * continue), each entry DMA'd to the EE table; maxent < 0 only counts the matches. */
static int32_t mc_getdir(int port, int slot, const char* name, uint32_t flags, int32_t maxent, uint32_t table) {
    int n = 0;
    flags &= 0xFFFF;
    if (maxent < 0) maxent = -1;
    do {
        if (maxent == 0) break;
        int r = card_check(port, slot);
        if (r) return r;
        if (!name[0]) return MC_NOENTRY;
        if (flags == 0) { r = search_start(name); if (r) return r; }
        else if (!search.valid) return MC_NOENTRY;
        if (maxent < 0) {                                    /* count only */
            while (search_next(NULL)) n++;
            return n;
        }
        mc_ent e;
        if (!search_next(&e)) break;
        uint8_t t[64];
        fill_tbl(t, &e);
        ee_write(table + 64u * (uint32_t)n, t, 64);
        n++;
        flags = 1;
    } while (--maxent > 0);
    return n;
}

static int32_t mc_getentspace(int port, int slot, const char* path) {
    int r = card_check(port, slot);
    if (r) return r;
    if (!path[0]) return MC_NOENTRY;
    mc_res* res = &res_a;
    resolve(path, res);
    if (res->status != 0) return MC_NOENTRY;
    if (!res->is_dir) return 0;
    if (!dir_load(res->path, &dir_a)) return MC_NOENTRY;
    return (2 + dir_a.n) & 1;                                /* free slot in the last cluster (+ deleted ones: none) */
}

static int32_t mc_chdir(int port, int slot, const char* path, char* curdir) {
    curdir[0] = 0;
    int r = card_check(port, slot);
    if (r) return r;
    if (!path[0]) return MC_NOENTRY;
    mc_res* res = &res_a;
    resolve(path, res);
    if (res->status != 0 || !res->is_dir) return MC_NOENTRY;
    strcpy(card.cwd, res->path);
    strcpy(curdir, card.cwd);
    return MC_OK;
}

static int remove_tree(const wchar_t* dir, int remove_self) {
    wchar_t p[HOST_MAX];
    if (wcslen(dir) + 3 >= HOST_MAX) return 0;
    wcscpy(p, dir); wcscat(p, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    int ok = 1;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (wcslen(dir) + 1 + wcslen(fd.cFileName) >= HOST_MAX) { ok = 0; continue; }
            wcscpy(p, dir); wcscat(p, L"\\"); wcscat(p, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= remove_tree(p, 1);
            else {
                SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
                ok &= DeleteFileW(p) != 0;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (remove_self) {
        host_writable(dir);
        ok &= RemoveDirectoryW(dir) != 0;
    }
    return ok;
}

static int32_t mc_delete(int port, int slot, const char* name, uint32_t flags) {
    int r = card_check(port, slot);
    if (r) return r;
    if (!name[0]) return MC_NOENTRY;
    if (last_is_dot(name)) return MC_DENIED;
    if (!valid_name(name)) return MC_NOENTRY;
    if (flags) { log_once("undelete", "sceMcDelete with flags %u (undelete) not supported", flags); return MC_NOENTRY; }
    mc_res* res = &res_a;
    resolve(name, res);
    if (res->status != 0 || res->is_root) return MC_NOENTRY;
    mc_ent e;
    if (!load_entry(res, &e)) return MC_NOENTRY;
    if (!(e.mode & M_WRITE)) return MC_DENIED;
    wchar_t p[HOST_MAX];
    if (!host_path(res->path, NULL, p)) return MC_DENIED;
    if (res->is_dir) {
        if (!dir_load(res->path, &dir_a)) return MC_DENIED;
        if (dir_a.n) return MC_NOTEMPTY;
        if (any_fd_on(res->path, -1, 0)) return MC_DENIED;
        if (!remove_tree(p, 1)) { log_once("rmdir", "cannot remove %ls", p); return MC_DENIED; }
    } else {
        if (any_fd_on(res->path, -1, 0)) return MC_DENIED;
        host_writable(p);
        if (!DeleteFileW(p)) { log_once("del", "cannot delete %ls", p); return MC_DENIED; }
    }
    if (dir_load(res->parent, &dir_a)) {
        dir_a.self.mtime = time_now();
        dir_save(res->parent, &dir_a);
    }
    card_changed();
    return MC_OK;
}

/* sceMcSetFileInfo; info is the caller's sceMcTblGetDir. */
static int32_t mc_setinfo(int port, int slot, const char* name, const uint8_t* info, uint32_t flags) {
    int r = card_check(port, slot);
    if (r) return r;
    if (!name[0]) return MC_NOENTRY;
    if (last_is_dot(name)) return MC_DENIED;
    mc_res* res = &res_a;
    resolve(name, res);
    if (res->status != 0) return MC_NOENTRY;
    if (res->is_root) return MC_DENIED;
    char newname[MC_NAME_MAX];
    if (flags & 0x10) {                                      /* rename: up to 31 characters */
        memcpy(newname, info + 0x20, MC_NAME_MAX - 1);
        newname[MC_NAME_MAX - 1] = 0;
        if (!newname[0] || !strcmp(newname, ".") || !strcmp(newname, "..") || strchr(newname, '/') ||
            !valid_name(newname))
            return MC_NOENTRY;
        int lk = lookup(res->parent, newname, NULL, NULL);
        if (lk == 1) return MC_NOENTRY;                      /* taken, by the entry itself too */
        if (lk == -1 && _stricmp(newname, res->name) != 0) return MC_DENIED;
    }
    mc_ent e;
    if (!load_entry(res, &e)) return MC_NOENTRY;
    if (flags & 0x08) e.attr = get32(info + 0x18);
    if ((flags & (0x04 | 0x200)) && e.is_dir)                /* attributes: directories only */
        e.mode = (uint16_t)((e.mode & ~ATTR_SET_MASK) | (get16(info + 0x14) & ATTR_SET_MASK));
    if (flags & 0x01) memcpy(e.ctime.b, info + 0x00, 8);
    if (flags & 0x02) memcpy(e.mtime.b, info + 0x08, 8);
    if (!store_entry(res, &e)) return MC_DENIED;
    if ((flags & 0x10) && strcmp(newname, res->name) != 0) {
        wchar_t from[HOST_MAX], to[HOST_MAX];
        char esc[ESC_MAX];
        wchar_t esc_w[ESC_MAX];
        escape_name(newname, esc);
        for (size_t k = 0; k <= strlen(esc); k++) esc_w[k] = (wchar_t)(unsigned char)esc[k];
        if (!host_path(res->path, NULL, from) || !host_path(res->parent, esc_w, to)) return MC_DENIED;
        if (!dir_load(res->parent, &dir_a)) return MC_DENIED;
        if (!MoveFileExW(from, to, 0)) { log_once("rename", "cannot rename %ls", from); return MC_DENIED; }
        int i = find_ent(&dir_a, res->name);
        if (i >= 0) strcpy(dir_a.e[i].name, newname);
        dir_save(res->parent, &dir_a);
        char newpath[MC_PATH_MAX];
        if (path_join(res->parent, newname, newpath))
            for (int k = 0; k < MC_MAX_FD; k++) if (fds[k].open && !strcmp(fds[k].path, res->path)) strcpy(fds[k].path, newpath);
    }
    return MC_OK;
}

static int32_t mc_format(int port, int slot, int formatted) {
    card_setup();
    if (port != 0 || slot != 0 || !card.ok) return MC_NOCARD;
    memset(fds, 0, sizeof fds);
    search.valid = 0;
    strcpy(card.cwd, "/");
    if (!remove_tree(card.root, 0)) log_once("wipe", "could not remove everything in %ls", card.root);
    wchar_t p[HOST_MAX];
    host_path("/", L"%unformatted", p);
    if (formatted) {
        dir_a.n = 0;
        default_self(card.root, &dir_a.self);
        dir_a.self.ctime = dir_a.self.mtime = time_now();
        dir_save("/", &dir_a);
    } else write_file_w(p, "", 0);
    card.formatted = formatted;
    card_changed();
    return MC_OK;
}

/* ---- the RPC server ------------------------------------------------------------------------------- */

static uint8_t rbuf[0x414];               /* mcserv's receive buffer: keeps bytes of earlier calls */
static uint32_t reply[3];                 /* mcserv's reply buffer: result, mcserv/mcman versions */

static void cstr(const uint8_t* p, char* out, size_t cap) {
    size_t n = strnlen((const char*)p, cap - 1);
    memcpy(out, p, n);
    out[n] = 0;
}

int mc_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
           uint8_t* out, uint32_t recv_size) {
    (void)ctx;
    if (sid != MC_SID) return 0;
    memcpy(rbuf, args, send_size < sizeof rbuf ? send_size : sizeof rbuf);
    const uint8_t* b = rbuf;
    /* descriptor calls: fd/port 0x00, port 0x04, slot 0x08, size 0x0C, offset/flag 0x10, origin/flag 0x14,
     * EE buffer 0x18, EE end-of-call struct 0x1C, write head bytes 0x20; name calls: port 0x00,
     * slot 0x04, flags 0x08, maxent 0x0C, EE pointer 0x10, name 0x14 (1024 bytes) */
    char name[0x401];
    cstr(b + 0x14, name, sizeof name);
    int32_t res = (int32_t)reply[0];
    int known = 1;

    switch (fno) {
    case FN_INIT:
        card_setup();
        reply[0] = 0;
        reply[1] = MCSERV_VERSION;
        reply[2] = MCMAN_VERSION;
        res = 0;
        break;

    case FN_GETINFO: {
        int port = (int)get32(b + 4), slot = (int)get32(b + 8);
        uint8_t st[0xC0];                                    /* type 0x00, free 0x04, formatted 0x90 */
        memset(st, 0, sizeof st);
        card_setup();
        int present = port == 0 && slot == 0 && card.ok;
        int32_t det = !present ? MC_NOCARD : card.seen ? MC_OK : card.formatted ? MC_NEWCARD : MC_NOFORMAT;
        if (present) card.seen = 1;
        if (get32(b + 0x14)) put32(st + 0x00, present ? 2 : 0);   /* sceMcTypePS2 */
        if (det >= MC_NEWCARD) {
            if (get32(b + 0x10)) put32(st + 0x04, card.formatted ? card_free() : 0);
            if (get32(b + 0x0C)) put32(st + 0x90, card.formatted ? 1 : 0);
        }
        ee_write(get32(b + 0x1C), st, sizeof st);
        res = det;
        break;
    }

    case FN_OPEN:
        res = mc_open((int)get32(b + 0), (int)get32(b + 4), name, get32(b + 8));
        break;
    case FN_CLOSE:
        res = mc_close((int)get32(b + 0));
        break;
    case FN_FLUSH:
        res = mc_flush((int)get32(b + 0));
        break;
    case FN_SEEK:
        res = mc_seek((int)get32(b + 0), (int32_t)get32(b + 0x10), (int)get32(b + 0x14));
        break;

    case FN_READ: {
        /* Data goes straight to the EE buffer, except the parts before the first and after the last
         * 64-byte boundary: those travel in the end-of-call struct {size1, size2, dest1, dest2,
         * data1[64], data2[64]} that libmc's callback copies into place. */
        static uint8_t chunk[IO_CHUNK];
        uint8_t st[0xC0];
        memset(st, 0, sizeof st);
        int fd = (int)get32(b + 0);
        int32_t size = (int32_t)get32(b + 0x0C);
        uint32_t dst = get32(b + 0x18);
        int32_t total = 0, err = 0, size1, size2, mid, r;
        if (size < 0) size = 0;
        size1 = size < 0x41 ? size : (int32_t)(((dst - 1) & ~63u) - (dst - 64));
        mid = size - size1;
        size2 = mid & 63;
        mid -= size2;
        put32(st + 0x00, (uint32_t)size1);
        put32(st + 0x04, (uint32_t)size2);
        if (size2) put32(st + 0x0C, dst + (uint32_t)size1 + (uint32_t)mid);
        if (size1) {
            r = mc_read(fd, st + 0x10, size1);
            if (r < 0) { put32(st + 0x00, 0); put32(st + 0x04, 0); err = r; goto read_done; }
            total = r;
            put32(st + 0x08, dst);
            dst += (uint32_t)r;
            if (r != size1) { put32(st + 0x00, (uint32_t)r); put32(st + 0x04, 0); goto read_done; }
        }
        while (mid) {
            int32_t want = mid < (int32_t)IO_CHUNK ? mid : (int32_t)IO_CHUNK;
            r = mc_read(fd, chunk, want);
            if (r < 0) { put32(st + 0x04, 0); err = r; goto read_done; }
            if (r != want) {                                  /* end of file inside the middle part */
                int32_t tail = r & 63;
                put32(st + 0x04, (uint32_t)tail);
                if (tail) {
                    put32(st + 0x0C, dst + (uint32_t)(r & ~63));
                    memcpy(st + 0x50, chunk + (r & ~63), (size_t)tail);
                }
                ee_write(dst, chunk, (uint32_t)(r - tail));
                total += r;
                goto read_done;
            }
            ee_write(dst, chunk, (uint32_t)r);
            total += r; mid -= r; dst += (uint32_t)r;
        }
        if (size2) {
            r = mc_read(fd, st + 0x50, size2);
            if (r < 0) { put32(st + 0x04, 0); err = r; goto read_done; }
            total += r;
            put32(st + 0x04, (uint32_t)r);
        }
    read_done:
        ee_write(get32(b + 0x1C), st, sizeof st);
        res = err ? err : total;
        break;
    }

    case FN_WRITE: {
        /* The first bytes up to a 16-byte boundary come in the descriptor (0x20); mcserv fetches the
         * rest from EE memory itself (sceSifGetOtherData), 8 KB at a time. */
        static uint8_t chunk[IO_CHUNK];
        int fd = (int)get32(b + 0);
        uint32_t head = get32(b + 0x14), src = get32(b + 0x18);
        int32_t rest = (int32_t)get32(b + 0x0C), total = 0, r;
        if (head) {
            if (head > sizeof rbuf - 0x20) head = sizeof rbuf - 0x20;
            r = mc_write(fd, b + 0x20, (int32_t)head);
            if (r != (int32_t)head) { res = r; break; }
            total = r;
        }
        res = total;
        while (rest) {
            int32_t n = rest < (int32_t)(IO_CHUNK + 1) ? rest : (int32_t)IO_CHUNK;
            if (n > 0) ee_read(src, chunk, (uint32_t)n);
            r = n > 0 ? mc_write(fd, chunk, n) : 0;
            if (r != n) { res = r >= 0 ? r + total : r; break; }
            total += n; rest -= n; src += (uint32_t)n;
            res = total;
        }
        break;
    }

    case FN_GETDIR:
        res = mc_getdir((int)get32(b + 0), (int)get32(b + 4), name, get32(b + 8), (int32_t)get32(b + 0x0C),
                        get32(b + 0x10));
        break;

    case FN_CHDIR: {
        static char cur[0x400];
        memset(cur, 0, sizeof cur);
        res = mc_chdir((int)get32(b + 0), (int)get32(b + 4), name, cur);
        if (get32(b + 0x10)) ee_write(get32(b + 0x10), cur, sizeof cur);
        break;
    }

    case FN_SETFILEINFO: {
        uint8_t info[64];
        ee_read(get32(b + 0x10), info, sizeof info);
        res = mc_setinfo((int)get32(b + 0), (int)get32(b + 4), name, info, get32(b + 8));
        break;
    }

    case FN_DELETE:
        res = mc_delete((int)get32(b + 0), (int)get32(b + 4), name, get32(b + 8));
        break;
    case FN_FORMAT:
        res = mc_format((int)get32(b + 4), (int)get32(b + 8), 1);
        break;
    case FN_UNFORMAT:
        res = mc_format((int)get32(b + 4), (int)get32(b + 8), 0);
        break;
    case FN_GETENTSPACE:
        res = mc_getentspace((int)get32(b + 0), (int)get32(b + 4), name);
        break;
    case FN_CHGPRITY: {                                      /* mcserv thread priority */
        uint32_t prio = get32(b + 0x14);
        res = prio - 9 < 0x73 ? 0 : -403;
        break;
    }
    case FN_SLOTMAX:                                         /* no multitap: one slot per port */
        res = 1;
        break;
    case FN_X33:
        log_once("fn33", "mcserv function 0x33 (port %u slot %u arg 0x%X) answered with 0", get32(b + 4),
                 get32(b + 8), get32(b + 0x10));
        res = 0;
        break;
    default:                                                 /* mcserv leaves its reply untouched */
        known = 0;
        log_once("unknown", "unknown mcserv function 0x%X: reply left unchanged", fno);
        break;
    }
    reply[0] = (uint32_t)res;

    if (ps2_log_sif && known) {
        if (fno == FN_OPEN || fno == FN_GETDIR || fno == FN_DELETE || fno == FN_GETENTSPACE || fno == FN_CHDIR ||
            fno == FN_SETFILEINFO)
            fprintf(stderr, "[mc] fn 0x%02X \"%s\" flags 0x%X -> %d\n", fno, name, get32(b + 8), res);
        else
            fprintf(stderr, "[mc] fn 0x%02X (0x%X 0x%X 0x%X 0x%X) -> %d\n", fno, get32(b + 0), get32(b + 4),
                    get32(b + 8), get32(b + 0x0C), res);
    }
    uint32_t n = recv_size < sizeof reply ? recv_size : (uint32_t)sizeof reply;
    memcpy(out, reply, n);
    return 1;
}
