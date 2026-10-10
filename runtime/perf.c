/* Performance instrumentation: the PS2_PERF speed report and the PS2_PROFILE sampling profiler.
 * See perf.h for what they print. */
#include "ps2_internal.h"
#include "perf.h"

#include <stdlib.h>
#include <windows.h>

static const char* const cat_names[PERF_N] = { "EE", "GS", "VU", "VIF", "IPU", "IOP", "present", "idle" };

static LARGE_INTEGER qpc_freq;
static int64_t report_qpc;                 /* QPC of the last report */
static uint64_t report_tsc, report_acc[PERF_N];
static uint32_t report_serviced, report_due, report_flips;
static double tsc_hz;                      /* measured between reports */
static int64_t start_qpc;
static uint64_t start_tsc;

/* ---- sampling profiler -------------------------------------------------------------------------- */

#define PROF_MAX (1u << 22)                /* samples kept (~35 minutes at 2 kHz) */
static HANDLE prof_thread_handle, prof_target;
static uint64_t* prof_rip;
static volatile LONG prof_count, prof_stop;

static DWORD WINAPI prof_thread(LPVOID arg) {
    (void)arg;
    /* 0.5 ms period with a high-resolution waitable timer (Windows 10 1803+), else Sleep(1) */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                          TIMER_ALL_ACCESS);
    if (!timer) timeBeginPeriod(1);
    CONTEXT c;
    while (!prof_stop) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -5000;          /* 100 ns units, relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        if (SuspendThread(prof_target) == (DWORD)-1) continue;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(prof_target, &c) && (uint32_t)prof_count < PROF_MAX)
            prof_rip[prof_count++] = c.Rip;  /* only this thread writes */
        ResumeThread(prof_target);
    }
    if (timer) CloseHandle(timer);
    return 0;
}

/* Function symbols from the executable's own COFF symbol table (kept by the mingw linker unless
 * the image is stripped): RVA-sorted, so a sample maps to the closest symbol below it. */
typedef struct { uint32_t rva; const char* name; } sym;
static sym* syms;
static int nsyms;

static int sym_cmp(const void* a, const void* b) {
    uint32_t x = ((const sym*)a)->rva, y = ((const sym*)b)->rva;
    return x < y ? -1 : x > y;
}

static void load_symbols(void) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);           /* kept: symbol names point into it */
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return; }
    fclose(f);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)buf;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(buf + dos->e_lfanew);
    const IMAGE_FILE_HEADER* fh = &nt->FileHeader;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    if (!fh->PointerToSymbolTable || (size_t)fh->PointerToSymbolTable + fh->NumberOfSymbols * 18ull > (size_t)size) return;
    const uint8_t* tab = buf + fh->PointerToSymbolTable;
    const char* strtab = (const char*)tab + fh->NumberOfSymbols * 18ull;
    syms = malloc(sizeof(sym) * fh->NumberOfSymbols);
    if (!syms) return;
    for (uint32_t i = 0; i < fh->NumberOfSymbols; i++) {
        const IMAGE_SYMBOL* s = (const IMAGE_SYMBOL*)(tab + 18ull * i);
        int secno = s->SectionNumber;
        if (secno > 0 && secno <= fh->NumberOfSections && (sec[secno - 1].Characteristics & IMAGE_SCN_CNT_CODE) &&
            (s->StorageClass == IMAGE_SYM_CLASS_EXTERNAL || s->StorageClass == IMAGE_SYM_CLASS_STATIC)) {
            const char* name;
            if (s->N.Name.Short) {
                char* n = calloc(1, 9);
                if (n) memcpy(n, s->N.ShortName, 8);
                name = n;
            } else {
                name = strtab + s->N.Name.Long;
            }
            if (name && name[0] != '.')     /* skip section symbols */
                syms[nsyms++] = (sym){ sec[secno - 1].VirtualAddress + s->Value, name };
        }
        i += s->NumberOfAuxSymbols;
    }
    qsort(syms, (size_t)nsyms, sizeof(sym), sym_cmp);
}

typedef struct { const char* name; uint32_t n; } hit;

static int hit_cmp(const void* a, const void* b) {
    uint32_t x = ((const hit*)a)->n, y = ((const hit*)b)->n;
    return x > y ? -1 : x < y;
}

