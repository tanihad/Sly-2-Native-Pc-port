/* Unit tests: Ed25519 (RFC 8032 test vectors), release manifests, URL policy, the game folder swap.
 *   test_release.exe <temp folder> [python-signed cases file]
 * Built and run by tests/unit/launcher/run.ps1. */
#define UNICODE
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "ed25519.h"
#include "release.h"

static int fails, passes;
#define CHECK(c, what) do { if (c) passes++; else { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static void hex(const char* h, uint8_t* out, size_t n) { if (!ed25519_from_hex(h, out, n)) { printf("bad hex\n"); exit(2); } }

/* RFC 8032 section 7.1: TEST 1, 2, 3 and TEST SHA(abc) */
static const struct { const char *pub, *msg, *sig; } VEC[] = {
    { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    { "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
      "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
};

static void test_ed25519(void) {
    uint8_t h[64];
    CHECK(ed25519_sha512("abc", 3, NULL, 0, NULL, 0, h), "sha512 runs");
    uint8_t want[64];
    hex(VEC[3].msg, want, 64);
    CHECK(!memcmp(h, want, 64), "SHA-512(abc) (FIPS 180-2 vector)");
    for (int i = 0; i < 4; i++) {
        uint8_t pub[32], sig[64], msg[64];
        size_t n = strlen(VEC[i].msg) / 2;
        hex(VEC[i].pub, pub, 32);
        hex(VEC[i].sig, sig, 64);
        hex(VEC[i].msg, msg, n);
        char what[64];
        snprintf(what, sizeof what, "RFC 8032 vector %d verifies", i + 1);
        CHECK(ed25519_verify(sig, msg, n, pub), what);
        uint8_t bad[64];
        memcpy(bad, sig, 64);
        bad[5] ^= 1;
        CHECK(!ed25519_verify(bad, msg, n, pub), "a changed R is rejected");
        memcpy(bad, sig, 64);
        bad[40] ^= 0x10;
        CHECK(!ed25519_verify(bad, msg, n, pub), "a changed S is rejected");
        uint8_t m2[65];
        memcpy(m2, msg, n);
        m2[n] = 'x';
        CHECK(!ed25519_verify(sig, m2, n + 1, pub), "a changed message is rejected");
        uint8_t pub2[32];
        hex(VEC[(i + 1) % 4].pub, pub2, 32);
        CHECK(!ed25519_verify(sig, msg, n, pub2), "another key is rejected");
    }
    /* S + L (same signature mathematically, non-canonical): must be rejected */
    {
        static const uint8_t L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
        uint8_t pub[32], sig[64];
        hex(VEC[0].pub, pub, 32);
        hex(VEC[0].sig, sig, 64);
        int c = 0;
        for (int i = 0; i < 32; i++) { int v = sig[32 + i] + L[i] + c; sig[32 + i] = (uint8_t)v; c = v >> 8; }
        CHECK(!ed25519_verify(sig, "", 0, pub), "S >= L is rejected (malleability)");
    }
    {   /* an invalid public key (y = p, non-canonical) */
        uint8_t pub[32], sig[64];
        memset(pub, 0xff, 32);
        pub[0] = 0xed;
        pub[31] = 0x7f;
        hex(VEC[0].sig, sig, 64);
        CHECK(!ed25519_verify(sig, "", 0, pub), "a non-canonical public key is rejected");
    }
}

/* lines "pubhex msghex sighex ok(0/1)" written by run.ps1 with scripts/sly2_ed25519.py */
static void test_python_cases(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { CHECK(0, "python cases file"); return; }
    static char pubh[128], msgh[4096], sigh[256];
    int ok, n = 0;
    while (fscanf(f, "%127s %4095s %255s %d", pubh, msgh, sigh, &ok) == 4) {
        uint8_t pub[32], sig[64], msg[2048];
        size_t mn = strcmp(msgh, "-") ? strlen(msgh) / 2 : 0;
        hex(pubh, pub, 32);
        hex(sigh, sig, 64);
        if (mn) hex(msgh, msg, mn);
        CHECK(ed25519_verify(sig, msg, mn, pub) == ok, "python-signed case");
        n++;
    }
    fclose(f);
    printf("  %d python-signed cases\n", n);
}

static const char* GOOD =
    "# Sly 2 PC port release\n"
    "format = 1\n"
    "product = sly2-pc\n"
    "version = 0.1.1300\n"
    "commit = abc1234\n"
    "date = 2026-11-03\n"
    "bundle = https://github.com/tanihad/sly2-decomp/releases/download/v0.1.1300/sly2-source-0.1.1300.zip\n"
    "bundle_size = 812345\n"
    "bundle_sha256 = 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n"
    "min_launcher = 0.1.1250\n"
    "page = https://github.com/tanihad/sly2-decomp/releases/tag/v0.1.1300\n"
    "future_key = ignored\n";

static int parses(const char* text) {
    release_manifest m;
    char why[256];
    return release_parse(text, strlen(text), &m, why, sizeof why);
}

static char* replace(const char* text, const char* a, const char* b) {
    static char out[4096];
    const char* p = strstr(text, a);
    if (!p) return strcpy(out, text);
    size_t k = (size_t)(p - text);
    memcpy(out, text, k);
    strcpy(out + k, b);
    strcat(out, p + strlen(a));
    return out;
}

static void test_manifest(void) {
    release_manifest m;
    char why[256];
    CHECK(release_parse(GOOD, strlen(GOOD), &m, why, sizeof why), "a good manifest parses");
    CHECK(!strcmp(m.version, "0.1.1300") && m.bundle_size == 812345 && m.bundle_sha256[1] == 0x11 && m.bundle_sha256[31] == 0xff &&
              !strcmp(m.min_launcher, "0.1.1250") && strstr(m.bundle, "sly2-source-0.1.1300.zip"),
          "fields read");
    /* CRLF + BOM */
    char crlf[4096] = "\xEF\xBB\xBF";
    for (const char* p = GOOD; *p; p++) { if (*p == '\n') strcat(crlf, "\r"); strncat(crlf, p, 1); }
    CHECK(parses(crlf), "CRLF and a BOM are fine");
    CHECK(!parses(replace(GOOD, "format = 1", "format = 2")), "an unknown format is refused");
    CHECK(!parses(replace(GOOD, "product = sly2-pc", "product = other")), "another product is refused");
    CHECK(!parses(replace(GOOD, "product = sly2-pc\n", "")), "no product is refused");
    CHECK(!parses(replace(GOOD, "version = 0.1.1300", "version = 0.1.x")), "a malformed version is refused");
    CHECK(!parses(replace(GOOD, "version = 0.1.1300\n", "")), "no version is refused");
    CHECK(!parses(replace(GOOD, "bundle_size = 812345", "bundle_size = -5")), "a negative size is refused");
    CHECK(!parses(replace(GOOD, "bundle_size = 812345", "bundle_size = 999999999999")), "a huge bundle is refused");
    CHECK(!parses(replace(GOOD, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", "0011")), "a short SHA-256 is refused");
    CHECK(!parses(replace(GOOD, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
                          "zz112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")), "a non-hex SHA-256 is refused");
    CHECK(!parses(replace(GOOD, "commit = abc1234\n", "commit = abc1234\nversion = 9.9\n")), "a repeated key is refused");
    CHECK(!parses(replace(GOOD, "commit = abc1234\n", "commit abc1234\n")), "a line without = is refused");
    CHECK(!parses(replace(GOOD, "bundle = https", "bundle_x = https")), "no bundle is refused");
    CHECK(parses(replace(GOOD, "min_launcher = 0.1.1250\n", "")), "min_launcher is optional");

    CHECK(release_version_cmp("0.1.1300", "0.1.1299") > 0, "1300 > 1299");
    CHECK(release_version_cmp("0.1.999", "0.1.1000") < 0, "numeric, not text order");
    CHECK(release_version_cmp("0.2", "0.1.5000") > 0, "0.2 > 0.1.5000");
    CHECK(release_version_cmp("1.0", "1.0") == 0, "equal");
    CHECK(release_version_cmp("garbage", "0.0.1") < 0, "malformed sorts lowest");
    CHECK(release_version_cmp("0.1.10", "0.1.9") > 0, "10 > 9");

    CHECK(release_url_allowed(L"https://github.com/x", 0), "https allowed");
    CHECK(!release_url_allowed(L"http://github.com/x", 0), "http refused");
    CHECK(!release_url_allowed(L"http://github.com/x", 1), "http to a remote host refused even in test builds");
    CHECK(!release_url_allowed(L"http://127.0.0.1:8080/x", 0), "loopback http refused in release builds");
    CHECK(release_url_allowed(L"http://127.0.0.1:8080/x", 1), "loopback http allowed in test builds");
    CHECK(!release_url_allowed(L"http://127.0.0.1.evil.com/x", 1), "a host that only starts like loopback is refused");
    CHECK(!release_url_allowed(L"file:///C:/x", 1), "file URLs refused");
    CHECK(!release_url_allowed(L"ftp://x", 1), "other schemes refused");

    wchar_t u[1100];
    release_url_resolve(L"https://h.example/a/b/sly2-release.txt", "sly2-source-1.zip", u, 1100);
    CHECK(!wcscmp(u, L"https://h.example/a/b/sly2-source-1.zip"), "relative bundle resolved against the manifest");
    release_url_resolve(L"https://h.example/a/b/sly2-release.txt", "https://other.example/z.zip", u, 1100);
    CHECK(!wcscmp(u, L"https://other.example/z.zip"), "absolute bundle kept");
    release_url_resolve(L"http://127.0.0.1:9/m.txt?x=1", "b.zip", u, 1100);
    CHECK(!wcscmp(u, L"http://127.0.0.1:9/b.zip"), "query dropped, host root");
}

/* ---- staging ------------------------------------------------------------------------------------ */

static wchar_t T[1024];

static void mk(const wchar_t* rel, const char* content) {
    wchar_t p[1024];
    _snwprintf(p, 1024, L"%ls\\%ls", T, rel);
    wchar_t d[1024];
    wcscpy(d, p);
    wchar_t* s = wcsrchr(d, L'\\');
    *s = 0;
    CreateDirectoryW(d, NULL);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD w;
    WriteFile(h, content, (DWORD)strlen(content), &w, NULL);
    CloseHandle(h);
}

static int has(const wchar_t* rel) {
    wchar_t p[1024];
    _snwprintf(p, 1024, L"%ls\\%ls", T, rel);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static void ver(const wchar_t* sub, char* out) { stage_version(T, sub, out, 64); }

static void stage_new(const char* v) {
    wchar_t st[1024];
    CHECK(stage_begin(T, st, 1024), "stage_begin");
    mk(L"game.new\\sly2.exe", v);
    CHECK(stage_mark_complete(T, v), "stage_mark_complete");
}

static void test_stage(void) {
    char v[64];
    unsigned long err;
    stage_delete_tree(T);
    CreateDirectoryW(T, NULL);
    /* first install */
    stage_new("1.0");
    CHECK(stage_commit(T, &err), "first commit");
    ver(L"game", v);
    CHECK(!strcmp(v, "1.0") && !has(L"game.new") && !has(L"game.old") && !has(L"game\\.complete"), "game = 1.0, no leftovers");
    /* update */
    stage_new("2.0");
    CHECK(stage_commit(T, &err), "update commit");
    ver(L"game", v);
    CHECK(!strcmp(v, "2.0"), "game = 2.0");
    ver(L"game.old", v);
    CHECK(!strcmp(v, "1.0"), "game.old = 1.0 (kept for rollback)");
    /* an incomplete staging folder is never committed */
    wchar_t st[1024];
    CHECK(stage_begin(T, st, 1024), "begin again");
    mk(L"game.new\\sly2.exe", "3.0-partial");
    CHECK(!stage_commit(T, &err) && err == ERROR_FILE_NOT_FOUND, "an unfinished build is not committed");
    ver(L"game", v);
    CHECK(!strcmp(v, "2.0"), "game still 2.0 after a failed build");
    CHECK(stage_recover(T) == 3 && !has(L"game.new"), "recover removes an unfinished staging folder");
    /* the game is running: the swap fails and nothing changes */
    stage_new("3.0");
    wchar_t exe[1024];
    _snwprintf(exe, 1024, L"%ls\\game\\sly2.exe", T);
    HANDLE lock = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    int committed = stage_commit(T, &err);
    CloseHandle(lock);
    ver(L"game", v);
    char vo[64];
    ver(L"game.old", vo);
    if (committed) printf("  (note: a folder with an open file could be renamed here)\n");
    CHECK(committed ? !strcmp(v, "3.0") : (!strcmp(v, "2.0") && has(L"game.new\\.complete")), "a locked game: all or nothing");
    if (!committed) CHECK(stage_commit(T, &err), "commit once the game is closed");
    ver(L"game", v);
    ver(L"game.old", vo);
    CHECK(!strcmp(v, "3.0") && !strcmp(vo, "2.0"), "3.0 current, 2.0 previous");
    /* rollback and forward again */
    CHECK(stage_rollback(T, &err), "rollback");
    ver(L"game", v);
    ver(L"game.old", vo);
    CHECK(!strcmp(v, "2.0") && !strcmp(vo, "3.0") && !has(L"game.swap"), "rolled back to 2.0");
    CHECK(stage_rollback(T, &err), "forward again");
    ver(L"game", v);
    CHECK(!strcmp(v, "3.0"), "3.0 again");
    /* power loss between the two renames of a commit: game missing, complete game.new present */
    stage_new("4.0");
    wchar_t cur[1024], old[1024];
    _snwprintf(cur, 1024, L"%ls\\game", T);
    _snwprintf(old, 1024, L"%ls\\game.old", T);
    stage_delete_tree(old);
    MoveFileExW(cur, old, 0);
    CHECK(stage_recover(T) == 1, "recover finishes an interrupted commit");
    ver(L"game", v);
    CHECK(!strcmp(v, "4.0") && !has(L"game\\.complete"), "game = 4.0");
    /* game missing, only game.old (crash during an unfinished build after the first rename) */
    stage_delete_tree(cur);
    CHECK(stage_recover(T) == 2, "recover restores the previous version");
    ver(L"game", v);
    CHECK(!strcmp(v, "3.0"), "game = 3.0 restored");
    CHECK(stage_recover(T) == 0, "nothing to recover");
    CHECK(!stage_rollback(T, &err), "no previous version: rollback does nothing");
    stage_delete_tree(T);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_release <temp folder> [python cases]\n"); return 2; }
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, T, 1024);
    test_ed25519();
    if (argc > 2) test_python_cases(argv[2]);
    test_manifest();
    test_stage();
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
