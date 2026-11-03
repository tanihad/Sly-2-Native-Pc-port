/* Sly 2 launcher / setup: command line (setup.h). The window is launcher.c.
 *
 *   Sly2-Setup.exe, "Sly 2 Launcher.exe"   the launcher window (first run: drop the disc image, install)
 *   ... /silent /iso=<image> [/dir=<folder>] [options]                  install without a window (CI, tests)
 *   ... /silent /repair                                                  rebuild the installed version
 *   ... /silent /update                                                  check, download, verify, rebuild, swap
 *   ... /rollback                                                        switch to the previous version
 *   Uninstall.exe [/uninstall] [/silent] [/deletesaves]                  remove the game
 *
 * Options: /dir= /iso= /noimport (play from the image itself; no copy) /from=<folder with the toolchain>
 * /mirror=<URL or folder to download the toolchain from> /offline /nodesktop /keeptools /keepbuild /play
 * /deletesaves /updateurl=<release manifest URL> /noupdatecheck
 * Test overrides (nothing of the player's is touched): /startmenu=<folder> /desktopdir=<folder>
 * /userdata=<the game's user data folder> /regkey=<HKCU subkey for the uninstall entry> /log=<file>
 * test builds (-DSLY2_TEST_BUILD) only: /pubkey=<hex> (another release key; plain HTTP to 127.0.0.1 allowed)
 * Tools: /version, /payload, /sha256 <file>, /checkdisc <image>, /download <url> <file> [sha256 [size]],
 * /checkupdate, /uitest (the window driven without showing it), /uishot=<folder> (rendered off-screen).
 */
#include "setup.h"

#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int launcher_run(setup_t* s);
int launcher_uitest(setup_t* s, const wchar_t* bad_iso, int install, int cancel_after);
int launcher_shots(setup_t* s, const wchar_t* folder);

static setup_t S;

/* ---- console output for the tool commands (a GUI program: stdout only when redirected) -------- */

static void out(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 4095, fmt, ap);
    buf[4095] = 0;
    va_end(ap);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        static int attached;
        if (!attached) attached = AttachConsole(ATTACH_PARENT_PROCESS) ? 1 : -1;
        h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE) return;
    }
    char* u = to_utf8(buf);
    DWORD w;
    WriteFile(h, u, (DWORD)strlen(u), &w, NULL);
    WriteFile(h, "\r\n", 2, &w, NULL);
    free(u);
    logw(L"%ls", buf);
}

/* ---- command line ------------------------------------------------------------------------------- */

static int opt(const wchar_t* a, const wchar_t* name, wchar_t* value) {
    size_t n = wcslen(name);
    if ((a[0] != L'/' && a[0] != L'-') || _wcsnicmp(a + 1, name, n)) return 0;
    if (value) {
        if (a[1 + n] != L'=' && a[1 + n] != L':') return 0;
        wcopy(value, MAXP, a + 2 + n);
        return 1;
    }
    return a[1 + n] == 0;
}

static void full_path(wchar_t* p) {
    if (!p[0]) return;
    wchar_t f[MAXP];
    if (GetFullPathNameW(p, MAXP, f, NULL)) wcopy(p, MAXP, f);
    size_t n = wcslen(p);
    while (n > 3 && (p[n - 1] == L'\\' || p[n - 1] == L'/')) p[--n] = 0;
}

/* settings an installation remembers (setup-state.ini), unless given on the command line */
static void load_state(setup_t* s, int regkey_given) {
    wchar_t v[MAXP];
    setup_state_get(s->dir, L"iso", v, MAXP);
    if (v[0] && file_exists(v) && !s->iso[0]) wcopy(s->iso, MAXP, v);
    setup_state_get(s->dir, L"keep_tools", v, MAXP);
    if (v[0] == L'1') s->keep_tools = 1;
    setup_state_get(s->dir, L"allusers", v, MAXP);
    if (v[0] == L'1') s->allusers = 1;
    setup_state_get(s->dir, L"regkey", v, MAXP);
    if (v[0] && !regkey_given) wcopy(s->regkey, MAXP, v);
    setup_state_get(s->dir, L"userdata", v, MAXP);
    if (v[0] && !s->userdata[0]) wcopy(s->userdata, MAXP, v);
    setup_state_get(s->dir, L"update_url", v, MAXP);
    if (v[0] && !s->update_url[0]) wcopy(s->update_url, MAXP, v);
}

static void installed_version(setup_t* s, char* out_v, size_t cap) {
    char v[64];
    stage_version(s->dir, GAME_DIR, v, sizeof v);
    char* sp = strchr(v, ' ');
    if (sp) *sp = 0;
    snprintf(out_v, cap, "%s", v);
}

