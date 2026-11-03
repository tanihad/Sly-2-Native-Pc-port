/* The launcher window: the player's front door (notes/install.md, "Launcher").
 *
 * One window with four states:
 *   welcome  not set up yet: drop the disc image (.iso) on the window (or click to choose it), Install
 *   working  importing the image, building the game, downloading an update: progress, Cancel
 *   error    what went wrong, Retry / Back / Show details
 *   ready    a big Play button; Settings, Saves folder, Repair, Uninstall, Previous version; the update check
 * The work itself is engine.c / update.c on a worker thread (the same code as /silent). Sly 2-flavoured
 * colours (night blue, gold), no artwork. Per-monitor DPI aware (the manifest), no console window.
 */
#include "setup.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { ST_WELCOME, ST_WORKING, ST_ERROR, ST_READY };
enum { JOB_INSTALL = 1, JOB_UPDATE, JOB_REPAIR };
enum {
    ID_PLAY = 2001, ID_INSTALL, ID_CANCEL, ID_RETRY, ID_BACK, ID_LOG, ID_SETTINGS, ID_SAVES, ID_REPAIR, ID_UNINSTALL,
    ID_PREVIOUS, ID_UPDATE, ID_AGREE,
};
enum { UPD_NONE, UPD_CHECKING, UPD_NEWER, UPD_CURRENT, UPD_FAILED, UPD_NEEDS_SETUP, UPD_LOCAL };
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 3)
#define WM_APP_CHECKED (WM_APP + 5)

/* colours: the night-blue of the game's menus, its gold title lettering */
#define C_BG RGB(0x13, 0x1c, 0x36)
#define C_HEADER RGB(0x0b, 0x11, 0x24)
#define C_PANEL RGB(0x1c, 0x29, 0x4b)
#define C_LINE RGB(0x3c, 0x4d, 0x7d)
#define C_TEXT RGB(0xec, 0xef, 0xf7)
#define C_DIM RGB(0x9d, 0xa9, 0xc8)
#define C_GOLD RGB(0xf6, 0xbd, 0x2f)
#define C_GOLD_DOWN RGB(0xd2, 0x9c, 0x1a)
#define C_BTN RGB(0x29, 0x3a, 0x68)
#define C_BTN_DOWN RGB(0x20, 0x2e, 0x55)
#define C_OK RGB(0x6b, 0xd9, 0x9b)
#define C_BAD RGB(0xff, 0x7a, 0x6e)

static setup_t* S;
static setup_t C;                                    /* the update check's own copy (runs beside a job) */
static HWND win;
static HINSTANCE inst;
static int state, dpi = 96, running, job, result = -1, exit_code = EXIT_OK, uitest;
static int disc_state;                               /* 0 none chosen, 1 ok, -1 refused */
static wchar_t disc_name[MAXP], disc_msg[600], err_msg[2600], notice[600];
static int notice_kind;                              /* 0 dim, 1 good */
static int upd_state, upd_checked, first_install;
static update_info upd;
static char game_ver[64], prev_ver[64];
static HFONT f_body, f_small, f_title, f_sub, f_big, f_status, f_button;
static HBRUSH br_bg;
static ITaskbarList3* taskbar;
static struct { CRITICAL_SECTION lock; int pm; wchar_t status[512], detail[256]; int pending; } prog;
static int prog_pm;
static wchar_t prog_status[512], prog_detail[256];

static int S_(int v) { return MulDiv(v, dpi, 96); }
static HWND item(int id) { return GetDlgItem(win, id); }

/* ---- fonts and DPI ---------------------------------------------------------------------------------- */

static HFONT mkfont(int pt, int weight) {
    NONCLIENTMETRICSW m;
    m.cbSize = sizeof m;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0);
    LOGFONTW lf = m.lfMessageFont;                   /* Segoe UI on Windows 10/11 */
    lf.lfHeight = -MulDiv(pt, dpi, 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&lf);
}

static void make_fonts(void) {
    HFONT* all[] = { &f_body, &f_small, &f_title, &f_sub, &f_big, &f_status, &f_button };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) if (*all[i]) DeleteObject(*all[i]);
    f_body = mkfont(10, FW_NORMAL);
    f_small = mkfont(9, FW_NORMAL);
    f_title = mkfont(20, FW_BOLD);
    f_sub = mkfont(10, FW_NORMAL);
    f_big = mkfont(22, FW_BOLD);
    f_status = mkfont(13, FW_SEMIBOLD);
    f_button = mkfont(10, FW_SEMIBOLD);
}

/* ---- state ------------------------------------------------------------------------------------------ */

static int has_previous(void) { return prev_ver[0] != 0; }

static void short_version(const char* v, char* out, size_t cap) {
    snprintf(out, cap, "%s", v);
    char* sp = strchr(out, ' ');
    if (sp) *sp = 0;
}

static void refresh_installed(void) {
    stage_version(S->dir, GAME_DIR, game_ver, sizeof game_ver);
    stage_version(S->dir, GAME_DIR L".old", prev_ver, sizeof prev_ver);
    wchar_t old[MAXP];
    wpath(old, MAXP, S->dir, GAME_DIR L".old\\sly2.exe");
    if (!file_exists(old)) prev_ver[0] = 0;
}