static void profile_report(void) {
    uint32_t n = (uint32_t)prof_count;
    if (!n) return;
    load_symbols();
    uint8_t* base = (uint8_t*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    uint64_t image_size = nt->OptionalHeader.SizeOfImage;

    hit* hits = calloc((size_t)nsyms + 64, sizeof(hit));
    if (!hits) return;
    uint32_t game = 0, outside = 0;
    /* other modules (system DLLs): counted per module, named by file */
    static struct { HMODULE m; uint32_t n; char name[64]; } mods[32];
    int nmods = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t rip = prof_rip[i];
        if (rip < (uint64_t)base || rip >= (uint64_t)base + image_size) {
            outside++;
            HMODULE m = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)(uintptr_t)rip, &m);
            int k = 0;
            while (k < nmods && mods[k].m != m) k++;
            if (k == nmods && nmods < 32) {
                mods[k].m = m;
                mods[k].n = 0;
                char full[MAX_PATH] = "?";
                if (m) GetModuleFileNameA(m, full, MAX_PATH);
                const char* slash = strrchr(full, '\\');
                snprintf(mods[k].name, sizeof mods[k].name, "[%s]", slash ? slash + 1 : full);
                nmods++;
            }
            if (k < nmods) mods[k].n++;
            continue;
        }
        uint32_t rva = (uint32_t)(rip - (uint64_t)base);
        int lo = 0, hi = nsyms;                    /* last symbol with rva <= sample */
        while (lo < hi) { int mid = (lo + hi) / 2; if (syms[mid].rva <= rva) lo = mid + 1; else hi = mid; }
        int s = lo - 1;
        if (s < 0) continue;
        hits[s].name = syms[s].name;
        hits[s].n++;
        if (!strncmp(syms[s].name, "f_", 2)) game++;
    }
    int nh = nsyms;
    for (int k = 0; k < nmods && nh < nsyms + 64; k++) hits[nh++] = (hit){ mods[k].name, mods[k].n };
    qsort(hits, (size_t)nh, sizeof(hit), hit_cmp);

    fprintf(stderr, "\n[profile] %u samples of the emulation thread: %.1f%% recompiled game code (f_*), "
                    "%.1f%% runtime, %.1f%% system DLLs. Hottest:\n",
            n, 100.0 * game / n, 100.0 * (n - game - outside) / n, 100.0 * outside / n);
    for (int k = 0; k < nh && k < 40 && hits[k].n; k++)
        fprintf(stderr, "  %6.2f%%  %7u  %s\n", 100.0 * hits[k].n / n, hits[k].n, hits[k].name);
    fflush(stderr);
    free(hits);
}

/* ---- speed report ------------------------------------------------------------------------------- */

void perf_init(void) {
    QueryPerformanceFrequency(&qpc_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    start_qpc = report_qpc = now.QuadPart;
    start_tsc = report_tsc = __rdtsc();
    const char* p = getenv("PS2_PERF");
    if (p && *p && *p != '0') {
        perf_state.on = 1;
        perf_state.cat = PERF_EE;
        perf_state.t0 = report_tsc;
    }
    p = getenv("PS2_PROFILE");
    if (p && *p && *p != '0') {
        prof_rip = VirtualAlloc(NULL, PROF_MAX * sizeof(uint64_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (prof_rip && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_target,
                                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
            prof_thread_handle = CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
            if (prof_thread_handle) SetThreadPriority(prof_thread_handle, THREAD_PRIORITY_TIME_CRITICAL);
        }
    }
}

void perf_vblank(uint32_t serviced, uint32_t due) {
    if (!perf_state.on) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t dq = now.QuadPart - report_qpc;
    if (dq < qpc_freq.QuadPart) return;           /* once per second */
    uint64_t t = __rdtsc();
    perf_state.acc[perf_state.cat] += t - perf_state.t0;
    perf_state.t0 = t;

    double secs = (double)dq / (double)qpc_freq.QuadPart;
    uint64_t total = t - report_tsc;
    tsc_hz = (double)total / secs;
    uint32_t ds = serviced - report_serviced, dd = due - report_due, df = perf_state.flips - report_flips;
    uint64_t d[PERF_N];
    for (int c = 0; c < PERF_N; c++) { d[c] = perf_state.acc[c] - report_acc[c]; report_acc[c] = perf_state.acc[c]; }

    char cats[256];
    int len = 0;
    for (int c = 0; c < PERF_N; c++)
        len += snprintf(cats + len, sizeof cats - (size_t)len, " %s %.1f%%", cat_names[c],
                        total ? 100.0 * (double)d[c] / (double)total : 0.0);
    /* busy = everything except idle, per serviced VBlank: the frame budget is 16.68 ms */
    double busy_ms = ds ? (double)(total - d[PERF_IDLE]) / tsc_hz * 1000.0 / ds : 0;
    fprintf(stderr, "[perf] %6.1fs vblanks %u/%u due (%.1f/s), flips %u (%.1f/s), busy %.2f ms/vblank |%s\n",
            (double)(now.QuadPart - start_qpc) / (double)qpc_freq.QuadPart, ds, dd, ds / secs, df, df / secs,
            busy_ms, cats);
    report_qpc = now.QuadPart;
    report_tsc = t;
    report_serviced = serviced;
    report_due = due;
    report_flips = perf_state.flips;
}

void perf_final(void) {
    static volatile LONG done;
    if (InterlockedExchange(&done, 1)) return;
    if (prof_thread_handle) {
        prof_stop = 1;
        WaitForSingleObject(prof_thread_handle, 200);
        profile_report();
    }
}
