/* Unit tests for runtime/mc.c (memory card service).
 *
 * Each helper builds the RPC arguments exactly as the game's libmc does (descriptor / name
 * descriptor layouts, write head bytes, end-of-call structs) and plays libmc's end callbacks
 * (read fix-up, GetInfo results), against a fake 32 MB EE RAM. The card lives in a host
 * directory next to the test executable.
 *
 *   test_mc.exe          fresh card: every RPC, host mapping, errors; leaves a save behind
 *   test_mc.exe persist  a new process (= a restart) checks that save is still there
 */
#include "hw.h"

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

/* ---- fakes for the neighbours mc.c links against ------------------------------------------- */

uint8_t* ps2_ram;
uint8_t* ps2_spr;
int ps2_log_sif;
void ps2_hw_read(ps2_ctx* ctx, uint32_t addr, void* out, unsigned size) { (void)ctx; (void)addr; memset(out, 0, size); }
void ps2_hw_write(ps2_ctx* ctx, uint32_t addr, const void* in, unsigned size) { (void)ctx; (void)addr; (void)in; (void)size; }

/* ---- test plumbing ------------------------------------------------------------------------- */

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    CHECK(_a == _b, "%s = %lld, expected %lld", #a, _a, _b); } while (0)

static uint8_t* ee(uint32_t a) { return ps2_ram + (a & (PS2_RAM_SIZE - 1)); }
static uint32_t ee32(uint32_t a) { uint32_t v; memcpy(&v, ee(a), 4); return v; }
static void ee_put32(uint32_t a, uint32_t v) { memcpy(ee(a), &v, 4); }

/* libmc's buffers (addresses as in the game) */
#define EE_DESC  0x00450900u          /* 48-byte descriptor */
#define EE_NAME  0x00450930u          /* 0x414-byte name descriptor */
#define EE_END   0x00450D80u          /* 0xC0-byte end-of-call struct */
#define EE_TABLE 0x00300000u          /* sceMcTblGetDir array */
#define EE_INFO  0x00310000u
#define EE_CUR   0x00320000u

static uint8_t iop_buf[0x4000];       /* the server's receive buffer in IOP RAM */
static uint32_t rdata[3];             /* libmc's receive buffer */

/* sceSifCallRpc: the send data lands in the server's buffer, the reply in rdata */
static int32_t rpc(uint32_t fno, uint32_t send_addr, uint32_t ssize, uint32_t rsize) {
    memcpy(iop_buf, ee(send_addr), ssize);
    uint8_t out[16] = { 0 };
    int handled = mc_rpc(NULL, 0x80000400u, fno, iop_buf, ssize, out, rsize);
    CHECK(handled, "mc_rpc didn't handle fn 0x%X", fno);
    memcpy(rdata, out, rsize < 12 ? rsize : 12);
    return (int32_t)rdata[0];
}

static void desc_clear(void) { memset(ee(EE_DESC), 0, 0x30); }
static void name_desc(int port, int slot, uint32_t flags, const char* name) {
    uint8_t* d = ee(EE_NAME);
    ee_put32(EE_NAME + 0, (uint32_t)port);
    ee_put32(EE_NAME + 4, (uint32_t)slot);
    ee_put32(EE_NAME + 8, flags);
    strncpy((char*)d + 0x14, name, 0x3FF);
    d[0x413] = 0;
}

static int32_t mc_init(void) { return rpc(0xFE, EE_DESC, 0x30, 12); }

static int32_t mc_getinfo(int port, int slot, int* type, int* freec, int* format) {
    desc_clear();
    ee_put32(EE_DESC + 4, (uint32_t)port);
    ee_put32(EE_DESC + 8, (uint32_t)slot);
    ee_put32(EE_DESC + 0x0C, format != NULL);
    ee_put32(EE_DESC + 0x10, freec != NULL);
    ee_put32(EE_DESC + 0x14, type != NULL);
    ee_put32(EE_DESC + 0x1C, EE_END);
    memset(ee(EE_END), 0xCD, 0xC0);
    int32_t r = rpc(0x01, EE_DESC, 0x30, 4);
    if (type) *type = (int)ee32(EE_END + 0);               /* libmc's callback (0x244518) */
    if (freec) *freec = (int)ee32(EE_END + 4);
    if (format) *format = (int)ee32(EE_END + 0x90);
    return r;
}

static int32_t mc_open(int port, int slot, const char* name, uint32_t mode) {
    name_desc(port, slot, mode, name);
    return rpc(0x02, EE_NAME, 0x414, 4);
}
static int32_t mc_mkdir(int port, int slot, const char* name) { return mc_open(port, slot, name, 0x40); }
static int32_t mc_close(int fd) { ee_put32(EE_DESC, (uint32_t)fd); return rpc(0x03, EE_DESC, 0x30, 4); }
static int32_t mc_flush(int fd) { ee_put32(EE_DESC, (uint32_t)fd); return rpc(0x0A, EE_DESC, 0x30, 4); }
static int32_t mc_seek(int fd, int32_t off, int origin) {
    ee_put32(EE_DESC, (uint32_t)fd);
    ee_put32(EE_DESC + 0x10, (uint32_t)off);
    ee_put32(EE_DESC + 0x14, (uint32_t)origin);
    return rpc(0x04, EE_DESC, 0x30, 4);
}

static uint32_t last_size1, last_size2, last_dest1, last_dest2;
static int32_t mc_read(int fd, uint32_t buf, int32_t size) {
    ee_put32(EE_DESC + 0, (uint32_t)fd);
    ee_put32(EE_DESC + 0x18, buf);
    ee_put32(EE_DESC + 0x0C, (uint32_t)size);
    ee_put32(EE_DESC + 0x1C, EE_END);
    memset(ee(EE_END), 0xCD, 0xC0);
    int32_t r = rpc(0x05, EE_DESC, 0x30, 4);
    /* libmc's callback (0x244110): copy the unaligned head and tail into place */
    last_size1 = ee32(EE_END + 0); last_size2 = ee32(EE_END + 4);
    last_dest1 = ee32(EE_END + 8); last_dest2 = ee32(EE_END + 12);
    if (last_size1) memcpy(ee(last_dest1), ee(EE_END + 0x10), last_size1);
    if (last_size2) memcpy(ee(last_dest2), ee(EE_END + 0x50), last_size2);
    return r;
}

static int32_t mc_write(int fd, uint32_t buf, int32_t size) {     /* libmc 0x2442D8 */
    ee_put32(EE_DESC + 0, (uint32_t)fd);
    if (size < 17) {
        ee_put32(EE_DESC + 0x14, (uint32_t)size);
        ee_put32(EE_DESC + 0x18, 0);
        ee_put32(EE_DESC + 0x0C, 0);
    } else {
        uint32_t head = ((buf - 1) & 0xFFFFFFF0u) - (buf - 16);
        ee_put32(EE_DESC + 0x18, buf + head);
        ee_put32(EE_DESC + 0x0C, (uint32_t)size - head);
        ee_put32(EE_DESC + 0x14, head);
    }
    uint32_t head = ee32(EE_DESC + 0x14);
    for (uint32_t i = 0; i < head; i++) ee(EE_DESC + 0x20)[i] = ee(buf)[i];
    return rpc(0x06, EE_DESC, 0x30, 4);
}

static int32_t mc_getdir(int port, int slot, const char* name, int flags, int maxent, uint32_t table) {
    name_desc(port, slot, (uint32_t)flags, name);
    ee_put32(EE_NAME + 0x0C, (uint32_t)maxent);
    ee_put32(EE_NAME + 0x10, table);
    return rpc(0x0D, EE_NAME, 0x414, 4);
}
static int32_t mc_delete(int port, int slot, const char* name) { name_desc(port, slot, 0, name); return rpc(0x0F, EE_NAME, 0x414, 4); }
static int32_t mc_getentspace(int port, int slot, const char* name) { name_desc(port, slot, 0, name); return rpc(0x12, EE_NAME, 0x414, 4); }
static int32_t mc_chdir(int port, int slot, const char* name, uint32_t cur) {
    name_desc(port, slot, 0, name);
    ee_put32(EE_NAME + 0x10, cur);
    return rpc(0x0C, EE_NAME, 0x414, 4);
}
static int32_t mc_setinfo(int port, int slot, const char* name, uint32_t info, uint32_t flags) {
    name_desc(port, slot, flags, name);
    ee_put32(EE_NAME + 0x10, info);
    return rpc(0x0E, EE_NAME, 0x414, 4);
}
static int32_t mc_format(int port, int slot, int fno) {
    desc_clear();
    ee_put32(EE_DESC + 4, (uint32_t)port);
    ee_put32(EE_DESC + 8, (uint32_t)slot);
    return rpc((uint32_t)fno, EE_DESC, 0x30, 4);
}

/* sceMcTblGetDir accessors */
typedef struct { char name[33]; uint32_t size, attr; uint16_t mode; uint8_t ctime[8], mtime[8]; } tbl;
static tbl tbl_at(int i) {
    tbl t;
    const uint8_t* p = ee(EE_TABLE + 64u * (uint32_t)i);
    memcpy(t.ctime, p, 8); memcpy(t.mtime, p + 8, 8);
    memcpy(&t.size, p + 0x10, 4); memcpy(&t.mode, p + 0x14, 2); memcpy(&t.attr, p + 0x18, 4);
    memcpy(t.name, p + 0x20, 32); t.name[32] = 0;
    return t;
}
static int list(const char* pattern, tbl* out, int max) {
    memset(ee(EE_TABLE), 0xEE, 64u * (uint32_t)max);
    int n = mc_getdir(0, 0, pattern, 0, max, EE_TABLE);
    for (int i = 0; i < n && i < max; i++) out[i] = tbl_at(i);
    return n;
}
static int find(const tbl* t, int n, const char* name) {
    for (int i = 0; i < n; i++) if (!strcmp(t[i].name, name)) return i;
    return -1;
}

/* ---- host helpers ------------------------------------------------------------------------------- */

static wchar_t card_dir[1024];

static void host(const wchar_t* rel, wchar_t* out) { swprintf(out, 1024, L"%ls\\%ls", card_dir, rel); }
static int host_exists(const wchar_t* rel) { wchar_t p[1024]; host(rel, p); return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static long long host_size(const wchar_t* rel) {
    wchar_t p[1024]; host(rel, p);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fa)) return -1;
    return ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}
