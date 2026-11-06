/* Build-time HLSL compiler for the GPU backend (notes/d3d11-renderer.md section 9).
 *
 *   hlslc <perms.txt> <out.h> [<cache dir>]
 *
 * Compiles every shader listed in perms.txt with D3DCompile from the d3dcompiler_47.dll that ships
 * with Windows (System32; nothing is downloaded) and writes the DXBC as byte arrays plus a table to
 * out.h. Each perms.txt line is
 *
 *   <name> <file.hlsl> <entry> <profile> [DEFINE[=value] ...]      (# starts a comment)
 *
 * with files relative to perms.txt; #include "x.hlsli" is resolved relative to the including file's
 * directory. Warnings are errors; any error fails with exit code 1 and leaves out.h untouched.
 * out.h is only rewritten when its content changes, so incremental builds stay incremental.
 *
 * (M5) The shaders are compiled on several threads, and with a cache directory each compiled shader is
 * kept there under a key of its perms.txt line and the content of every .hlsl / .hlsli file next to
 * perms.txt: an unchanged shader is not compiled again (the integer ubershader's permutations take
 * minutes of CPU otherwise). */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT(WINAPI* compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                    UINT, UINT, ID3DBlob**, ID3DBlob**);

static char* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* p = malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if (p) { p[n] = 0; if (size) *size = (size_t)n; }
    return p;
}

static void dir_of(const char* path, char* dir, size_t cap) {
    snprintf(dir, cap, "%s", path);
    char* s = strrchr(dir, '/');
    char* b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (s) s[1] = 0; else dir[0] = 0;
}

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* ---- include handler: paths relative to the including file (or the top-level file) ---- */
typedef struct {
    ID3DInclude base;
    char top_dir[MAX_PATH];
    struct { const void* data; char dir[MAX_PATH]; } open[32];
    int nopen;
} includer;

static HRESULT WINAPI inc_open(ID3DInclude* self, D3D_INCLUDE_TYPE type, LPCSTR name, LPCVOID parent, LPCVOID* data,
                               UINT* bytes) {
    (void)type;
    includer* in = (includer*)self;
    const char* dir = in->top_dir;
    for (int i = 0; i < in->nopen; i++)
        if (in->open[i].data == parent) dir = in->open[i].dir;
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s%s", dir, name);
    size_t n = 0;
    char* p = read_file(path, &n);
    if (!p) { fprintf(stderr, "hlslc: cannot open include %s\n", path); return E_FAIL; }
    if (in->nopen < 32) {
        in->open[in->nopen].data = p;
        dir_of(path, in->open[in->nopen].dir, MAX_PATH);
        in->nopen++;
    }
    *data = p;
    *bytes = (UINT)n;
    return S_OK;
}

static HRESULT WINAPI inc_close(ID3DInclude* self, LPCVOID data) {
    includer* in = (includer*)self;
    for (int i = 0; i < in->nopen; i++)
        if (in->open[i].data == data) { in->open[i] = in->open[--in->nopen]; break; }
    free((void*)data);
    return S_OK;
}

static ID3DIncludeVtbl inc_vtbl = { inc_open, inc_close };

/* ---- output buffer ---- */
static char* out;
static size_t out_len, out_cap;

static void emit(const char* fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(out + out_len, out_cap - out_len, fmt, ap);
        va_end(ap);
        if (n >= 0 && out_len + (size_t)n < out_cap) { out_len += (size_t)n; return; }
        out_cap = out_cap ? out_cap * 2 : 1 << 20;
        out = realloc(out, out_cap);
        if (!out) { fprintf(stderr, "hlslc: out of memory\n"); exit(1); }
    }
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* ---- jobs ---- */
typedef struct {
    char name[64], file[MAX_PATH], entry[64], profile[16], line[512];
    char defbuf[40][128];
    int nd;
    uint64_t key;
    unsigned char* code;
    unsigned size;
    char* err;                           /* compiler messages */
    int failed;
} job;

static job jobs[512];
static int njobs;
static volatile LONG next_job;
static compile_fn compile;
static char base[MAX_PATH], cache_dir[MAX_PATH];

