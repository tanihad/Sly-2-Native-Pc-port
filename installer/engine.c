/* Sly2-Setup: the install, finish and uninstall steps (setup.h). The wizard (main.c) runs setup_install
 * on a worker thread; /silent runs it directly.
 *
 * <dir>                       sly2.exe, README.txt, Controller test.cmd (the built game), Uninstall.exe,
 *                             setup.log, setup-state.ini
 * <dir>\builder               our sources (from the payload), tools\ (llvm-mingw, uv, python, uv-cache),
 *                             .venv, orig\SCUS_973.16 (from the player's disc), build\ (intermediates),
 *                             dist\ (the packaged game before it is copied into <dir>)
 */
#include "setup.h"
#include "art.h"

#include <shellapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LNK_NAME L"Sly 2 - Band of Thieves.lnk"
#define APP_NAME L"Sly 2: Band of Thieves"

const wchar_t* setup_default_regkey(void) { return L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Sly2-PC"; }

void setup_defaults(setup_t* s) {
    memset(s, 0, sizeof *s);
    InitializeCriticalSection(&s->lock);
    wchar_t la[MAXP];
    if (known_folder(KF_LOCALAPPDATA, la, MAXP)) wfmt(s->dir, MAXP, L"%ls\\Programs\\Sly2", la);
    else wcopy(s->dir, MAXP, L"C:\\Games\\Sly2");
    wcopy(s->regkey, MAXP, setup_default_regkey());
    s->desktop_shortcut = 1;
    s->play = 1;
    s->delete_saves = -1;
}

/* ---- messages and progress ------------------------------------------------------------------- */

void setup_message(setup_t* s, const wchar_t* text, UINT flags) {
    logw(L"[message] %ls", text);
    if (!s->silent && !s->uitest) MessageBoxW(NULL, text, APP_NAME, flags | MB_SETFOREGROUND);
}

int setup_ask(setup_t* s, const wchar_t* text, UINT flags) {
    if (s->silent || s->uitest) {
        int def = (flags & MB_DEFBUTTON2) ? ((flags & 0xF) == MB_YESNO ? IDNO : IDCANCEL) : ((flags & 0xF) == MB_YESNO ? IDYES : IDOK);
        logw(L"[question] %ls -> %ls", text, def == IDYES || def == IDOK ? L"yes" : L"no");
        return def;
    }
    return MessageBoxW(NULL, text, APP_NAME, flags | MB_SETFOREGROUND);
}

static void say(setup_t* s, const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    buf[2047] = 0;
    va_end(ap);
    logw(L"%ls", buf);
    if (s->logline) s->logline(s->ui, buf);
}

static void report(setup_t* s, int permille, const wchar_t* detail) {
    if (permille > 1000) permille = 1000;
    if (s->pm_scale) permille = s->pm_off + permille * s->pm_scale / 1000;
    if (s->progress) s->progress(s->ui, permille, s->status, detail);
}

/* the next step: its share of the bar and its status line */
static void step(setup_t* s, int base, int span, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(s->status, 511, fmt, ap);
    s->status[511] = 0;
    va_end(ap);
    s->pm_base = base;
    s->pm_span = span;
    logw(L"== %ls", s->status);
    if (s->logline) s->logline(s->ui, s->status);
    report(s, base, L"");
}

static void sub(setup_t* s, uint64_t done, uint64_t total, const wchar_t* detail) {
    int pm = s->pm_base + (total ? (int)((uint64_t)s->pm_span * (done > total ? total : done) / total) : 0);
    report(s, pm, detail);
}

void setup_bytes(setup_t* s, const wchar_t* label, uint64_t done, uint64_t total) {
    static DWORD t0;
    static uint64_t d0;
    static wchar_t last[96];
    if (wcscmp(last, label ? label : L"") || done < d0) { wcopy(last, 96, label ? label : L""); t0 = GetTickCount(); d0 = done; }
    wchar_t a[32], b[32], line[256];
    fmt_size(a, 32, done);
    fmt_size(b, 32, total);
    DWORD dt = GetTickCount() - t0;
    if (dt > 500 && done > d0) {
        wchar_t r[32];
        fmt_size(r, 32, (uint64_t)((double)(done - d0) * 1000.0 / dt));
        wfmt(line, 256, L"%ls: %ls of %ls (%ls/s)", label, a, b, r);
    } else wfmt(line, 256, L"%ls: %ls of %ls", label, a, b);
    sub(s, done, total, line);
}

static int fail(setup_t* s, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(s->error, 2047, fmt, ap);
    s->error[2047] = 0;
    va_end(ap);
    say(s, L"ERROR: %ls", s->error);
    return EXIT_FAILED;
}

/* ---- paths ----------------------------------------------------------------------------------- */

static void bpath(setup_t* s, wchar_t* out, const wchar_t* rel) {   /* <dir>\builder\rel */
    wchar_t b[MAXP];
    wpath(b, MAXP, s->dir, L"builder");
    if (rel && *rel) wpath(out, MAXP, b, rel); else wcopy(out, MAXP, b);
}

static void dpath(setup_t* s, wchar_t* out, const wchar_t* rel) { wpath(out, MAXP, s->dir, rel); }

static void state_path(setup_t* s, wchar_t* out) { dpath(s, out, L"setup-state.ini"); }

void setup_state_set(setup_t* s, const wchar_t* key, const wchar_t* value);
#define state_set setup_state_set
#define state_get setup_state_get
#define user_data_dir setup_user_data_dir
#define game_running setup_game_running

void setup_state_set(setup_t* s, const wchar_t* key, const wchar_t* value) {
    wchar_t p[MAXP];
    state_path(s, p);
    if (!file_exists(p)) {                           /* UTF-16 file: WritePrivateProfileString keeps Unicode */
        static const uint8_t bom[2] = { 0xFF, 0xFE };
        write_file(p, bom, 2);
    }
    WritePrivateProfileStringW(L"setup", key, value, p);
}

void setup_state_get(const wchar_t* dir, const wchar_t* key, wchar_t* out, size_t cap) {
    wchar_t p[MAXP];
    wpath(p, MAXP, dir, L"setup-state.ini");
    GetPrivateProfileStringW(L"setup", key, L"", out, (DWORD)cap, p);
}

void setup_user_data_dir(setup_t* s, wchar_t* out) {
    wchar_t p[MAXP];
    dpath(s, p, L"portable.txt");
    if (s->userdata[0]) wcopy(out, MAXP, s->userdata);
    else if (file_exists(p)) wcopy(out, MAXP, s->dir);
    else {
        wchar_t la[MAXP];
        if (!known_folder(KF_LOCALAPPDATA, la, MAXP)) { out[0] = 0; return; }
        wpath(out, MAXP, la, L"Sly2");
    }
}

static HKEY reg_root(setup_t* s) { return s->allusers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

static void shortcut_paths(setup_t* s, wchar_t* start, wchar_t* desk) {
    wchar_t d[MAXP];
    if (s->startmenu[0]) wcopy(d, MAXP, s->startmenu);
    else known_folder(s->allusers ? KF_COMMON_PROGRAMS : KF_PROGRAMS, d, MAXP);
    wpath(start, MAXP, d, LNK_NAME);
    if (s->desktop[0]) wcopy(d, MAXP, s->desktop);
    else known_folder(s->allusers ? KF_PUBLIC_DESKTOP : KF_DESKTOP, d, MAXP);
    wpath(desk, MAXP, d, LNK_NAME);
}

int setup_find_existing(setup_t* s) {
    wchar_t loc[MAXP];
    for (int machine = 0; machine < 2; machine++) {
        HKEY root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        if (!reg_get_str(root, s->regkey, L"InstallLocation", loc, MAXP) || !loc[0]) continue;
        wchar_t st[MAXP];
        wpath(st, MAXP, loc, L"setup-state.ini");
        if (!file_exists(st)) continue;
        wcopy(s->dir, MAXP, loc);
        s->existing = 1;
        s->allusers = machine;
        wchar_t v[MAXP];
        state_get(loc, L"iso", v, MAXP);
        if (v[0] && file_exists(v) && !s->iso[0]) wcopy(s->iso, MAXP, v);
        state_get(loc, L"keep_tools", v, MAXP);
        if (v[0] == L'1') s->keep_tools = 1;
        return 1;
    }
    return 0;
}

/* ---- disk space ------------------------------------------------------------------------------ */

uint64_t setup_space_needed(setup_t* s) {
    const uint64_t MB = 1024 * 1024;
    uint64_t need = SETUP_NEED_SOURCES_MB * MB + SETUP_NEED_BUILD_MB * MB + SETUP_NEED_GAME_MB * MB;
    wchar_t p[MAXP];
    for (int i = 0; i < SETUP_NPINS; i++) {
        wchar_t d[MAXP];
        wpath(d, MAXP, L"tools", SETUP_PINS[i].dir);
        bpath(s, p, d);
        if (!dir_exists(p)) need += SETUP_PINS[i].installed_mb * MB + SETUP_PINS[i].size;   /* archive + extracted */
    }
    bpath(s, p, L"tools\\python");
    if (!dir_exists(p)) need += SETUP_NEED_PYTHON_MB * MB;
    bpath(s, p, L".venv");
    if (!dir_exists(p)) need += SETUP_NEED_VENV_MB * MB;
    return need + need / 10;                        /* + 10% */
}

/* ---- archives -------------------------------------------------------------------------------- */

static int from_payload(setup_t* s, const payload_entry* e, const wchar_t* dest) {
    int r = payload_extract(s, e, dest);
    if (r > 0) return EXIT_OK;
    if (s->cancel) return EXIT_CANCELLED;
    if (r < 0) return fail(s, L"This setup file is damaged (%hs does not match its SHA-256). Download Setup again.", e->name);
    return fail(s, L"Cannot unpack %hs from the setup file (error %lu; is the disk full?).", e->name, GetLastError());
}
static int verify(setup_t* s, const wchar_t* path, const char* sha, const wchar_t* label) {
    uint8_t d[32];
    report(s, s->pm_base + s->pm_span, L"");
    if (!sha256_file(s, path, d)) return 0;
    if (!sha256_equal_hex(d, sha)) {
        wchar_t got[65];
        sha256_hex(d, got);
        say(s, L"%ls: SHA-256 mismatch (expected %hs, got %ls)", label, sha, got);
        return 0;
    }
    say(s, L"%ls: SHA-256 OK", label);
    return 1;
}

/* A verified copy of a pinned archive. *path = where it is; *ours = it is in our _downloads (deleted
 * after extraction). EXIT_OK or an error. */
static int get_archive(setup_t* s, const setup_pin* p, wchar_t* path, int* ours) {
    wchar_t dl[MAXP], dldir[MAXP];
    bpath(s, dldir, L"tools\\_downloads");
    mkdir_p(dldir);
    wpath(dl, MAXP, dldir, p->file);
    *ours = 1;
    char name8[128];
    WideCharToMultiByte(CP_UTF8, 0, p->file, -1, name8, sizeof name8, NULL, NULL);

    const payload_entry* e = payload_find(name8);    /* the offline setup carries it */
    if (e) {
        int r0;
        if (!sha256_equal_hex(e->sha256, p->sha256)) return fail(s, L"The setup file carries a different %ls than this setup expects.", p->name);
        say(s, L"%ls: from this setup program", p->name);
        if ((r0 = from_payload(s, e, dl)) != 0) return r0;
        wcopy(path, MAXP, dl);
        return EXIT_OK;
    }
    if (s->from[0]) {                                /* /from=<folder> (setup.ps1 -From layout) */
        static const wchar_t* subs[] = { L"", L"_downloads", L"tools\\_downloads" };
        for (int i = 0; i < 3; i++) {
            wchar_t d[MAXP], c[MAXP];
            if (*subs[i]) wpath(d, MAXP, s->from, subs[i]); else wcopy(d, MAXP, s->from);
            wpath(c, MAXP, d, p->file);
            if (file_exists(c)) {
                say(s, L"%ls: %ls", p->name, c);
                if (verify(s, c, p->sha256, p->file)) { wcopy(path, MAXP, c); *ours = 0; return EXIT_OK; }
                if (s->cancel) return EXIT_CANCELLED;
            }
        }
    }
    if (file_size(dl) == p->size) {                  /* complete from an earlier run */
        if (verify(s, dl, p->sha256, p->file)) { wcopy(path, MAXP, dl); return EXIT_OK; }
        if (s->cancel) return EXIT_CANCELLED;
        DeleteFileW(dl);
    }
    if (s->offline_only)
        return fail(s, L"%ls (%ls) is not in the offline package%ls%ls.", p->name, p->file, s->from[0] ? L" or in " : L"", s->from);

    wchar_t url[MAXP * 2], part[MAXP];
    wfmt(part, MAXP, L"%ls.part", dl);
    if (s->mirror[0]) {
        wchar_t m[MAXP];
        wcopy(m, MAXP, s->mirror);
        size_t n = wcslen(m);
        while (n && (m[n - 1] == L'/' || m[n - 1] == L'\\')) m[--n] = 0;
        if (!_wcsnicmp(m, L"file:///", 8)) { memmove(m, m + 8, (wcslen(m + 8) + 1) * sizeof(wchar_t)); for (wchar_t* c = m; *c; c++) if (*c == L'/') *c = L'\\'; }
        if (!wcsstr(m, L"://")) {                    /* a folder: copy */
            wchar_t src[MAXP];
            wpath(src, MAXP, m, p->file);
            say(s, L"%ls: copying %ls", p->name, src);
            if (!CopyFileW(src, part, FALSE)) return fail(s, L"%ls is not in the download folder %ls (error %lu).", p->file, m, GetLastError());
            goto check;
        }
        wfmt(url, MAXP * 2, L"%ls/%ls", m, p->file);
    } else wcopy(url, MAXP * 2, p->url);
    say(s, L"%ls: downloading %ls", p->name, url);
    int r = net_download(s, url, part, p->size, p->file, 5);
    if (r == -2) return EXIT_CANCELLED;
    if (r) { say(s, L"ERROR: %ls", s->error); return EXIT_FAILED; }
check:
    if (!verify(s, part, p->sha256, p->file)) {
        if (s->cancel) return EXIT_CANCELLED;
        DeleteFileW(part);
        return fail(s, L"The download of %ls is damaged or not the expected file (its SHA-256 does not match). Click Retry to "
                       L"download it again.", p->file);
    }
    if (!MoveFileExW(part, dl, MOVEFILE_REPLACE_EXISTING)) return fail(s, L"Cannot write %ls (error %lu).", dl, GetLastError());
    wcopy(path, MAXP, dl);
    return EXIT_OK;
}

typedef struct { int n, total; const wchar_t* label; } tar_state;

static void tar_line(setup_t* s, void* user, const char* line) {
    tar_state* t = user;
    if (line[0] == 'x' && line[1] == ' ') {
        t->n++;
        if ((t->n & 31) == 0) {
            wchar_t d[128];
            wfmt(d, 128, L"%ls: %d of %d files", t->label, t->n, t->total);
            sub(s, (uint64_t)t->n, (uint64_t)(t->total ? t->total : t->n + 1), d);
        }
    }
}

/* Unpack a zip / tar.gz with Windows' own tar.exe (bsdtar, Windows 10 1803+) into dest. */
static int untar(setup_t* s, const wchar_t* archive, const wchar_t* dest, const wchar_t* extra, int entries, const wchar_t* label) {
    wchar_t tar[MAXP], cmd[MAXP * 3];
    GetSystemDirectoryW(tar, MAXP);
    wcsncat(tar, L"\\tar.exe", MAXP - wcslen(tar) - 1);
    if (!file_exists(tar)) return fail(s, L"Windows' tar.exe is missing (%ls): Setup needs Windows 10 version 1809 or later.", tar);
    if (!mkdir_p(dest)) return fail(s, L"Cannot create %ls (error %lu).", dest, GetLastError());
    wfmt(cmd, MAXP * 3, L"\"%ls\" -xvf \"%ls\" -C \"%ls\" %ls", tar, archive, dest, extra ? extra : L"");
    tar_state t = { 0, entries, label };
    DWORD code;
    s->quiet_child = 1;                              /* (one line per file: only the count is logged) */
    int r = run_hidden(s, cmd, dest, NULL, tar_line, &t, &code);
    s->quiet_child = 0;
    loga("  %d entries unpacked", t.n);
    if (r == -2) return EXIT_CANCELLED;
    if (r || code) return fail(s, L"Unpacking %ls failed (tar exit code %ld; is the disk full?).", label, (long)code);
    return EXIT_OK;
}

static int install_pin(setup_t* s, const setup_pin* p, int base, int span) {
    wchar_t dest[MAXP], check[MAXP], stamp[MAXP], rel[MAXP];
    wpath(rel, MAXP, L"tools", p->dir);
    bpath(s, dest, rel);
    wpath(check, MAXP, dest, p->check);
    wpath(stamp, MAXP, dest, L".sly2-pin");
    char* have = NULL;
    size_t n;
    if (file_exists(check) && read_file(stamp, &have, &n) && n >= 64 && !_strnicmp(have, p->sha256, 64)) {
        free(have);
        step(s, base, span, L"%ls: already installed", p->name);
        return EXIT_OK;
    }
    free(have);
    step(s, base, span * 3 / 4, L"Getting %ls", p->name);
    wchar_t arch[MAXP];
    int ours;
    int r = get_archive(s, p, arch, &ours);
    if (r) return r;
    step(s, base + span * 3 / 4, span - span * 3 / 4, L"Unpacking %ls", p->name);
    wchar_t tmp[MAXP];
    wfmt(tmp, MAXP, L"%ls.tmp", dest);
    delete_tree(tmp);
    delete_tree(dest);
    r = untar(s, arch, tmp, p->excludes, p->entries, p->name);
    if (r) { delete_tree(tmp); return r; }
    /* a single top-level folder (llvm-mingw-<version>-ucrt-x86_64): that is the tool's folder */
    wchar_t pat[MAXP], only[MAXP] = L"";
    wpath(pat, MAXP, tmp, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    int count = 0, isdir = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            count++;
            isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            wpath(only, MAXP, tmp, fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    BOOL moved;
    if (p->flatten && count == 1 && isdir) { moved = MoveFileExW(only, dest, 0); delete_tree(tmp); }
    else moved = MoveFileExW(tmp, dest, 0);
    if (!moved || !file_exists(check)) { delete_tree(tmp); return fail(s, L"%ls: %ls is missing after unpacking.", p->name, p->check); }
    write_file(stamp, p->sha256, 64);
    if (ours) DeleteFileW(arch);                     /* (extracted: the archive is not needed any more) */
    say(s, L"%ls: installed", p->name);
    return EXIT_OK;
}

/* ---- python ---------------------------------------------------------------------------------- */

static int find_python(setup_t* s, const wchar_t* root, wchar_t* out) {
    wchar_t pat[MAXP];
    wfmt(pat, MAXP, L"%ls\\cpython-%ls-windows-x86_64-*", root, SETUP_PYTHON_VERSION);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    int ok = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            wchar_t d[MAXP], py[MAXP];
            wpath(d, MAXP, root, fd.cFileName);
            wpath(py, MAXP, d, L"python.exe");
            if (file_exists(py)) { wcopy(out, MAXP, py); ok = 1; break; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    (void)s;
    return ok;
}

static void uv_env(setup_t* s, wchar_t* env, size_t cap) {
    wchar_t py[MAXP], cache[MAXP];
    bpath(s, py, L"tools\\python");
    bpath(s, cache, L"tools\\uv-cache");
    /* NAME=value\0 ... \0\0 */
    int n = _snwprintf(env, cap, L"UV_PYTHON_INSTALL_DIR=%ls%lcUV_CACHE_DIR=%ls%lcUV_PYTHON_DOWNLOADS=manual%lcUV_NO_PROGRESS=1%lc"
                                 L"PYTHONUTF8=1%lcPYTHONIOENCODING=utf-8%lcPYTHONUNBUFFERED=1%lc%lc",
                       py, 0, cache, 0, 0, 0, 0, 0, 0, 0);
    if (n < 0) env[0] = env[1] = 0;
}

static int run_tool(setup_t* s, const wchar_t* cmd, line_fn fn, void* user, const wchar_t* what) {
    wchar_t env[MAXP * 3], cwd[MAXP];
    uv_env(s, env, MAXP * 3);
    bpath(s, cwd, NULL);
    DWORD code;
    int r = run_hidden(s, cmd, cwd, env, fn, user, &code);
    if (r == -2) return EXIT_CANCELLED;
    if (r) return fail(s, L"%ls: the program could not be started.", what);
    if (code) { s->error[0] = 0; wfmt(s->error, 2048, L"%ls failed (exit code %ld). The details are in %ls.", what, (long)code, log_path()); return EXIT_FAILED; }
    return EXIT_OK;
}

static int install_python(setup_t* s, int base, int span) {
    wchar_t pyroot[MAXP], py[MAXP], uv[MAXP], cmd[MAXP * 3];
    bpath(s, pyroot, L"tools\\python");
    bpath(s, uv, L"tools\\uv\\uv.exe");
    if (find_python(s, pyroot, py)) {
        step(s, base, span, L"Python %ls: already installed", SETUP_PYTHON_VERSION);
        return EXIT_OK;
    }
    step(s, base, span, L"Getting Python %ls", SETUP_PYTHON_VERSION);
    mkdir_p(pyroot);
    const payload_entry* e = payload_find("python.zip");
    if (e) {
        wchar_t z[MAXP];
        bpath(s, z, L"tools\\_downloads\\python.zip");
        wchar_t dd[MAXP];
        bpath(s, dd, L"tools\\_downloads");
        mkdir_p(dd);
        int r0 = from_payload(s, e, z);
        if (r0) return r0;
        int r = untar(s, z, pyroot, NULL, 0, L"Python");
        DeleteFileW(z);
        if (r) return r;
    } else {
        int copied = 0;
        if (s->from[0]) {
            static const wchar_t* subs[] = { L"python", L"tools\\python" };
            for (int i = 0; i < 2 && !copied; i++) {
                wchar_t d[MAXP], src[MAXP];
                wpath(d, MAXP, s->from, subs[i]);
                if (!find_python(s, d, src)) continue;
                wchar_t* slash = wcsrchr(src, L'\\');
                *slash = 0;                          /* the cpython-... folder */
                wchar_t dst[MAXP];
                wpath(dst, MAXP, pyroot, wcsrchr(src, L'\\') + 1);
                say(s, L"Python: copying %ls", src);
                if (!copy_tree(s, src, dst, 0)) return s->cancel ? EXIT_CANCELLED : fail(s, L"Copying Python from %ls failed.", src);
                copied = 1;
            }
        }
        if (!copied) {
            if (s->offline_only) return fail(s, L"Python %ls is not in the offline package.", SETUP_PYTHON_VERSION);
            /* uv downloads python-build-standalone and checks it against the SHA-256 built into uv (pinned
             * above by its own SHA-256). --no-bin / --no-registry: nothing outside the install folder. */
            wfmt(cmd, MAXP * 3, L"\"%ls\" python install %ls --no-bin --no-registry --no-config --no-progress", uv, SETUP_PYTHON_VERSION);
            int r = run_tool(s, cmd, NULL, NULL, L"Downloading Python");
            if (r) {
                if (r == EXIT_FAILED)
                    wfmt(s->error, 2048, L"Downloading Python %ls failed: is this PC online? Click Retry to try again. "
                                         L"(Details: %ls)", SETUP_PYTHON_VERSION, log_path());
                return r;
            }
        }
    }
    if (!find_python(s, pyroot, py)) return fail(s, L"Python %ls is missing after installation.", SETUP_PYTHON_VERSION);
    return EXIT_OK;
}

static int install_packages(setup_t* s, int base, int span) {
    wchar_t venv[MAXP], vpy[MAXP], pyroot[MAXP], py[MAXP], uv[MAXP], cache[MAXP], req[MAXP], cmd[MAXP * 3];
    bpath(s, venv, L".venv");
    bpath(s, vpy, L".venv\\Scripts\\python.exe");
    bpath(s, pyroot, L"tools\\python");
    bpath(s, uv, L"tools\\uv\\uv.exe");
    bpath(s, cache, L"tools\\uv-cache");
    bpath(s, req, L"requirements.txt");
    step(s, base, span, L"Installing the Python packages (rabbitizer, pyelftools)");
    if (!find_python(s, pyroot, py)) return fail(s, L"Python is missing.");
    if (!file_exists(vpy)) {
        delete_tree(venv);
        wfmt(cmd, MAXP * 3, L"\"%ls\" venv \"%ls\" --python \"%ls\" --no-config --no-progress", uv, venv, py);
        int r = run_tool(s, cmd, NULL, NULL, L"Creating the Python environment");
        if (r) return r;
    }
    int seeded = 0;
    const payload_entry* e = payload_find("uv-cache.zip");
    if (e) {
        wchar_t z[MAXP];
        bpath(s, z, L"tools\\_downloads\\uv-cache.zip");
        int r0 = from_payload(s, e, z);
        if (r0) return r0;
        int r = untar(s, z, cache, NULL, 0, L"package cache");
        DeleteFileW(z);
        if (r) return r;
        seeded = 1;
    } else if (s->from[0]) {
        static const wchar_t* subs[] = { L"uv-cache", L"tools\\uv-cache" };
        for (int i = 0; i < 2 && !seeded; i++) {
            wchar_t c[MAXP], tag[MAXP];
            wpath(c, MAXP, s->from, subs[i]);
            wpath(tag, MAXP, c, L"CACHEDIR.TAG");
            if (!file_exists(tag)) continue;
            say(s, L"Packages: from the cache in %ls", c);
            if (!copy_tree(s, c, cache, 1)) return s->cancel ? EXIT_CANCELLED : fail(s, L"Copying the package cache from %ls failed.", c);
            seeded = 1;
        }
    }
    /* --require-hashes: every file must match a SHA-256 in requirements.txt (PyPI's published digests) */
    wfmt(cmd, MAXP * 3, L"\"%ls\" pip install --python \"%ls\" --require-hashes -r \"%ls\" --no-config --no-progress%ls", uv, vpy, req,
         seeded || s->offline_only ? L" --offline" : L"");
    int r = run_tool(s, cmd, NULL, NULL, L"Installing the Python packages");
    if (r == EXIT_FAILED && seeded && !s->offline_only) {
        say(s, L"Packages: not all in the local cache: downloading them from PyPI");
        wfmt(cmd, MAXP * 3, L"\"%ls\" pip install --python \"%ls\" --require-hashes -r \"%ls\" --no-config --no-progress", uv, vpy, req);
        r = run_tool(s, cmd, NULL, NULL, L"Installing the Python packages");
    }
    return r;
}

/* ---- the build ------------------------------------------------------------------------------- */

typedef struct { int dummy; } build_state;

static void build_line(setup_t* s, void* user, const char* line) {
    (void)user;
    if (strncmp(line, "[progress] ", 11)) return;
    const char* l = line + 11;
    /* scripts/build_game.py --progress: stage names in order, and "compile i/n" */
    static const struct { const char* name; int pm; const wchar_t* text; } stages[] = {
        { "generate", 0, L"Translating the game's code for the PC" },
        { "iop", 110, L"Translating the sound modules" },
        { "vu", 150, L"Translating the graphics programs" },
        { "shaders", 165, L"Compiling the graphics shaders" },
        { "compile", 230, L"Compiling" },
        { "link", 935, L"Linking the game" },
        { "package", 950, L"Packaging the game" },
        { "art", 965, L"Making the game's pictures from your disc" },
    };
    for (size_t i = 0; i < sizeof stages / sizeof stages[0]; i++) {
        size_t n = strlen(stages[i].name);
        if (strncmp(l, stages[i].name, n) || (l[n] && l[n] != ' ')) continue;
        int base = SETUP_PM_BUILD + (SETUP_PM_BUILD_SPAN * stages[i].pm) / 1000;
        int next = SETUP_PM_BUILD + (SETUP_PM_BUILD_SPAN * (i + 1 < sizeof stages / sizeof stages[0] ? stages[i + 1].pm : 1000)) / 1000;
        int a = 0, b = 0;
        if (l[n] == ' ' && sscanf(l + n + 1, "%d/%d", &a, &b) == 2 && b > 0) {
            wchar_t d[96];
            wfmt(d, 96, L"%d of %d files", a, b);
            wfmt(s->status, 512, L"Compiling the game (%d of %d files)", a, b);
            report(s, base + (next - base) * a / b, d);
        } else {
            wcopy(s->status, 512, stages[i].text);
            say(s, L"%ls", stages[i].text);
            report(s, base, L"");
        }
        return;
    }
}

static int build_game(setup_t* s) {
    wchar_t vpy[MAXP], script[MAXP], out[MAXP], dist[MAXP], cmd[MAXP * 4];
    bpath(s, vpy, L".venv\\Scripts\\python.exe");
    step(s, SETUP_PM_ELF, SETUP_PM_BUILD - SETUP_PM_ELF, L"Reading the game's executable from your disc image");
    bpath(s, script, L"scripts\\extract_elf.py");
    wfmt(cmd, MAXP * 4, L"\"%ls\" -u \"%ls\" --iso \"%ls\" --force", vpy, script, s->iso);
    int r = run_tool(s, cmd, NULL, NULL, L"Reading the disc image");
    if (r) return r;
    step(s, SETUP_PM_BUILD, SETUP_PM_BUILD_SPAN, L"Building the game");
    bpath(s, script, L"scripts\\build_game.py");
    bpath(s, out, L"build\\rel");
    bpath(s, dist, L"dist");
    delete_tree(dist);
    /* --disc/--art: the game's icon and the launcher's backdrop made from the player's disc (scripts/disc_art.py);
     * never fails the build (without them: our own icon and theme) */
    wfmt(cmd, MAXP * 4, L"\"%ls\" -u \"%ls\" --release --out \"%ls\" --package \"%ls\" --progress --disc \"%ls\" --art", vpy, script,
         out, dist, s->iso);
    build_state bs = { 0 };
    /* this launcher puts the sources' AI model into game\models itself (install_game_files): build_game.py then
     * leaves it out of the package (it puts it there for 1.0.x launchers, which copy only top-level files) */
    SetEnvironmentVariableW(L"SLY2_SETUP_PLACES_MODEL", L"1");
    r = run_tool(s, cmd, build_line, &bs, L"Building the game");
    SetEnvironmentVariableW(L"SLY2_SETUP_PLACES_MODEL", NULL);
    return r;
}

/* ---- sly2.ini: [game] disc ------------------------------------------------------------------- */

/* Sets [game] disc = iso in the game's settings file, so the first start needs no file dialog. An
 * existing file keeps everything else; a new one gets only this key (the game adds the rest). */
static int set_disc_in_ini(setup_t* s) {
    wchar_t ud[MAXP], ini[MAXP];
    user_data_dir(s, ud);
    if (!ud[0]) return 0;
    mkdir_p(ud);
    wpath(ini, MAXP, ud, L"sly2.ini");
    char* iso8 = to_utf8(s->iso);
    char* text = NULL;
    size_t n = 0;
    read_file(ini, &text, &n);
    size_t cap = n + strlen(iso8) + 256;
    char* out = malloc(cap);
    size_t o = 0;
    int in_game = 0, done = 0, saw_game = 0;
    const char* nl = (text && strstr(text, "\r\n")) || !text ? "\r\n" : "\n";
    const char* p = text ? text : "";
    while (*p) {
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p + 1) : strlen(p);
        const char* t = p;
        while (t < p + len && (*t == ' ' || *t == '\t')) t++;
        if (*t == '[') {
            if (in_game && !done) {                  /* [game] without a disc key: add it at its end */
                o += (size_t)snprintf(out + o, cap - o, "disc = %s%s", iso8, nl);
                done = 1;
            }
            in_game = !_strnicmp(t, "[game]", 6);
            saw_game |= in_game;
        } else if (in_game && !done && !_strnicmp(t, "disc", 4)) {
            const char* q = t + 4;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                o += (size_t)snprintf(out + o, cap - o, "disc = %s%s", iso8, nl);
                done = 1;
                p += len;
                continue;
            }
        }
        memcpy(out + o, p, len);
        o += len;
        p += len;
    }
    if (!done) {
        if (o && out[o - 1] != '\n') o += (size_t)snprintf(out + o, cap - o, "%s", nl);
        if (!saw_game) o += (size_t)snprintf(out + o, cap - o, "%s[game]%s", o ? nl : "", nl);
        o += (size_t)snprintf(out + o, cap - o, "disc = %s%s", iso8, nl);
    }
    int ok = write_file(ini, out, o);
    say(s, ok ? L"Disc image set in %ls" : L"Cannot write %ls (the game will ask for the disc image)", ini);
    free(out);
    free(text);
    free(iso8);
    return ok;
}

/* ---- registration ---------------------------------------------------------------------------- */

static void register_uninstall(setup_t* s, uint64_t bytes) {
    HKEY root = reg_root(s);
    wchar_t un[MAXP], v[MAXP * 2], icon[MAXP];
    dpath(s, un, L"Uninstall.exe");
    art_icon_location(s, icon);                      /* <dir>\art\sly2.ico (owner / disc art), else the launcher */
    if (!file_exists(icon)) wcopy(icon, MAXP, un);
    reg_set_str(root, s->regkey, L"DisplayName", APP_NAME L" (PC port)");
    char gv[64];
    stage_version(s->dir, GAME_DIR, gv, sizeof gv);
    char* sp = strchr(gv, ' ');
    if (sp) *sp = 0;
    if (gv[0]) { wfmt(v, MAXP * 2, L"%hs", gv); reg_set_str(root, s->regkey, L"DisplayVersion", v); }
    else reg_set_str(root, s->regkey, L"DisplayVersion", SETUP_VERSION);
    reg_set_str(root, s->regkey, L"Publisher", L"tanihad");
    reg_set_str(root, s->regkey, L"InstallLocation", s->dir);
    wfmt(v, MAXP * 2, L"%ls,0", icon);
    reg_set_str(root, s->regkey, L"DisplayIcon", v);
    wfmt(v, MAXP * 2, L"\"%ls\" /uninstall", un);
    reg_set_str(root, s->regkey, L"UninstallString", v);
    wfmt(v, MAXP * 2, L"\"%ls\" /uninstall /silent", un);
    reg_set_str(root, s->regkey, L"QuietUninstallString", v);
    reg_set_dword(root, s->regkey, L"NoModify", 1);
    reg_set_dword(root, s->regkey, L"NoRepair", 1);
    if (bytes) reg_set_dword(root, s->regkey, L"EstimatedSize", (DWORD)(bytes / 1024));
}

/* sly2.exe in use (the game is running)? Both layouts: <dir>\game\sly2.exe and the old <dir>\sly2.exe. */
int setup_game_running(setup_t* s) {
    static const wchar_t* where[] = { GAME_DIR L"\\sly2.exe", L"sly2.exe" };
    for (int i = 0; i < 2; i++) {
        wchar_t exe[MAXP];
        dpath(s, exe, where[i]);
        if (!file_exists(exe)) continue;
        HANDLE h = CreateFileW(exe, GENERIC_READ | DELETE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_SHARING_VIOLATION) return 1; continue; }
        CloseHandle(h);
    }
    return 0;
}

int setup_is_installed(setup_t* s) {
    wchar_t exe[MAXP], st[MAXP], v[MAXP];
    dpath(s, exe, GAME_DIR L"\\sly2.exe");
    state_path(s, st);
    if (!file_exists(exe) || !file_exists(st)) return 0;
    state_get(s->dir, L"iso", v, MAXP);              /* the image the game plays from (normally data\sly2.img) */
    return v[0] && file_exists(v);
}

void setup_bundle_path(setup_t* s, const char* version, wchar_t* out) {
    char v[64];
    size_t n = 0;
    while (version[n] && version[n] != ' ' && n < sizeof v - 1) { v[n] = version[n]; n++; }
    v[n] = 0;
    wchar_t name[128];
    wfmt(name, 128, BUNDLE_DIR L"\\sly2-source-%hs.zip", v);
    dpath(s, out, name);
}

/* this program without its payload, as dst (Uninstall.exe, the launcher) */
static int write_stub(setup_t* s, const wchar_t* dst) {
    (void)s;
    if (!_wcsicmp(dst, self_path())) return 1;
    HANDLE in = CreateFileW(self_path(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    uint64_t n = payload_stub_size();
    char* buf = malloc((size_t)n);
    DWORD got = 0;
    int ok = buf && ReadFile(in, buf, (DWORD)n, &got, NULL) && got == n;
    CloseHandle(in);
    if (ok) {
        ok = write_file(dst, buf, (size_t)n);
        if (!ok && (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_SHARING_VIOLATION)) {
            /* the old launcher is running: a running exe can be renamed, then replaced */
            wchar_t old[MAXP];
            wfmt(old, MAXP, L"%ls.old", dst);
            DeleteFileW(old);
            if (MoveFileExW(dst, old, MOVEFILE_REPLACE_EXISTING)) {
                ok = write_file(dst, buf, (size_t)n);
                MoveFileExW(old, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                DeleteFileW(old);
            }
        }
    }
    free(buf);
    return ok;
}

int setup_copy_launcher(setup_t* s) {
    wchar_t p[MAXP];
    dpath(s, p, L"Uninstall.exe");
    int ok = write_stub(s, p);
    dpath(s, p, LAUNCHER_EXE);
    return write_stub(s, p) && ok;
}

/* ---- install ----------------------------------------------------------------------------------- */

static int install_sources(setup_t* s) {
    step(s, 0, SETUP_PM_TOOLS, L"Unpacking the PC version's sources");
    wchar_t b[MAXP], z[MAXP], v[MAXP];
    bpath(s, b, NULL);
    mkdir_p(b);
    bpath(s, z, L"source.zip");
    if (s->bundle[0]) {                              /* an update's bundle (verified) or the kept one (repair) */
        say(s, L"Sources: %ls", s->bundle);
        if (!CopyFileW(s->bundle, z, FALSE)) return fail(s, L"Cannot read the game's sources %ls (error %lu).", s->bundle, GetLastError());
    } else {
        const payload_entry* e = payload_find("source.zip");
        if (!e) return fail(s, L"The game's sources are missing: download Setup again and run it.");
        int r0 = from_payload(s, e, z);
        if (r0) return r0;
    }
    static const wchar_t* old[] = { L"recomp", L"runtime", L"scripts", L"requirements.txt", L"version.txt", L"models",
                                    L"THIRD-PARTY-NOTICES.txt" };
    for (size_t i = 0; i < sizeof old / sizeof old[0]; i++) { bpath(s, v, old[i]); delete_tree(v); }
    int r = untar(s, z, b, NULL, 0, L"sources");
    if (r) { DeleteFileW(z); return r; }
    /* the version of these sources: version.txt = "<commit count> <hash>" -> "0.1.<count> <hash>" */
    char* vt = NULL;
    size_t n;
    bpath(s, v, L"version.txt");
    unsigned count = 0;
    char hash[48] = "";
    if (!read_file(v, &vt, &n) || sscanf(vt, "%u %47s", &count, hash) != 2) {
        free(vt);
        DeleteFileW(z);
        return fail(s, L"The game's sources are incomplete (no version.txt).");
    }
    free(vt);
    snprintf(s->built_version, sizeof s->built_version, "0.1.%u %s", count, hash);
    if (s->expect_version[0]) {
        char vv[32];
        snprintf(vv, sizeof vv, "0.1.%u", count);
        if (strcmp(vv, s->expect_version)) {
            DeleteFileW(z);
            return fail(s, L"The update's sources are version %hs, not %hs as announced: not installed.", vv, s->expect_version);
        }
    }
    /* keep the bundle: Repair rebuilds this version later without a download */
    wchar_t keep[MAXP], kd[MAXP];
    setup_bundle_path(s, s->built_version, keep);
    dpath(s, kd, BUNDLE_DIR);
    mkdir_p(kd);
    if (_wcsicmp(keep, s->bundle) && !MoveFileExW(z, keep, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        say(s, L"(cannot keep the sources in %ls: error %lu)", keep, GetLastError());
    DeleteFileW(z);
    /* an update: objects built by another version are not reused (the payload's file times are older
     * than the player's objects, so the build's time check could keep stale ones) */
    char* built = NULL;
    bpath(s, v, L"built-version.txt");
    char want[128];
    snprintf(want, sizeof want, "%s", s->built_version);
    if (read_file(v, &built, &n) && strcmp(built, want)) {
        wchar_t bd[MAXP];
        bpath(s, bd, L"build");
        say(s, L"Sources updated (%hs -> %hs): rebuilding from scratch", built, want);
        delete_tree(bd);
    }
    free(built);
    return EXIT_OK;
}

static int install_game_files(setup_t* s) {
    step(s, SETUP_PM_INSTALL, 1000 - SETUP_PM_INSTALL, L"Installing the game");
    wchar_t dist[MAXP], pat[MAXP], staging[MAXP];
    bpath(s, dist, L"dist");
    wpath(pat, MAXP, dist, L"*");
    /* staged: the new game goes into game.new and replaces game in one rename; until then the installed
     * version stays untouched (a failed or cancelled update leaves it playable) */
    if (!stage_begin(s->dir, staging, MAXP)) return fail(s, L"Cannot prepare %ls\\game.new (error %lu).", s->dir, GetLastError());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return fail(s, L"The build produced no game folder.");
    int ok = 1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!_wcsicmp(fd.cFileName, MODEL_FILE)) continue;      /* (the 1.0.x launchers' place: this one uses models\) */
        wchar_t a[MAXP], b[MAXP];
        wpath(a, MAXP, dist, fd.cFileName);
        wpath(b, MAXP, staging, fd.cFileName);
        if (!CopyFileW(a, b, FALSE)) { fail(s, L"Cannot copy %ls (error %lu: is the disk full?).", b, GetLastError()); ok = 0; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!ok) return EXIT_FAILED;
    {                                                /* dist\art: pictures made from the player's disc (optional) */
        wchar_t ap[MAXP], sa[MAXP];
        wpath(ap, MAXP, dist, L"art\\*");
        wpath(sa, MAXP, staging, L"art");
        h = FindFirstFileW(ap, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            mkdir_p(sa);
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t a[MAXP], b[MAXP];
                wpath(a, MAXP, dist, L"art");
                wpath(a, MAXP, a, fd.cFileName);
                wpath(b, MAXP, sa, fd.cFileName);
                if (!CopyFileW(a, b, FALSE)) say(s, L"(cannot copy %ls: error %lu; the launcher uses its own art)", a, GetLastError());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    {                                                /* the AI model (Look = Reimagined) from the verified sources */
        wchar_t md[MAXP];
        char why[256] = "";
        bpath(s, md, L"models");
        int m = stage_place_model(md, staging, why, sizeof why);
        if (m < 0) return fail(s, L"Installing the AI model failed: %hs. Click Retry.", why);
        if (m > 0) say(s, L"AI model: %ls\\models\\" MODEL_FILE L" (SHA-256 checked)", staging);
        else say(s, L"AI model: not in these sources");
    }
    if (!stage_mark_complete(s->dir, s->built_version)) return fail(s, L"Cannot write into %ls (error %lu).", staging, GetLastError());
    for (int tries = 0;; tries++) {
        unsigned long err;
        if (stage_commit(s->dir, &err)) break;
        if (tries < 20 && (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION)) { Sleep(250); continue; }   /* a scanner */
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION)
            return fail(s, L"The game is in use: close Sly 2 and click Retry.");
        return fail(s, L"Cannot replace the game folder (error %lu).", err);
    }
    say(s, L"Installed version %hs.", s->built_version);
    {                                                /* (the unpacked model: game\models has it, the kept bundle too) */
        wchar_t md[MAXP];
        bpath(s, md, L"models");
        delete_tree(md);
    }
    /* the old installer's layout (the game directly in <dir>): replaced by <dir>\game */
    static const wchar_t* old_files[] = { L"sly2.exe", L"README.txt", L"Controller test.cmd" };
    for (size_t i = 0; i < sizeof old_files / sizeof old_files[0]; i++) {
        wchar_t p[MAXP];
        dpath(s, p, old_files[i]);
        if (file_exists(p)) { DeleteFileW(p); say(s, L"Removed the old %ls (the game is in %ls\\game now)", old_files[i], s->dir); }
    }
    /* keep only the bundles of the current and the previous game */
    char cur[64], prev[64];
    stage_version(s->dir, GAME_DIR, cur, sizeof cur);
    stage_version(s->dir, GAME_DIR L".old", prev, sizeof prev);
    wchar_t keep1[MAXP], keep2[MAXP] = L"", bpat[MAXP];
    setup_bundle_path(s, cur, keep1);
    if (prev[0]) setup_bundle_path(s, prev, keep2);
    dpath(s, bpat, BUNDLE_DIR L"\\sly2-source-*.zip");
    h = FindFirstFileW(bpat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[MAXP];
            dpath(s, p, BUNDLE_DIR);
            wpath(p, MAXP, p, fd.cFileName);
            if (_wcsicmp(p, keep1) && _wcsicmp(p, keep2)) DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wchar_t v[MAXP];
    bpath(s, v, L"built-version.txt");
    write_file(v, s->built_version, strlen(s->built_version));
    return EXIT_OK;
}

/* ---- the disc image ---------------------------------------------------------------------------- */

typedef struct { setup_t* s; uint64_t total; } copy_ctx;

static DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER sz, LARGE_INTEGER sd, DWORD stream,
                                    DWORD reason, HANDLE a, HANDLE b, LPVOID user) {
    (void)sz; (void)sd; (void)stream; (void)reason; (void)a; (void)b;
    copy_ctx* c = user;
    static DWORD last;
    if (GetTickCount() - last > 100 || done.QuadPart == total.QuadPart) {
        last = GetTickCount();
        setup_bytes(c->s, L"Copying your disc image", (uint64_t)done.QuadPart, (uint64_t)total.QuadPart);
    }
    return c->s->cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

/* The player's image becomes <dir>\data\sly2.img: the game reads only this copy afterwards, so the original
 * (on a USB drive, in Downloads, ...) is not needed any more. A whole-image copy: the game reads its data
 * by sector number from its own table of contents, not through the ISO 9660 file names (the file system
 * lists only 34 MB of the 2.9 GB), so nothing smaller would keep every sector where the game expects it. */
int setup_import_image(setup_t* s) {
    wchar_t img[MAXP], part[MAXP], dd[MAXP], why[512];
    dpath(s, img, IMAGE_REL);
    wfmt(part, MAXP, L"%ls.part", img);
    dpath(s, dd, L"data");
    if (s->image_src[0] && !_wcsicmp(s->image_src, img)) s->image_src[0] = 0;   /* (it is the copy) */
    if (!s->image_src[0]) {
        if (file_exists(img) && disc_check_iso(img, why, 512) == DISC_OK) { wcopy(s->iso, MAXP, img); return EXIT_OK; }
        return fail(s, L"The game's copy of your disc image is missing or damaged. Drop your Sly 2 disc image (.iso) on "
                       L"this window to import it again.");
    }
    step(s, 0, 1000, L"Copying your disc image");
    if (disc_check_iso(s->image_src, why, 512) != DISC_OK) { fail(s, L"%ls", why); return EXIT_BAD_DISC; }
    if (!mkdir_p(dd)) return fail(s, L"Cannot create %ls (error %lu).", dd, GetLastError());
    copy_ctx c = { s, file_size(s->image_src) };
    say(s, L"Copying %ls to %ls", s->image_src, img);
    BOOL cancel = FALSE;
    if (!CopyFileExW(s->image_src, part, copy_progress, &c, &cancel, 0)) {
        DWORD e = GetLastError();
        DeleteFileW(part);
        if (s->cancel || e == ERROR_REQUEST_ABORTED) return EXIT_CANCELLED;
        if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) return fail(s, L"The disk is full: free up some space and click Retry.");
        return fail(s, L"Cannot copy your disc image (error %lu). Is the drive it is on still connected?", e);
    }
    if (disc_check_iso(part, why, 512) != DISC_OK || file_size(part) != c.total) {
        DeleteFileW(part);
        return fail(s, L"The copy of your disc image is damaged (%ls). Click Retry to copy it again.", why);
    }
    if (!MoveFileExW(part, img, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        DeleteFileW(part);
        return fail(s, L"Cannot write %ls (error %lu).", img, e);
    }
    say(s, L"Disc image imported: %ls", img);
    wcopy(s->iso, MAXP, img);
    s->image_src[0] = 0;
    return EXIT_OK;
}

int setup_rollback(setup_t* s) {
    if (game_running(s)) return fail(s, L"Sly 2 is running: close the game first.");
    unsigned long err;
    if (!stage_rollback(s->dir, &err)) return fail(s, L"Cannot switch versions (error %lu).", err);
    char v[64];
    stage_version(s->dir, GAME_DIR, v, sizeof v);
    say(s, L"Switched to version %hs.", v);
    register_uninstall(s, 0);
    return EXIT_OK;
}

int setup_install(setup_t* s) {
    s->error[0] = 0;
    InterlockedExchange(&s->cancel, 0);
    DWORD err;
    if (!dir_writable(s->dir, &err)) {
        if (err == ERROR_ACCESS_DENIED && !is_elevated()) {
            wfmt(s->error, 2048, L"Setup cannot write to %ls: this folder needs administrator rights.", s->dir);
            return EXIT_NEEDS_ADMIN;
        }
        return fail(s, L"Setup cannot write to %ls (error %lu).", s->dir, err);
    }
    wchar_t lp[MAXP];
    dpath(s, lp, L"setup.log");
    if (!s->log_fixed) log_open(lp);
    loga("---- Sly 2 Setup %ls (%ls), %s", SETUP_VERSION, SETUP_COMMIT, s->existing ? "update" : "new installation");
    logw(L"folder %ls, disc %ls, from '%ls', mirror '%ls', offline %d, all users %d", s->dir, s->iso, s->from, s->mirror,
         s->offline_only, s->allusers);
    if (payload_count() > 1) loga("offline setup: %d payload entries", payload_count());

    /* the disc image: the imported copy (normal), or with /noimport the player's file itself */
    wchar_t why[512], img[MAXP];
    dpath(s, img, IMAGE_REL);
    int importing = 0;
    uint64_t need_image = 0;
    if (s->no_import) {
        if (!s->image_src[0]) wcopy(s->image_src, MAXP, s->iso);
        wcopy(s->iso, MAXP, s->image_src);
        if (disc_check_iso(s->iso, why, 512) != DISC_OK) { fail(s, L"%ls: %ls", s->iso, why); return EXIT_BAD_DISC; }
    } else {
        if (!s->image_src[0] && s->iso[0] && _wcsicmp(s->iso, img)) wcopy(s->image_src, MAXP, s->iso);
        if (s->image_src[0] && !_wcsicmp(s->image_src, img)) s->image_src[0] = 0;
        if (s->image_src[0]) {
            if (disc_check_iso(s->image_src, why, 512) != DISC_OK) { fail(s, L"%ls: %ls", s->image_src, why); return EXIT_BAD_DISC; }
            importing = 1;
            need_image = file_size(s->image_src) + (64ull << 20);
            uint64_t have_img = file_size(img);
            if (have_img != (uint64_t)-1) need_image = need_image > have_img ? need_image - have_img : 0;
        } else if (!file_exists(img) || disc_check_iso(img, why, 512) != DISC_OK) {
            fail(s, L"The game's copy of your disc image is missing or damaged. Drop your Sly 2 disc image (.iso) on the "
                    L"window to import it again.");
            return EXIT_BAD_DISC;
        }
    }
    uint64_t need = setup_space_needed(s) + need_image, have = disk_free(s->dir);
    if (have < need) {
        wchar_t a[32], b[32];
        fmt_size(a, 32, need);
        fmt_size(b, 32, have);
        fail(s, L"Not enough free disk space: %ls is needed on this drive (%ls%ls), %ls is free. Free up some space and "
                L"click Retry.", a, importing ? L"the copy of your disc image, then " : L"", L"building the game", b);
        return EXIT_NO_SPACE;
    }
    if (game_running(s)) return fail(s, L"Sly 2 is running: close the game and click Retry.");

    /* registered first, so a cancelled or failed installation can be removed in Settings > Apps */
    if (!setup_copy_launcher(s)) return fail(s, L"Cannot write the launcher into %ls (error %lu).", s->dir, GetLastError());
    state_set(s, L"version", SETUP_VERSION);
    state_set(s, L"dir", s->dir);
    state_set(s, L"allusers", s->allusers ? L"1" : L"0");
    state_set(s, L"regkey", s->regkey);
    if (s->userdata[0]) state_set(s, L"userdata", s->userdata);
    register_uninstall(s, 0);

    int r = EXIT_OK;
    int pm_off = s->pm_off, pm_scale = s->pm_scale ? s->pm_scale : 1000;
    /* the sources first (a second; a damaged setup file or bundle is found before the 3 GB copy) */
    s->pm_scale = 1;
    r = install_sources(s);
    if (!r && importing) {                           /* the copy takes the first 30% of the bar */
        s->pm_off = pm_off;
        s->pm_scale = pm_scale * 3 / 10;
        r = setup_import_image(s);
    } else if (!r && !s->no_import) wcopy(s->iso, MAXP, img);
    s->pm_off = pm_off + (importing ? pm_scale * 3 / 10 : 0);
    s->pm_scale = pm_scale - (importing ? pm_scale * 3 / 10 : 0);
    if (!r) state_set(s, L"iso", s->iso);
    for (int i = 0; !r && i < SETUP_NPINS; i++) {
        int base = SETUP_PM_TOOLS + (SETUP_PM_PYTHON - SETUP_PM_TOOLS) * SETUP_PINS[i].pm_from / 1000;
        int end = SETUP_PM_TOOLS + (SETUP_PM_PYTHON - SETUP_PM_TOOLS) * SETUP_PINS[i].pm_to / 1000;
        r = install_pin(s, &SETUP_PINS[i], base, end - base);
    }
    if (!r) r = install_python(s, SETUP_PM_PYTHON, SETUP_PM_PACKAGES - SETUP_PM_PYTHON);
    if (!r) r = install_packages(s, SETUP_PM_PACKAGES, SETUP_PM_ELF - SETUP_PM_PACKAGES);
    if (!r) r = build_game(s);
    if (!r) r = install_game_files(s);
    s->pm_off = pm_off;
    s->pm_scale = pm_scale == 1000 ? 0 : pm_scale;
    if (r) {
        if (s->cancel || r == EXIT_CANCELLED) { say(s, L"Cancelled."); return EXIT_CANCELLED; }
        if (!s->error[0]) wfmt(s->error, 2048, L"Setup failed. The details are in %ls.", log_path());
        return r;
    }

    set_disc_in_ini(s);
    wchar_t start[MAXP], desk[MAXP], exe[MAXP], icon[MAXP];
    shortcut_paths(s, start, desk);
    dpath(s, exe, LAUNCHER_EXE);
    const wchar_t* art = L"";
    art_icon_refresh(s, &art);                       /* the owner's art, else the disc's, else the launcher's own icon */
    art_icon_location(s, icon);
    say(s, L"Game icon: %ls", art[0] ? art : L"the launcher's own");
    if (make_shortcut(start, exe, NULL, s->dir, L"Play Sly 2: Band of Thieves", icon)) state_set(s, L"startmenu_lnk", start);
    /* an existing desktop shortcut (also the old installer's, which started sly2.exe) now opens the launcher */
    wchar_t dl[MAXP];
    state_get(s->dir, L"desktop_lnk", dl, MAXP);
    if (dl[0] && file_exists(dl)) make_shortcut(dl, exe, NULL, s->dir, L"Play Sly 2: Band of Thieves", icon);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);   /* (Explorer reloads shortcut icons) */
    {
        wchar_t gv[64];
        wfmt(gv, 64, L"%hs", s->built_version);
        state_set(s, L"game_version", gv);
    }
    register_uninstall(s, tree_size(s->dir));

    wchar_t p[MAXP];
    static const wchar_t* build_parts[] = { L"build", L"orig", L"dist" };
    s->used_build = 0;
    for (int i = 0; i < 3; i++) { bpath(s, p, build_parts[i]); s->used_build += tree_size(p); }
    bpath(s, p, NULL);
    s->used_tools = tree_size(p) - s->used_build;
    wchar_t a[32], b[32];
    fmt_size(a, 32, s->used_build);
    fmt_size(b, 32, s->used_tools);
    say(s, L"Installed. Build files %ls, build tools %ls.", a, b);
    step(s, 1000, 0, L"Sly 2: Band of Thieves is installed.");
    return EXIT_OK;
}

/* Starts <dir>\game\sly2.exe with the game's disc image as its argument (the runtime's "sly2.exe x.iso"
 * start; sly2.ini's [game] disc names the same file for a start without the launcher). */
int setup_launch_game(setup_t* s) {
    wchar_t exe[MAXP], gd[MAXP], img[MAXP], args[MAXP + 4];
    dpath(s, gd, GAME_DIR);
    wpath(exe, MAXP, gd, L"sly2.exe");
    state_get(s->dir, L"iso", img, MAXP);
    if (!img[0] || !file_exists(img)) dpath(s, img, IMAGE_REL);
    wfmt(args, MAXP + 4, L"\"%ls\"", img);
    HINSTANCE r;
    if (is_elevated()) {                             /* never start the game as administrator (sly2.ini names the image) */
        wfmt(args, MAXP + 4, L"\"%ls\"", exe);
        r = ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    } else r = ShellExecuteW(NULL, L"open", exe, file_exists(img) ? args : NULL, gd, SW_SHOWNORMAL);
    logw(L"start %ls %ls: %ls", exe, args, (INT_PTR)r > 32 ? L"ok" : L"failed");
    return (INT_PTR)r > 32;
}

/* The owner's art may change between runs (%LOCALAPPDATA%\Sly2\art): the launcher calls this at every start.
 * When the game's icon changed, the shortcuts this setup made and the Apps & features entry show the new one. */
int setup_refresh_icon(setup_t* s) {
    const wchar_t* art = L"";
    if (!art_icon_refresh(s, &art)) return 0;
    wchar_t exe[MAXP], icon[MAXP], v[MAXP];
    dpath(s, exe, LAUNCHER_EXE);
    art_icon_location(s, icon);
    static const wchar_t* links[] = { L"startmenu_lnk", L"desktop_lnk" };
    for (int i = 0; i < 2; i++) {
        state_get(s->dir, links[i], v, MAXP);
        if (v[0] && file_exists(v)) make_shortcut(v, exe, NULL, s->dir, L"Play Sly 2: Band of Thieves", icon);
    }
    wchar_t dn[MAXP];
    if (reg_get_str(reg_root(s), s->regkey, L"DisplayName", dn, MAXP)) {   /* (only an existing entry) */
        wfmt(v, MAXP, L"%ls,0", icon);
        reg_set_str(reg_root(s), s->regkey, L"DisplayIcon", v);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    logw(L"Game icon now: %ls", art[0] ? art : L"the launcher's own");
    return 1;
}

int setup_finish(setup_t* s) {
    wchar_t start[MAXP], desk[MAXP], exe[MAXP], p[MAXP], icon[MAXP];
    shortcut_paths(s, start, desk);
    dpath(s, exe, LAUNCHER_EXE);
    art_icon_location(s, icon);
    if (s->desktop_shortcut) {
        if (make_shortcut(desk, exe, NULL, s->dir, L"Play Sly 2: Band of Thieves", icon)) state_set(s, L"desktop_lnk", desk);
    }
    if (s->keep_build) s->keep_tools = 1;           /* (the intermediates are useless without the tools) */
    if (!s->keep_build) {
        static const wchar_t* parts[] = { L"build", L"orig", L"dist", L"tools\\_downloads" };
        for (int i = 0; i < 4; i++) { bpath(s, p, parts[i]); delete_tree(p); }
        say(s, L"Removed the build files.");
    }
    if (!s->keep_tools) {
        bpath(s, p, NULL);
        delete_tree(p);
        say(s, L"Removed the build tools.");
    }
    state_set(s, L"keep_tools", s->keep_tools ? L"1" : L"0");
    register_uninstall(s, tree_size(s->dir));
    wchar_t a[32];
    fmt_size(a, 32, tree_size(s->dir));
    say(s, L"Finished: %ls uses %ls.", s->dir, a);
    if (s->play) setup_launch_game(s);
    return EXIT_OK;
}

/* ---- uninstall ------------------------------------------------------------------------------- */

int setup_uninstall(setup_t* s) {
    wchar_t v[MAXP], p[MAXP];
    state_get(s->dir, L"version", v, MAXP);
    wchar_t st[MAXP];
    state_path(s, st);
    if (!file_exists(st)) {
        setup_message(s, L"No Sly 2 installation was found in this folder.", MB_ICONERROR);
        return EXIT_FAILED;
    }
    state_get(s->dir, L"allusers", v, MAXP);
    s->allusers = v[0] == L'1';
    state_get(s->dir, L"regkey", v, MAXP);
    if (v[0]) wcopy(s->regkey, MAXP, v);
    state_get(s->dir, L"userdata", v, MAXP);
    if (v[0] && !s->userdata[0]) wcopy(s->userdata, MAXP, v);
    if (s->allusers && !is_elevated()) {
        wfmt(s->error, 2048, L"Removing an installation for all users needs administrator rights.");
        return EXIT_NEEDS_ADMIN;
    }
    if (setup_ask(s, L"Remove Sly 2: Band of Thieves from this PC?\n\nYour saves and settings are kept unless you choose "
                     L"to delete them in the next step.", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return EXIT_CANCELLED;
    while (game_running(s)) {
        if (setup_ask(s, L"Sly 2 is running. Close the game, then click Retry.", MB_RETRYCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDRETRY)
            return EXIT_CANCELLED;
    }
    if (s->wait_pid) {                               /* the launcher closes itself after starting the uninstall */
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, s->wait_pid);
        if (ph) { WaitForSingleObject(ph, 10000); CloseHandle(ph); }
    }
    wchar_t tmp[MAXP];
    GetTempPathW(MAXP, tmp);
    wcsncat(tmp, L"sly2-uninstall.log", MAXP - wcslen(tmp) - 1);
    log_open(tmp);
    logw(L"---- uninstall %ls", s->dir);

    static const wchar_t* links[] = { L"startmenu_lnk", L"desktop_lnk" };
    for (int i = 0; i < 2; i++) {
        state_get(s->dir, links[i], v, MAXP);
        if (v[0] && file_exists(v)) { DeleteFileW(v); logw(L"removed %ls", v); }
    }
    static const wchar_t* files[] = { L"builder", L"sly2.exe", L"README.txt", L"Controller test.cmd", L"setup.log",
                                      GAME_DIR, GAME_DIR L".new", GAME_DIR L".old", GAME_DIR L".swap", L"data", BUNDLE_DIR,
                                      UPDATES_DIR, LAUNCHER_EXE, LAUNCHER_EXE L".old", L"art" };
    int ok = 1;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        dpath(s, p, files[i]);
        if (!delete_tree(p)) { ok = 0; logw(L"cannot remove %ls", p); }
    }
    reg_delete_tree(reg_root(s), s->regkey);
    logw(L"removed the registry entry %ls", s->regkey);

    wchar_t ud[MAXP];
    user_data_dir(s, ud);
    int portable = !_wcsicmp(ud, s->dir);
    int del = s->delete_saves;
    if (del < 0 && dir_exists(ud) && !portable) {
        wchar_t q[MAXP + 256];
        wfmt(q, MAXP + 256, L"Also delete your saves and settings?\n\n%ls\n\nChoose No to keep them (for example to play "
                            L"again later).", ud);
        del = setup_ask(s, q, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    }
    if (del > 0) {
        if (portable) {
            static const wchar_t* pf[] = { L"memcard", L"sly2.ini", L"sly2.log", L"sly2.previous.log", L"portable.txt" };
            for (size_t i = 0; i < sizeof pf / sizeof pf[0]; i++) { dpath(s, p, pf[i]); delete_tree(p); }
        } else delete_tree(ud);
        logw(L"removed the saves and settings in %ls", ud);
    } else logw(L"kept the saves and settings in %ls", ud);

    state_path(s, p);
    DeleteFileW(p);
    dpath(s, p, L"Uninstall.exe");
    if (!_wcsicmp(p, self_path())) {
        /* the running Uninstall.exe and the folder go when this process has ended */
        wchar_t cmd[MAXP * 3], sys[MAXP];
        GetSystemDirectoryW(sys, MAXP);
        wfmt(cmd, MAXP * 3, L"\"%ls\\cmd.exe\" /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"%ls\" & rmdir \"%ls\"", sys, p, s->dir);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        GetTempPathW(MAXP, sys);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    } else {
        DeleteFileW(p);
        RemoveDirectoryW(s->dir);                    /* only if empty: the player's own files stay */
    }
    logw(L"uninstall %ls", ok ? L"complete" : L"incomplete (see above)");
    if (!ok) { wfmt(s->error, 2048, L"Some files could not be removed (see %ls).", tmp); return EXIT_FAILED; }
    return EXIT_OK;
}
