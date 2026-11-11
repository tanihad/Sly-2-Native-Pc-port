/* Signed release manifests (parse, verify, version order, URL policy) and the atomic game folder swap.
 * See release.h. No UI and no setup_t: tests/unit/launcher compiles this file on its own. */
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include "release.h"
#include "ed25519.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>

/* ---- manifest ----------------------------------------------------------------------------------- */

static void setwhy(char* why, size_t cap, const char* fmt, const char* a) {
    if (why && cap) snprintf(why, cap, fmt, a ? a : "");
}

static int copy_val(char* dst, size_t cap, const char* v, size_t n) {
    if (n >= cap) return 0;
    memcpy(dst, v, n);
    dst[n] = 0;
    return 1;
}

static int valid_version(const char* v) {
    int digits = 0, parts = 0;
    for (const char* p = v;; p++) {
        if (*p >= '0' && *p <= '9') { if (++digits > 9) return 0; continue; }
        if (!digits) return 0;
        parts++;
        digits = 0;
        if (!*p) break;
        if (*p != '.') return 0;
    }
    return parts >= 1 && parts <= 4;
}

int release_parse(const char* text, size_t n, release_manifest* m, char* why, size_t cap) {
    memset(m, 0, sizeof *m);
    if (n > RELEASE_MAX_MANIFEST) { setwhy(why, cap, "the manifest is too large", NULL); return 0; }
    int have_format = 0, have_product = 0, have_sha = 0, have_size = 0;
    unsigned seen = 0;
    const char* p = text;
    const char* end = text + n;
    if (n >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;   /* UTF-8 BOM */
    while (p < end) {
        const char* e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        const char* a = p;
        const char* b = e;
        p = e < end ? e + 1 : end;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t')) b--;
        if (a == b || *a == '#') continue;
        if (memchr(a, 0, (size_t)(b - a))) { setwhy(why, cap, "the manifest contains a NUL byte", NULL); return 0; }
        const char* eq = memchr(a, '=', (size_t)(b - a));
        if (!eq) { setwhy(why, cap, "a manifest line has no '='", NULL); return 0; }
        const char* k1 = eq;
        while (k1 > a && (k1[-1] == ' ' || k1[-1] == '\t')) k1--;
        const char* v = eq + 1;
        while (v < b && (*v == ' ' || *v == '\t')) v++;
        size_t kn = (size_t)(k1 - a), vn = (size_t)(b - v);
        char key[32];
        if (!kn || kn >= sizeof key) { setwhy(why, cap, "a manifest key is empty or too long", NULL); return 0; }
        memcpy(key, a, kn);
        key[kn] = 0;
        static const char* known[] = { "format", "product", "version", "commit", "date", "bundle", "bundle_size",
                                       "bundle_sha256", "min_launcher", "page", "notes" };
        int idx = -1;
        for (int i = 0; i < (int)(sizeof known / sizeof known[0]); i++)
            if (!strcmp(key, known[i])) idx = i;
        if (idx < 0) continue;                          /* a newer manifest's extra key */
        if (seen & (1u << idx)) { setwhy(why, cap, "the manifest repeats the key '%s'", key); return 0; }
        seen |= 1u << idx;
        char tmp[80];
        int ok = 1;
        switch (idx) {
        case 0:
            ok = copy_val(tmp, sizeof tmp, v, vn) && atoi(tmp) == RELEASE_FORMAT && vn == 1;
            if (!ok) { setwhy(why, cap, "the manifest has an unknown format (a newer launcher is needed)", NULL); return 0; }
            have_format = 1;
            break;
        case 1:
            ok = vn == strlen(RELEASE_PRODUCT) && !memcmp(v, RELEASE_PRODUCT, vn);
            if (!ok) { setwhy(why, cap, "the manifest is for another product", NULL); return 0; }
            have_product = 1;
            break;
        case 2: ok = copy_val(m->version, sizeof m->version, v, vn) && valid_version(m->version); break;
        case 3: ok = copy_val(m->commit, sizeof m->commit, v, vn); break;
        case 4: ok = copy_val(m->date, sizeof m->date, v, vn); break;
        case 5: ok = copy_val(m->bundle, sizeof m->bundle, v, vn) && vn > 0; break;
        case 6: {
            ok = vn > 0 && vn <= 12 && copy_val(tmp, sizeof tmp, v, vn);
            for (size_t i = 0; ok && i < vn; i++) ok = isdigit((unsigned char)tmp[i]);
            if (ok) m->bundle_size = _strtoui64(tmp, NULL, 10);
            ok = ok && m->bundle_size > 0 && m->bundle_size <= RELEASE_MAX_BUNDLE;
            have_size = ok;
            break;
        }
        case 7: ok = vn == 64 && ed25519_from_hex(v, m->bundle_sha256, 32); have_sha = ok; break;
        case 8: ok = copy_val(m->min_launcher, sizeof m->min_launcher, v, vn) && (!vn || valid_version(m->min_launcher)); break;
        case 9: ok = copy_val(m->page, sizeof m->page, v, vn); break;
        case 10: ok = copy_val(m->notes, sizeof m->notes, v, vn); break;
        }
        if (!ok) { setwhy(why, cap, "the manifest's '%s' value is invalid", key); return 0; }
    }
    if (!have_format) { setwhy(why, cap, "the manifest has no format line", NULL); return 0; }
    if (!have_product) { setwhy(why, cap, "the manifest names no product", NULL); return 0; }
    if (!m->version[0]) { setwhy(why, cap, "the manifest has no version", NULL); return 0; }
    if (!m->bundle[0] || !have_size || !have_sha) { setwhy(why, cap, "the manifest does not describe a bundle", NULL); return 0; }
    setwhy(why, cap, "ok", NULL);
    return 1;
}

int release_verify(const char* text, size_t n, const char* sig_text, size_t sig_n, const uint8_t pub[32]) {
    size_t a = 0, b = sig_n;
    while (a < b && isspace((unsigned char)sig_text[a])) a++;
    while (b > a && isspace((unsigned char)sig_text[b - 1])) b--;
    uint8_t sig[64];
    if (b - a != 128 || !ed25519_from_hex(sig_text + a, sig, 64)) return 0;
    return ed25519_verify(sig, text, n, pub);
}

int release_version_cmp(const char* a, const char* b) {
    int va = valid_version(a), vb = valid_version(b);
    if (!va || !vb) return va - vb;
    for (;;) {
        unsigned long x = strtoul(a, (char**)&a, 10), y = strtoul(b, (char**)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (!*a && !*b) return 0;
        if (*a) a++;
        if (*b) b++;
    }
}

/* ---- URLs --------------------------------------------------------------------------------------- */

int release_url_allowed(const wchar_t* url, int allow_loopback_http) {
    if (!_wcsnicmp(url, L"https://", 8)) return url[8] != 0 && url[8] != L'/';
    if (_wcsnicmp(url, L"http://", 7) || !allow_loopback_http) return 0;
    const wchar_t* h = url + 7;
    static const wchar_t* loop[] = { L"127.0.0.1", L"localhost", L"[::1]" };
    for (int i = 0; i < 3; i++) {
        size_t n = wcslen(loop[i]);
        if (!_wcsnicmp(h, loop[i], n) && (h[n] == L':' || h[n] == L'/' || h[n] == 0)) return 1;
    }
    return 0;
}

void release_url_resolve(const wchar_t* base, const char* ref, wchar_t* out, size_t cap) {
    wchar_t r[1100];
    if (!MultiByteToWideChar(CP_UTF8, 0, ref, -1, r, 1100)) r[0] = 0;
    if (wcsstr(r, L"://")) { wcsncpy(out, r, cap - 1); out[cap - 1] = 0; return; }
    wcsncpy(out, base, cap - 1);
    out[cap - 1] = 0;
    wchar_t* q = wcschr(out, L'?');
    if (q) *q = 0;
    wchar_t* slash = wcsrchr(out, L'/');
    if (slash && slash > wcsstr(out, L"://") + 2) slash[1] = 0;
    else wcsncat(out, L"/", cap - wcslen(out) - 1);
    wcsncat(out, r, cap - wcslen(out) - 1);
}

/* ---- the game folder swap --------------------------------------------------------------------------- */

static void sub(wchar_t* out, const wchar_t* dir, const wchar_t* name) { _snwprintf(out, 1024, L"%ls\\%ls", dir, name); out[1023] = 0; }

static int is_dir(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_file(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int stage_delete_tree(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) return 1;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); return DeleteFileW(p) != 0; }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t pat[1024];
        sub(pat, p, L"*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                wchar_t c[1024];
                sub(c, p, fd.cFileName);
                stage_delete_tree(c);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (int i = 0; i < 20; i++) {                  /* (a scanner may hold the folder for a moment) */
        if (RemoveDirectoryW(p) || GetLastError() == ERROR_FILE_NOT_FOUND) return 1;
        Sleep(50);
    }
    return 0;
}

int stage_begin(const wchar_t* dir, wchar_t* staging, size_t cap) {
    wchar_t p[1024];
    sub(p, dir, L"game.new");
    if (!stage_delete_tree(p)) return 0;
    if (!CreateDirectoryW(p, NULL)) return 0;
    if (staging) { wcsncpy(staging, p, cap - 1); staging[cap - 1] = 0; }
    return 1;
}

static int write_small(const wchar_t* path, const char* text) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0, n = (DWORD)strlen(text);
    int ok = WriteFile(h, text, n, &w, NULL) && w == n;
    ok &= FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

int stage_mark_complete(const wchar_t* dir, const char* version) {
    wchar_t p[1024], f[1024];
    sub(p, dir, L"game.new");
    sub(f, p, L"sly2-version.txt");
    if (!write_small(f, version)) return 0;
    sub(f, p, L".complete");
    return write_small(f, version);
}

int stage_commit(const wchar_t* dir, unsigned long* err) {
    wchar_t cur[1024], nw[1024], old[1024], mark[1024];
    sub(cur, dir, L"game");
    sub(nw, dir, L"game.new");
    sub(old, dir, L"game.old");
    sub(mark, nw, L".complete");
    *err = 0;
    if (!is_file(mark)) { *err = ERROR_FILE_NOT_FOUND; return 0; }
    int had = is_dir(cur);
    if (had) {
        if (!stage_delete_tree(old)) { *err = GetLastError(); return 0; }
        if (!MoveFileExW(cur, old, 0)) { *err = GetLastError(); return 0; }   /* in use: the game is running */
    }
    if (!MoveFileExW(nw, cur, 0)) {
        *err = GetLastError();
        if (had) MoveFileExW(old, cur, 0);          /* undo: the previous version stays current */
        return 0;
    }
    sub(mark, cur, L".complete");
    DeleteFileW(mark);
    return 1;
}

int stage_recover(const wchar_t* dir) {
    wchar_t cur[1024], nw[1024], old[1024], mark[1024];
    sub(cur, dir, L"game");
    sub(nw, dir, L"game.new");
    sub(old, dir, L"game.old");
    sub(mark, nw, L".complete");
    if (!is_dir(cur)) {
        if (is_dir(nw) && is_file(mark)) {
            if (MoveFileExW(nw, cur, 0)) { sub(mark, cur, L".complete"); DeleteFileW(mark); return 1; }
        }
        if (is_dir(old) && MoveFileExW(old, cur, 0)) return 2;
    }
    if (is_dir(nw) && !is_file(mark)) { stage_delete_tree(nw); return 3; }
    return 0;
}

int stage_rollback(const wchar_t* dir, unsigned long* err) {
    wchar_t cur[1024], old[1024], tmp[1024];
    sub(cur, dir, L"game");
    sub(old, dir, L"game.old");
    sub(tmp, dir, L"game.swap");
    *err = 0;
    if (!is_dir(old) || !is_dir(cur)) { *err = ERROR_FILE_NOT_FOUND; return 0; }
    stage_delete_tree(tmp);
    if (!MoveFileExW(cur, tmp, 0)) { *err = GetLastError(); return 0; }
    if (!MoveFileExW(old, cur, 0)) { *err = GetLastError(); MoveFileExW(tmp, cur, 0); return 0; }
    if (!MoveFileExW(tmp, old, 0)) *err = GetLastError();   /* (the current one is right; the old one stays as game.swap) */
    return 1;
}

void stage_version(const wchar_t* dir, const wchar_t* subdir, char* out, size_t cap) {
    wchar_t d[1024], f[1024];
    sub(d, dir, subdir);
    sub(f, d, L"sly2-version.txt");
    out[0] = 0;
    HANDLE h = CreateFileW(f, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    if (ReadFile(h, out, (DWORD)cap - 1, &got, NULL)) out[got] = 0;
    CloseHandle(h);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
}

/* ---- the AI model ------------------------------------------------------------------------------ */

int release_sha256_file(const wchar_t* path, uint8_t out[32]) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
             BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0));
    enum { CHUNK = 1 << 20 };
    uint8_t* buf = ok ? malloc(CHUNK) : NULL;
    ok = ok && buf;
    for (DWORD got = 0; ok;) {
        if (!ReadFile(f, buf, CHUNK, &got, NULL)) { ok = 0; break; }
        if (!got) break;
        ok = BCRYPT_SUCCESS(BCryptHashData(h, buf, got, 0));
    }
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(h, out, 32, 0));
    free(buf);
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

