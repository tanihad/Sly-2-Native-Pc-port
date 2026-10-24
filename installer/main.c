/* Sly2-Setup: command line and the wizard window (setup.h).
 *
 *   Sly2-Setup.exe                         the wizard
 *   Sly2-Setup.exe /silent /iso=<image> [/dir=<folder>] [options]       no window (CI, tests)
 *   Uninstall.exe [/uninstall] [/silent] [/deletesaves]                 remove the game
 *
 * Options: /dir= /iso= /from=<folder with the toolchain> /mirror=<URL or folder to download from>
 * /offline (never download) /nodesktop /keeptools /keepbuild /play /deletesaves
 * Test overrides (nothing of the player's is touched): /startmenu=<folder> /desktopdir=<folder>
 * /userdata=<the game's user data folder> /regkey=<HKCU subkey for the uninstall entry> /log=<file>
 * Tools: /version, /payload, /sha256 <file>, /checkdisc <image>, /download <url> <file> [sha256 [size]],
 * /uitest (the wizard's pages driven without showing the window).
 */
#include "setup.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

/* ---- the wizard ------------------------------------------------------------------------------- */

enum { P_WELCOME, P_LEGAL, P_FOLDER, P_DISC, P_READY, P_PROGRESS, P_FINISH, P_COUNT };
enum {
    ID_BACK = 1001, ID_NEXT, ID_CANCEL, ID_SHOWLOG,
    ID_WELCOME, ID_LEGALTEXT, ID_AGREE,
    ID_DIRLABEL, ID_DIR, ID_BROWSEDIR, ID_SPACE, ID_DIRNOTE,
    ID_ISOLABEL, ID_ISO, ID_BROWSEISO, ID_DISCSTATUS, ID_DISCHELP,
    ID_SUMMARY,
    ID_STATUS, ID_PROGRESS, ID_DETAIL, ID_LOGLIST,
    ID_DONE, ID_CLEAN, ID_KEEPTOOLS, ID_DESKTOP, ID_PLAY,
};
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_LOG (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)
#define WM_APP_FINISHED (WM_APP + 4)

static HWND win;
static HINSTANCE inst;
static int page, dpi = 96, running, result = -1, disc_ok, disc_checked, start_page, exit_code = EXIT_CANCELLED;
static HFONT font, font_bold, font_title;
static HBRUSH brush_white;
static ITaskbarList3* taskbar;
static struct { HWND h; int page; int id; } ctl[64];
static int nctl;
static struct { CRITICAL_SECTION lock; int pm; wchar_t status[512], detail[256]; int pending; } prog;

static const wchar_t* titles[P_COUNT][2] = {
    { L"Welcome", L"Sly 2: Band of Thieves for Windows (native PC port)" },
    { L"Your copy of the game", L"Please read this before you continue" },
    { L"Install folder", L"Where Setup puts the game" },
    { L"Your disc image", L"The game is built from your own Sly 2 disc" },
    { L"Ready to install", L"Setup has everything it needs" },
    { L"Installing", L"Building Sly 2 for this PC" },
    { L"Done", L"Sly 2: Band of Thieves is installed" },
};

static int S_(int v) { return MulDiv(v, dpi, 96); }

static HWND item(int id) {
    for (int i = 0; i < nctl; i++) if (ctl[i].id == id) return ctl[i].h;
    return NULL;
}

static HWND add(int pg, int id, const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, S_(x), S_(y), S_(w), S_(h), win, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    if (nctl < 64) { ctl[nctl].h = c; ctl[nctl].page = pg; ctl[nctl].id = id; nctl++; }
    return c;
}

static void set_text(int id, const wchar_t* t) { SetWindowTextW(item(id), t); }

static void get_text(int id, wchar_t* out) {
    GetWindowTextW(item(id), out, MAXP);
    /* (a path pasted with quotes) */
    size_t n = wcslen(out);
    if (n >= 2 && out[0] == L'"' && out[n - 1] == L'"') { memmove(out, out + 1, (n - 2) * sizeof(wchar_t)); out[n - 2] = 0; }
}