static size_t host_read(const wchar_t* rel, void* buf, size_t cap) {
    wchar_t p[1024]; host(rel, p);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(h, buf, (DWORD)cap, &got, NULL);
    CloseHandle(h);
    return got;
}
static void wipe(const wchar_t* dir) {
    wchar_t p[1024];
    swprintf(p, 1024, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        swprintf(p, 1024, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { wipe(p); RemoveDirectoryW(p); }
        else DeleteFileW(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static uint8_t pattern_byte(uint32_t i) { return (uint8_t)(i * 7 + (i >> 8) * 13 + 1); }

#define SAVE_DIR "/BASCUS-97316YAOTWTD!"

/* ---- tests ---------------------------------------------------------------------------------------- */

static void test_init_and_info(void) {
    CHECK_EQ(mc_init(), 0);
    CHECK_EQ(rdata[1], 0x210);                                /* mcserv version (game needs >= 0x20A) */
    CHECK_EQ(rdata[2], 0x226);                                /* mcman version (game needs >= 0x20E) */

    int type = -1, freec = -1, format = -1;
    CHECK_EQ(mc_getinfo(0, 0, &type, &freec, &format), -1);  /* first look: a new formatted card */
    CHECK_EQ(type, 2);
    CHECK_EQ(freec, 8000);
    CHECK_EQ(format, 1);
    CHECK_EQ(mc_getinfo(0, 0, &type, &freec, &format), 0);   /* same card since */
    CHECK_EQ(mc_getinfo(1, 0, &type, &freec, &format), -10); /* port 1: no card */
    CHECK_EQ(type, 0);
    CHECK_EQ(freec, 0);
    CHECK_EQ(format, 0);
    CHECK_EQ(mc_getinfo(0, 1, NULL, NULL, NULL), -10);       /* no multitap */
    CHECK(host_exists(L"%meta"), "root sidecar missing");
}

static void test_format(void) {
    CHECK_EQ(mc_mkdir(0, 0, "/junk"), 0);
    CHECK_EQ(mc_format(0, 0, 0x11), 0);                      /* unformat */
    int type, freec, format;
    CHECK_EQ(mc_getinfo(0, 0, &type, &freec, &format), 0);
    CHECK_EQ(format, 0);
    CHECK_EQ(freec, 0);
    CHECK_EQ(mc_open(0, 0, "/x", 0x202), -2);
    CHECK_EQ(mc_getdir(0, 0, "/*", 0, 4, EE_TABLE), -2);
    CHECK(!host_exists(L"junk"), "unformat kept files");
    CHECK_EQ(mc_format(0, 0, 0x10), 0);                      /* format */
    CHECK_EQ(mc_getinfo(0, 0, &type, &freec, &format), 0);
    CHECK_EQ(format, 1);
    CHECK_EQ(freec, 8000);
    CHECK_EQ(mc_getdir(0, 0, "/*", 0, 4, EE_TABLE), 0);
    CHECK_EQ(mc_format(1, 0, 0x10), -10);
}

static void test_mkdir_and_space(void) {
    CHECK_EQ(mc_getentspace(0, 0, "/"), 0);                  /* ".", "..": cluster full */
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR, 0, -1, 0), 0);        /* the game's "does my save exist" probe */
    CHECK_EQ(mc_mkdir(0, 0, SAVE_DIR), 0);
    CHECK_EQ(mc_mkdir(0, 0, SAVE_DIR), -4);                  /* exists */
    CHECK_EQ(mc_mkdir(0, 0, SAVE_DIR "/sub"), -5);           /* directories only in the root */
    CHECK_EQ(mc_mkdir(0, 0, "/nope/sub"), -4);
    CHECK_EQ(mc_mkdir(1, 0, "/x"), -10);
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR, 0, -1, 0), 1);
    CHECK_EQ(mc_getentspace(0, 0, "/"), 1);                  /* 3 entries: one free slot */
    CHECK_EQ(mc_getentspace(0, 0, SAVE_DIR), 0);
    CHECK_EQ(mc_getentspace(0, 0, "/nope"), -4);
    int freec;
    CHECK_EQ(mc_getinfo(0, 0, NULL, &freec, NULL), 0);
    CHECK_EQ(freec, 8001 - 2 - 1);                           /* root: 2 clusters, save dir: 1 */
    CHECK(host_exists(L"BASCUS-97316YAOTWTD!\\%meta"), "save dir sidecar missing");
}

