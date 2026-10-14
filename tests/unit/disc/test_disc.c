/* Tests for runtime/disc.c (ISO 9660 boot executable lookup, SHA-256, validation) and the disc
 * choice in runtime/launch.c (the file dialog's loop with a scripted prompt, the stored path,
 * launch_load_game's sources).
 *
 * Synthetic disc images are written to build\unit\disc\work. With --real <image.iso> <SCUS_973.16>
 * the real disc image is checked too: its boot executable must be byte for byte that file.
 *
 * Usage: test_disc.exe [--real <iso> <elf>]
 */
#include "disc.h"
#include "launch.h"
#include "ps2_internal.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int checks, failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                 \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

/* ---- what launch.c needs from the rest of the runtime ---- */
#define FAKE_ELF_SIZE 5000u
const uint32_t ps2_elf_size = FAKE_ELF_SIZE;
const char ps2_elf_sha256[] = "c3948f6ee71c011ccfe45d29ff4adb19bcf3bef3ff1e9b95d473358667c1aaeb";
static wchar_t cdvd_image[1024];
void cdvd_set_image(const wchar_t* path) { wcsncpy(cdvd_image, path, 1023); }
void mc_set_default_dir(const wchar_t* dir) { (void)dir; }
void mc_quiesce(void) {}

static uint8_t fake_elf[FAKE_ELF_SIZE];
static void make_fake_elf(void) {
    for (uint32_t i = 0; i < FAKE_ELF_SIZE; i++) fake_elf[i] = (uint8_t)(i * 7 + 3);
    memcpy(fake_elf, "\x7f" "ELF", 4);
}

/* ---- synthetic ISO 9660 images ---- */
static wchar_t work[MAX_PATH];

typedef struct {
    const char* cnf;             /* SYSTEM.CNF text, NULL: no SYSTEM.CNF */
    const char* boot_name;       /* the boot file's directory entry name ("SCUS_973.16;1") */
    int in_subdir;               /* the boot file lies in \DATA */
    const uint8_t* elf;
    uint32_t elf_size;
    int truncate;                /* bytes cut off the end of the image */
} iso_spec;

