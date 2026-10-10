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
static int64_t start_qpc;

/* ---- symbols ------------------------------------------------------------------------------------ */

/* Function symbols from the executable's own COFF symbol table (kept by the mingw linker unless
 * the image is stripped): RVA-sorted, so an address maps to the closest symbol below it. */
typedef struct { uint32_t rva; const char* name; } sym;
static sym* syms;
static int nsyms;
static uint8_t* image_base;
static uint64_t image_size;

static int sym_cmp(const void* a, const void* b) {
    uint32_t x = ((const sym*)a)->rva, y = ((const sym*)b)->rva;
    return x < y ? -1 : x > y;
}

static void load_symbols(void) {
    image_base = (uint8_t*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64* mnt = (const IMAGE_NT_HEADERS64*)(image_base + ((const IMAGE_DOS_HEADER*)image_base)->e_lfanew);
    image_size = mnt->OptionalHeader.SizeOfImage;

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
    if (!fh->PointerToSymbolTable || (size_t)fh->PointerToSymbolTable + fh->NumberOfSymbols * 18ull > (size_t)size) {
        fprintf(stderr, "[profile] no COFF symbol table in the executable: functions are shown as addresses\n");
        return;
    }
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

/* Index of the symbol containing host address `pc`, or -1 (outside the executable / no symbol). */
static int sym_of(uint64_t pc) {
    if (pc < (uint64_t)image_base || pc >= (uint64_t)image_base + image_size) return -1;
    uint32_t rva = (uint32_t)(pc - (uint64_t)image_base);
    int lo = 0, hi = nsyms;                    /* last symbol with rva <= pc */
    while (lo < hi) { int mid = (lo + hi) / 2; if (syms[mid].rva <= rva) lo = mid + 1; else hi = mid; }
    return lo - 1;
}

/* ---- sampling profiler -------------------------------------------------------------------------
 * The sampler thread suspends the emulation thread, records the function it is in (self) and walks
 * its stack with the x64 unwind tables (every function on the stack gets an inclusive count, once
 * per sample), then resumes it. Counts are aggregated per symbol on the fly. Samples outside the
 * executable (system DLLs) are kept raw and named by module at the end; resolving module names
 * needs the loader lock, which the suspended thread might hold, so it is not done while sampling. */

#define PROF_DEPTH 48
#define OUTSIDE_MAX (1u << 20)
static HANDLE prof_thread_handle, prof_target;
static uint32_t *prof_self, *prof_incl;
static uint64_t* outside_rip;
static uint32_t outside_n, prof_samples, prof_game_self, prof_unknown;
static volatile LONG prof_stop;
static int64_t prof_skip_qpc;               /* PS2_PROFILE_SKIP: ignore samples before this QPC */

static void prof_sample(void) {
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(prof_target) == (DWORD)-1) return;
    if (!GetThreadContext(prof_target, &c)) { ResumeThread(prof_target); return; }
    /* The walk only reads stack memory between Rsp and the end of the committed region holding it
     * (the running fiber's stack), so a bad unwind can never fault. */
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t lo = c.Rsp, hi = 0;
    if (VirtualQuery((LPCVOID)(uintptr_t)c.Rsp, &mbi, sizeof mbi)) hi = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    int seen[PROF_DEPTH], nseen = 0;
    for (int depth = 0; depth < PROF_DEPTH && c.Rip && c.Rsp >= lo && c.Rsp + 8 <= hi; depth++) {
        int s = sym_of(c.Rip);
        if (depth == 0) {
            if (s >= 0) { prof_self[s]++; if (!strncmp(syms[s].name, "f_", 2)) prof_game_self++; }
            else if (c.Rip >= (uint64_t)image_base && c.Rip < (uint64_t)image_base + image_size) prof_unknown++;
            else if (outside_n < OUTSIDE_MAX) outside_rip[outside_n++] = c.Rip;
        }
        if (s >= 0) {
            int dup = 0;
            for (int k = 0; k < nseen; k++) if (seen[k] == s) { dup = 1; break; }
            if (!dup) { seen[nseen++] = s; prof_incl[s]++; }
        }
        DWORD64 base = 0, prev_rsp = c.Rsp;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (!fe) {                              /* leaf function: return address on top of the stack */
            c.Rip = *(const DWORD64*)(uintptr_t)c.Rsp;
            c.Rsp += 8;
        } else {
            PVOID handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &handler_data, &frame, NULL);
        }
        if (c.Rsp <= prev_rsp) break;           /* no progress: corrupt or unknown frame */
    }
    ResumeThread(prof_target);
    prof_samples++;
}