static int checked(int id) { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void check(int id, int on) { SendMessageW(item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

static void update_space(void) {
    wchar_t d[MAXP], a[32], b[32], t[512];
    get_text(ID_DIR, d);
    full_path(d);
    wcopy(S.dir, MAXP, d);
    fmt_size(a, 32, setup_space_needed(&S));
    fmt_size(b, 32, disk_free(d));
    wfmt(t, 512, L"Setup needs %ls of free space on this drive while it builds the game (%ls free). Afterwards the "
                 L"game takes about %d MB, plus about %d MB if you keep the build tools for updates.",
         a, b, SETUP_NEED_GAME_MB, SETUP_KEEP_TOOLS_MB);
    set_text(ID_SPACE, t);
}

static void update_summary(void) {
    wchar_t t[4096], a[32], src[MAXP + 128];
    fmt_size(a, 32, setup_space_needed(&S));
    if (payload_find(SETUP_PINS[0].file8)) wcopy(src, MAXP + 128, L"included in this setup program (nothing is downloaded)");
    else if (S.from[0]) wfmt(src, MAXP + 128, L"from %ls", S.from);
    else wfmt(src, MAXP + 128, L"downloaded from GitHub and PyPI (about %d MB; each file is checked against its "
                               L"published SHA-256)", SETUP_DOWNLOAD_MB);
    wfmt(t, 4096, L"Install folder:\r\n    %ls\r\n\r\nDisc image:\r\n    %ls\r\n\r\nBuild tools (compiler, Python):\r\n    %ls\r\n\r\n"
                  L"Disk space needed while building: %ls\r\n\r\nBuilding takes about 5 to 20 minutes, depending on your PC. "
                  L"You can keep using the PC meanwhile.%ls",
         S.dir, S.iso, src, a, S.existing ? L"\r\n\r\nThe existing installation is updated; your saves and settings are kept." : L"");
    set_text(ID_SUMMARY, t);
}

static void show_page(int p) {
    page = p;
    for (int i = 0; i < nctl; i++) if (ctl[i].page >= 0) ShowWindow(ctl[i].h, ctl[i].page == p ? SW_SHOW : SW_HIDE);
    ShowWindow(item(ID_BACK), p == P_WELCOME || p >= P_PROGRESS ? SW_HIDE : SW_SHOW);
    ShowWindow(item(ID_SHOWLOG), p >= P_PROGRESS ? SW_SHOW : SW_HIDE);
    EnableWindow(item(ID_BACK), p > start_page);
    const wchar_t* next = p == P_READY ? (S.existing ? L"&Update" : L"&Install") : p == P_FINISH ? L"&Finish" : L"&Next >";
    set_text(ID_NEXT, next);
    EnableWindow(item(ID_NEXT), p == P_LEGAL ? checked(ID_AGREE) : p != P_PROGRESS);
    ShowWindow(item(ID_CANCEL), p == P_FINISH ? SW_HIDE : SW_SHOW);
    set_text(ID_CANCEL, L"Cancel");
    EnableWindow(item(ID_CANCEL), TRUE);
    if (p == P_FOLDER) update_space();
    if (p == P_READY) update_summary();
    RECT r = { 0, 0, S_(620), S_(64) };
    InvalidateRect(win, &r, TRUE);
    if (p != P_PROGRESS) SetFocus(item(ID_NEXT));
    logw(L"[ui] page %d", p);
}

static void browse(int id, int folder) {
    IFileOpenDialog* d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&d))) return;
    DWORD o = 0;
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC f[] = { { L"PS2 disc image (*.iso)", L"*.iso" }, { L"All files (*.*)", L"*.*" } };
        IFileOpenDialog_SetFileTypes(d, 2, f);
        IFileOpenDialog_SetTitle(d, L"Choose your Sly 2: Band of Thieves disc image (.iso)");
    } else IFileOpenDialog_SetTitle(d, L"Choose the install folder");
    if (SUCCEEDED(IFileOpenDialog_Show(d, win))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                wchar_t v[MAXP];
                wcopy(v, MAXP, p);
                if (folder) {                        /* a folder picked for the game: <it>\Sly2 unless empty-named */
                    const wchar_t* leaf = wcsrchr(v, L'\\');
                    if (!leaf || _wcsicmp(leaf + 1, L"Sly2")) wcsncat(v, v[wcslen(v) - 1] == L'\\' ? L"Sly2" : L"\\Sly2", MAXP - wcslen(v) - 1);
                }
                set_text(id, v);
                CoTaskMemFree(p);
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(d);
}

static int check_disc(void) {
    wchar_t iso[MAXP], why[512];
    get_text(ID_ISO, iso);
    full_path(iso);
    wcopy(S.iso, MAXP, iso);
    disc_checked = 1;
    if (!iso[0]) { disc_ok = 0; set_text(ID_DISCSTATUS, L"Choose the .iso file of your disc."); return 0; }
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int r = disc_check_iso(iso, why, 512);
    SetCursor(old);
    disc_ok = r == DISC_OK;
    set_text(ID_DISCSTATUS, why);
    logw(L"[ui] disc %ls: %ls", iso, why);
    InvalidateRect(item(ID_DISCSTATUS), NULL, TRUE);
    return disc_ok;
}

/* Can a folder be created here (nearest existing parent writable)? Nothing is left behind. */
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