static void put32le(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put32be(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int dirrec(uint8_t* p, uint32_t lbn, uint32_t size, int dir, const char* name, size_t nlen) {
    int len = 33 + (int)nlen + ((nlen & 1) ? 0 : 1);
    memset(p, 0, (size_t)len);
    p[0] = (uint8_t)len;
    put32le(p + 2, lbn); put32be(p + 6, lbn);
    put32le(p + 10, size); put32be(p + 14, size);
    p[25] = dir ? 2 : 0;
    p[28] = 1;
    p[32] = (uint8_t)nlen;
    memcpy(p + 33, name, nlen);
    return len;
}

/* sectors: 16 PVD, 17 terminator, 18 root, 19 \DATA, 20 SYSTEM.CNF, 21.. boot file */
static int write_iso(const wchar_t* path, const iso_spec* s) {
    uint32_t elf_sectors = (s->elf_size + DISC_SECTOR - 1) / DISC_SECTOR;
    uint32_t total = 21 + elf_sectors;
    size_t bytes = (size_t)total * DISC_SECTOR;
    uint8_t* img = calloc(1, bytes);
    uint8_t* pvd = img + 16 * DISC_SECTOR;
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    memcpy(pvd + 40, "SLY2_TEST", 9);
    dirrec(pvd + 156, 18, DISC_SECTOR, 1, "\0", 1);
    uint8_t* term = img + 17 * DISC_SECTOR;
    term[0] = 0xFF; memcpy(term + 1, "CD001", 5); term[6] = 1;
    uint8_t* root = img + 18 * DISC_SECTOR;
    int o = dirrec(root, 18, DISC_SECTOR, 1, "\0", 1);
    o += dirrec(root + o, 18, DISC_SECTOR, 1, "\1", 1);
    o += dirrec(root + o, 19, DISC_SECTOR, 1, "DATA", 4);
    if (s->cnf) {
        o += dirrec(root + o, 20, (uint32_t)strlen(s->cnf), 0, "SYSTEM.CNF;1", 12);
        memcpy(img + 20 * DISC_SECTOR, s->cnf, strlen(s->cnf));
    }
    uint8_t* sub = img + 19 * DISC_SECTOR;
    int so = dirrec(sub, 19, DISC_SECTOR, 1, "\0", 1);
    so += dirrec(sub + so, 18, DISC_SECTOR, 1, "\1", 1);
    if (s->boot_name) {
        uint8_t* d = s->in_subdir ? sub + so : root + o;
        dirrec(d, 21, s->elf_size, 0, s->boot_name, strlen(s->boot_name));
        memcpy(img + 21 * DISC_SECTOR, s->elf, s->elf_size);
    }
    FILE* f = _wfopen(path, L"wb");
    if (!f) { free(img); return 0; }
    size_t n = bytes - (size_t)s->truncate;
    int ok = fwrite(img, 1, n, f) == n;
    fclose(f);
    free(img);
    return ok;
}

static const wchar_t* iso_path(const wchar_t* name) {
    static wchar_t p[8][MAX_PATH + 64];
    static int k;
    wchar_t* r = p[k++ & 7];
    swprintf(r, MAX_PATH + 64, L"%ls\\%ls", work, name);
    return r;
}

#define CNF_OK "BOOT2 = cdrom0:\\SCUS_973.16;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n"

static void test_sha256(void) {
    char hex[65];
    disc_sha256_hex("", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty: %s", hex);
    disc_sha256_hex("abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc: %s", hex);
    const char* m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* 56 bytes: two padding blocks */
    disc_sha256_hex(m56, strlen(m56), hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "56 bytes: %s", hex);
    static char a[1000000];
    memset(a, 'a', sizeof a);
    disc_sha256_hex(a, sizeof a, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "a x 10^6: %s", hex);
    disc_sha256_hex(fake_elf, FAKE_ELF_SIZE, hex);
    CHECK(!strcmp(hex, ps2_elf_sha256), "the fake executable's hash (as recorded above): %s", hex);
}

static int check(const wchar_t* path, uint8_t** elf, size_t* n) { return launch_check_disc(path, elf, n); }

static void test_images(void) {
    iso_spec good = { CNF_OK, "SCUS_973.16;1", 0, fake_elf, FAKE_ELF_SIZE, 0 };
    const wchar_t* p = iso_path(L"good.iso");
    CHECK(write_iso(p, &good), "write");
    disc_boot b;
    CHECK(disc_find_boot(p, &b) == DISC_OK && !strcmp(b.boot_name, "SCUS_973.16") && b.lbn == 21 && b.size == FAKE_ELF_SIZE,
          "boot file found: '%s' lbn %u size %u", b.boot_name, b.lbn, b.size);
    uint8_t* elf = NULL;
    size_t n = 0;
    int e = check(p, &elf, &n);
    CHECK(e == DISC_OK && elf && n == FAKE_ELF_SIZE && !memcmp(elf, fake_elf, n), "good image: %s", disc_why(e));
    free(elf);
    CHECK(check(p, NULL, NULL) == DISC_OK, "check without copying");

    iso_spec s = good;                                   /* lower case, no version, in a subdirectory */
    s.cnf = "boot2=cdrom0:\\data\\scus_973.16\n";
    s.boot_name = "SCUS_973.16";
    s.in_subdir = 1;
    p = iso_path(L"subdir.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_OK, "boot file in \\DATA: %s", disc_why(e));

    static uint8_t other[FAKE_ELF_SIZE];
    memcpy(other, fake_elf, sizeof other);
    other[1234] ^= 1;
    s = good; s.elf = other;
    p = iso_path(L"patched.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_WRONG_VERSION, "one byte different: %s", disc_why(e));

    s = good; s.elf_size = FAKE_ELF_SIZE - 1;
    p = iso_path(L"shorter.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_WRONG_VERSION, "different size: %s", disc_why(e));

    s = good; s.cnf = "BOOT2 = cdrom0:\\SLUS_202.67;1\r\n"; s.boot_name = "SLUS_202.67;1";
    p = iso_path(L"other_game.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_WRONG_GAME, "another game: %s", disc_why(e));

    s = good; s.cnf = NULL;
    p = iso_path(L"no_cnf.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_NO_CNF, "no SYSTEM.CNF: %s", disc_why(e));

    s = good; s.cnf = "BOOT = cdrom:\\PSX.EXE;1\r\n";
    p = iso_path(L"ps1.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_NO_BOOT, "PS1 style SYSTEM.CNF: %s", disc_why(e));

    s = good; s.boot_name = "SCUS_973.17;1";
    p = iso_path(L"missing_boot.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_NO_BOOT, "boot file missing: %s", disc_why(e));

    s = good; s.truncate = 2000;                         /* the boot file's last 856 bytes are missing */
    p = iso_path(L"truncated.iso");
    write_iso(p, &s);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_READ, "truncated image: %s", disc_why(e));

    p = iso_path(L"text.iso");
    FILE* f = _wfopen(p, L"wb");
    for (int i = 0; i < 20000; i++) fputc('x', f);
    fclose(f);
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_NOT_ISO, "not an image: %s", disc_why(e));
    p = iso_path(L"empty.iso");
    fclose(_wfopen(p, L"wb"));
    CHECK((e = check(p, NULL, NULL)) == DISC_ERR_NOT_ISO, "empty file: %s", disc_why(e));
    CHECK((e = check(iso_path(L"does_not_exist.iso"), NULL, NULL)) == DISC_ERR_OPEN, "no file: %s", disc_why(e));

    p = iso_path(L"Sl\u00FD 2 \u2013 \u65E5\u672C.iso");     /* a name outside the ANSI code page */
    CHECK(write_iso(p, &good), "write unicode name");
    CHECK((e = check(p, NULL, NULL)) == DISC_OK, "unicode path: %s", disc_why(e));
}

/* ---- the dialog loop with a scripted prompt ---- */
static const wchar_t* prompt_script[4];
static int prompt_calls, retry_calls, retry_answer;
static int fake_prompt(wchar_t* path, size_t cap) {
    const wchar_t* next = prompt_script[prompt_calls++];
    if (!next) return 0;                                  /* cancelled */
    wcsncpy(path, next, cap - 1);
    path[cap - 1] = 0;
    return 1;
}
static int fake_retry(const wchar_t* path, const char* why) {
    (void)path;
    retry_calls++;
    printf("  retry asked: %s\n", why);
    return retry_answer;
}

static int ini_has(const char* needle) {
    wchar_t ini[MAX_PATH + 64];
    settings_user_path(L"sly2.ini", ini, MAX_PATH + 64);
    FILE* f = _wfopen(ini, L"rb");
    if (!f) return 0;
    static char text[65536];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    return strstr(text, needle) != NULL;
}

static void test_choose(void) {
    wchar_t ini[MAX_PATH + 64];
    settings_user_path(L"sly2.ini", ini, MAX_PATH + 64);
    DeleteFileW(ini);
    settings_load();
    wchar_t path[1024] = L"";
    uint8_t* elf = NULL;
    size_t n = 0;

    prompt_script[0] = iso_path(L"other_game.iso");      /* a wrong disc, then the right one */
    prompt_script[1] = iso_path(L"Sl\u00FD 2 \u2013 \u65E5\u672C.iso");
    prompt_script[2] = NULL;
    prompt_calls = retry_calls = 0;
    retry_answer = 1;
    int r = launch_choose_disc(fake_prompt, fake_retry, path, 1024, &elf, &n);
    CHECK(r == 1 && prompt_calls == 2 && retry_calls == 1, "chosen after one retry (%d, %d prompts, %d retries)", r,
          prompt_calls, retry_calls);
    CHECK(elf && n == FAKE_ELF_SIZE && !memcmp(elf, fake_elf, n), "the executable comes from the chosen image");
    free(elf);
    elf = NULL;
    CHECK(ini_has("[game]") && ini_has("\\Sl\xC3\xBD 2 \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC.iso"),
          "the choice is stored in sly2.ini (UTF-8)");
    settings_load();
    wchar_t back[1024];
    MultiByteToWideChar(CP_UTF8, 0, ps2_settings.disc, -1, back, 1024);
    CHECK(!wcscmp(back, prompt_script[1]), "and loads back: '%s'", ps2_settings.disc);

    prompt_script[0] = NULL;                              /* cancelled at once */
    prompt_calls = retry_calls = 0;
    CHECK(launch_choose_disc(fake_prompt, fake_retry, path, 1024, &elf, &n) == 0 && retry_calls == 0, "cancel");
    prompt_script[0] = iso_path(L"text.iso");             /* a bad file, then "Cancel" in the question */
    prompt_script[1] = iso_path(L"good.iso");
    prompt_calls = retry_calls = 0;
    retry_answer = 0;
    CHECK(launch_choose_disc(fake_prompt, fake_retry, path, 1024, &elf, &n) == 0 && prompt_calls == 1, "give up");
}

/* launch_load_game's sources, the successful paths (failures exit the process) */
static void test_load_game(const wchar_t* exe_elf) {
    size_t n = 0;
    _wputenv(L"PS2_ISO=");
    /* [game] disc in sly2.ini (as stored by test_choose) */
    settings_load();
    uint8_t* elf = launch_load_game(1, NULL, 1, &n);
    CHECK(elf && n == FAKE_ELF_SIZE && !memcmp(elf, fake_elf, n), "from sly2.ini's disc");
    CHECK(wcsstr(cdvd_image, L"\u65E5\u672C.iso") != NULL, "cdvd gets that image: %ls", cdvd_image);
    free(elf);
    /* PS2_ISO wins over sly2.ini */
    wchar_t env[MAX_PATH + 80];
    swprintf(env, MAX_PATH + 80, L"PS2_ISO=%ls", iso_path(L"subdir.iso"));
    _wputenv(env);
    elf = launch_load_game(1, NULL, 1, &n);
    CHECK(elf && n == FAKE_ELF_SIZE && wcsstr(cdvd_image, L"subdir.iso"), "PS2_ISO first: %ls", cdvd_image);
    free(elf);
    /* argv[1] a disc image wins over both */
    char arg[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, iso_path(L"good.iso"), -1, arg, sizeof arg, NULL, NULL);
    char* argv[] = { "sly2.exe", arg, NULL };
    elf = launch_load_game(2, argv, 1, &n);
    CHECK(elf && n == FAKE_ELF_SIZE && wcsstr(cdvd_image, L"good.iso"), "argv[1] image first: %ls", cdvd_image);
    free(elf);
    /* argv[1] an ELF: that file, unchecked; the disc from PS2_ISO */
    FILE* f = _wfopen(exe_elf, L"wb");
    fwrite("\x7f" "ELF-anything", 1, 12, f);
    fclose(f);
    WideCharToMultiByte(CP_UTF8, 0, exe_elf, -1, arg, sizeof arg, NULL, NULL);
    elf = launch_load_game(2, argv, 1, &n);
    CHECK(elf && n == 12 && !memcmp(elf, "\x7f" "ELF-any", 8) && wcsstr(cdvd_image, L"subdir.iso"),
          "argv[1] ELF, disc from PS2_ISO: %ls", cdvd_image);
    free(elf);
    _wputenv(L"PS2_ISO=");
}

/* The real disc image: its boot executable is exactly the extracted file. */
static void test_real(const char* iso, const char* elf_path) {
    FILE* f = fopen(elf_path, "rb");
    if (!f) { printf("real disc: SKIPPED (no %s)\n", elf_path); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* want = malloc((size_t)size);
    size_t got_n = fread(want, 1, (size_t)size, f);
    fclose(f);
    char hex[65];
    disc_sha256_hex(want, got_n, hex);
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, iso, -1, w, 1024);
    uint8_t* elf = NULL;
    size_t n = 0;
    int e = disc_check(w, LAUNCH_BOOT_ELF, (uint32_t)size, hex, &elf, &n);
    CHECK(e == DISC_OK && n == (size_t)size && !memcmp(elf, want, n), "real disc image: %s", disc_why(e));
    printf("real disc: %s holds %s (%ld bytes, sha256 %s): %s\n", iso, LAUNCH_BOOT_ELF, size, hex, disc_why(e));
    free(elf);
    free(want);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    DWORD dn = GetModuleFileNameW(NULL, work, MAX_PATH - 40);
    wchar_t* slash = dn ? wcsrchr(work, L'\\') : NULL;
    if (slash) *slash = 0; else wcscpy(work, L".");
    wchar_t env[MAX_PATH + 64];
    swprintf(env, MAX_PATH + 64, L"PS2_USER_DIR=%ls\\userdata", work);   /* never the player's folder */
    _wputenv(env);
    wcscat(work, L"\\work");
    CreateDirectoryW(work, NULL);
    make_fake_elf();

    test_sha256();
    test_images();
    test_choose();
    test_load_game(iso_path(L"plain.elf"));
    if (argc == 4 && !strcmp(argv[1], "--real")) test_real(argv[2], argv[3]);

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