static DWORD WINAPI prof_thread(LPVOID arg) {
    (void)arg;
    /* 0.5 ms period with a high-resolution waitable timer (Windows 10 1803+), else Sleep(1) */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                          TIMER_ALL_ACCESS);
    if (!timer) timeBeginPeriod(1);
    while (!prof_stop) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -5000;          /* 100 ns units, relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= prof_skip_qpc) prof_sample();
    }
    if (timer) CloseHandle(timer);
    return 0;
}

typedef struct { const char* name; uint32_t n; } hit;

static int hit_cmp(const void* a, const void* b) {
    uint32_t x = ((const hit*)a)->n, y = ((const hit*)b)->n;
    return x > y ? -1 : x < y;
}

static void print_top(const char* title, hit* hits, int nh, uint32_t n, int count) {
    qsort(hits, (size_t)nh, sizeof(hit), hit_cmp);
    fprintf(stderr, "[profile] %s:\n", title);
    for (int k = 0; k < nh && k < count && hits[k].n; k++)
        fprintf(stderr, "  %6.2f%%  %7u  %s\n", 100.0 * hits[k].n / n, hits[k].n, hits[k].name);
}

static void profile_report(void) {
    uint32_t n = prof_samples;
    if (!n) return;
    hit* hits = calloc((size_t)nsyms + 64, sizeof(hit));
    if (!hits) return;
    /* system DLLs: counted per module, named by file */
    static struct { HMODULE m; uint32_t n; char name[64]; } mods[32];
    int nmods = 0;
    for (uint32_t i = 0; i < outside_n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)outside_rip[i], &m);
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
    }

    fprintf(stderr, "\n[profile] %u samples of the emulation thread (self time): %.1f%% recompiled game code (f_*), "
                    "%.1f%% runtime, %.1f%% system DLLs\n",
            n, 100.0 * prof_game_self / n, 100.0 * (n - prof_game_self - outside_n) / n, 100.0 * outside_n / n);
    int nh = 0;
    for (int s = 0; s < nsyms; s++) if (prof_self[s]) hits[nh++] = (hit){ syms[s].name, prof_self[s] };
    for (int k = 0; k < nmods; k++) hits[nh++] = (hit){ mods[k].name, mods[k].n };
    if (prof_unknown) hits[nh++] = (hit){ "[executable, no symbol]", prof_unknown };
    print_top("hottest functions, self", hits, nh, n, 45);
    nh = 0;
    for (int s = 0; s < nsyms; s++) if (prof_incl[s]) hits[nh++] = (hit){ syms[s].name, prof_incl[s] };
    print_top("hottest functions, inclusive (on the stack)", hits, nh, n, 60);
    fflush(stderr);
    free(hits);
}

/* ---- speed report ------------------------------------------------------------------------------- */

void perf_init(void) {
    QueryPerformanceFrequency(&qpc_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    start_qpc = report_qpc = now.QuadPart;
    report_tsc = __rdtsc();
    const char* p = getenv("PS2_PERF");
    if (p && *p && *p != '0') {
        perf_state.on = 1;
        perf_state.cat = PERF_EE;
        perf_state.t0 = report_tsc;
    }
    p = getenv("PS2_PROFILE");
    if (p && *p && *p != '0') {
        load_symbols();
        const char* skip = getenv("PS2_PROFILE_SKIP");             /* seconds */
        prof_skip_qpc = now.QuadPart + (skip ? (int64_t)(atof(skip) * (double)qpc_freq.QuadPart) : 0);
        prof_self = calloc((size_t)nsyms + 1, sizeof(uint32_t));
        prof_incl = calloc((size_t)nsyms + 1, sizeof(uint32_t));
        outside_rip = malloc(OUTSIDE_MAX * sizeof(uint64_t));
        if (prof_self && prof_incl && outside_rip &&
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_target,
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
    double tsc_hz = (double)total / secs;          /* measured over the same interval */
    uint32_t ds = serviced - report_serviced, dd = due - report_due, df = perf_state.flips - report_flips;
    uint64_t d[PERF_N];
    for (int c = 0; c < PERF_N; c++) { d[c] = perf_state.acc[c] - report_acc[c]; report_acc[c] = perf_state.acc[c]; }

    char cats[256];
    int len = 0;
    for (int c = 0; c < PERF_N; c++)
        len += snprintf(cats + len, sizeof cats - (size_t)len, " %s %.1f%%", cat_names[c],
                        total ? 100.0 * (double)d[c] / (double)total : 0.0);
    /* busy = everything except idle, per serviced VBlank: the budget is 16.68 ms */
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
        WaitForSingleObject(prof_thread_handle, 500);
        profile_report();
    }
}