static void run_job(job* j) {
    char cpath[MAX_PATH * 2];
    if (cache_dir[0]) {
        snprintf(cpath, sizeof cpath, "%s\\%s.%016llx.dxbc", cache_dir, j->name, (unsigned long long)j->key);
        size_t n = 0;
        unsigned char* c = (unsigned char*)read_file(cpath, &n);
        if (c && n) { j->code = c; j->size = (unsigned)n; return; }
        free(c);
    }
    D3D_SHADER_MACRO defs[41];
    for (int i = 0; i < j->nd; i++) {
        defs[i].Name = j->defbuf[i];
        char* eq = strchr(j->defbuf[i], '=');
        if (eq) *eq++ = 0;
        defs[i].Definition = eq ? eq : "1";
    }
    defs[j->nd].Name = NULL;
    defs[j->nd].Definition = NULL;
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s%s", base, j->file);
    size_t n = 0;
    char* src = read_file(path, &n);
    if (!src) { j->failed = 1; j->err = _strdup("cannot read the file"); return; }
    includer inc;
    memset(&inc, 0, sizeof inc);
    inc.base.lpVtbl = &inc_vtbl;
    dir_of(path, inc.top_dir, sizeof inc.top_dir);
    ID3DBlob *code = NULL, *err = NULL;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = compile(src, n, path, defs, &inc.base, j->entry, j->profile, flags, 0, &code, &err);
    free(src);
    if (err) {
        j->err = _strdup((const char*)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    if (FAILED(hr) || !code) { j->failed = 1; return; }
    j->size = (unsigned)ID3D10Blob_GetBufferSize(code);
    j->code = malloc(j->size);
    memcpy(j->code, ID3D10Blob_GetBufferPointer(code), j->size);
    ID3D10Blob_Release(code);
    if (cache_dir[0]) {
        char tmp[MAX_PATH * 2 + 32];
        snprintf(tmp, sizeof tmp, "%s.%lu.tmp", cpath, (unsigned long)GetCurrentThreadId());
        FILE* f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(j->code, 1, j->size, f) == j->size;
            fclose(f);
            if (!ok || !MoveFileExA(tmp, cpath, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        LONG i = InterlockedIncrement(&next_job) - 1;
        if (i >= njobs) return 0;
        run_job(&jobs[i]);
    }
}

/* FNV of the name and content of every .hlsl / .hlsli file in dir (sorted by name) */
static int cmp_str(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }
static uint64_t sources_key(const char* dir) {
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s*.hlsl*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    char* names[256];
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do { if (n < 256) names[n++] = _strdup(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    uint64_t k = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s%s", dir, names[i]);
        size_t sz = 0;
        char* c = read_file(p, &sz);
        k = fnv(k, names[i], strlen(names[i]) + 1);
        if (c) k = fnv(k, c, sz);
        free(c);
        free(names[i]);
    }
    return k;
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) { fprintf(stderr, "usage: hlslc <perms.txt> <out.h> [<cache dir>]\n"); return 2; }
    char sysdir[MAX_PATH], dll[MAX_PATH + 32];
    GetSystemDirectoryA(sysdir, sizeof sysdir);
    snprintf(dll, sizeof dll, "%s\\d3dcompiler_47.dll", sysdir);
    HMODULE m = LoadLibraryA(dll);
    compile = m ? (compile_fn)(void*)GetProcAddress(m, "D3DCompile") : NULL;
    if (!compile) { fprintf(stderr, "hlslc: %s not found\n", dll); return 1; }
    if (argc == 4) {
        snprintf(cache_dir, sizeof cache_dir, "%s", argv[3]);
        CreateDirectoryA(cache_dir, NULL);
    }

    char* perms = read_file(argv[1], NULL);
    if (!perms) { fprintf(stderr, "hlslc: cannot read %s\n", argv[1]); return 1; }
    dir_of(argv[1], base, sizeof base);
    uint64_t skey = sources_key(base);
    {   /* the compiler itself is part of the key (Windows updates replace it) */
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExA(dll, GetFileExInfoStandard, &a)) skey = fnv(skey, &a.ftLastWriteTime, sizeof a.ftLastWriteTime);
    }

    for (char* line = strtok(perms, "\n"); line; line = strtok(NULL, "\n")) {
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        char copy[512];
        snprintf(copy, sizeof copy, "%s", line);
        char* tok[40];
        int nt = 0;
        for (char* p = line; *p && nt < 40;) {
            while (*p && is_space(*p)) *p++ = 0;
            if (!*p) break;
            tok[nt++] = p;
            while (*p && !is_space(*p)) p++;
        }
        if (!nt) continue;
        if (nt < 4 || njobs >= 512) { fprintf(stderr, "hlslc: bad line in %s: %s\n", argv[1], tok[0]); return 1; }
        job* j = &jobs[njobs++];
        snprintf(j->name, sizeof j->name, "%s", tok[0]);
        snprintf(j->file, sizeof j->file, "%s", tok[1]);
        snprintf(j->entry, sizeof j->entry, "%s", tok[2]);
        snprintf(j->profile, sizeof j->profile, "%s", tok[3]);
        for (int i = 4; i < nt; i++) snprintf(j->defbuf[j->nd++], sizeof j->defbuf[0], "%s", tok[i]);
        uint64_t k = skey;
        for (int i = 0; i < nt; i++) k = fnv(k, tok[i], strlen(tok[i]) + 1);
        j->key = k;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nt = (int)si.dwNumberOfProcessors;
    if (nt > 16) nt = 16;
    if (nt < 1) nt = 1;
    HANDLE th[16];
    for (int i = 0; i < nt; i++) th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForMultipleObjects((DWORD)nt, th, TRUE, INFINITE);
    for (int i = 0; i < nt; i++) CloseHandle(th[i]);

    int failed = 0;
    emit("/* Generated by scripts/hlslc.c from runtime/gpu/shaders/perms.txt (d3dcompiler_47, /O3,\n"
         " * warnings as errors). Do not edit; not committed (build output). */\n#pragma once\n\n");
    for (int i = 0; i < njobs; i++) {
        job* j = &jobs[i];
        if (j->err) fprintf(stderr, "%s (%s %s %s):\n%s\n", j->name, j->file, j->entry, j->profile, j->err);
        if (j->failed) { fprintf(stderr, "hlslc: %s failed\n", j->name); failed = 1; continue; }
        emit("static const unsigned char gpu_sh_%s[%u] = {", j->name, j->size);
        for (unsigned b = 0; b < j->size; b++) emit("%s%u,", b % 24 ? "" : "\n    ", j->code[b]);
        emit("\n};\n\n");
    }
    if (failed) return 1;

    emit("enum {\n");
    for (int i = 0; i < njobs; i++) emit("    GPU_SH_%s,\n", jobs[i].name);
    emit("    GPU_SH_COUNT\n};\n\n");
    emit("#ifndef GPU_SHADER_BLOB_DEFINED\n#define GPU_SHADER_BLOB_DEFINED\n"
         "typedef struct { const char* name; const char* profile; const unsigned char* code; unsigned size; } "
         "gpu_shader_blob;\n#endif\n\n");
    emit("static const gpu_shader_blob gpu_shaders[GPU_SH_COUNT] = {\n");
    for (int i = 0; i < njobs; i++)
        emit("    { \"%s\", \"%s\", gpu_sh_%s, %u },\n", jobs[i].name, jobs[i].profile, jobs[i].name, jobs[i].size);
    emit("};\n");

    size_t old_n = 0;
    char* old = read_file(argv[2], &old_n);
    if (old && old_n == out_len && !memcmp(old, out, out_len)) {
        printf("hlslc: %d shaders, %s unchanged\n", njobs, argv[2]);
        return 0;
    }
    FILE* f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, out_len, f) != out_len) { fprintf(stderr, "hlslc: cannot write %s\n", argv[2]); return 1; }
    fclose(f);
    printf("hlslc: %d shaders -> %s\n", njobs, argv[2]);
    return 0;
}
