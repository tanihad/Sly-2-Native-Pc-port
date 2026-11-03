/* Launcher updates: fetch the signed release manifest, check it, download and verify the source bundle.
 * The build itself is the normal one (setup_install with s->bundle), staged and swapped (release.h).
 *
 * Trust: the manifest must carry a valid Ed25519 signature by the key built into this program
 * (release_key.h, made by scripts/release_keygen.py); it names the bundle's size and SHA-256, and the
 * bundle is checked against both before anything reads it. HTTPS only (WinHTTP, the system's TLS); test
 * builds (-DSLY2_TEST_BUILD) also accept plain HTTP to 127.0.0.1 / localhost and a /pubkey= test key. */
#include "setup.h"
#include "ed25519.h"
#include "release_key.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MANIFEST_URL L"https://github.com/tanihad/sly2-decomp/releases/latest/download/sly2-release.txt"

static uint8_t test_key[32];
static int have_test_key;

const wchar_t* update_default_url(void) { return DEFAULT_MANIFEST_URL; }

int update_allow_loopback_http(void) {
#ifdef SLY2_TEST_BUILD
    return 1;
#else
    return 0;
#endif
}

void update_set_test_key(const char* hex) {
#ifdef SLY2_TEST_BUILD
    have_test_key = strlen(hex) == 64 && ed25519_from_hex(hex, test_key, 32);
#else
    (void)hex;
#endif
}

static const uint8_t* public_key(void) { return have_test_key ? test_key : SLY2_RELEASE_PUBKEY; }

/* a small file into memory (at most `limit` bytes), through a temp file in <dir>\updates */
static int fetch_small(setup_t* s, const wchar_t* url, const wchar_t* name, char** data, size_t* n, uint64_t limit) {
    wchar_t dir[MAXP], f[MAXP], part[MAXP];
    wpath(dir, MAXP, s->dir, UPDATES_DIR);
    mkdir_p(dir);
    wpath(f, MAXP, dir, name);
    wfmt(part, MAXP, L"%ls.part", f);
    DeleteFileW(part);
    s->net_https_only = 1;
    s->net_limit = limit;
    int r = net_download(s, url, part, 0, name, 2);
    s->net_limit = 0;
    s->net_https_only = 0;
    if (r) { DeleteFileW(part); return r == -2 ? -2 : 0; }
    int ok = read_file(part, data, n) && *n <= limit;
    DeleteFileW(part);
    return ok;
}

int update_check(setup_t* s, const char* installed, update_info* u) {
    memset(u, 0, sizeof *u);
    const wchar_t* url = s->update_url[0] ? s->update_url : DEFAULT_MANIFEST_URL;
    if (!release_url_allowed(url, update_allow_loopback_http())) {
        wfmt(s->error, 2048, L"Couldn't check for updates: the update address is not a secure (https://) address.");
        return 0;
    }
    wchar_t sig_url[MAXP + 8];
    wfmt(sig_url, MAXP + 8, L"%ls.sig", url);
    char *text = NULL, *sig = NULL;
    size_t tn = 0, sn = 0;
    int r = fetch_small(s, url, L"sly2-release.txt", &text, &tn, RELEASE_MAX_MANIFEST);
    if (r == 1) r = fetch_small(s, sig_url, L"sly2-release.txt.sig", &sig, &sn, 1024);
    if (r != 1) {
        logw(L"[update] check failed: %ls", s->error);
        wfmt(s->error, 2048, L"Couldn't check for updates (no connection to the update server).");
        free(text);
        free(sig);
        return 0;
    }
    int ok = 0;
    char why[256] = "";
    if (!release_verify(text, tn, sig, sn, public_key())) {
        logw(L"[update] the manifest's signature is not valid: ignored");
        wfmt(s->error, 2048, L"Couldn't check for updates: the update information is not signed correctly, so it was ignored.");
    } else if (!release_parse(text, tn, &u->m, why, sizeof why)) {
        logw(L"[update] manifest: %hs", why);
        wfmt(s->error, 2048, L"Couldn't check for updates: %hs.", why);
    } else {
        release_url_resolve(url, u->m.bundle, u->bundle_url, MAXP * 2);
        char mine[64];
        snprintf(mine, sizeof mine, "%ls", SETUP_VERSION);
        u->newer = release_version_cmp(u->m.version, installed && installed[0] ? installed : "0") > 0;
        u->launcher_too_old = u->m.min_launcher[0] && release_version_cmp(u->m.min_launcher, mine) > 0;
        loga("[update] latest %s (installed %s)%s%s", u->m.version, installed ? installed : "-", u->newer ? ": newer" : "",
             u->launcher_too_old ? ", needs a newer launcher" : "");
        ok = 1;
    }
    free(text);
    free(sig);
    return ok;
}

int update_download(setup_t* s, update_info* u) {
    if (!release_url_allowed(u->bundle_url, update_allow_loopback_http()))
        { wfmt(s->error, 2048, L"The update's download address is not secure (https://): not downloaded."); return EXIT_FAILED; }
    wchar_t dir[MAXP], f[MAXP], part[MAXP];
    wpath(dir, MAXP, s->dir, UPDATES_DIR);
    mkdir_p(dir);
    wchar_t name[96];
    wfmt(name, 96, L"sly2-source-%hs.zip", u->m.version);
    wpath(f, MAXP, dir, name);
    wfmt(part, MAXP, L"%ls.part", f);
    s->net_https_only = 1;
    s->net_limit = u->m.bundle_size;
    int r = net_download(s, u->bundle_url, part, u->m.bundle_size, name, 5);
    s->net_limit = 0;
    s->net_https_only = 0;
    if (r == -2) return EXIT_CANCELLED;
    if (r) return EXIT_FAILED;
    uint8_t d[32];
    if (file_size(part) != u->m.bundle_size || !sha256_file(s, part, d) || memcmp(d, u->m.bundle_sha256, 32)) {
        DeleteFileW(part);
        if (s->cancel) return EXIT_CANCELLED;
        logw(L"[update] %ls: size or SHA-256 does not match the signed manifest: deleted", name);
        wfmt(s->error, 2048, L"The downloaded update is damaged or not the published file (its checksum does not match), "
                             L"so it was not installed. Click Update to try again.");
        return EXIT_FAILED;
    }
    if (!MoveFileExW(part, f, MOVEFILE_REPLACE_EXISTING)) { wfmt(s->error, 2048, L"Cannot write %ls (error %lu).", f, GetLastError()); return EXIT_FAILED; }
    loga("[update] %ls: size and SHA-256 match the signed manifest", name);
    wcopy(s->bundle, MAXP, f);
    return EXIT_OK;
}
