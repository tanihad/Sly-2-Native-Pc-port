/* Sly2-Setup: log, strings, files, processes, SHA-256, registry, shortcuts, the payload (setup.h). */
#include "setup.h"

#include <bcrypt.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- log ---------------------------------------------------------------------------------------- */

static SRWLOCK log_lock = SRWLOCK_INIT;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static wchar_t log_name[MAXP];
static char log_early[65536];        /* lines written before the log file is known */
static size_t log_early_n;

const wchar_t* log_path(void) { return log_name; }

static void log_write(const char* s, size_t n) {
    DWORD w;
    if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, s, (DWORD)n, &w, NULL);
    else if (log_early_n + n < sizeof log_early) { memcpy(log_early + log_early_n, s, n); log_early_n += n; }
}

void log_open(const wchar_t* path) {
    AcquireSRWLockExclusive(&log_lock);
    if (log_file == INVALID_HANDLE_VALUE || _wcsicmp(path, log_name)) {
        HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
            log_file = h;
            wcopy(log_name, MAXP, path);
            if (log_early_n) { log_write(log_early, log_early_n); log_early_n = 0; }
        }
    }
    ReleaseSRWLockExclusive(&log_lock);
}

void log_close(void) {
    AcquireSRWLockExclusive(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    log_file = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&log_lock);
}

static void log_line(const char* text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[32];
    int n = snprintf(head, sizeof head, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    AcquireSRWLockExclusive(&log_lock);
    log_write(head, (size_t)n);
    log_write(text, strlen(text));
    log_write("\r\n", 2);
    ReleaseSRWLockExclusive(&log_lock);
}

void loga(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line(buf);
}

void logw(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 4095, fmt, ap);
    buf[4095] = 0;
    va_end(ap);
    char* u = to_utf8(buf);
    log_line(u ? u : "?");
    free(u);
}

/* ---- strings ------------------------------------------------------------------------------------ */

void wcopy(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!cap) return;
    size_t n = src ? wcslen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memmove(dst, src, n * sizeof(wchar_t));
    dst[n] = 0;
}

void wpath(wchar_t* out, size_t cap, const wchar_t* a, const wchar_t* b) {
    wchar_t tmp[MAXP * 2];
    size_t n = wcslen(a);
    if (n && (a[n - 1] == L'\\' || a[n - 1] == L'/')) _snwprintf(tmp, MAXP * 2 - 1, L"%ls%ls", a, b);
    else _snwprintf(tmp, MAXP * 2 - 1, L"%ls\\%ls", a, b);
    tmp[MAXP * 2 - 1] = 0;
    wcopy(out, cap, tmp);
}

void wfmt(wchar_t* out, size_t cap, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(out, cap - 1, fmt, ap);
    out[cap - 1] = 0;
    va_end(ap);
}

void fmt_size(wchar_t* out, size_t cap, uint64_t b) {
    double mb = (double)b / (1024.0 * 1024.0);
    if (mb >= 1000.0) wfmt(out, cap, L"%.1f GB", mb / 1024.0);
    else if (mb >= 10.0) wfmt(out, cap, L"%.0f MB", mb);
    else wfmt(out, cap, L"%.1f MB", mb);
}

char* to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char* s = malloc(n > 0 ? (size_t)n : 1);
    if (!s) return NULL;
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) s[0] = 0;
    return s;
}