static void test_write_read(void) {
    const uint32_t src = 0x00100003, n = 100000;
    for (uint32_t i = 0; i < n; i++) ee(src)[i] = pattern_byte(i);
    int fd = mc_open(0, 0, SAVE_DIR "/save0", 0x202);
    CHECK_EQ(fd, 0);
    CHECK_EQ(mc_write(fd, src, (int32_t)n), (int32_t)n);
    CHECK_EQ(mc_write(fd, src, 5), 5);                       /* small write: all in the descriptor */
    CHECK_EQ(mc_close(fd), 0);
    CHECK_EQ(mc_close(fd), -5);                              /* already closed */
    CHECK_EQ(host_size(L"BASCUS-97316YAOTWTD!\\save0"), n + 5);
    static uint8_t buf[200000];
    size_t got = host_read(L"BASCUS-97316YAOTWTD!\\save0", buf, sizeof buf);
    CHECK_EQ(got, n + 5);
    int same = 1;
    for (uint32_t i = 0; i < n; i++) if (buf[i] != pattern_byte(i)) { same = 0; break; }
    for (uint32_t i = 0; i < 5; i++) if (buf[n + i] != pattern_byte(i)) same = 0;
    CHECK(same, "host file content differs from what was written");

    int freec;
    mc_getinfo(0, 0, NULL, &freec, NULL);
    CHECK_EQ(freec, 8001 - 2 - 2 - 98);                      /* + save0 (98 clusters), save dir now 3 entries */

    /* read back: aligned middle by DMA, head and tail via the end-of-call struct */
    fd = mc_open(0, 0, SAVE_DIR "/save0", 1);
    CHECK_EQ(fd, 0);
    const uint32_t dst = 0x00200005, rn = 70001;
    memset(ee(dst - 16), 0xAA, rn + 64);
    CHECK_EQ(mc_read(fd, dst, (int32_t)rn), (int32_t)rn);
    CHECK_EQ(last_size1, 59);
    CHECK_EQ(last_dest1, dst);
    CHECK_EQ(last_size2, 54);
    CHECK_EQ(last_dest2, dst + 59 + 69888);
    same = 1;
    for (uint32_t i = 0; i < rn; i++) if (ee(dst)[i] != pattern_byte(i)) { same = 0; break; }
    CHECK(same, "read data differs");
    CHECK(ee(dst - 1)[0] == 0xAA && ee(dst + rn)[0] == 0xAA, "read wrote outside the buffer");

    /* small read (<= 64 bytes): everything in data1 */
    CHECK_EQ(mc_read(fd, 0x00280000, 10), 10);
    CHECK_EQ(last_size1, 10);
    CHECK_EQ(last_size2, 0);
    same = 1;
    for (uint32_t i = 0; i < 10; i++) if (ee(0x00280000)[i] != pattern_byte(rn + i)) same = 0;
    CHECK(same, "small read data differs");

    /* short read at the end of the file, inside the middle part */
    CHECK_EQ(mc_seek(fd, 99000, 0), 99000);
    memset(ee(0x00290000), 0, 6000);
    CHECK_EQ(mc_read(fd, 0x00290000, 5000), 1005);
    CHECK_EQ(last_size1, 0);
    CHECK_EQ(last_size2, 1005 & 63);
    CHECK_EQ(last_dest2, 0x00290000 + (1005 & ~63));
    same = 1;
    for (uint32_t i = 0; i < 1000; i++) if (ee(0x00290000)[i] != pattern_byte(99000 + i)) same = 0;
    for (uint32_t i = 0; i < 5; i++) if (ee(0x00290000)[1000 + i] != pattern_byte(i)) same = 0;
    CHECK(same, "short read data differs");
    CHECK_EQ(mc_read(fd, 0x00290000, 100), 0);               /* at EOF */

    /* short read inside the head part */
    CHECK_EQ(mc_seek(fd, -3, 2), (int32_t)n + 2);
    CHECK_EQ(mc_read(fd, 0x002A0001, 200), 3);
    CHECK_EQ(last_size1, 3);
    CHECK_EQ(last_size2, 0);
    CHECK_EQ(mc_seek(fd, 10, 1), (int32_t)n + 15);
    CHECK_EQ(mc_seek(fd, -1000000, 1), 0);                   /* clamped at 0 */
    CHECK_EQ(mc_write(fd, src, 4), -5);                      /* opened read-only */
    CHECK_EQ(mc_flush(fd), 0);
    CHECK_EQ(mc_close(fd), 0);
    CHECK_EQ(mc_read(fd, dst, 4), -5);
    CHECK_EQ(mc_seek(7, 0, 0), -5);

    /* O_CREAT on an existing file starts it afresh */
    fd = mc_open(0, 0, SAVE_DIR "/save0", 0x202);
    CHECK_EQ(mc_write(fd, src, 10), 10);
    CHECK_EQ(mc_close(fd), 0);
    CHECK_EQ(host_size(L"BASCUS-97316YAOTWTD!\\save0"), 10);
    /* write-only file can't be read */
    fd = mc_open(0, 0, SAVE_DIR "/save0", 2);
    CHECK_EQ(mc_read(fd, dst, 4), -5);
    CHECK_EQ(mc_open(0, 0, SAVE_DIR "/save0", 2), -5);       /* second writer */
    int rfd = mc_open(0, 0, SAVE_DIR "/save0", 1);           /* reader alongside: fine */
    CHECK(rfd >= 0, "reader alongside a writer refused (%d)", rfd);
    CHECK_EQ(mc_open(0, 0, SAVE_DIR "/save0", 0x202), -5);   /* replacing an open file */
    mc_close(rfd);
    mc_close(fd);
}

