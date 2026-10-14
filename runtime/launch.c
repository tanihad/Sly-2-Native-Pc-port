/* Start-up and shut-down as a PC game: log file, disc image choice, user data folders. See launch.h. */
#include "launch.h"
#include "disc.h"
#include "ps2_internal.h"
#include "settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define TITLE L"Sly 2: Band of Thieves"

/* A wide path as UTF-8 for the log (printf's %ls stops at the first character outside the C
 * locale). Four rotating buffers: up to four per log line. */
static const char* u8(const wchar_t* w) {
    static char buf[4][3 * 1100];
    static int k;
    char* b = buf[k++ & 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, b, (int)sizeof buf[0], NULL, NULL)) strcpy(b, "?");
    return b;
}

static int has_console = 1;
static int show_boxes;                   /* fatal errors and questions as message boxes */

int launch_has_console(void) { return has_console; }

void launch_init(void) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    has_console = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    show_boxes = !has_console;
    const wchar_t* dir = settings_user_dir();
    if (!has_console) {                  /* GUI build started without a console: log to a file */
        wchar_t log[MAX_PATH + 80], prev[MAX_PATH + 80];
        if (settings_user_path(L"sly2.log", log, MAX_PATH + 80) &&
            settings_user_path(L"sly2.previous.log", prev, MAX_PATH + 80)) {
            MoveFileExW(log, prev, MOVEFILE_REPLACE_EXISTING);
            if (_wfreopen(log, L"w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    fprintf(stderr, "[launch] user data: %s%s\n", u8(dir), settings_portable() ? " (portable)" : "");
}

static void message_box(const char* text, UINT flags, int* answer) {
    wchar_t w[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 2048)) wcscpy(w, L"Sly 2 could not start.");
    int r = MessageBoxW(NULL, w, TITLE, flags | MB_SETFOREGROUND);
    if (answer) *answer = r;
}

void launch_fatal(const char* fmt, ...) {
    char msg[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[launch] fatal: %s\n", msg);
    fflush(stderr);
    if (show_boxes) message_box(msg, MB_OK | MB_ICONERROR, NULL);
    exit(1);
}

void launch_exit(int code) {
    /* A card operation in progress finishes first (a few ms; mc_quiesce gives up after 2 s). */
    fprintf(stderr, "[launch] exit (%d)\n", code);
    mc_quiesce();
    fflush(stderr);
    fflush(stdout);
    ExitProcess((UINT)code);
}

/* ---- the disc image ------------------------------------------------------------------------------- */

int launch_check_disc(const wchar_t* path, uint8_t** elf, size_t* size) {
    return disc_check(path, LAUNCH_BOOT_ELF, ps2_elf_size, ps2_elf_sha256, elf, size);
}

static int utf8_to_wide(const char* s, wchar_t* out, size_t cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, (int)cap) > 0;
}

static int wide_to_utf8(const wchar_t* s, char* out, size_t cap) {
    return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)cap, NULL, NULL) > 0;
}

int launch_choose_disc(launch_prompt_fn prompt, launch_retry_fn retry, wchar_t* path, size_t cap, uint8_t** elf,
                       size_t* size) {
    for (;;) {
        if (!prompt(path, cap)) return 0;
        int err = launch_check_disc(path, elf, size);
        if (err == DISC_OK) {
            char u[sizeof ps2_settings.disc];
            if (wide_to_utf8(path, u, sizeof u)) {
                const char* const kv[1][3] = { { "game", "disc", u } };
                if (!settings_set(kv, 1)) fprintf(stderr, "[launch] cannot write sly2.ini: the disc will be asked for again\n");
                strcpy(ps2_settings.disc, u);
            }
            fprintf(stderr, "[launch] disc image chosen: %s\n", u8(path));
            return 1;
        }
        fprintf(stderr, "[launch] %s: %s\n", u8(path), disc_why(err));
        if (!retry(path, disc_why(err))) return 0;
    }
}

/* The Windows file-open dialog (comdlg32, loaded when needed: nothing else in the runtime uses it). */
typedef BOOL(WINAPI* open_dialog_fn)(OPENFILENAMEW*);
typedef HRESULT(WINAPI* co_init_fn)(void*, DWORD);
typedef void(WINAPI* co_uninit_fn)(void);