wchar_t* from_utf8(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) {                                    /* not UTF-8: the ANSI code page */
        n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
        wchar_t* w = malloc((n > 0 ? (size_t)n : 1) * sizeof(wchar_t));
        if (w && (n <= 0 || !MultiByteToWideChar(CP_ACP, 0, s, -1, w, n))) w[0] = 0;
        return w;
    }
    wchar_t* w = malloc((size_t)n * sizeof(wchar_t));
    if (w && !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

/* ---- files -------------------------------------------------------------------------------------- */

int file_exists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_exists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t file_size(const wchar_t* p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return (uint64_t)-1;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

int mkdir_p(const wchar_t* p) {
    if (dir_exists(p)) return 1;
    wchar_t tmp[MAXP];
    wcopy(tmp, MAXP, p);
    size_t n = wcslen(tmp);
    while (n && (tmp[n - 1] == L'\\' || tmp[n - 1] == L'/')) tmp[--n] = 0;
    wchar_t* slash = wcsrchr(tmp, L'\\');
    if (slash && slash > tmp && slash[-1] != L':' && !(slash == tmp + 1 && tmp[0] == L'\\')) {
        *slash = 0;
        if (!mkdir_p(tmp)) return 0;
        *slash = L'\\';
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int remove_file_retry(const wchar_t* p) {
    for (int i = 0; i < 20; i++) {
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return 1;
        if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED) return 0;
        Sleep(50);                                   /* a virus scanner or the indexer has it open */
    }
    return 0;
}

static int remove_dir_retry(const wchar_t* p) {
    for (int i = 0; i < 20; i++) {
        SetFileAttributesW(p, FILE_ATTRIBUTE_DIRECTORY);
        if (RemoveDirectoryW(p) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return 1;
        if (GetLastError() == ERROR_DIR_NOT_EMPTY && i > 4) return 0;
        Sleep(50);
    }
    return 0;
}

int delete_tree(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) return 1;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return remove_file_retry(p);
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return remove_dir_retry(p);   /* a junction: only the link */
    wchar_t pat[MAXP];
    wpath(pat, MAXP, p, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    int ok = 1;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t c[MAXP];
            wpath(c, MAXP, p, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ok &= remove_dir_retry(c);
                else ok &= delete_tree(c);
            } else ok &= remove_file_retry(c);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return remove_dir_retry(p) && ok;
}

uint64_t tree_size(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return file_size(p);
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    uint64_t sum = 0;
    wchar_t pat[MAXP];
    wpath(pat, MAXP, p, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                wchar_t c[MAXP];
                wpath(c, MAXP, p, fd.cFileName);
                sum += tree_size(c);
            }
        } else sum += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return sum;
}

int copy_tree(setup_t* s, const wchar_t* src, const wchar_t* dst, int skip_locks) {
    if (s && s->cancel) return 0;
    if (!mkdir_p(dst)) { logw(L"cannot create %ls (error %lu)", dst, GetLastError()); return 0; }
    wchar_t pat[MAXP];
    wpath(pat, MAXP, src, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* links: not copied */
        if (skip_locks && !_wcsicmp(fd.cFileName, L".lock")) continue;
        wchar_t a[MAXP], b[MAXP];
        wpath(a, MAXP, src, fd.cFileName);
        wpath(b, MAXP, dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copy_tree(s, a, b, skip_locks);
        else if (!CopyFileW(a, b, FALSE)) { logw(L"cannot copy %ls (error %lu)", a, GetLastError()); ok = 0; }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

int dir_writable(const wchar_t* dir, DWORD* err) {
    *err = 0;
    if (!mkdir_p(dir)) { *err = GetLastError(); return 0; }
    wchar_t t[MAXP];
    wfmt(t, MAXP, L"%ls\\~sly2-setup-%lu.tmp", dir, GetCurrentProcessId());
    HANDLE h = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) { *err = GetLastError(); return 0; }
    CloseHandle(h);
    return 1;
}

uint64_t disk_free(const wchar_t* path) {
    wchar_t p[MAXP];
    wcopy(p, MAXP, path);
    for (;;) {                                       /* the nearest existing parent */
        ULARGE_INTEGER avail;
        if (dir_exists(p) && GetDiskFreeSpaceExW(p, &avail, NULL, NULL)) return avail.QuadPart;
        wchar_t* slash = wcsrchr(p, L'\\');
        if (!slash) return 0;
        if (slash > p && slash[-1] == L':') { slash[1] = 0; if (GetDiskFreeSpaceExW(p, &avail, NULL, NULL)) return avail.QuadPart; return 0; }
        *slash = 0;
    }
}

int read_file(const wchar_t* p, char** data, size_t* n) {
    *data = NULL;
    *n = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (64 << 20)) { CloseHandle(h); return 0; }
    char* b = malloc((size_t)sz.QuadPart + 1);
    DWORD got = 0;
    int ok = b && ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart;
    CloseHandle(h);
    if (!ok) { free(b); return 0; }
    b[got] = 0;
    *data = b;
    *n = got;
    return 1;
}

int write_file(const wchar_t* p, const void* data, size_t n) {
    wchar_t tmp[MAXP];
    wfmt(tmp, MAXP, L"%ls.tmp", p);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    int ok = WriteFile(h, data, (DWORD)n, &w, NULL) && w == n;
    ok &= CloseHandle(h) != 0;
    if (ok) ok = MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp);
    return ok;
}

const wchar_t* self_path(void) {
    static wchar_t p[MAXP];
    if (!p[0]) GetModuleFileNameW(NULL, p, MAXP);
    return p;
}

int is_elevated(void) {
    HANDLE tok;
    TOKEN_ELEVATION e = { 0 };
    DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n);
    CloseHandle(tok);
    return ok && e.TokenIsElevated;
}

/* ---- SHA-256 (bcrypt) --------------------------------------------------------------------------- */

static BCRYPT_ALG_HANDLE sha_alg(void) {
    static BCRYPT_ALG_HANDLE alg;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) alg = NULL;
        InitOnceComplete(&once, 0, NULL);
    }
    return alg;
}