/* ---- tool commands ---------------------------------------------------------------------------- */

static int tool_command(int argc, wchar_t** argv) {
    const wchar_t* c = argv[1];
    if (opt(c, L"version", NULL)) {
        out(L"Sly2-Setup %ls (%ls)%ls", SETUP_VERSION, SETUP_COMMIT, update_allow_loopback_http() ? L" test build" : L"");
        return EXIT_OK;
    }
    if (opt(c, L"payload", NULL)) {
        out(L"program %llu bytes, %d payload entries", (unsigned long long)payload_stub_size(), payload_count());
        for (int i = 0; i < payload_count(); i++) {
            const payload_entry* e = payload_at(i);
            wchar_t hx[65];
            sha256_hex(e->sha256, hx);
            out(L"  %hs %llu %ls", e->name, (unsigned long long)e->size, hx);
        }
        return EXIT_OK;
    }
    if (opt(c, L"sha256", NULL) && argc > 2) {
        uint8_t d[32];
        wchar_t hx[65];
        if (!sha256_file(&S, argv[2], d)) { out(L"cannot read %ls", argv[2]); return EXIT_FAILED; }
        sha256_hex(d, hx);
        out(L"%ls", hx);
        return EXIT_OK;
    }
    if (opt(c, L"checkdisc", NULL) && argc > 2) {
        wchar_t why[512];
        int r = disc_check_iso(argv[2], why, 512);
        out(L"%d %ls", r, why);
        return r == DISC_OK ? EXIT_OK : EXIT_BAD_DISC;
    }
    if (opt(c, L"download", NULL) && argc > 3) {     /* /download <url> <file> [sha256 [size]] */
        wchar_t part[MAXP];
        wfmt(part, MAXP, L"%ls.part", argv[3]);
        uint64_t size = argc > 5 ? _wcstoui64(argv[5], NULL, 10) : 0;
        const wchar_t* leafname = wcsrchr(argv[3], L'\\');
        int r = net_download(&S, argv[2], part, size, leafname ? leafname + 1 : argv[3], 5);
        if (r) { out(L"FAILED: %ls", S.error); return r == -2 ? EXIT_CANCELLED : EXIT_FAILED; }
        if (argc > 4) {
            uint8_t d[32];
            char hex[80];
            WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, hex, sizeof hex, NULL, NULL);
            if (!sha256_file(&S, part, d) || !sha256_equal_hex(d, hex)) { out(L"FAILED: SHA-256 mismatch"); DeleteFileW(part); return EXIT_FAILED; }
        }
        if (!MoveFileExW(part, argv[3], MOVEFILE_REPLACE_EXISTING)) { out(L"FAILED: cannot write %ls", argv[3]); return EXIT_FAILED; }
        out(L"OK %llu bytes", (unsigned long long)file_size(argv[3]));
        return EXIT_OK;
    }
    return -1;
}

/* /update without a window: check, download + verify, staged build, swap */
static int silent_update(void) {
    char installed[64];
    installed_version(&S, installed, sizeof installed);
    update_info u;
    if (!update_check(&S, installed, &u)) { out(L"update check failed: %ls", S.error); return EXIT_FAILED; }
    out(L"installed %hs, latest %hs%ls", installed[0] ? installed : "-", u.m.version, u.newer ? L": updating" : L": nothing to do");
    if (!u.newer) return EXIT_OK;
    if (u.launcher_too_old) { out(L"version %hs needs a newer launcher (%hs): download the new Setup", u.m.version, u.m.min_launcher); return EXIT_FAILED; }
    int r = update_download(&S, &u);
    if (r) { out(L"download failed: %ls", S.error); return r; }
    snprintf(S.expect_version, sizeof S.expect_version, "%s", u.m.version);
    wchar_t dl[MAXP];
    wcopy(dl, MAXP, S.bundle);
    r = setup_install(&S);
    DeleteFileW(dl);                                 /* (a copy is kept in bundle\) */
    if (r) { out(L"update failed (%d): %ls", r, S.error); return r; }
    S.desktop_shortcut = 0;
    S.play = 0;
    setup_finish(&S);
    installed_version(&S, installed, sizeof installed);
    out(L"updated to %hs", installed);
    return EXIT_OK;
}