static void test_open_errors(void) {
    CHECK_EQ(mc_open(0, 0, SAVE_DIR "/missing", 1), -4);
    CHECK_EQ(mc_open(0, 0, "/missing/file", 0x202), -4);
    CHECK_EQ(mc_open(0, 0, SAVE_DIR "/sa*e", 0x202), -4);     /* wildcards */
    CHECK_EQ(mc_open(0, 0, "", 1), -4);
    CHECK_EQ(mc_open(1, 0, SAVE_DIR "/save0", 1), -10);
    CHECK_EQ(mc_open(0, 0, "/bascus-97316yaotwtd!/save0", 1), -4);   /* names are case-sensitive */
    CHECK_EQ(mc_open(0, 0, SAVE_DIR "/SAVE0", 0x202), -5);   /* host can't hold save0 and SAVE0 */
    int a = mc_open(0, 0, SAVE_DIR "/a", 0x202), b = mc_open(0, 0, SAVE_DIR "/b", 0x202);
    int c = mc_open(0, 0, SAVE_DIR "/c", 0x202);
    CHECK(a == 0 && b == 1 && c == 2, "fds %d %d %d", a, b, c);
    CHECK_EQ(mc_open(0, 0, SAVE_DIR "/d", 0x202), -7);       /* three handles */
    mc_close(a); mc_close(b); mc_close(c);
    int d = mc_open(0, 0, SAVE_DIR, 1);                       /* a directory opens as a handle */
    CHECK(d >= 0, "opening a directory: %d", d);
    CHECK_EQ(mc_read(d, 0x00280000, 4), -5);
    mc_close(d);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/a"), 0);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/b"), 0);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/c"), 0);
}