/* SHA-256 of [offset, offset + size) of an open file, optionally copied into out_h. */
static int hash_copy(setup_t* s, HANDLE in, uint64_t offset, uint64_t size, HANDLE out_h, const wchar_t* label,
                     uint8_t digest[32]) {
    BCRYPT_ALG_HANDLE alg = sha_alg();
    BCRYPT_HASH_HANDLE hh = NULL;
    if (!alg || BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) != 0) return 0;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(in, li, NULL, FILE_BEGIN);
    enum { CHUNK = 1 << 20 };
    uint8_t* buf = malloc(CHUNK);
    uint64_t done = 0;
    int ok = buf != NULL;
    DWORD last = GetTickCount();
    while (ok && done < size) {
        if (s && s->cancel) { ok = 0; break; }
        DWORD want = (DWORD)((size - done) < CHUNK ? (size - done) : CHUNK), got = 0, w = 0;
        if (!ReadFile(in, buf, want, &got, NULL) || got != want) { ok = 0; break; }
        BCryptHashData(hh, buf, got, 0);
        if (out_h && (!WriteFile(out_h, buf, got, &w, NULL) || w != got)) { ok = 0; break; }
        done += got;
        if (s && label && GetTickCount() - last > 100) { last = GetTickCount(); setup_bytes(s, label, done, size); }
    }
    if (ok) ok = BCryptFinishHash(hh, digest, 32, 0) == 0;
    BCryptDestroyHash(hh);
    free(buf);
    return ok;
}

int sha256_file(setup_t* s, const wchar_t* path, uint8_t out[32]) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    int ok = GetFileSizeEx(h, &sz) && hash_copy(s, h, 0, (uint64_t)sz.QuadPart, NULL, NULL, out);
    CloseHandle(h);
    return ok;
}

void sha256_hex(const uint8_t d[32], wchar_t out[65]) {
    static const wchar_t* hx = L"0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[64] = 0;
}

int sha256_equal_hex(const uint8_t d[32], const char* hex) {
    if (strlen(hex) != 64) return 0;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        char two[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        if (sscanf(two, "%2x", &v) != 1 || v != d[i]) return 0;
    }
    return 1;
}

/* ---- processes ---------------------------------------------------------------------------------- */

static int env_name_eq(const wchar_t* entry, const wchar_t* name) {
    size_t n = wcslen(name);
    return !_wcsnicmp(entry, name, n) && entry[n] == L'=';
}

/* The child's environment: ours without variables that would change what Python / uv do, plus env_add
 * ("NAME=value\0...\0\0"). */
static wchar_t* make_env(const wchar_t* env_add) {
    static const wchar_t* drop[] = { L"PYTHONHOME", L"PYTHONPATH", L"PYTHONSTARTUP", L"PYTHONUSERBASE",
                                     L"PYTHONNOUSERSITE", L"VIRTUAL_ENV", L"CONDA_PREFIX", L"PIP_REQUIRE_VIRTUALENV" };
    wchar_t* cur = GetEnvironmentStringsW();
    size_t n = 0, add_n = 0;
    for (const wchar_t* e = cur; *e; e += wcslen(e) + 1) n += wcslen(e) + 1;
    if (env_add) for (const wchar_t* e = env_add; *e; e += wcslen(e) + 1) add_n += wcslen(e) + 1;
    wchar_t* out = malloc((n + add_n + 2) * sizeof(wchar_t));
    wchar_t* o = out;
    for (const wchar_t* e = cur; *e; e += wcslen(e) + 1) {
        int skip = !_wcsnicmp(e, L"UV_", 3);
        for (size_t i = 0; !skip && i < sizeof drop / sizeof drop[0]; i++) skip = env_name_eq(e, drop[i]);
        if (!skip && env_add)
            for (const wchar_t* a = env_add; *a && !skip; a += wcslen(a) + 1) {
                const wchar_t* eq = wcschr(a, L'=');
                if (eq && !_wcsnicmp(e, a, (size_t)(eq - a) + 1)) skip = 1;
            }
        if (skip) continue;
        size_t l = wcslen(e) + 1;
        memcpy(o, e, l * sizeof(wchar_t));
        o += l;
    }
    if (env_add) for (const wchar_t* a = env_add; *a; a += wcslen(a) + 1) {
        size_t l = wcslen(a) + 1;
        memcpy(o, a, l * sizeof(wchar_t));
        o += l;
    }
    *o++ = 0;
    if (o == out + 1) *o++ = 0;
    FreeEnvironmentStringsW(cur);
    return out;
}