static int dialog_prompt(wchar_t* path, size_t cap) {
    static open_dialog_fn open_dialog;
    if (!open_dialog) open_dialog = (open_dialog_fn)(void*)GetProcAddress(LoadLibraryW(L"comdlg32.dll"), "GetOpenFileNameW");
    if (!open_dialog) return 0;
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    co_init_fn co_init = ole ? (co_init_fn)(void*)GetProcAddress(ole, "CoInitializeEx") : NULL;
    co_uninit_fn co_uninit = ole ? (co_uninit_fn)(void*)GetProcAddress(ole, "CoUninitialize") : NULL;
    HRESULT hr = co_init ? co_init(NULL, 0x2 /* COINIT_APARTMENTTHREADED */) : E_FAIL;
    wchar_t start[MAX_PATH] = L"";
    if (path[0]) {                                   /* open where the last choice was */
        wcsncpy(start, path, MAX_PATH - 1);
        wchar_t* slash = wcsrchr(start, L'\\');
        if (slash) *slash = 0; else start[0] = 0;
    }
    path[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"PS2 disc image (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)cap;
    ofn.lpstrInitialDir = start[0] ? start : NULL;
    ofn.lpstrTitle = L"Sly 2: Band of Thieves - choose your game disc image (.iso)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    BOOL ok = open_dialog(&ofn);
    if (SUCCEEDED(hr) && co_uninit) co_uninit();
    return ok != 0;
}

static int dialog_retry(const wchar_t* path, const char* why) {
    char u[1200] = "";
    wide_to_utf8(path, u, sizeof u);
    char msg[2048];
    snprintf(msg, sizeof msg, "%s\n\ncannot be used: %s.\n\nThis game needs a disc image (.iso) of your own "
             "Sly 2: Band of Thieves (USA, SCUS-97316) disc.\n\nChoose another file?", u, why);
    int a = 0;
    message_box(msg, MB_RETRYCANCEL | MB_ICONWARNING, &a);
    return a == IDRETRY;
}

static uint8_t* read_file(const char* path, size_t* size) {
    wchar_t w[1024];
    FILE* f = utf8_to_wide(path, w, 1024) ? _wfopen(w, L"rb") : NULL;
    if (!f) f = fopen(path, "rb");                   /* a name in the ANSI code page */
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 && n < (1 << 30) ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = buf ? (size_t)n : 0;
    return buf;
}

/* A path from the command line or the environment, as a wide string. */
static void arg_to_wide(const char* s, wchar_t* out, size_t cap) {
    if (!utf8_to_wide(s, out, cap)) MultiByteToWideChar(CP_ACP, 0, s, -1, out, (int)cap);
}

uint8_t* launch_load_game(int argc, char** argv, int headless, size_t* size) {
    static wchar_t disc[1024];
    const wchar_t* env = _wgetenv(L"PS2_ISO");
    if (headless) show_boxes = 0;
    if (argc > 1) {
        uint8_t* buf = read_file(argv[1], size);
        if (!buf) launch_fatal("cannot open %s", argv[1]);
        if (*size >= 4 && !memcmp(buf, "\x7f" "ELF", 4)) {   /* the executable itself (development) */
            if (env && *env) cdvd_set_image(env);
            else if (ps2_settings.disc[0] && utf8_to_wide(ps2_settings.disc, disc, 1024)) cdvd_set_image(disc);
            return buf;
        }
        free(buf);
        arg_to_wide(argv[1], disc, 1024);           /* a disc image (dropped on the exe, or "sly2.exe x.iso") */
    } else if (env && *env) {
        wcsncpy(disc, env, 1023);
    } else if (ps2_settings.disc[0]) {
        if (!utf8_to_wide(ps2_settings.disc, disc, 1024)) disc[0] = 0;
    }

    uint8_t* elf = NULL;
    int err = disc[0] ? launch_check_disc(disc, &elf, size) : -1;
    if (err > 0) {
        char u[1200] = "";
        wide_to_utf8(disc, u, sizeof u);
        int from_ini = argc <= 1 && !(env && *env);
        if (!from_ini || !show_boxes)
            launch_fatal("the disc image %s cannot be used: %s.%s", u, disc_why(err),
                         from_ini ? " Set [game] disc in sly2.ini to your Sly 2 (USA) disc image." : "");
        fprintf(stderr, "[launch] the disc image in sly2.ini, %s, cannot be used: %s\n", u, disc_why(err));
        if (!dialog_retry(disc, disc_why(err))) { fprintf(stderr, "[launch] no disc image chosen\n"); exit(1); }
    }
    if (err) {                                       /* nothing usable configured */
        if (!show_boxes)
            launch_fatal("no disc image. Run with your Sly 2: Band of Thieves (USA) disc image: set [game] disc in "
                         "%s\\sly2.ini, or PS2_ISO=<image.iso>, or give the image (or the extracted SCUS_973.16) as "
                         "the first argument.", u8(settings_user_dir()));
        if (!launch_choose_disc(dialog_prompt, dialog_retry, disc, 1024, &elf, size)) {
            fprintf(stderr, "[launch] no disc image chosen\n");
            exit(1);
        }
    }
    fprintf(stderr, "[launch] %s from the disc image %s\n", LAUNCH_BOOT_ELF, u8(disc));
    cdvd_set_image(disc);
    return elf;
}