static void test_getdir(void) {
    static tbl t[32];
    /* root lists no "." / ".." */
    int n = list("/*", t, 18);
    CHECK_EQ(n, 1);
    CHECK(n >= 1 && !strcmp(t[0].name, "BASCUS-97316YAOTWTD!"), "root entry %s", t[0].name);
    CHECK_EQ(t[0].mode, 0x8427);
    CHECK_EQ(t[0].size, 0);

    /* a subdirectory lists ".", ".." and its files */
    n = list(SAVE_DIR "/*", t, 18);
    CHECK_EQ(n, 3);
    CHECK(!strcmp(t[0].name, ".") && !strcmp(t[1].name, "..") && !strcmp(t[2].name, "save0"), "order %s %s %s",
          t[0].name, t[1].name, t[2].name);
    CHECK_EQ(t[0].mode, 0x8427);
    CHECK_EQ(t[1].mode, 0x8427);
    CHECK_EQ(t[2].mode, 0x8497);                              /* closed after writing */
    CHECK_EQ(t[2].size, 10);
    uint16_t year; memcpy(&year, t[2].mtime + 6, 2);
    CHECK(year >= 2024 && t[2].mtime[5] >= 1 && t[2].mtime[5] <= 12, "bad mtime year %u month %u", year, t[2].mtime[5]);

    /* pattern quirks: '?' also matches the end of a name; "*" then literal */
    const char* names[] = { "save1", "save10", "save", "sav", "icon.sys" };
    for (int i = 0; i < 5; i++) {
        char p[64];
        snprintf(p, sizeof p, SAVE_DIR "/%s", names[i]);
        int fd = mc_open(0, 0, p, 0x202);
        CHECK(fd >= 0, "create %s: %d", names[i], fd);
        mc_close(fd);
    }
    n = list(SAVE_DIR "/save?", t, 18);
    CHECK_EQ(n, 3);                                           /* save0, save1, save */
    CHECK(find(t, n, "save0") >= 0 && find(t, n, "save1") >= 0 && find(t, n, "save") >= 0, "save? matches");
    n = list(SAVE_DIR "/s*0", t, 18);
    CHECK_EQ(n, 2);                                           /* save0, save10 */
    n = list(SAVE_DIR "/*.sys", t, 18);
    CHECK_EQ(n, 1);
    n = list(SAVE_DIR "/icon.sys", t, 18);
    CHECK_EQ(n, 1);
    n = list(SAVE_DIR "/nothing", t, 18);
    CHECK_EQ(n, 0);
    CHECK_EQ(mc_getdir(0, 0, "/nope/*", 0, 18, EE_TABLE), -4);
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR "/save0/*", 0, 18, EE_TABLE), -4);   /* not a directory */
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR "/*", 0, -1, 0), 2 + 6);

    /* continuation: mode 1 picks up where the last call stopped */
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR "/*", 0, 3, EE_TABLE), 3);
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR "/*", 1, 18, EE_TABLE), 5);
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR "/*", 1, 18, EE_TABLE), 0);
    CHECK_EQ(mc_getdir(0, 0, SAVE_DIR "/*", 0, 0, EE_TABLE), 0);
    CHECK_EQ(mc_getdir(0, 0, "", 0, 4, EE_TABLE), -4);

    CHECK_EQ(mc_getentspace(0, 0, SAVE_DIR), 0);              /* 8 entries */
    for (int i = 0; i < 5; i++) {
        char p[64];
        snprintf(p, sizeof p, SAVE_DIR "/%s", names[i]);
        CHECK_EQ(mc_delete(0, 0, p), 0);
    }
    CHECK_EQ(mc_getentspace(0, 0, SAVE_DIR), 1);              /* 3 entries */
}