int run_hidden(setup_t* s, const wchar_t* cmdline, const wchar_t* cwd, const wchar_t* env_add, line_fn fn,
               void* user, DWORD* code) {
    *code = (DWORD)-1;
    if (s->cancel) return -2;
    logw(L"> %ls", cmdline);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    wchar_t* cl = _wcsdup(cmdline);
    wchar_t* env = make_env(env_add);
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    /* below normal priority: the build uses every core, the PC stays usable */
    BOOL ok = CreateProcessW(NULL, cl, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
                             env, cwd, &si, &pi);
    DWORD err = GetLastError();
    free(cl);
    free(env);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        loga("cannot start the program (error %lu)", err);
        CloseHandle(rd);
        if (job) CloseHandle(job);
        return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    EnterCriticalSection(&s->lock);
    s->job = job;
    LeaveCriticalSection(&s->lock);
    if (s->cancel && job) TerminateJobObject(job, 1);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    char buf[8192], line[4096];
    size_t ln = 0;
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r' || ln == sizeof line - 1) {
                if (ln) {
                    line[ln] = 0;
                    loga("  %s", line);
                    if (fn) fn(s, user, line);
                }
                ln = 0;
                if (c == '\n' || c == '\r') continue;
            }
            line[ln++] = c;
        }
    }
    if (ln) { line[ln] = 0; loga("  %s", line); if (fn) fn(s, user, line); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    EnterCriticalSection(&s->lock);
    s->job = NULL;
    LeaveCriticalSection(&s->lock);
    if (job) CloseHandle(job);                       /* (kills anything the program left running) */
    loga("  (exit code %lu)", *code);
    return s->cancel ? -2 : 0;
}

/* ---- shortcuts, known folders, registry ------------------------------------------------------- */

int make_shortcut(const wchar_t* lnk, const wchar_t* target, const wchar_t* args, const wchar_t* workdir,
                  const wchar_t* desc, const wchar_t* icon) {
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = NULL;
    IPersistFile* pf = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&sl);
    if (SUCCEEDED(hr)) {
        IShellLinkW_SetPath(sl, target);
        if (args) IShellLinkW_SetArguments(sl, args);
        if (workdir) IShellLinkW_SetWorkingDirectory(sl, workdir);
        if (desc) IShellLinkW_SetDescription(sl, desc);
        if (icon) IShellLinkW_SetIconLocation(sl, icon, 0);
        hr = IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void**)&pf);
        if (SUCCEEDED(hr)) {
            wchar_t dir[MAXP];
            wcopy(dir, MAXP, lnk);
            wchar_t* slash = wcsrchr(dir, L'\\');
            if (slash) { *slash = 0; mkdir_p(dir); }
            hr = IPersistFile_Save(pf, lnk, TRUE);
            IPersistFile_Release(pf);
        }
        IShellLinkW_Release(sl);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) logw(L"cannot create the shortcut %ls (0x%08lx)", lnk, (unsigned long)hr);
    return SUCCEEDED(hr);
}

int known_folder(int which, wchar_t* out, size_t cap) {
    const GUID* id;
    switch (which) {
    case KF_LOCALAPPDATA: id = &FOLDERID_LocalAppData; break;
    case KF_PROGRAMS: id = &FOLDERID_Programs; break;
    case KF_COMMON_PROGRAMS: id = &FOLDERID_CommonPrograms; break;
    case KF_DESKTOP: id = &FOLDERID_Desktop; break;
    case KF_PUBLIC_DESKTOP: id = &FOLDERID_PublicDesktop; break;
    default: id = &FOLDERID_ProgramFiles; break;
    }
    PWSTR p = NULL;
    out[0] = 0;
    if (FAILED(SHGetKnownFolderPath(id, 0, NULL, &p))) return 0;
    wcopy(out, cap, p);
    CoTaskMemFree(p);
    return out[0] != 0;
}

int reg_set_str(HKEY root, const wchar_t* key, const wchar_t* name, const wchar_t* value) {
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == 0;
}

int reg_set_dword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD value) {
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&value, sizeof value);
    RegCloseKey(k);
    return r == 0;
}