/* every button, where it is in each state (logical 96-DPI units; w = 0: hidden there) */
static void place(int id, int x, int y, int w, int h) {
    HWND c = item(id);
    if (!c) return;
    if (!w) { ShowWindow(c, SW_HIDE); return; }
    SetWindowPos(c, NULL, S_(x), S_(y), S_(w), S_(h), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(c, SW_SHOW);
}

static void layout(void) {
    static const int all[] = { ID_PLAY, ID_INSTALL, ID_CANCEL, ID_RETRY, ID_BACK, ID_LOG, ID_SETTINGS, ID_SAVES, ID_REPAIR,
                               ID_UNINSTALL, ID_PREVIOUS, ID_UPDATE, ID_AGREE };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ShowWindow(item(all[i]), SW_HIDE);
    switch (state) {
    case ST_WELCOME:
        place(ID_AGREE, 32, 293, 16, 16);
        place(ID_INSTALL, 448, 372, 160, 44);
        EnableWindow(item(ID_INSTALL), disc_state == 1 && SendMessageW(item(ID_AGREE), BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case ST_WORKING:
        place(ID_LOG, 32, 376, 140, 36);
        place(ID_CANCEL, 488, 376, 120, 36);
        EnableWindow(item(ID_CANCEL), !S->cancel);
        break;
    case ST_ERROR:
        place(ID_LOG, 32, 376, 140, 36);
        place(ID_BACK, 312, 376, 120, 36);
        place(ID_RETRY, 448, 372, 160, 44);
        break;
    case ST_READY: {
        place(ID_PLAY, 210, 124, 220, 68);
        if (upd_state == UPD_NEWER || upd_state == UPD_LOCAL || upd_state == UPD_NEEDS_SETUP) place(ID_UPDATE, 456, 262, 144, 34);
        int ids[5] = { ID_SETTINGS, ID_SAVES, ID_REPAIR, ID_UNINSTALL, ID_PREVIOUS };
        int n = has_previous() ? 5 : 4, gap = 10, w = (576 - gap * (n - 1)) / n;
        for (int i = 0; i < n; i++) place(ids[i], 32 + i * (w + gap), 376, w, 36);
        break;
    }
    }
    if (state == ST_READY) {
        SetWindowTextW(item(ID_UPDATE), upd_state == UPD_NEEDS_SETUP ? L"Get it" : L"Update");
    }
    InvalidateRect(win, NULL, FALSE);
}

static void set_state(int st) {
    state = st;
    logw(L"[ui] state %d", st);
    layout();
    HWND focus = st == ST_READY ? item(ID_PLAY) : st == ST_WELCOME ? item(ID_INSTALL) : st == ST_ERROR ? item(ID_RETRY) : item(ID_CANCEL);
    if (focus && IsWindowEnabled(focus) && !uitest) SetFocus(focus);
}

/* ---- the disc image -------------------------------------------------------------------------------- */

static void take_image(const wchar_t* path) {
    wchar_t why[512], full[MAXP];
    if (!GetFullPathNameW(path, MAXP, full, NULL)) wcopy(full, MAXP, path);
    const wchar_t* leaf = wcsrchr(full, L'\\');
    wcopy(disc_name, MAXP, leaf ? leaf + 1 : full);
    if (dir_exists(full)) {
        disc_state = -1;
        wcopy(disc_msg, 600, L"That is a folder. Drop the disc image file itself (a file ending in .iso).");
    } else {
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        int r = disc_check_iso(full, why, 512);
        SetCursor(old);
        disc_state = r == DISC_OK ? 1 : -1;
        if (r == DISC_OK) {
            wcopy(S->image_src, MAXP, full);
            wcopy(disc_msg, 600, L"Sly 2: Band of Thieves (USA). Ready to install.");
        } else {
            S->image_src[0] = 0;
            wfmt(disc_msg, 600, L"This file can't be used. %ls", why);
        }
    }
    logw(L"[ui] image %ls: %ls", full, disc_msg);
    if (state == ST_WELCOME) layout();
}

static void choose_image(void) {
    IFileOpenDialog* d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&d))) return;
    DWORD o = 0;
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    COMDLG_FILTERSPEC f[] = { { L"Disc image (*.iso)", L"*.iso" }, { L"All files (*.*)", L"*.*" } };
    IFileOpenDialog_SetFileTypes(d, 2, f);
    IFileOpenDialog_SetTitle(d, L"Choose your Sly 2: Band of Thieves disc image (.iso)");
    if (SUCCEEDED(IFileOpenDialog_Show(d, win))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) { take_image(p); CoTaskMemFree(p); }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
}

static void on_drop(HDROP drop) {
    wchar_t p[MAXP], pick[MAXP] = L"";
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < n; i++) {                   /* several files: the first .iso, else the first file */
        if (!DragQueryFileW(drop, i, p, MAXP)) continue;
        const wchar_t* ext = wcsrchr(p, L'.');
        if (!pick[0] || (ext && !_wcsicmp(ext, L".iso") && _wcsicmp(wcsrchr(pick, L'.') ? wcsrchr(pick, L'.') : L"", L".iso")))
            wcopy(pick, MAXP, p);
    }
    DragFinish(drop);
    if (!pick[0]) return;
    if (state == ST_WELCOME || (state == ST_ERROR && !setup_is_installed(S))) {
        if (state == ST_ERROR) set_state(ST_WELCOME);
        take_image(pick);
    } else if (state == ST_READY) {
        wcopy(notice, 600, L"The game is already set up and does not need the disc image any more.");
        notice_kind = 0;
        InvalidateRect(win, NULL, FALSE);
    }
}

/* ---- the install folder ------------------------------------------------------------------------------ */

static int path_writable(const wchar_t* dir, DWORD* err) {
    wchar_t p[MAXP];
    wcopy(p, MAXP, dir);
    while (!dir_exists(p)) {
        wchar_t* s = wcsrchr(p, L'\\');
        if (!s || s == p) { *err = ERROR_PATH_NOT_FOUND; return 0; }
        *s = 0;
        if (wcslen(p) == 2 && p[1] == L':') wcsncat(p, L"\\", MAXP - 3);
    }
    wchar_t t[MAXP];
    wfmt(t, MAXP, L"%ls%ls~sly2-setup-%lu.tmp", p, p[wcslen(p) - 1] == L'\\' ? L"" : L"\\", GetCurrentProcessId());
    HANDLE h = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) { *err = GetLastError(); return 0; }
    CloseHandle(h);
    *err = 0;
    return 1;
}

static void quote_arg(wchar_t* cmd, size_t cap, const wchar_t* name, const wchar_t* v) {
    if (!v[0]) return;
    wchar_t a[MAXP + 32];
    wfmt(a, MAXP + 32, L" \"/%ls=%ls\"", name, v);
    wcsncat(cmd, a, cap - wcslen(cmd) - 1);
}

static int restart_elevated(const wchar_t* args) {
    SHELLEXECUTEINFOW e;
    memset(&e, 0, sizeof e);
    e.cbSize = sizeof e;
    e.fMask = SEE_MASK_NOCLOSEPROCESS;
    e.hwnd = win;
    e.lpVerb = L"runas";
    e.lpFile = self_path();
    e.lpParameters = args;
    e.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&e)) return 0;
    if (e.hProcess) CloseHandle(e.hProcess);
    return 1;
}