static void test_names(void) {
    /* bytes Windows can't keep in a file name are escaped on the host */
    int fd = mc_open(0, 0, SAVE_DIR "/a:b%c.", 0x202);
    CHECK(fd >= 0, "create a:b%%c.: %d", fd);
    mc_close(fd);
    CHECK(host_exists(L"BASCUS-97316YAOTWTD!\\a%3Ab%25c%2E"), "escaped host name missing");
    fd = mc_open(0, 0, SAVE_DIR "/CON", 0x202);
    CHECK(fd >= 0, "create CON: %d", fd);
    mc_close(fd);
    CHECK(host_exists(L"BASCUS-97316YAOTWTD!\\%43ON"), "device name not escaped");
    static tbl t[32];
    int n = list(SAVE_DIR "/*", t, 18);
    CHECK(find(t, n, "a:b%c.") >= 0 && find(t, n, "CON") >= 0, "escaped names not listed back");
    /* 33+ character names are cut to 32 */
    fd = mc_open(0, 0, SAVE_DIR "/0123456789abcdef0123456789ABCDEFxyz", 0x202);
    CHECK(fd >= 0, "long name: %d", fd);
    mc_close(fd);
    n = list(SAVE_DIR "/0123*", t, 18);
    CHECK_EQ(n, 1);
    CHECK(n == 1 && !strcmp(t[0].name, "0123456789abcdef0123456789ABCDEF"), "long name listed as %s", t[0].name);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/0123456789abcdef0123456789ABCDEF"), 0);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/a:b%c."), 0);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/CON"), 0);
    /* host files that aren't card names stay invisible */
    wchar_t p[1024];
    host(L"BASCUS-97316YAOTWTD!\\not a card name", p);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CloseHandle(h);
    n = list(SAVE_DIR "/*", t, 18);
    CHECK_EQ(n, 3);
    DeleteFileW(p);
}

static void test_delete(void) {
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR), -6);                  /* not empty */
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/missing"), -4);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/."), -5);
    CHECK_EQ(mc_delete(0, 0, "/"), -4);
    CHECK_EQ(mc_delete(0, 0, ""), -4);
    CHECK_EQ(mc_mkdir(0, 0, "/empty"), 0);
    CHECK_EQ(mc_delete(0, 0, "/empty"), 0);
    CHECK(!host_exists(L"empty"), "deleted directory still on the host");
    int fd = mc_open(0, 0, SAVE_DIR "/tmp", 0x202);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/tmp"), -5);           /* open */
    mc_close(fd);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/tmp"), 0);
}

static void test_subdir_limit(void) {
    /* a subdirectory holds 20 entries: ".", ".." and 18 files */
    int made = 0, r = 0;
    for (int i = 0; i < 25; i++) {
        char p[64];
        snprintf(p, sizeof p, SAVE_DIR "/f%02d", i);
        r = mc_open(0, 0, p, 0x202);
        if (r < 0) break;
        mc_close(r);
        made++;
    }
    CHECK_EQ(made, 17);                                       /* save0 is already there */
    CHECK_EQ(r, -5);
    for (int i = 0; i < made; i++) {
        char p[64];
        snprintf(p, sizeof p, SAVE_DIR "/f%02d", i);
        mc_delete(0, 0, p);
    }
}