int reg_get_str(HKEY root, const wchar_t* key, const wchar_t* name, wchar_t* out, size_t cap) {
    DWORD n = (DWORD)(cap * sizeof(wchar_t)), type = 0;
    out[0] = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, &type, out, &n)) { out[0] = 0; return 0; }
    return 1;
}

int reg_delete_tree(HKEY root, const wchar_t* key) {
    LONG r = RegDeleteTreeW(root, key);
    if (r && r != ERROR_FILE_NOT_FOUND) return 0;
    r = RegDeleteKeyW(root, key);
    return r == 0 || r == ERROR_FILE_NOT_FOUND;
}

/* ---- the payload (scripts/make_installer.py) -----------------------------------------------------
 * [setup program][entry data ...][table: count x payload_entry][trailer: "SLY2PAY1", u32 count, u32 0,
 * u64 table offset]. If the file is Authenticode-signed after the payload was appended, the signature
 * follows the trailer: the end is where the PE certificate table starts. */

static payload_entry* entries;
static int n_entries = -1;
static uint64_t stub_size;

static uint64_t signed_end(HANDLE h, uint64_t fsize) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    DWORD got;
    LARGE_INTEGER li = { 0 };
    SetFilePointerEx(h, li, NULL, FILE_BEGIN);
    if (!ReadFile(h, &dos, sizeof dos, &got, NULL) || got != sizeof dos || dos.e_magic != IMAGE_DOS_SIGNATURE) return fsize;
    li.QuadPart = dos.e_lfanew;
    SetFilePointerEx(h, li, NULL, FILE_BEGIN);
    if (!ReadFile(h, &nt, sizeof nt, &got, NULL) || got != sizeof nt || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fsize;
    IMAGE_DATA_DIRECTORY sec = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    if (sec.VirtualAddress && sec.VirtualAddress < fsize) return sec.VirtualAddress;   /* a file offset */
    return fsize;
}

int payload_open(void) {
    if (n_entries >= 0) return n_entries;
    n_entries = 0;
    HANDLE h = CreateFileW(self_path(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    uint64_t end = signed_end(h, (uint64_t)sz.QuadPart);
    stub_size = end;
    struct { char magic[8]; uint32_t count, zero; uint64_t table; } tr;
    DWORD got;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)(end - sizeof tr);
    if (end > sizeof tr && SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, &tr, sizeof tr, &got, NULL) &&
        got == sizeof tr && !memcmp(tr.magic, "SLY2PAY1", 8) && tr.count < 64 &&
        tr.table + (uint64_t)tr.count * sizeof(payload_entry) + sizeof tr == end) {
        entries = calloc(tr.count ? tr.count : 1, sizeof(payload_entry));
        li.QuadPart = (LONGLONG)tr.table;
        SetFilePointerEx(h, li, NULL, FILE_BEGIN);
        DWORD want = tr.count * (DWORD)sizeof(payload_entry);
        if (entries && ReadFile(h, entries, want, &got, NULL) && got == want) {
            n_entries = (int)tr.count;
            stub_size = tr.table;
            for (int i = 0; i < n_entries; i++) {
                entries[i].name[sizeof entries[i].name - 1] = 0;
                if (entries[i].offset < stub_size) stub_size = entries[i].offset;
            }
        }
    }
    CloseHandle(h);
    return n_entries;
}

int payload_count(void) { return payload_open(); }
const payload_entry* payload_at(int i) { return i >= 0 && i < payload_open() ? &entries[i] : NULL; }
uint64_t payload_stub_size(void) { payload_open(); return stub_size; }

const payload_entry* payload_find(const char* name) {
    for (int i = 0; i < payload_open(); i++)
        if (!_stricmp(entries[i].name, name)) return &entries[i];
    return NULL;
}

int payload_extract(setup_t* s, const payload_entry* e, const wchar_t* dest) {
    HANDLE in = CreateFileW(self_path(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    wchar_t tmp[MAXP];
    wfmt(tmp, MAXP, L"%ls.part", dest);
    HANDLE out = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }
    uint8_t d[32];
    wchar_t* label = from_utf8(e->name);
    int ok = hash_copy(s, in, e->offset, e->size, out, label, d);
    free(label);
    CloseHandle(in);
    ok &= CloseHandle(out) != 0;
    if (ok && memcmp(d, e->sha256, 32)) {
        loga("payload %s: SHA-256 mismatch (the setup file is damaged: download it again)", e->name);
        DeleteFileW(tmp);
        return -1;
    }
    if (ok) ok = MoveFileExW(tmp, dest, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp);
    return ok;
}