static void overrides_args(wchar_t* cmd, size_t cap) {
    quote_arg(cmd, cap, L"from", S.from);
    quote_arg(cmd, cap, L"mirror", S.mirror);
    quote_arg(cmd, cap, L"startmenu", S.startmenu);
    quote_arg(cmd, cap, L"desktopdir", S.desktop);
    quote_arg(cmd, cap, L"userdata", S.userdata);
    if (_wcsicmp(S.regkey, setup_default_regkey())) quote_arg(cmd, cap, L"regkey", S.regkey);
    if (S.offline_only) wcsncat(cmd, L" /offline", cap - wcslen(cmd) - 1);
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

static int folder_next(void) {
    wchar_t d[MAXP];
    get_text(ID_DIR, d);
    full_path(d);
    if (wcslen(d) < 3 || d[1] != L':' || d[2] != L'\\') {
        if (!(d[0] == L'\\' && d[1] == L'\\')) { setup_message(&S, L"Enter a full folder path, for example C:\\Games\\Sly2.", MB_ICONWARNING); return 0; }
    }
    wcopy(S.dir, MAXP, d);
    set_text(ID_DIR, d);
    DWORD err;
    if (!path_writable(d, &err)) {
        if (err == ERROR_ACCESS_DENIED && !is_elevated()) {
            wchar_t q[MAXP + 400];
            wfmt(q, MAXP + 400, L"Installing into\n%ls\nneeds administrator rights (it is a folder for all users).\n\nRestart "
                                L"Setup as administrator? (Or choose a folder of your own, such as the suggested one.)", d);
            if (setup_ask(&S, q, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                wchar_t args[MAXP * 8];
                wfmt(args, MAXP * 8, L"/allusers /page=disc \"/dir=%ls\"", d);
                if (S.iso[0]) quote_arg(args, MAXP * 8, L"iso", S.iso);
                overrides_args(args, MAXP * 8);
                if (restart_elevated(args)) { exit_code = EXIT_OK; DestroyWindow(win); }
            }
            return 0;
        }
        wchar_t m[MAXP + 200];
        wfmt(m, MAXP + 200, L"Setup cannot create a folder there (error %lu). Choose another folder.", err);
        setup_message(&S, m, MB_ICONWARNING);
        return 0;
    }
    uint64_t need = setup_space_needed(&S), have = disk_free(d);
    if (have < need) {
        wchar_t a[32], b[32], m[512];
        fmt_size(a, 32, need);
        fmt_size(b, 32, have);
        wfmt(m, 512, L"There is not enough free space on this drive: Setup needs %ls while it builds the game, and %ls is "
                     L"free. Free up some space or choose a folder on another drive.", a, b);
        setup_message(&S, m, MB_ICONWARNING);
        return 0;
    }
    wchar_t st[MAXP];
    wpath(st, MAXP, d, L"setup-state.ini");
    S.existing = file_exists(st);
    if (!S.existing && dir_exists(d)) {
        wchar_t pat[MAXP];
        wpath(pat, MAXP, d, L"*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        int n = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) n++; while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (n && setup_ask(&S, L"This folder is not empty. Install Sly 2 into it anyway?\n\n(Uninstalling later removes only "
                               L"the files Setup put there.)", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return 0;
    }
    return 1;
}

static void install_progress(void* ui, int pm, const wchar_t* status, const wchar_t* detail) {
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

static void install_logline(void* ui, const wchar_t* line) {
    (void)ui;
    PostMessageW(win, WM_APP_LOG, 0, (LPARAM)_wcsdup(line));
}

static DWORD WINAPI install_thread(void* arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int r = setup_install(&S);
    CoUninitialize();
    PostMessageW(win, WM_APP_DONE, (WPARAM)r, 0);
    return 0;
}

static DWORD WINAPI finish_thread(void* arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    setup_finish(&S);
    CoUninitialize();
    PostMessageW(win, WM_APP_FINISHED, 0, 0);
    return 0;
}

static void start_install(void) {
    show_page(P_PROGRESS);
    SendMessageW(item(ID_PROGRESS), PBM_SETPOS, 0, 0);
    set_text(ID_STATUS, L"Starting...");
    set_text(ID_DETAIL, L"");
    running = 1;
    result = -1;
    EnableWindow(item(ID_NEXT), FALSE);
    set_text(ID_NEXT, L"&Next >");
    S.progress = install_progress;
    S.logline = install_logline;
    if (taskbar) ITaskbarList3_SetProgressState(taskbar, win, TBPF_NORMAL);
    HANDLE t = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void on_next(void) {
    switch (page) {
    case P_WELCOME: show_page(P_LEGAL); break;
    case P_LEGAL: if (checked(ID_AGREE)) show_page(P_FOLDER); break;
    case P_FOLDER: if (folder_next()) show_page(P_DISC); break;
    case P_DISC: if (check_disc()) show_page(P_READY); break;
    case P_READY: start_install(); break;
    case P_PROGRESS: if (!running && result != EXIT_OK) start_install(); break;   /* Retry */
    case P_FINISH:
        S.desktop_shortcut = checked(ID_DESKTOP);
        S.keep_build = !checked(ID_CLEAN);
        S.keep_tools = checked(ID_KEEPTOOLS);
        S.play = checked(ID_PLAY);
        for (int id = ID_NEXT; id <= ID_SHOWLOG; id++) EnableWindow(item(id), FALSE);
        set_text(ID_DONE, L"Finishing...");
        {
            HANDLE t = CreateThread(NULL, 0, finish_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
        }
        break;
    }
}

static void on_cancel(void) {
    if (page == P_FINISH) { on_next(); return; }
    if (page == P_PROGRESS && running) {
        if (setup_ask(&S, L"Stop the installation?\n\nWhat is downloaded so far is kept: running Setup again continues from "
                          L"there.", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        InterlockedExchange(&S.cancel, 1);
        EnterCriticalSection(&S.lock);
        if (S.job) TerminateJobObject(S.job, 1);
        LeaveCriticalSection(&S.lock);
        EnableWindow(item(ID_CANCEL), FALSE);
        set_text(ID_STATUS, L"Stopping...");
        return;
    }
    if (page == P_PROGRESS || setup_ask(&S, L"Exit Setup? Sly 2 is not installed yet.", MB_YESNO | MB_ICONQUESTION) == IDYES)
        DestroyWindow(win);
}

static void on_done(int r) {
    running = 0;
    result = r;
    if (taskbar) ITaskbarList3_SetProgressState(taskbar, win, r == EXIT_OK ? TBPF_NOPROGRESS : TBPF_ERROR);
    if (r == EXIT_OK) {
        exit_code = EXIT_OK;
        wchar_t t[2048], a[32], b[32];
        fmt_size(a, 32, S.used_build);
        fmt_size(b, 32, S.used_tools);
        wfmt(t, 2048, L"Sly 2: Band of Thieves is installed in\r\n%ls\r\n\r\nStart it from the Start menu (\"Sly 2 - Band of "
                      L"Thieves\"). It plays from your disc image, so keep the image where it is:\r\n%ls", S.dir, S.iso);
        set_text(ID_DONE, t);
        wfmt(t, 2048, L"Free up disk space: remove the build files (%ls)", a);
        set_text(ID_CLEAN, t);
        wfmt(t, 2048, L"Keep the build tools for faster updates (%ls; otherwise Setup downloads them again)", b);
        set_text(ID_KEEPTOOLS, t);
        check(ID_CLEAN, 1);
        check(ID_KEEPTOOLS, S.keep_tools);
        check(ID_DESKTOP, S.desktop_shortcut);
        check(ID_PLAY, !S.uitest);
        show_page(P_FINISH);
        return;
    }
    wchar_t t[2600];
    if (r == EXIT_CANCELLED) {
        exit_code = EXIT_CANCELLED;
        wcopy(t, 2600, L"Installation stopped. Click Retry to continue, or Close.");
    } else if (r == EXIT_NEEDS_ADMIN) {
        exit_code = r;
        wfmt(t, 2600, L"%ls Go back and choose another folder, or run Setup as administrator.", S.error);
    } else {
        exit_code = r;
        wfmt(t, 2600, L"Setup could not finish:\r\n%ls", S.error[0] ? S.error : L"(see the log)");
    }
    set_text(ID_STATUS, t);
    set_text(ID_DETAIL, L"");
    set_text(ID_NEXT, L"&Retry");
    EnableWindow(item(ID_NEXT), r != EXIT_NEEDS_ADMIN);
    set_text(ID_CANCEL, L"&Close");
    EnableWindow(item(ID_CANCEL), TRUE);
    InvalidateRect(item(ID_STATUS), NULL, TRUE);
}

static void create_controls(void) {
    NONCLIENTMETRICSW m;
    m.cbSize = sizeof m;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0);
    LOGFONTW lf = m.lfMessageFont;
    font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    font_bold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 3 / 2;
    font_title = CreateFontIndirectW(&lf);
    const int X = 28, W = 564, Y = 84;

    add(-1, ID_SHOWLOG, L"BUTTON", L"Show &log", WS_TABSTOP | BS_PUSHBUTTON, 0, 16, 412, 88, 26);
    add(-1, ID_BACK, L"BUTTON", L"< &Back", WS_TABSTOP | BS_PUSHBUTTON, 0, 330, 412, 88, 26);
    add(-1, ID_NEXT, L"BUTTON", L"&Next >", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 422, 412, 88, 26);
    add(-1, ID_CANCEL, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 516, 412, 88, 26);
    for (int i = 0; i < 4; i++) ShowWindow(ctl[i].h, SW_SHOW);

    wchar_t t[4096];
    wfmt(t, 4096,
         L"This sets up Sly 2: Band of Thieves (2004) as a native Windows game: higher resolutions and widescreen, "
         L"controllers (DualSense, DualShock 4, Xbox) and keyboard, an Options menu. It is an unofficial fan project.\r\n\r\n"
         L"The game itself is not included. Setup builds it on this PC from your own copy: an .iso disc image of "
         L"Sly 2: Band of Thieves (USA, SCUS-97316). It %ls, translates the game's code for Windows and installs the "
         L"game with a Start menu shortcut and an uninstaller.\r\n\r\n"
         L"This takes about 5 to 20 minutes. No administrator rights are needed.%ls",
         payload_find(SETUP_PINS[0].file8) ? L"uses the build tools included in this setup program (no internet needed)"
                                           : L"downloads the free build tools it needs (about " SETUP_DOWNLOAD_MB_STR L" MB)",
         S.existing ? L"\r\n\r\nSly 2 is already installed on this PC: Setup updates it. Your saves and settings are kept." : L"");
    add(P_WELCOME, ID_WELCOME, L"STATIC", t, 0, 0, X, Y, W, 300);

    add(P_LEGAL, ID_LEGALTEXT, L"EDIT",
        L"Sly 2: Band of Thieves is a trademark and copyright of Sucker Punch Productions and Sony Interactive "
        L"Entertainment. This is an unofficial fan project, not affiliated with or endorsed by them.\r\n\r\n"
        L"This setup program contains no game code, data or artwork: only the project's own recompiler, the PC runtime "
        L"and (in the offline setup) free build tools. You need your own, legally obtained disc of Sly 2: Band of Thieves "
        L"(USA, SCUS-97316) as an .iso image.\r\n\r\n"
        L"The program Setup builds (sly2.exe) contains code translated from your copy of the game. Use it on your own "
        L"PCs; do not share it publicly. The game reads its data from your disc image while you play.\r\n\r\n"
        L"The build tools come from their publishers: llvm-mingw (LLVM, Apache 2.0 with LLVM exception; MinGW-w64), uv and "
        L"Python (python-build-standalone; PSF license), rabbitizer and pyelftools (PyPI). They are installed only inside "
        L"the install folder and can be removed after the build.\r\n\r\n"
        L"The software is provided as is, without warranty of any kind.",
        WS_VSCROLL | ES_MULTILINE | ES_READONLY | WS_TABSTOP, WS_EX_CLIENTEDGE, X, Y, W, 240);
    add(P_LEGAL, ID_AGREE, L"BUTTON", L"I have my own copy of the game, and I will not share the program built from it",
        WS_TABSTOP | BS_AUTOCHECKBOX, 0, X, Y + 256, W, 24);

    add(P_FOLDER, ID_DIRLABEL, L"STATIC", L"Setup installs Sly 2 into this folder:", 0, 0, X, Y, W, 20);
    add(P_FOLDER, ID_DIR, L"EDIT", S.dir, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, X, Y + 24, W - 100, 24);
    add(P_FOLDER, ID_BROWSEDIR, L"BUTTON", L"B&rowse...", WS_TABSTOP | BS_PUSHBUTTON, 0, X + W - 92, Y + 23, 92, 26);
    add(P_FOLDER, ID_SPACE, L"STATIC", L"", 0, 0, X, Y + 64, W, 60);
    add(P_FOLDER, ID_DIRNOTE, L"STATIC",
        L"The suggested folder is in your user profile and needs no administrator rights. A folder in Program Files "
        L"installs the game for all users of this PC and asks for administrator rights.\r\n\r\nYour saves and settings are "
        L"kept separately, in %LOCALAPPDATA%\\Sly2.",
        0, 0, X, Y + 136, W, 100);

    add(P_DISC, ID_ISOLABEL, L"STATIC", L"Your Sly 2: Band of Thieves disc image (.iso):", 0, 0, X, Y, W, 20);
    add(P_DISC, ID_ISO, L"EDIT", S.iso, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, X, Y + 24, W - 100, 24);
    add(P_DISC, ID_BROWSEISO, L"BUTTON", L"B&rowse...", WS_TABSTOP | BS_PUSHBUTTON, 0, X + W - 92, Y + 23, 92, 26);
    add(P_DISC, ID_DISCSTATUS, L"STATIC", L"", 0, 0, X, Y + 60, W, 48);
    add(P_DISC, ID_DISCHELP, L"STATIC",
        L"Make the image from your own disc with a disc imaging program. It must be the USA release (SCUS-97316) as an "
        L"unmodified .iso; .bin/.cue, .chd and .cso images must be converted to .iso first. Setup checks the image before "
        L"it continues.\r\n\r\nThe game reads the image while you play, so keep it where it is (if you move it, the game "
        L"asks for it again).",
        0, 0, X, Y + 120, W, 110);

    add(P_READY, ID_SUMMARY, L"STATIC", L"", 0, 0, X, Y, W, 300);

    SendMessageW(add(P_PROGRESS, ID_STATUS, L"STATIC", L"", 0, 0, X, Y, W, 52), WM_SETFONT, (WPARAM)font_bold, FALSE);
    add(P_PROGRESS, ID_PROGRESS, PROGRESS_CLASSW, L"", 0, 0, X, Y + 56, W, 20);
    SendMessageW(item(ID_PROGRESS), PBM_SETRANGE32, 0, 1000);
    add(P_PROGRESS, ID_DETAIL, L"STATIC", L"", SS_ENDELLIPSIS, 0, X, Y + 82, W, 20);
    add(P_PROGRESS, ID_LOGLIST, L"LISTBOX", L"", WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, WS_EX_CLIENTEDGE,
        X, Y + 108, W, 192);

    add(P_FINISH, ID_DONE, L"STATIC", L"", 0, 0, X, Y, W, 116);
    add(P_FINISH, ID_CLEAN, L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 0, X, Y + 128, W, 24);
    add(P_FINISH, ID_KEEPTOOLS, L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, 0, X, Y + 156, W, 36);
    add(P_FINISH, ID_DESKTOP, L"BUTTON", L"Create a &desktop shortcut", WS_TABSTOP | BS_AUTOCHECKBOX, 0, X, Y + 198, W, 24);
    add(P_FINISH, ID_PLAY, L"BUTTON", L"&Play now", WS_TABSTOP | BS_AUTOCHECKBOX, 0, X, Y + 226, W, 24);
    SendMessageW(item(ID_ISO), EM_SETCUEBANNER, TRUE, (LPARAM)L"for example D:\\Games\\Sly 2 - Band of Thieves (USA).iso");
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_NEXT || id == IDOK) { if (IsWindowEnabled(item(ID_NEXT))) on_next(); }
        else if (id == ID_BACK) { if (page > start_page && page < P_PROGRESS) show_page(page - 1); }
        else if (id == ID_CANCEL || id == IDCANCEL) { if (IsWindowEnabled(item(ID_CANCEL))) on_cancel(); }
        else if (id == ID_AGREE) EnableWindow(item(ID_NEXT), checked(ID_AGREE));
        else if (id == ID_BROWSEDIR) browse(ID_DIR, 1);
        else if (id == ID_BROWSEISO) { browse(ID_ISO, 0); check_disc(); }
        else if (id == ID_DIR && code == EN_CHANGE && page == P_FOLDER) update_space();
        else if (id == ID_ISO && code == EN_CHANGE && disc_checked) { disc_checked = 0; disc_ok = 0; set_text(ID_DISCSTATUS, L""); }
        else if (id == ID_SHOWLOG && log_path()[0]) ShellExecuteW(h, L"open", L"notepad.exe", log_path(), NULL, SW_SHOWNORMAL);
        return 0;
    }
    case WM_APP_PROGRESS: {
        EnterCriticalSection(&prog.lock);
        int pm = prog.pm;
        wchar_t st[512], de[256];
        wcopy(st, 512, prog.status);
        wcopy(de, 256, prog.detail);
        prog.pending = 0;
        LeaveCriticalSection(&prog.lock);
        if (running) {
            SendMessageW(item(ID_PROGRESS), PBM_SETPOS, (WPARAM)pm, 0);
            wchar_t cur[512];
            GetWindowTextW(item(ID_STATUS), cur, 512);
            if (wcscmp(cur, st) && !S.cancel) SetWindowTextW(item(ID_STATUS), st);
            GetWindowTextW(item(ID_DETAIL), cur, 256);
            if (wcscmp(cur, de)) SetWindowTextW(item(ID_DETAIL), de);
            if (taskbar) ITaskbarList3_SetProgressValue(taskbar, win, (ULONGLONG)pm, 1000);
        }
        return 0;
    }
    case WM_APP_LOG: {
        wchar_t* line = (wchar_t*)lp;
        HWND lb = item(ID_LOGLIST);
        if (line && lb) {
            if (SendMessageW(lb, LB_GETCOUNT, 0, 0) > 800) SendMessageW(lb, LB_DELETESTRING, 0, 0);
            LRESULT i = SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line);
            SendMessageW(lb, LB_SETTOPINDEX, (WPARAM)i, 0);
            HDC dc = GetDC(lb);
            SelectObject(dc, font);
            SIZE sz;
            GetTextExtentPoint32W(dc, line, (int)wcslen(line), &sz);
            ReleaseDC(lb, dc);
            if (sz.cx + 8 > SendMessageW(lb, LB_GETHORIZONTALEXTENT, 0, 0)) SendMessageW(lb, LB_SETHORIZONTALEXTENT, (WPARAM)(sz.cx + 8), 0);
        }
        free(line);
        return 0;
    }
    case WM_APP_DONE: on_done((int)wp); return 0;
    case WM_APP_FINISHED: exit_code = EXIT_OK; DestroyWindow(h); return 0;
    case WM_TIMER:                                   /* /uicancel=<s>: Cancel (answered yes) while it runs */
        KillTimer(h, wp);
        if (wp == 77 && running) {
            loga("[uitest] cancel");
            InterlockedExchange(&S.cancel, 1);
            EnterCriticalSection(&S.lock);
            if (S.job) TerminateJobObject(S.job, 1);
            LeaveCriticalSection(&S.lock);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if (c == item(ID_DISCSTATUS) && disc_checked) SetTextColor(dc, disc_ok ? RGB(0, 120, 40) : RGB(200, 0, 0));
        else if (c == item(ID_STATUS) && !running && result > 0) SetTextColor(dc, result == EXIT_CANCELLED ? GetSysColor(COLOR_WINDOWTEXT) : RGB(200, 0, 0));
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        if (c == item(ID_LEGALTEXT)) return (LRESULT)brush_white;
        return (LRESULT)brush_white;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, brush_white);
        RECT bar = { 0, S_(400), r.right, r.bottom };
        FillRect((HDC)wp, &bar, GetSysColorBrush(COLOR_BTNFACE));
        RECT line = { 0, S_(64), r.right, S_(64) + 1 };
        FillRect((HDC)wp, &line, GetSysColorBrush(COLOR_3DSHADOW));
        line.top = S_(400);
        line.bottom = line.top + 1;
        FillRect((HDC)wp, &line, GetSysColorBrush(COLOR_3DLIGHT));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SelectObject(dc, font_title);
        RECT r = { S_(28), S_(10), S_(540), S_(38) };
        DrawTextW(dc, titles[page][0], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, font);
        r.top = S_(38);
        r.bottom = S_(58);
        DrawTextW(dc, titles[page][1], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        HICON ic = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S_(40), S_(40), 0);
        if (ic) { DrawIconEx(dc, S_(620 - 28 - 40), S_(12), ic, S_(40), S_(40), 0, NULL, DI_NORMAL); DestroyIcon(ic); }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE: if (IsWindowEnabled(item(ID_CANCEL)) || page == P_FINISH) on_cancel(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg == RegisterWindowMessageW(L"TaskbarButtonCreated") && !taskbar)
        CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void**)&taskbar);
    return DefWindowProcW(h, msg, wp, lp);
}

static int create_window(int show) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&prog.lock);
    HDC sdc = GetDC(NULL);
    dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    brush_white = GetSysColorBrush(COLOR_WINDOW);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"Sly2Setup";
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S_(620), S_(450) };
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    win = CreateWindowExW(0, wc.lpszClassName, L"Sly 2: Band of Thieves Setup", style, wa.left + (wa.right - wa.left - w) / 2,
                          wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt, NULL, NULL, inst, NULL);
    if (!win) return 0;
    create_controls();
    show_page(start_page);
    if (show) { ShowWindow(win, SW_SHOW); UpdateWindow(win); SetForegroundWindow(win); }
    return 1;
}