static void test_setinfo(void) {
    static tbl t[8];
    uint8_t* info = ee(EE_INFO);
    memset(info, 0, 64);
    uint8_t ct[8] = { 0, 1, 2, 3, 4, 5, 0xD0, 0x07 }, mt[8] = { 0, 6, 7, 8, 9, 10, 0xD1, 0x07 };
    memcpy(info, ct, 8);
    memcpy(info + 8, mt, 8);
    uint16_t mode = 0x8427 | 0x0008;                           /* copy-protect the save directory */
    memcpy(info + 0x14, &mode, 2);
    uint32_t attr = 0x12345678;
    memcpy(info + 0x18, &attr, 4);
    CHECK_EQ(mc_setinfo(0, 0, SAVE_DIR, EE_INFO, 0x01 | 0x02 | 0x04 | 0x08), 0);
    int n = list(SAVE_DIR, t, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(t[0].mode, 0x842F);
    CHECK_EQ(t[0].attr, 0x12345678);
    CHECK(!memcmp(t[0].ctime, ct, 8) && !memcmp(t[0].mtime, mt, 8), "times not set");

    /* files: attribute changes are ignored; times and the attribute word apply */
    mode = 0x8497 & ~0x0002;
    memcpy(info + 0x14, &mode, 2);
    CHECK_EQ(mc_setinfo(0, 0, SAVE_DIR "/save0", EE_INFO, 0x02 | 0x04), 0);
    n = list(SAVE_DIR "/save0", t, 8);
    CHECK_EQ(t[0].mode, 0x8497);
    CHECK(!memcmp(t[0].mtime, mt, 8), "file mtime not set");

    /* rename */
    strcpy((char*)info + 0x20, "renamed");
    CHECK_EQ(mc_setinfo(0, 0, SAVE_DIR "/save0", EE_INFO, 0x10), 0);
    CHECK(host_exists(L"BASCUS-97316YAOTWTD!\\renamed") && !host_exists(L"BASCUS-97316YAOTWTD!\\save0"), "rename on host");
    n = list(SAVE_DIR "/renamed", t, 8);
    CHECK_EQ(n, 1);
    CHECK(!memcmp(t[0].mtime, mt, 8), "rename lost the metadata");
    int fd = mc_open(0, 0, SAVE_DIR "/other", 0x202);
    mc_close(fd);
    strcpy((char*)info + 0x20, "other");
    CHECK_EQ(mc_setinfo(0, 0, SAVE_DIR "/renamed", EE_INFO, 0x10), -4);   /* name taken */
    strcpy((char*)info + 0x20, "..");
    CHECK_EQ(mc_setinfo(0, 0, SAVE_DIR "/renamed", EE_INFO, 0x10), -4);
    strcpy((char*)info + 0x20, "save0");
    CHECK_EQ(mc_setinfo(0, 0, SAVE_DIR "/renamed", EE_INFO, 0x10), 0);
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/other"), 0);
    CHECK_EQ(mc_setinfo(0, 0, SAVE_DIR "/missing", EE_INFO, 0x02), -4);
    CHECK_EQ(mc_setinfo(0, 0, "/", EE_INFO, 0x02), -5);
}

static void test_chdir(void) {
    memset(ee(EE_CUR), 0x55, 0x400);
    CHECK_EQ(mc_chdir(0, 0, SAVE_DIR, EE_CUR), 0);
    CHECK(!strcmp((char*)ee(EE_CUR), SAVE_DIR), "curdir %s", (char*)ee(EE_CUR));
    int fd = mc_open(0, 0, "save0", 1);                       /* relative to the new directory */
    CHECK(fd >= 0, "relative open: %d", fd);
    mc_close(fd);
    static tbl t[8];
    CHECK_EQ(list("save*", t, 8), 1);
    CHECK_EQ(mc_chdir(0, 0, "..", EE_CUR), 0);
    CHECK(!strcmp((char*)ee(EE_CUR), "/"), "curdir after .. %s", (char*)ee(EE_CUR));
    CHECK_EQ(mc_chdir(0, 0, "/missing", EE_CUR), -4);
    CHECK_EQ(mc_chdir(0, 0, SAVE_DIR "/save0", EE_CUR), -4);
}

static void test_full_card(void) {
    int freec;
    mc_getinfo(0, 0, NULL, &freec, NULL);
    const uint32_t chunk = 0x00400000;                        /* 4 MB of EE RAM as the source */
    memset(ee(chunk), 0x5A, 4u << 20);
    int fd = mc_open(0, 0, SAVE_DIR "/big", 0x202);
    CHECK(fd >= 0, "create big: %d", fd);
    int32_t total = 0, r;
    while ((r = mc_write(fd, chunk, 4 << 20)) > 0) total += r;
    CHECK_EQ(r, -3);
    mc_close(fd);
    int after;
    mc_getinfo(0, 0, NULL, &after, NULL);
    CHECK_EQ(after, 0);
    CHECK(total >= (freec - 1) * 1024 && total <= freec * 1024, "wrote %d with %d clusters free", total, freec);
    CHECK_EQ(mc_open(0, 0, SAVE_DIR "/more", 0x202), -3);    /* no cluster for a new entry */
    CHECK_EQ(mc_delete(0, 0, SAVE_DIR "/big"), 0);
    mc_getinfo(0, 0, NULL, &after, NULL);
    CHECK_EQ(after, freec);
}

static void test_misc_rpcs(void) {
    desc_clear();
    ee_put32(EE_DESC + 0x14, 50);
    CHECK_EQ(rpc(0x14, EE_DESC, 0x30, 4), 0);                 /* change mcserv thread priority */
    ee_put32(EE_DESC + 0x14, 5);
    CHECK_EQ(rpc(0x14, EE_DESC, 0x30, 4), -403);
    CHECK_EQ(rpc(0x13, EE_DESC, 0x30, 4), -403);              /* unknown: reply left as it was */
    uint8_t out[4];
    CHECK_EQ(mc_rpc(NULL, 0x80000401u, 1, iop_buf, 0x30, out, 4), 0);   /* other servers aren't ours */
}

/* Saves copied onto the host (no sidecars) show up with default metadata. */
static void test_host_import(void) {
    wchar_t p[1024];
    DWORD w;
    host(L"IMPORTED", p);
    CreateDirectoryW(p, NULL);
    host(L"IMPORTED\\data", p);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(h, "hello world", 11, &w, NULL);
    CloseHandle(h);
    host(L"IMPORTED\\Thumbs.db", p);                          /* hidden/system: not a card file */
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, NULL);
    CloseHandle(h);
    host(L"IMPORTED\\%E9t%E9", p);                            /* card name "\xE9t\xE9" */
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CloseHandle(h);

    static tbl t[16];
    int n = list("/*", t, 16);
    CHECK_EQ(n, 2);
    int i = find(t, n, "IMPORTED");
    CHECK(i >= 0 && t[i].mode == 0x8427, "imported directory: index %d", i);
    n = list("/IMPORTED/*", t, 16);
    CHECK_EQ(n, 4);                                           /* . .. data \xE9t\xE9 */
    i = find(t, n, "data");
    CHECK(i >= 0 && t[i].mode == 0x8497 && t[i].size == 11, "imported file: index %d", i);
    CHECK(find(t, n, "\xE9t\xE9") >= 0, "non-ASCII name not listed");
    int fd = mc_open(0, 0, "/IMPORTED/data", 1);
    CHECK_EQ(mc_read(fd, 0x00280000, 64), 11);
    CHECK(!memcmp(ee(0x00280000), "hello world", 11), "imported data differs");
    mc_close(fd);
    fd = mc_open(0, 0, "/IMPORTED/\xE9t\xE9", 1);
    CHECK(fd >= 0, "open non-ASCII name: %d", fd);
    mc_close(fd);
    CHECK_EQ(mc_delete(0, 0, "/IMPORTED"), -6);
    CHECK_EQ(mc_delete(0, 0, "/IMPORTED/data"), 0);
    CHECK_EQ(mc_delete(0, 0, "/IMPORTED/\xE9t\xE9"), 0);
    CHECK_EQ(mc_delete(0, 0, "/IMPORTED"), 0);                /* only host junk left: empty */
    CHECK(!host_exists(L"IMPORTED"), "imported directory still on the host");
}