/* a folder chosen with "Change": checked like the old wizard's folder page */
static int use_folder(const wchar_t* d) {
    DWORD err;
    if (!path_writable(d, &err)) {
        if (err == ERROR_ACCESS_DENIED && !is_elevated()) {
            wchar_t q[MAXP + 400];
            wfmt(q, MAXP + 400, L"Installing into\n%ls\nneeds administrator rights (it is a folder for all users).\n\nRestart "
                                L"as administrator? (Or choose a folder of your own, such as the suggested one.)", d);
            if (setup_ask(S, q, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                wchar_t args[MAXP * 8];
                wfmt(args, MAXP * 8, L"/allusers \"/dir=%ls\"", d);
                quote_arg(args, MAXP * 8, L"iso", S->image_src);
                quote_arg(args, MAXP * 8, L"from", S->from);
                quote_arg(args, MAXP * 8, L"mirror", S->mirror);
                if (restart_elevated(args)) { exit_code = EXIT_OK; DestroyWindow(win); }
            }
            return 0;
        }
        wchar_t m[MAXP + 200];
        wfmt(m, MAXP + 200, L"A folder cannot be created there (error %lu). Choose another folder.", err);
        setup_message(S, m, MB_ICONWARNING);
        return 0;
    }
    wchar_t st[MAXP];
    wpath(st, MAXP, d, L"setup-state.ini");
    if (!file_exists(st) && dir_exists(d)) {
        wchar_t pat[MAXP];
        wpath(pat, MAXP, d, L"*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        int n = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) n++; while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (n && setup_ask(S, L"This folder is not empty. Install Sly 2 into it anyway?\n\n(Uninstalling later removes only "
                              L"the files it put there.)", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return 0;
    }
    wcopy(S->dir, MAXP, d);
    S->existing = file_exists(st);
    return 1;
}

static void change_folder(void) {
    IFileOpenDialog* d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&d))) return;
    DWORD o = 0;
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
    IFileOpenDialog_SetTitle(d, L"Choose where to install Sly 2");
    if (SUCCEEDED(IFileOpenDialog_Show(d, win))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                wchar_t v[MAXP];
                wcopy(v, MAXP, p);
                const wchar_t* leaf = wcsrchr(v, L'\\');
                if (!leaf || _wcsicmp(leaf + 1, L"Sly2")) wcsncat(v, v[wcslen(v) - 1] == L'\\' ? L"Sly2" : L"\\Sly2", MAXP - wcslen(v) - 1);
                use_folder(v);
                CoTaskMemFree(p);
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
    InvalidateRect(win, NULL, FALSE);
}

/* ---- jobs ----------------------------------------------------------------------------------------- */

static void job_progress(void* ui, int pm, const wchar_t* status, const wchar_t* detail) {
    (void)ui;
    EnterCriticalSection(&prog.lock);
    prog.pm = pm;
    wcopy(prog.status, 512, status);
    wcopy(prog.detail, 256, detail);
    int post = !prog.pending;
    prog.pending = 1;
    LeaveCriticalSection(&prog.lock);
    if (post) PostMessageW(win, WM_APP_PROGRESS, 0, 0);
}

static DWORD WINAPI job_thread(void* arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int r = EXIT_OK;
    int first = first_install;
    wchar_t dl[MAXP];
    setup_state_get(S->dir, L"desktop_lnk", dl, MAXP);
    if (job == JOB_UPDATE && upd_state == UPD_NEWER) {
        S->pm_off = 0;
        S->pm_scale = 100;                           /* the download: the first 10% */
        wcopy(S->status, 512, L"Downloading the update");
        r = update_download(S, &upd);
        S->pm_off = 100;
        S->pm_scale = 900;
        if (!r) snprintf(S->expect_version, sizeof S->expect_version, "%s", upd.m.version);
    } else S->pm_off = S->pm_scale = 0;
    if (!r) r = setup_install(S);
    S->pm_off = S->pm_scale = 0;
    if (!r) {
        S->play = 0;
        S->keep_build = 0;
        S->desktop_shortcut = first && !dl[0];
        setup_finish(S);
        S->existing = 1;
    }
    if (job == JOB_UPDATE && S->bundle[0]) {         /* the download (a copy is kept in bundle\) */
        wchar_t ud[MAXP];
        wpath(ud, MAXP, S->dir, UPDATES_DIR);
        if (!_wcsnicmp(S->bundle, ud, wcslen(ud))) DeleteFileW(S->bundle);
    }
    S->bundle[0] = 0;
    S->expect_version[0] = 0;
    CoUninitialize();
    PostMessageW(win, WM_APP_DONE, (WPARAM)r, 0);
    return 0;
}

static void start_job(int kind) {
    if (running) return;
    job = kind;
    S->error[0] = 0;
    InterlockedExchange(&S->cancel, 0);
    if (kind == JOB_REPAIR) {
        wchar_t b[MAXP];
        setup_bundle_path(S, game_ver, b);
        if (game_ver[0] && file_exists(b)) wcopy(S->bundle, MAXP, b);
        else if (!payload_find("source.zip")) {
            wcopy(err_msg, 2600, L"The game's sources are missing, so it cannot be rebuilt here. Download Setup again and run it: "
                                 L"your saves and settings are kept.");
            result = EXIT_FAILED;
            set_state(ST_ERROR);
            return;
        }
        S->image_src[0] = 0;
    }
    if (kind == JOB_UPDATE && upd_state == UPD_LOCAL) S->bundle[0] = 0;   /* this setup file's sources */
    first_install = kind == JOB_INSTALL && !setup_is_installed(S);
    if (first_install) S->keep_tools = 1;           /* updates then rebuild without downloading the tools again */
    prog_pm = 0;
    wcopy(prog_status, 512, kind == JOB_UPDATE ? L"Updating Sly 2" : kind == JOB_REPAIR ? L"Repairing Sly 2" : L"Setting up Sly 2");
    prog_detail[0] = 0;
    running = 1;
    result = -1;
    S->launcher = 1;
    S->progress = job_progress;
    if (taskbar) ITaskbarList3_SetProgressState(taskbar, win, TBPF_NORMAL);
    set_state(ST_WORKING);
    HANDLE t = CreateThread(NULL, 0, job_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void on_done(int r) {
    running = 0;
    result = r;
    if (taskbar) ITaskbarList3_SetProgressState(taskbar, win, r == EXIT_OK ? TBPF_NOPROGRESS : TBPF_ERROR);
    refresh_installed();
    if (r == EXIT_OK) {
        exit_code = EXIT_OK;
        char v[64];
        short_version(game_ver, v, sizeof v);
        if (job == JOB_UPDATE) wfmt(notice, 600, L"Updated to version %hs. Your saves and settings are as they were.", v);
        else if (job == JOB_REPAIR) wfmt(notice, 600, L"Repaired: version %hs was built again.", v);
        else wcopy(notice, 600, L"All set! Your disc image is no longer needed: the game has its own copy.");
        notice_kind = 1;
        upd_state = UPD_NONE;
        disc_state = 0;
        set_state(ST_READY);
        return;
    }
    exit_code = r;
    char v[64];
    short_version(game_ver, v, sizeof v);
    wchar_t keep[200] = L"";
    if (job != JOB_INSTALL && game_ver[0]) wfmt(keep, 200, L"\n\nVersion %hs is unchanged and still ready to play.", v);
    if (r == EXIT_CANCELLED) wfmt(err_msg, 2600, L"Stopped. Click Retry to continue where it stopped.%ls", keep);
    else if (r == EXIT_NEEDS_ADMIN) wfmt(err_msg, 2600, L"%ls Go back and choose another folder.", S->error);
    else wfmt(err_msg, 2600, L"%ls%ls", S->error[0] ? S->error : L"Something went wrong (see the details).", keep);
    set_state(ST_ERROR);
}

static void cancel_job(void) {
    if (!running) return;
    if (setup_ask(S, L"Stop now?\n\nYou can continue later: what is done so far is kept.", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    InterlockedExchange(&S->cancel, 1);
    EnterCriticalSection(&S->lock);
    if (S->job) TerminateJobObject(S->job, 1);
    LeaveCriticalSection(&S->lock);
    wcopy(prog_status, 512, L"Stopping...");
    layout();
}

/* ---- the update check ------------------------------------------------------------------------------ */

static DWORD WINAPI check_thread(void* arg) {
    (void)arg;
    char installed[64];
    short_version(game_ver, installed, sizeof installed);
    static update_info u;
    int ok = update_check(&C, installed, &u);
    upd = u;
    PostMessageW(win, WM_APP_CHECKED, (WPARAM)ok, 0);
    return 0;
}

static void start_check(void) {
    if (upd_checked || running) return;
    upd_checked = 1;
    char installed[64], mine[64];
    short_version(game_ver, installed, sizeof installed);
    snprintf(mine, sizeof mine, "%ls", SETUP_VERSION);
    if (payload_find("source.zip") && release_version_cmp(mine, installed) > 0) {   /* a newer Setup was started */
        upd_state = UPD_LOCAL;
        layout();
        return;
    }
    wchar_t v[16];
    setup_state_get(S->dir, L"update_check", v, 16);
    if (v[0] == L'0') return;
    memcpy(&C, S, sizeof C);
    C.progress = NULL;
    C.logline = NULL;
    C.cancel = 0;
    C.job = NULL;
    upd_state = UPD_CHECKING;
    HANDLE t = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void on_checked(int ok) {
    if (!ok) { upd_state = UPD_FAILED; logw(L"[ui] %ls", C.error); }
    else if (!upd.newer) upd_state = UPD_CURRENT;
    else upd_state = upd.launcher_too_old ? UPD_NEEDS_SETUP : UPD_NEWER;
    if (state == ST_READY) layout();
}

/* ---- the ready screen's buttons ---------------------------------------------------------------------- */

static void open_folder(const wchar_t* p) { ShellExecuteW(win, L"open", p, NULL, NULL, SW_SHOWNORMAL); }

static void on_command(int id) {
    switch (id) {
    case ID_PLAY:
        if (setup_game_running(S)) { wcopy(notice, 600, L"Sly 2 is already running."); notice_kind = 0; InvalidateRect(win, NULL, FALSE); break; }
        if (uitest) { loga("[uitest] play"); break; }
        if (setup_launch_game(S)) { exit_code = EXIT_OK; DestroyWindow(win); }
        else { wcopy(notice, 600, L"The game could not be started: try Repair."); notice_kind = 0; InvalidateRect(win, NULL, FALSE); }
        break;
    case ID_INSTALL: if (disc_state == 1) start_job(JOB_INSTALL); break;
    case ID_AGREE: layout(); break;
    case ID_RETRY:
        if (job == JOB_INSTALL && !setup_is_installed(S) && !S->image_src[0]) {   /* the image check failed: choose again */
            wchar_t img[MAXP];
            wpath(img, MAXP, S->dir, IMAGE_REL);
            if (!file_exists(img)) { set_state(ST_WELCOME); break; }
        }
        start_job(job ? job : JOB_INSTALL);
        break;
    case ID_BACK: set_state(setup_is_installed(S) ? ST_READY : ST_WELCOME); break;
    case ID_CANCEL: cancel_job(); break;
    case ID_LOG: if (log_path()[0]) ShellExecuteW(win, L"open", L"notepad.exe", log_path(), NULL, SW_SHOWNORMAL); break;
    case ID_SETTINGS:
        setup_message(S, L"Graphics, display, sound and controls are set in the game itself, while you play:\n\npress Esc "
                         L"(keyboard) or the PS button (DualSense / DualShock 4), or pause the game (Start / Options) and "
                         L"choose Options.\n\nChanges apply at once and are remembered.", MB_ICONINFORMATION);
        break;
    case ID_SAVES: {
        wchar_t ud[MAXP];
        setup_user_data_dir(S, ud);
        mkdir_p(ud);
        if (!uitest) open_folder(ud);
        break;
    }
    case ID_REPAIR:
        if (setup_ask(S, L"Build the game again from its sources?\n\nThis fixes a damaged installation. It takes a few minutes; "
                         L"your saves and settings are kept.", MB_YESNO | MB_ICONQUESTION) == IDYES)
            start_job(JOB_REPAIR);
        break;
    case ID_PREVIOUS: {
        char pv[64], cv[64];
        short_version(prev_ver, pv, sizeof pv);
        short_version(game_ver, cv, sizeof cv);
        wchar_t q[300];
        wfmt(q, 300, L"Switch back to version %hs?\n\nVersion %hs is kept, so you can switch again later. Saves and settings are "
                     L"not affected.", pv, cv);
        if (setup_ask(S, q, MB_YESNO | MB_ICONQUESTION) != IDYES) break;
        if (setup_rollback(S) == EXIT_OK) { wfmt(notice, 600, L"Now on version %hs.", pv); notice_kind = 1; }
        else { wcopy(notice, 600, S->error); notice_kind = 0; }
        refresh_installed();
        layout();
        break;
    }
    case ID_UPDATE:
        if (upd_state == UPD_NEEDS_SETUP) {
            if (!strncmp(upd.m.page, "https://", 8)) {   /* (only a secure web page is opened) */
                wchar_t u[600];
                wfmt(u, 600, L"%hs", upd.m.page);
                if (!uitest) open_folder(u);
            }
            break;
        }
        if (setup_game_running(S)) { wcopy(notice, 600, L"Close Sly 2 first, then click Update."); notice_kind = 0; InvalidateRect(win, NULL, FALSE); break; }
        start_job(JOB_UPDATE);
        break;
    case ID_UNINSTALL: {
        wchar_t un[MAXP], args[64];
        wpath(un, MAXP, S->dir, L"Uninstall.exe");
        wfmt(args, 64, L"/uninstall /waitpid=%lu", GetCurrentProcessId());
        if (uitest) { loga("[uitest] uninstall"); break; }
        if ((INT_PTR)ShellExecuteW(win, L"open", un, args, S->dir, SW_SHOWNORMAL) > 32) { exit_code = EXIT_OK; DestroyWindow(win); }
        break;
    }
    }
}

/* ---- painting -------------------------------------------------------------------------------------- */

static RECT R(int x, int y, int w, int h) { RECT r = { S_(x), S_(y), S_(x + w), S_(y + h) }; return r; }

static void text(HDC dc, HFONT f, COLORREF c, const wchar_t* t, RECT r, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &r, fmt | DT_NOPREFIX);
}

static void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void round_rect(HDC dc, RECT r, int radius, COLORREF fillc, COLORREF line, int dashed) {
    HBRUSH b = CreateSolidBrush(fillc);
    LOGBRUSH lb = { BS_SOLID, line, 0 };
    HPEN p = ExtCreatePen(PS_GEOMETRIC | (dashed ? PS_DASH : PS_SOLID) | PS_ENDCAP_FLAT, (DWORD)S_(dashed ? 2 : 1), &lb, 0, NULL);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S_(radius), S_(radius));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static RECT drop_zone(void) { return R(32, 116, 576, 160); }
static RECT agree_text(void) { return R(54, 290, 554, 22); }
static RECT folder_link(void) { return R(32, 318, 576, 22); }

static void paint(HDC dc, RECT client) {
    fill(dc, client, C_BG);
    SetBkMode(dc, TRANSPARENT);
    RECT hdr = { 0, 0, client.right, S_(96) };
    fill(dc, hdr, C_HEADER);
    RECT gold = { 0, S_(96), client.right, S_(98) };
    fill(dc, gold, C_GOLD);
    HICON ic = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S_(56), S_(56), 0);
    if (ic) { DrawIconEx(dc, S_(28), S_(20), ic, S_(56), S_(56), 0, NULL, DI_NORMAL); DestroyIcon(ic); }
    text(dc, f_title, C_GOLD, L"Sly 2: Band of Thieves", R(100, 18, 520, 36), DT_SINGLELINE | DT_VCENTER);
    const wchar_t* sub = state == ST_READY ? L"Ready to play" : state == ST_WORKING ? L"Working on it - this takes a few minutes"
                       : state == ST_ERROR ? L"That didn't work" : L"Set up the game from your own disc";
    text(dc, f_sub, C_DIM, sub, R(101, 54, 520, 24), DT_SINGLELINE | DT_VCENTER);

    if (state == ST_WELCOME) {
        RECT z = drop_zone();
        COLORREF line = disc_state > 0 ? C_OK : disc_state < 0 ? C_BAD : C_LINE;
        round_rect(dc, z, 14, C_PANEL, line, disc_state == 0);
        if (disc_state == 0) {
            text(dc, f_status, C_TEXT, L"Drop your Sly 2 disc image here", R(48, 150, 544, 32), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            text(dc, f_body, C_DIM, L"or click here to choose the .iso file", R(48, 186, 544, 24), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            text(dc, f_small, C_DIM, L"Sly 2: Band of Thieves, USA release (SCUS-97316), as an .iso image of your own disc",
                 R(48, 230, 544, 22), DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else {
            text(dc, f_status, C_TEXT, disc_name, R(48, 134, 544, 32), DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
            text(dc, f_body, disc_state > 0 ? C_OK : C_BAD, disc_msg, R(56, 172, 528, 70), DT_CENTER | DT_WORDBREAK);
            text(dc, f_small, C_DIM, L"Click to choose another file", R(48, 246, 544, 22), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        text(dc, f_body, C_TEXT, L"I made this disc image from my own copy of the game.", agree_text(), DT_SINGLELINE | DT_VCENTER);
        wchar_t t[MAXP + 64];
        wfmt(t, MAXP + 64, L"Install folder:  %ls", S->dir);
        RECT fl = folder_link();
        fl.right -= S_(80);
        text(dc, f_small, C_DIM, t, fl, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
        text(dc, f_small, C_GOLD, L"Change...", R(528, 318, 80, 22), DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        wchar_t need[400], a[32];
        fmt_size(a, 32, setup_space_needed(S) + (disc_state > 0 ? file_size(S->image_src) : (3ull << 30)));
        wfmt(need, 400, L"Needs about %ls of free space. The game is built on this PC from your disc; nothing from the game "
                        L"is downloaded.", a);
        text(dc, f_small, C_DIM, need, R(32, 368, 400, 52), DT_WORDBREAK);
    } else if (state == ST_WORKING) {
        text(dc, f_status, C_TEXT, S->cancel ? L"Stopping..." : prog_status, R(32, 128, 576, 56), DT_WORDBREAK);
        RECT bar = R(32, 194, 576, 14);
        round_rect(dc, bar, 14, C_PANEL, C_LINE, 0);
        RECT f = bar;
        f.right = f.left + (int)((long long)(bar.right - bar.left) * prog_pm / 1000);
        if (f.right > f.left + S_(6)) round_rect(dc, f, 14, C_GOLD, C_GOLD, 0);
        text(dc, f_small, C_DIM, prog_detail, R(32, 214, 576, 22), DT_SINGLELINE | DT_END_ELLIPSIS);
        wchar_t pc[16];
        wfmt(pc, 16, L"%d%%", prog_pm / 10);
        text(dc, f_small, C_DIM, pc, R(508, 214, 100, 22), DT_SINGLELINE | DT_RIGHT);
        text(dc, f_small, C_DIM, L"You can keep using your PC meanwhile. Closing this window stops the work; it continues "
                                 L"where it stopped next time.", R(32, 300, 576, 44), DT_WORDBREAK);
    } else if (state == ST_ERROR) {
        text(dc, f_status, result == EXIT_CANCELLED ? C_TEXT : C_BAD, err_msg, R(32, 124, 576, 230), DT_WORDBREAK | DT_END_ELLIPSIS);
    } else if (state == ST_READY) {
        char v[64];
        short_version(game_ver, v, sizeof v);
        wchar_t t[600];
        wfmt(t, 600, L"Version %hs", v[0] ? v : "?");
        text(dc, f_small, C_DIM, t, R(32, 198, 576, 22), DT_CENTER | DT_SINGLELINE);
        if (notice[0]) text(dc, f_body, notice_kind ? C_OK : C_DIM, notice, R(32, 222, 576, 36), DT_CENTER | DT_WORDBREAK);
        RECT u = R(32, 262, 400, 34);
        switch (upd_state) {
        case UPD_CHECKING: text(dc, f_small, C_DIM, L"Checking for updates...", u, DT_SINGLELINE | DT_VCENTER); break;
        case UPD_CURRENT: text(dc, f_small, C_DIM, L"You have the latest version.", u, DT_SINGLELINE | DT_VCENTER); break;
        case UPD_FAILED: text(dc, f_small, C_DIM, L"Couldn't check for updates.", u, DT_SINGLELINE | DT_VCENTER); break;
        case UPD_NEWER:
        case UPD_NEEDS_SETUP:
        case UPD_LOCAL: {
            RECT band = R(32, 256, 576, 46);
            round_rect(dc, band, 10, C_PANEL, C_GOLD, 0);
            if (upd_state == UPD_LOCAL) wfmt(t, 600, L"This setup file has version %ls. Update?", SETUP_VERSION);
            else if (upd_state == UPD_NEEDS_SETUP) wfmt(t, 600, L"Version %hs is available. It needs the new Setup.", upd.m.version);
            else wfmt(t, 600, L"Version %hs is available. Update?", upd.m.version);
            text(dc, f_body, C_TEXT, t, R(48, 262, 390, 34), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            break;
        }
        }
        text(dc, f_small, C_DIM, L"Your saves and settings are kept in your user folder, separate from the game, so updates and "
                                 L"repairs never touch them.", R(32, 316, 576, 40), DT_WORDBREAK);
    }
}

static void draw_button(const DRAWITEMSTRUCT* d) {
    int id = (int)d->CtlID;
    int primary = id == ID_PLAY || id == ID_INSTALL || id == ID_RETRY || id == ID_UPDATE;
    int down = (d->itemState & ODS_SELECTED) != 0, disabled = (d->itemState & ODS_DISABLED) != 0;
    RECT r = d->rcItem;
    fill(d->hDC, r, C_BG);
    COLORREF bg = primary ? (down ? C_GOLD_DOWN : C_GOLD) : (down ? C_BTN_DOWN : C_BTN);
    COLORREF fg = primary ? C_HEADER : C_TEXT;
    if (disabled) { bg = C_PANEL; fg = C_LINE; }
    round_rect(d->hDC, r, id == ID_PLAY ? 18 : 10, bg, disabled ? C_LINE : bg, 0);
    wchar_t t[128];
    GetWindowTextW(d->hwndItem, t, 128);
    SetBkMode(d->hDC, TRANSPARENT);
    text(d->hDC, id == ID_PLAY ? f_big : id == ID_INSTALL || id == ID_RETRY ? f_status : f_button, fg, t, r,
         DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -S_(4), -S_(4));
        DrawFocusRect(d->hDC, &f);
    }
}

/* ---- window ------------------------------------------------------------------------------------------ */

static void create_controls(void) {
    static const struct { int id; const wchar_t* t; } b[] = {
        { ID_PLAY, L"PLAY" }, { ID_INSTALL, L"Install" }, { ID_CANCEL, L"Cancel" }, { ID_RETRY, L"Retry" }, { ID_BACK, L"Back" },
        { ID_LOG, L"Show details" }, { ID_SETTINGS, L"Settings" }, { ID_SAVES, L"Saves folder" }, { ID_REPAIR, L"Repair" },
        { ID_UNINSTALL, L"Uninstall" }, { ID_PREVIOUS, L"Previous" }, { ID_UPDATE, L"Update" },
    };
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++)
        CreateWindowExW(0, L"BUTTON", b[i].t, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, win, (HMENU)(INT_PTR)b[i].id, inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10, win, (HMENU)(INT_PTR)ID_AGREE, inst, NULL);
    if (S->existing) SendMessageW(item(ID_AGREE), BM_SETCHECK, BST_CHECKED, 0);   /* (agreed at the first install) */
}

static int hit(RECT r, LPARAM lp) { POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) }; return PtInRect(&r, p); }

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: on_command(LOWORD(wp)); return 0;
    case WM_DRAWITEM: draw_button((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_DROPFILES: on_drop((HDROP)wp); return 0;
    case WM_LBUTTONUP:
        if (state != ST_WELCOME) return 0;
        if (hit(drop_zone(), lp)) choose_image();
        else if (hit(agree_text(), lp)) { SendMessageW(item(ID_AGREE), BM_CLICK, 0, 0); }
        else if (hit(R(528, 318, 80, 22), lp)) change_folder();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && state == ST_WELCOME) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            RECT z = drop_zone(), l = R(528, 318, 80, 22), a = agree_text();
            if (PtInRect(&z, p) || PtInRect(&l, p) || PtInRect(&a, p)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
        }
        break;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, C_BG);
        return (LRESULT)br_bg;
    case WM_APP_PROGRESS: {
        EnterCriticalSection(&prog.lock);
        prog_pm = prog.pm;
        if (!S->cancel) wcopy(prog_status, 512, prog.status);
        wcopy(prog_detail, 256, prog.detail);
        prog.pending = 0;
        LeaveCriticalSection(&prog.lock);
        if (taskbar) ITaskbarList3_SetProgressValue(taskbar, win, (ULONGLONG)prog_pm, 1000);
        RECT r = R(0, 110, 640, 140);
        InvalidateRect(h, &r, FALSE);
        return 0;
    }
    case WM_APP_DONE: on_done((int)wp); return 0;
    case WM_APP_CHECKED: on_checked((int)wp); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c;
        GetClientRect(h, &c);
        HDC mdc = CreateCompatibleDC(dc);             /* double-buffered: no flicker while the bar moves */
        HBITMAP bm = CreateCompatibleBitmap(dc, c.right, c.bottom);
        HGDIOBJ old = SelectObject(mdc, bm);
        paint(mdc, c);
        BitBlt(dc, 0, 0, c.right, c.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(bm);
        DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        make_fonts();
        const RECT* r = (const RECT*)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_CLOSE:
        if (running) { cancel_job(); if (!S->cancel) return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg == RegisterWindowMessageW(L"TaskbarButtonCreated") && !taskbar)
        CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void**)&taskbar);
    return DefWindowProcW(h, msg, wp, lp);
}

static int create_window(setup_t* s, int show) {
    S = s;
    inst = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&prog.lock);
    br_bg = CreateSolidBrush(C_BG);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"Sly2Launcher";
    RegisterClassExW(&wc);
    /* the DPI of the monitor the window opens on (per-monitor aware: WM_DPICHANGED when it moves) */
    POINT pt = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    typedef HRESULT(WINAPI * gdfm_fn)(HMONITOR, int, UINT*, UINT*);
    gdfm_fn gdfm = (gdfm_fn)(void*)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (gdfm && SUCCEEDED(gdfm(mon, 0, &dx, &dy))) dpi = (int)dy;
    else { HDC sdc = GetDC(NULL); dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc); }
    make_fonts();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S_(640), S_(440) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, (UINT)dpi);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    RECT wa = mi.rcWork;
    win = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Sly 2: Band of Thieves", style, wa.left + (wa.right - wa.left - w) / 2,
                          wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt, NULL, NULL, inst, NULL);
    if (!win) return 0;
    /* drops from Explorer also reach an elevated launcher (all-users install) */
    ChangeWindowMessageFilterEx(win, WM_DROPFILES, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(win, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(win, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, NULL);
    create_controls();
    if (S->existing) { stage_recover(S->dir); refresh_installed(); }
    if (S->image_src[0]) take_image(S->image_src);  /* /iso= (also after the restart as administrator) */
    set_state(setup_is_installed(S) ? ST_READY : ST_WELCOME);
    if (state == ST_READY && !S->silent && !S->no_update_check) start_check();
    if (show) { ShowWindow(win, SW_SHOW); UpdateWindow(win); SetForegroundWindow(win); }
    return 1;
}

int launcher_run(setup_t* s) {
    if (!create_window(s, 1)) return EXIT_FAILED;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(win, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (taskbar) ITaskbarList3_Release(taskbar);
    return exit_code;
}

/* ---- /uitest and /uishot: the window driven or rendered without being shown ----------------------------- */

/* the control's own visibility (IsWindowVisible is false for every child of a hidden window) */
static int shown(HWND c) { return c && (GetWindowLongPtrW(c, GWL_STYLE) & WS_VISIBLE) != 0; }

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

/* a WM_DROPFILES exactly as Explorer sends it (DROPFILES + a double-NUL-terminated wide file list) */
static void fake_drop(const wchar_t* path) {
    size_t n = wcslen(path);
    HGLOBAL g = GlobalAlloc(GHND, sizeof(DROPFILES) + (n + 2) * sizeof(wchar_t));
    DROPFILES* df = GlobalLock(g);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((char*)df + sizeof(DROPFILES), path, n * sizeof(wchar_t));
    GlobalUnlock(g);
    SendMessageW(win, WM_DROPFILES, (WPARAM)g, 0);
}

static void wait_job(void) {
    MSG m;
    while (running && GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    pump();
}

int launcher_uitest(setup_t* s, const wchar_t* bad_iso, int install, int cancel_after) {
    int fails = 0;
#define EXPECT(c, what) do { if (c) loga("[uitest] ok: %s", what); else { loga("[uitest] FAIL: %s", what); fails++; } } while (0)
    uitest = 1;
    s->uitest = 1;
    wchar_t iso[MAXP];
    wcopy(iso, MAXP, s->image_src[0] ? s->image_src : s->iso);
    s->image_src[0] = 0;
    s->iso[0] = 0;
    if (!create_window(s, 0)) return EXIT_FAILED;
    EXPECT(!IsWindowVisible(win), "the window is never shown");
    if (!setup_is_installed(s)) {
        EXPECT(state == ST_WELCOME, "not set up: the drop screen");
        EXPECT(!IsWindowEnabled(item(ID_INSTALL)), "Install disabled before an image is dropped");
        if (bad_iso[0]) {
            fake_drop(bad_iso);
            logw(L"[uitest] bad image says: %ls", disc_msg);
            EXPECT(disc_state < 0 && disc_msg[0] && !IsWindowEnabled(item(ID_INSTALL)), "a wrong file is refused with a message");
        }
        wchar_t tmpd[MAXP];
        GetTempPathW(MAXP, tmpd);
        fake_drop(tmpd);
        EXPECT(disc_state < 0 && wcsstr(disc_msg, L"folder"), "a dropped folder is explained");
        fake_drop(iso);
        EXPECT(disc_state > 0 && !_wcsicmp(s->image_src, iso), "the right image is accepted (drag and drop)");
        if (!s->existing) {
            EXPECT(!IsWindowEnabled(item(ID_INSTALL)), "Install waits for the own-copy box");
            SendMessageW(item(ID_AGREE), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(win, WM_COMMAND, ID_AGREE, 0);
        }
        EXPECT(IsWindowEnabled(item(ID_INSTALL)), "Install enabled");
        if (install) {
            SendMessageW(win, WM_COMMAND, ID_INSTALL, 0);
            EXPECT(state == ST_WORKING && running, "Install starts");
            if (cancel_after > 0) {
                Sleep((DWORD)cancel_after * 1000);
                pump();
                InterlockedExchange(&S->cancel, 1);
                EnterCriticalSection(&S->lock);
                if (S->job) TerminateJobObject(S->job, 1);
                LeaveCriticalSection(&S->lock);
                wait_job();
                EXPECT(state == ST_ERROR && result == EXIT_CANCELLED, "Cancel stops it; Retry offered");
                SendMessageW(win, WM_COMMAND, ID_RETRY, 0);
                EXPECT(running, "Retry starts again");
            }
            wait_job();
            logw(L"[uitest] result %d: %ls", result, err_msg);
            EXPECT(result == EXIT_OK && state == ST_READY && shown(item(ID_PLAY)), "installed: the Play screen");
        }
    } else {                                         /* set up already: the Play screen and the update check */
        EXPECT(state == ST_READY && shown(item(ID_PLAY)), "set up: the Play screen");
        char gv[64];
        short_version(game_ver, gv, sizeof gv);
        loga("[uitest] installed version %s, previous %s", gv, prev_ver[0] ? prev_ver : "-");
        if (s->update_url[0] && !s->no_update_check) {
            start_check();
            for (int i = 0; i < 300 && upd_state == UPD_CHECKING; i++) { Sleep(100); pump(); }
            loga("[uitest] update check: state %d, latest %s", upd_state, upd.m.version);
            EXPECT(upd_state != UPD_CHECKING, "the update check finished");
            if (upd_state == UPD_NEWER) EXPECT(shown(item(ID_UPDATE)), "a newer version: Update offered");
            if (upd_state == UPD_FAILED) EXPECT(!shown(item(ID_UPDATE)), "check failed: no Update button, a quiet note");
        }
        install = 1;                                 /* (no made-up screens over a real installation) */
    }
    if (!install) {                                  /* the other screens, with made-up results */
        running = 1;
        set_state(ST_WORKING);
        wcopy(S->error, 2048, L"Not enough free disk space.");
        job = JOB_INSTALL;
        on_done(EXIT_FAILED);
        EXPECT(state == ST_ERROR && shown(item(ID_RETRY)) && wcsstr(err_msg, L"disk space"), "an error: reason + Retry");
        on_done(EXIT_CANCELLED);
        EXPECT(state == ST_ERROR && wcsstr(err_msg, L"Stopped"), "cancelled: Stopped + Retry");
        strcpy(game_ver, "0.1.1000 abc");
        strcpy(prev_ver, "0.1.900 def");
        state = ST_READY;
        upd_state = UPD_NEWER;
        strcpy(upd.m.version, "0.1.1100");
        layout();
        EXPECT(shown(item(ID_PLAY)) && shown(item(ID_UPDATE)) && shown(item(ID_PREVIOUS)),
               "ready: Play, Update offered, previous version");
        upd_state = UPD_FAILED;
        layout();
        EXPECT(!shown(item(ID_UPDATE)), "a failed check offers no Update");
        prev_ver[0] = 0;
        layout();
        EXPECT(!shown(item(ID_PREVIOUS)), "no previous version: no button");
        SendMessageW(win, WM_COMMAND, ID_SETTINGS, 0);
        SendMessageW(win, WM_COMMAND, ID_SAVES, 0);
        EXPECT(1, "Settings and Saves folder answer");
        running = 0;
    } else if (state == ST_READY) {
        SendMessageW(win, WM_COMMAND, ID_PLAY, 0);
        EXPECT(1, "Play pressed (not started in tests)");
    }
    DestroyWindow(win);
    pump();
    loga("[uitest] %d failure(s)", fails);
    return fails ? EXIT_FAILED : EXIT_OK;
}

static void save_bmp(const wchar_t* path) {
    RECT r;
    GetClientRect(win, &r);
    int w = r.right, h = r.bottom;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = NULL;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bm);
    PrintWindow(win, mdc, 1 /* PW_CLIENTONLY */ | 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();
    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof fh);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + (DWORD)(w * h * 4);
    size_t n = fh.bfSize;
    uint8_t* buf = malloc(n);
    memcpy(buf, &fh, sizeof fh);
    memcpy(buf + sizeof fh, &bi.bmiHeader, sizeof bi.bmiHeader);
    memcpy(buf + fh.bfOffBits, bits, (size_t)w * h * 4);
    write_file(path, buf, n);
    free(buf);
    SelectObject(mdc, old);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

static void shot(const wchar_t* folder, const wchar_t* name) {
    layout();
    RedrawWindow(win, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    for (int k = 0; k < 4; k++) { Sleep(60); pump(); }   /* (DWM composes off-screen windows late) */
    wchar_t p[MAXP];
    wfmt(p, MAXP, L"%ls\\%ls.bmp", folder, name);
    save_bmp(p);
}

int launcher_shots(setup_t* s, const wchar_t* folder) {
    uitest = 1;
    s->uitest = 1;
    s->silent = 1;                                   /* (no update check) */
    wchar_t iso[MAXP];
    wcopy(iso, MAXP, s->image_src[0] ? s->image_src : s->iso);
    s->image_src[0] = 0;
    if (!create_window(s, 0)) return EXIT_FAILED;
    /* off-screen and never activated: rendered for layout checks without appearing on the screen */
    SetWindowLongPtrW(win, GWL_EXSTYLE, GetWindowLongPtrW(win, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(win, NULL, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(win, SW_SHOWNOACTIVATE);
    mkdir_p(folder);
    state = ST_WELCOME;
    disc_state = 0;
    shot(folder, L"1-welcome");
    wcopy(disc_name, MAXP, L"Sly 2 - Band of Thieves (Europe).iso");
    wcopy(disc_msg, 600, L"This file can't be used. This disc is SCES_524.29 (a European / PAL disc): another game or another "
                         L"region. The PC version needs Sly 2: Band of Thieves, USA release (SCUS-97316).");
    disc_state = -1;
    shot(folder, L"2-wrong-disc");
    if (iso[0]) take_image(iso);
    else { disc_state = 1; wcopy(disc_name, MAXP, L"Sly 2 - Band of Thieves (USA).iso"); wcopy(disc_msg, 600, L"Sly 2: Band of Thieves (USA). Ready to install."); }
    SendMessageW(item(ID_AGREE), BM_SETCHECK, BST_CHECKED, 0);
    shot(folder, L"3-disc-ok");
    state = ST_WORKING;
    prog_pm = 520;
    wcopy(prog_status, 512, L"Compiling the game (61 of 135 files)");
    wcopy(prog_detail, 256, L"61 of 135 files");
    shot(folder, L"4-working");
    state = ST_ERROR;
    result = EXIT_FAILED;
    wcopy(err_msg, 2600, L"Not enough free disk space: 4.1 GB is needed on this drive (the copy of your disc image, then building "
                         L"the game), 2.3 GB is free. Free up some space and click Retry.");
    shot(folder, L"5-error");
    state = ST_READY;
    strcpy(game_ver, "0.1.1300 abc1234");
    strcpy(prev_ver, "0.1.1250 def5678");
    wcopy(notice, 600, L"All set! Your disc image is no longer needed: the game has its own copy.");
    notice_kind = 1;
    upd_state = UPD_NEWER;
    strcpy(upd.m.version, "0.1.1320");
    shot(folder, L"6-ready-update");
    notice[0] = 0;
    prev_ver[0] = 0;
    upd_state = UPD_FAILED;
    shot(folder, L"7-ready-offline");
    DestroyWindow(win);
    pump();
    return EXIT_OK;
}