int stage_place_model(const wchar_t* src_dir, const wchar_t* staging, char* why, size_t cap) {
    wchar_t src[1024], sum[1024], dd[1024], dst[1024], part[1024];
    sub(src, src_dir, MODEL_FILE);
    sub(sum, src_dir, MODEL_FILE L".sha256");
    if (!is_file(src)) return 0;                     /* (sources of a version without the model) */
    sub(dd, staging, L"models");
    sub(dst, dd, MODEL_FILE);
    _snwprintf(part, 1024, L"%ls.part", dst);
    part[1023] = 0;
    char text[160] = "";
    HANDLE h = CreateFileW(sum, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        if (ReadFile(h, text, sizeof text - 1, &got, NULL)) text[got] = 0;
        CloseHandle(h);
    }
    uint8_t want[32], have[32];
    if (strlen(text) < 64 || (text[64] && !isspace((unsigned char)text[64])) || !ed25519_from_hex(text, want, 32)) {
        setwhy(why, cap, "the sources' models\\x4plus.bin.sha256 is missing or not a SHA-256", NULL);
        return -1;
    }
    if (!CreateDirectoryW(dd, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        setwhy(why, cap, "cannot create the models folder", NULL);
        return -1;
    }
    DeleteFileW(part);
    if (!CopyFileW(src, part, FALSE)) {
        setwhy(why, cap, "cannot copy models\\x4plus.bin (is the disk full?)", NULL);
        DeleteFileW(part);
        RemoveDirectoryW(dd);
        return -1;
    }
    if (!release_sha256_file(part, have) || memcmp(have, want, 32)) {
        setwhy(why, cap, "the copied models\\x4plus.bin does not match its SHA-256: not installed", NULL);
        DeleteFileW(part);
        RemoveDirectoryW(dd);
        return -1;
    }
    if (!MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)) {
        setwhy(why, cap, "cannot write models\\x4plus.bin", NULL);
        DeleteFileW(part);
        RemoveDirectoryW(dd);
        return -1;
    }
    return 1;
}