/* Leave a save for the second process: the same layout the game writes. */
static void make_persistent_save(void) {
    const char* files[] = { SAVE_DIR "/icon.sys", SAVE_DIR "/save1" };
    for (int i = 0; i < 2; i++) {
        for (uint32_t k = 0; k < 3000; k++) ee(0x00100000)[k] = pattern_byte(k + (uint32_t)i);
        int fd = mc_open(0, 0, files[i], 0x202);
        CHECK(fd >= 0, "create %s: %d", files[i], fd);
        CHECK_EQ(mc_write(fd, 0x00100000, 3000), 3000);
        mc_close(fd);
    }
}

static void test_persist(void) {
    int type, freec, format;
    CHECK_EQ(mc_init(), 0);
    CHECK_EQ(mc_getinfo(0, 0, &type, &freec, &format), -1);  /* a new process sees a "new" card */
    CHECK_EQ(format, 1);
    /* root: 3 entries (2), save dir: . .. save0 icon.sys save1 (3), save0 10 B (1), 2 x 3000 B (3 + 3) */
    CHECK_EQ(freec, 8001 - 2 - 3 - 1 - 3 - 3);
    static tbl t[16];
    int n = list("/*", t, 16);
    CHECK_EQ(n, 1);
    CHECK_EQ(t[0].mode, 0x842F);                               /* set by test_setinfo */
    CHECK_EQ(t[0].attr, 0x12345678);
    uint8_t ct[8] = { 0, 1, 2, 3, 4, 5, 0xD0, 0x07 };
    CHECK(!memcmp(t[0].ctime, ct, 8), "directory ctime lost across restart");
    n = list(SAVE_DIR "/*", t, 16);
    CHECK_EQ(n, 5);
    CHECK(n == 5 && !strcmp(t[2].name, "save0") && !strcmp(t[3].name, "icon.sys") && !strcmp(t[4].name, "save1"),
          "entry order not kept");
    uint8_t mt[8] = { 0, 6, 7, 8, 9, 10, 0xD1, 0x07 };
    CHECK(!memcmp(t[2].mtime, mt, 8), "file mtime lost across restart");
    int fd = mc_open(0, 0, SAVE_DIR "/save1", 1);
    CHECK_EQ(mc_read(fd, 0x00200010, 3000), 3000);
    int same = 1;
    for (uint32_t k = 0; k < 3000; k++) if (ee(0x00200010)[k] != pattern_byte(k + 1)) same = 0;
    CHECK(same, "save1 content differs after restart");
    mc_close(fd);
}

int main(int argc, char** argv) {
    ps2_ram = calloc(1, PS2_RAM_SIZE + 16);
    ps2_spr = calloc(1, PS2_SPR_SIZE + 16);
    GetModuleFileNameW(NULL, card_dir, 1000);
    wchar_t* s = wcsrchr(card_dir, L'\\');
    if (s) *s = 0;
    wcscat(card_dir, L"\\card");
    wchar_t env[1100];
    swprintf(env, 1100, L"PS2_MEMCARD_DIR=%ls", card_dir);
    _wputenv(env);

    int persist = argc > 1 && !strcmp(argv[1], "persist");
    if (!persist) {
        CreateDirectoryW(card_dir, NULL);
        wipe(card_dir);
        test_init_and_info();
        test_format();
        test_mkdir_and_space();
        test_write_read();
        test_open_errors();
        test_getdir();
        test_names();
        test_delete();
        test_subdir_limit();
        test_setinfo();
        test_chdir();
        test_full_card();
        test_misc_rpcs();
        test_host_import();
        make_persistent_save();
    } else test_persist();

    printf("%s: %d checks, %d failed\n", persist ? "mc persist" : "mc", checks, failures);
    return failures ? 1 : 0;
}