/* ---- entry ------------------------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)hi;
    (void)prev;
    (void)cmdline;
    (void)show;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    setup_defaults(&S);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t v[MAXP], logfile[MAXP] = L"", bad_iso[MAXP] = L"", shots[MAXP] = L"";
    int silent = 0, uninstall = 0, uitest = 0, ui_install = 0, ui_cancel_after = 0, regkey_given = 0;
    int do_repair = 0, do_update = 0, do_rollback = 0, check_only = 0, no_check = 0, keeptools_given = 0;
    if (argc > 1) {
        int r = tool_command(argc, argv);
        if (r >= 0) return r;
    }
    wchar_t self_dir[MAXP], st[MAXP];
    wcopy(self_dir, MAXP, self_path());
    *wcsrchr(self_dir, L'\\') = 0;
    const wchar_t* leaf = wcsrchr(self_path(), L'\\') + 1;
    if (!_wcsicmp(leaf, L"Uninstall.exe")) {         /* Uninstall.exe in the install folder: uninstall mode */
        uninstall = 1;
        wcopy(S.dir, MAXP, self_dir);
    }
    int dir_given = 0;
    for (int i = 1; i < argc; i++) {
        const wchar_t* a = argv[i];
        if (opt(a, L"silent", NULL) || opt(a, L"quiet", NULL) || opt(a, L"S", NULL)) silent = 1;
        else if (opt(a, L"uninstall", NULL)) uninstall = 1;
        else if (opt(a, L"dir", v)) { wcopy(S.dir, MAXP, v); dir_given = 1; }
        else if (opt(a, L"iso", v)) wcopy(S.image_src, MAXP, v);
        else if (opt(a, L"noimport", NULL)) S.no_import = 1;
        else if (opt(a, L"from", v)) wcopy(S.from, MAXP, v);
        else if (opt(a, L"mirror", v)) wcopy(S.mirror, MAXP, v);
        else if (opt(a, L"startmenu", v)) wcopy(S.startmenu, MAXP, v);
        else if (opt(a, L"desktopdir", v)) wcopy(S.desktop, MAXP, v);
        else if (opt(a, L"userdata", v)) wcopy(S.userdata, MAXP, v);
        else if (opt(a, L"regkey", v)) { wcopy(S.regkey, MAXP, v); regkey_given = 1; }
        else if (opt(a, L"log", v)) wcopy(logfile, MAXP, v);
        else if (opt(a, L"offline", NULL)) S.offline_only = 1;
        else if (opt(a, L"nodesktop", NULL)) S.desktop_shortcut = 0;
        else if (opt(a, L"keeptools", NULL)) { S.keep_tools = 1; keeptools_given = 1; }
        else if (opt(a, L"keepbuild", NULL)) S.keep_build = 1;
        else if (opt(a, L"play", NULL)) S.play = 2;
        else if (opt(a, L"deletesaves", NULL)) S.delete_saves = 1;
        else if (opt(a, L"keepsaves", NULL)) S.delete_saves = 0;
        else if (opt(a, L"allusers", NULL)) S.allusers = 1;
        else if (opt(a, L"repair", NULL)) do_repair = 1;
        else if (opt(a, L"update", NULL)) do_update = 1;
        else if (opt(a, L"rollback", NULL)) do_rollback = 1;
        else if (opt(a, L"checkupdate", NULL)) check_only = 1;
        else if (opt(a, L"noupdatecheck", NULL)) no_check = 1;
        else if (opt(a, L"updateurl", v)) wcopy(S.update_url, MAXP, v);
        else if (opt(a, L"pubkey", v)) { char h[80]; WideCharToMultiByte(CP_UTF8, 0, v, -1, h, sizeof h, NULL, NULL); update_set_test_key(h); }
        else if (opt(a, L"waitpid", v)) S.wait_pid = (DWORD)_wtoi(v);
        else if (opt(a, L"page", v)) { /* (the old wizard's restart option) */ }
        else if (opt(a, L"uitest", NULL)) uitest = 1;
        else if (opt(a, L"badiso", v)) wcopy(bad_iso, MAXP, v);
        else if (opt(a, L"uiinstall", NULL)) ui_install = 1;
        else if (opt(a, L"uicancel", v)) ui_cancel_after = _wtoi(v);
        else if (opt(a, L"uishot", v)) { wcopy(shots, MAXP, v); full_path(shots); }
        else {
            wchar_t m[MAXP + 64];
            wfmt(m, MAXP + 64, L"Unknown option: %ls", a);
            if (!silent) MessageBoxW(NULL, m, L"Sly 2", MB_ICONERROR);
            return EXIT_USAGE;
        }
    }
    (void)keeptools_given;
    if (logfile[0]) { log_open(logfile); S.log_fixed = 1; }
    S.silent = silent;
    S.uitest = uitest;
    full_path(S.dir);
    full_path(S.image_src);
    full_path(S.from);
    full_path(S.startmenu);
    full_path(S.desktop);
    full_path(S.userdata);
    full_path(bad_iso);
    if (S.mirror[0] && !wcsstr(S.mirror, L"://")) full_path(S.mirror);
    if (S.from[0] && !dir_exists(S.from)) {
        wchar_t m[MAXP + 64];
        wfmt(m, MAXP + 64, L"/from: folder not found: %ls", S.from);
        setup_message(&S, m, MB_ICONERROR);
        return EXIT_USAGE;
    }

    if (uninstall) {
        int r = setup_uninstall(&S);
        if (r == EXIT_NEEDS_ADMIN && !silent) {
            wchar_t args[MAXP * 2];
            wfmt(args, MAXP * 2, L"/uninstall \"/dir=%ls\"", S.dir);
            SHELLEXECUTEINFOW e;
            memset(&e, 0, sizeof e);
            e.cbSize = sizeof e;
            e.fMask = SEE_MASK_NOCLOSEPROCESS;
            e.lpVerb = L"runas";
            wchar_t tmp[MAXP];                       /* (a copy: the original is deleted) */
            GetTempPathW(MAXP, tmp);
            wcsncat(tmp, L"Sly2-Uninstall.exe", MAXP - wcslen(tmp) - 1);
            CopyFileW(self_path(), tmp, FALSE);
            e.lpFile = tmp;
            e.lpParameters = args;
            e.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&e) && e.hProcess) {
                WaitForSingleObject(e.hProcess, INFINITE);
                DWORD code = 1;
                GetExitCodeProcess(e.hProcess, &code);
                CloseHandle(e.hProcess);
                return (int)code;
            }
            return EXIT_CANCELLED;
        }
        if (!silent) {
            if (r == EXIT_OK) setup_message(&S, L"Sly 2: Band of Thieves was removed from this PC.", MB_ICONINFORMATION);
            else if (r != EXIT_CANCELLED) setup_message(&S, S.error[0] ? S.error : L"Uninstalling failed.", MB_ICONERROR);
        }
        return r;
    }

    /* which installation: the launcher's own folder, /dir=, or the one registered (a Setup run again) */
    wpath(st, MAXP, self_dir, L"setup-state.ini");
    if (!dir_given && file_exists(st)) { wcopy(S.dir, MAXP, self_dir); S.existing = 1; }
    else if (!dir_given) setup_find_existing(&S);
    else { wpath(st, MAXP, S.dir, L"setup-state.ini"); S.existing = file_exists(st); }
    if (S.existing) load_state(&S, regkey_given);
    if (S.existing) stage_recover(S.dir);
    S.no_update_check = no_check;

    if (uitest) return launcher_uitest(&S, bad_iso, ui_install, ui_cancel_after);
    if (shots[0]) { S.uitest = 1; return launcher_shots(&S, shots); }

    if (check_only) {
        char installed[64];
        installed_version(&S, installed, sizeof installed);
        update_info u;
        if (!update_check(&S, installed, &u)) { out(L"failed: %ls", S.error); return EXIT_FAILED; }
        out(L"%ls %hs (installed %hs)%ls", u.newer ? L"newer" : L"current", u.m.version, installed[0] ? installed : "-",
            u.launcher_too_old ? L", needs a newer launcher" : L"");
        return EXIT_OK;
    }
    if (do_rollback) {
        if (!logfile[0]) { wchar_t lp[MAXP]; wpath(lp, MAXP, S.dir, L"setup.log"); log_open(lp); }
        int r = setup_rollback(&S);
        char now[64];
        installed_version(&S, now, sizeof now);
        if (r) out(L"rollback failed: %ls", S.error);
        else out(L"now on %hs", now);
        return r;
    }

    if (silent) {
        if (!logfile[0]) {
            wchar_t t[MAXP];
            GetTempPathW(MAXP, t);
            wcsncat(t, L"sly2-setup.log", MAXP - wcslen(t) - 1);
            log_open(t);
        }
        if (do_update) return silent_update();
        if (do_repair) {                             /* the kept sources of the installed version */
            char cur[64];
            stage_version(S.dir, GAME_DIR, cur, sizeof cur);
            wchar_t b[MAXP];
            setup_bundle_path(&S, cur, b);
            if (cur[0] && file_exists(b)) wcopy(S.bundle, MAXP, b);
        }
        if (!S.image_src[0] && !S.iso[0] && !S.existing) { loga("/silent needs /iso=<disc image>"); return EXIT_USAGE; }
        int play = S.play == 2;
        int r = setup_install(&S);
        if (r) { loga("setup failed (%d): %ls", r, S.error); return r; }
        S.play = play;
        setup_finish(&S);
        return EXIT_OK;
    }
    if (S.allusers && !is_elevated()) S.allusers = 0;
    int r = launcher_run(&S);
    CoUninitialize();
    return r;
}