static int run_wizard(void) {
    if (!create_window(1)) return EXIT_FAILED;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(win, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (taskbar) ITaskbarList3_Release(taskbar);
    return exit_code;
}

/* /uitest: the pages driven through their handlers in a window that is never shown. */
static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

static int ui_install, ui_cancel_after;              /* /uiinstall, /uicancel=<seconds> */

static int ui_test(const wchar_t* bad_iso) {
    int fails = 0;
#define EXPECT(c, what) do { if (c) loga("[uitest] ok: %s", what); else { loga("[uitest] FAIL: %s", what); fails++; } } while (0)
    wchar_t iso[MAXP];
    wcopy(iso, MAXP, S.iso);
    S.iso[0] = 0;
    if (!create_window(0)) return EXIT_FAILED;
    EXPECT(page == P_WELCOME && !IsWindowVisible(win), "starts on the welcome page, hidden");
    SendMessageW(win, WM_COMMAND, ID_NEXT, 0);
    EXPECT(page == P_LEGAL, "next -> legal page");
    EXPECT(!IsWindowEnabled(item(ID_NEXT)), "next disabled until the box is ticked");
    check(ID_AGREE, 1);
    SendMessageW(win, WM_COMMAND, ID_AGREE, 0);
    EXPECT(IsWindowEnabled(item(ID_NEXT)), "next enabled after ticking");
    SendMessageW(win, WM_COMMAND, ID_NEXT, 0);
    EXPECT(page == P_FOLDER, "-> folder page");
    wchar_t sp[512];
    GetWindowTextW(item(ID_SPACE), sp, 512);
    EXPECT(wcsstr(sp, L"free") != NULL, "space line shown");
    SendMessageW(win, WM_COMMAND, ID_NEXT, 0);
    EXPECT(page == P_DISC, "-> disc page");
    if (bad_iso[0]) {
        set_text(ID_ISO, bad_iso);
        SendMessageW(win, WM_COMMAND, ID_NEXT, 0);
        wchar_t st[512];
        GetWindowTextW(item(ID_DISCSTATUS), st, 512);
        logw(L"[uitest] bad image says: %ls", st);
        EXPECT(page == P_DISC && !disc_ok && st[0], "a wrong image is refused with a message");
    }
    set_text(ID_ISO, iso);
    SendMessageW(win, WM_COMMAND, ID_NEXT, 0);
    EXPECT(page == P_READY && disc_ok, "the right image -> ready page");
    wchar_t sum[4096];
    GetWindowTextW(item(ID_SUMMARY), sum, 4096);
    EXPECT(wcsstr(sum, iso) != NULL, "summary names the image");
    SendMessageW(win, WM_COMMAND, ID_BACK, 0);
    EXPECT(page == P_DISC, "back -> disc page");
    if (ui_install) {                                /* the real installation through the wizard */
        SendMessageW(win, WM_COMMAND, ID_NEXT, 0);
        SendMessageW(win, WM_COMMAND, ID_NEXT, 0);
        EXPECT(page == P_PROGRESS && running, "Install starts the installation");
        if (ui_cancel_after > 0) SetTimer(win, 77, (UINT)ui_cancel_after * 1000, NULL);
        MSG m;
        while (result < 0 && GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (ui_cancel_after > 0) {
            EXPECT(result == EXIT_CANCELLED && page == P_PROGRESS && IsWindowEnabled(item(ID_NEXT)), "Cancel stops it; Retry offered");
            SendMessageW(win, WM_COMMAND, ID_NEXT, 0);   /* Retry */
            EXPECT(running, "Retry starts again");
            while (result < 0 && GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        EXPECT(result == EXIT_OK && page == P_FINISH, "installed through the wizard");
        wchar_t done[2048];
        GetWindowTextW(item(ID_DONE), done, 2048);
        EXPECT(wcsstr(done, S.dir) != NULL, "the Done page names the folder");
        check(ID_PLAY, 0);
        SendMessageW(win, WM_COMMAND, ID_NEXT, 0);       /* Finish: clean-up, then the window closes */
        while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        EXPECT(exit_code == EXIT_OK, "Finish closes the wizard");
        loga("[uitest] %d failure(s)", fails);
        return fails ? EXIT_FAILED : EXIT_OK;
    }
    S.used_build = 300u << 20;
    S.used_tools = 900u << 20;
    on_done(EXIT_FAILED);
    EXPECT(IsWindowEnabled(item(ID_NEXT)), "an error offers Retry");
    on_done(EXIT_OK);
    EXPECT(page == P_FINISH && checked(ID_CLEAN) && checked(ID_DESKTOP) && !checked(ID_KEEPTOOLS), "finish page defaults");
    pump();
    DestroyWindow(win);
    pump();
    loga("[uitest] %d failure(s)", fails);
    return fails ? EXIT_FAILED : EXIT_OK;
}

/* /uishot=<folder>: every page rendered into <folder>\page<N>.bmp, the window off-screen and never
 * activated (layout checks without showing anything on the owner's screen). */
static void save_bmp(const wchar_t* path) {
    RECT r;
    GetWindowRect(win, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
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
    PrintWindow(win, mdc, 2 /* PW_RENDERFULLCONTENT */);
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

static int ui_shots(const wchar_t* folder) {
    if (!create_window(0)) return EXIT_FAILED;
    SetWindowLongPtrW(win, GWL_EXSTYLE, GetWindowLongPtrW(win, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(win, NULL, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(win, SW_SHOWNOACTIVATE);
    mkdir_p(folder);
    wchar_t p[MAXP];
    for (int pg = 0; pg < P_COUNT; pg++) {
        if (pg == P_DISC) { set_text(ID_ISO, S.iso); check_disc(); }
        if (pg == P_LEGAL) check(ID_AGREE, 1);
        show_page(pg);
        if (pg == P_PROGRESS) {
            running = 1;
            SendMessageW(item(ID_PROGRESS), PBM_SETPOS, 520, 0);
            set_text(ID_STATUS, L"Compiling the game (61 of 135 files)");
            set_text(ID_DETAIL, L"61 of 135 files");
            static const wchar_t* lines[] = { L"== Installing the recompiler and the PC runtime", L"== Getting llvm-mingw 20261006",
                                              L"llvm-mingw-20261006-ucrt-x86_64.zip: SHA-256 OK", L"== Unpacking llvm-mingw 20261006",
                                              L"llvm-mingw 20261006: installed", L"== Building the game", L"Translating the game's code (MIPS -> C)" };
            for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) SendMessageW(win, WM_APP_LOG, 0, (LPARAM)_wcsdup(lines[i]));
        }
        if (pg == P_FINISH) { running = 0; S.used_build = 302u << 20; S.used_tools = 640u << 20; on_done(EXIT_OK); }
        RedrawWindow(win, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        for (int k = 0; k < 4; k++) { Sleep(60); pump(); }   /* (DWM composes off-screen windows late) */
        wfmt(p, MAXP, L"%ls\\page%d.bmp", folder, pg);
        save_bmp(p);
    }
    running = 0;
    show_page(P_PROGRESS);
    wcopy(S.error, 2048, L"Downloading from github.com failed: could not connect: this PC seems to be offline (or a firewall "
                         L"blocks Setup) (error 12007).");
    on_done(EXIT_FAILED);
    RedrawWindow(win, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    for (int k = 0; k < 4; k++) { Sleep(60); pump(); }
    wfmt(p, MAXP, L"%ls\\page-error.bmp", folder);
    save_bmp(p);
    DestroyWindow(win);
    pump();
    return EXIT_OK;
}

/* ---- tool commands ---------------------------------------------------------------------------- */

static int tool_command(int argc, wchar_t** argv) {
    const wchar_t* c = argv[1];
    if (opt(c, L"version", NULL)) { out(L"Sly2-Setup %ls (%ls)", SETUP_VERSION, SETUP_COMMIT); return EXIT_OK; }
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

/* ---- entry ------------------------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev;
    (void)cmdline;
    (void)show;
    inst = hi;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    setup_defaults(&S);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t v[MAXP], logfile[MAXP] = L"", bad_iso[MAXP] = L"", shots[MAXP] = L"";
    int silent = 0, uninstall = 0, uitest = 0, page_disc = 0;
    if (argc > 1) {
        int r = tool_command(argc, argv);
        if (r >= 0) return r;
    }
    /* Uninstall.exe in the install folder: uninstall mode */
    const wchar_t* leaf = wcsrchr(self_path(), L'\\');
    if (leaf && !_wcsicmp(leaf + 1, L"Uninstall.exe")) {
        uninstall = 1;
        wcopy(S.dir, MAXP, self_path());
        *wcsrchr(S.dir, L'\\') = 0;
    }
    int dir_given = 0;
    for (int i = 1; i < argc; i++) {
        const wchar_t* a = argv[i];
        if (opt(a, L"silent", NULL) || opt(a, L"quiet", NULL) || opt(a, L"S", NULL)) silent = 1;
        else if (opt(a, L"uninstall", NULL)) uninstall = 1;
        else if (opt(a, L"dir", v)) { wcopy(S.dir, MAXP, v); dir_given = 1; }
        else if (opt(a, L"iso", v)) wcopy(S.iso, MAXP, v);
        else if (opt(a, L"from", v)) wcopy(S.from, MAXP, v);
        else if (opt(a, L"mirror", v)) wcopy(S.mirror, MAXP, v);
        else if (opt(a, L"startmenu", v)) wcopy(S.startmenu, MAXP, v);
        else if (opt(a, L"desktopdir", v)) wcopy(S.desktop, MAXP, v);
        else if (opt(a, L"userdata", v)) wcopy(S.userdata, MAXP, v);
        else if (opt(a, L"regkey", v)) wcopy(S.regkey, MAXP, v);
        else if (opt(a, L"log", v)) wcopy(logfile, MAXP, v);
        else if (opt(a, L"offline", NULL)) S.offline_only = 1;
        else if (opt(a, L"nodesktop", NULL)) S.desktop_shortcut = 0;
        else if (opt(a, L"keeptools", NULL)) S.keep_tools = 1;
        else if (opt(a, L"keepbuild", NULL)) S.keep_build = 1;
        else if (opt(a, L"play", NULL)) S.play = 2;
        else if (opt(a, L"deletesaves", NULL)) S.delete_saves = 1;
        else if (opt(a, L"keepsaves", NULL)) S.delete_saves = 0;
        else if (opt(a, L"allusers", NULL)) S.allusers = 1;
        else if (opt(a, L"page", v)) page_disc = !_wcsicmp(v, L"disc");
        else if (opt(a, L"uitest", NULL)) uitest = 1;
        else if (opt(a, L"badiso", v)) wcopy(bad_iso, MAXP, v);
        else if (opt(a, L"uiinstall", NULL)) ui_install = 1;
        else if (opt(a, L"uicancel", v)) ui_cancel_after = _wtoi(v);
        else if (opt(a, L"uishot", v)) { wcopy(shots, MAXP, v); full_path(shots); }
        else {
            wchar_t m[MAXP + 64];
            wfmt(m, MAXP + 64, L"Unknown option: %ls", a);
            if (!silent) MessageBoxW(NULL, m, L"Sly 2 Setup", MB_ICONERROR);
            return EXIT_USAGE;
        }
    }
    if (logfile[0]) { log_open(logfile); S.log_fixed = 1; }
    S.silent = silent;
    S.uitest = uitest;
    full_path(S.dir);
    full_path(S.iso);
    full_path(S.from);
    full_path(S.startmenu);
    full_path(S.desktop);
    full_path(S.userdata);
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

    if (!dir_given) setup_find_existing(&S);
    else {
        wchar_t st[MAXP];
        wpath(st, MAXP, S.dir, L"setup-state.ini");
        S.existing = file_exists(st);
    }
    if (uitest) return ui_test(bad_iso);
    if (shots[0]) { S.uitest = 1; return ui_shots(shots); }

    if (silent) {
        if (!S.iso[0]) { loga("/silent needs /iso=<disc image>"); return EXIT_USAGE; }
        if (!logfile[0]) {
            wchar_t t[MAXP];
            GetTempPathW(MAXP, t);
            wcsncat(t, L"sly2-setup.log", MAXP - wcslen(t) - 1);
            log_open(t);
        }
        int play = S.play == 2;
        int r = setup_install(&S);
        if (r) { loga("setup failed (%d): %ls", r, S.error); return r; }
        S.play = play;
        setup_finish(&S);
        return EXIT_OK;
    }
    if (page_disc) start_page = P_DISC;
    if (S.allusers && !is_elevated()) S.allusers = 0;
    int r = run_wizard();
    CoUninitialize();
    return r;
}
