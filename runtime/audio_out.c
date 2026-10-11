/* Audio output: pulls 48 kHz stereo from the native sound backend (sd_render) on its own thread
 * and plays it through WASAPI (shared mode, event driven, 16-bit PCM with Windows doing any
 * format/rate conversion: AUTOCONVERTPCM | SRC_DEFAULT_QUALITY).
 *
 * The sound backend is rendered on demand: each device period we render exactly what the device
 * asks for to keep about PS2_AUDIO_LATENCY (40) ms queued, so voice positions (NAX/ENDX) advance
 * with real playback. Without a device (or PS2_AUDIO_NULL=1) the same thread renders in real time
 * into a null sink; a device that disappears is retried every few seconds.
 *
 * COM and avrt are loaded at run time (LoadLibrary), so linking needs no extra libraries; the
 * GUIDs are defined here so libuuid isn't needed either. */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"
#include "sd_internal.h"

#define RATE 48000

static const GUID k_CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID k_IID_IMMDeviceEnumerator  = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID k_IID_IAudioClient         = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID k_IID_IAudioRenderClient   = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

typedef HRESULT (WINAPI *pfn_CoInitializeEx)(LPVOID, DWORD);
typedef void    (WINAPI *pfn_CoUninitialize)(void);
typedef HRESULT (WINAPI *pfn_CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
typedef HANDLE  (WINAPI *pfn_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);

static pfn_CoInitializeEx   p_CoInitializeEx;
static pfn_CoUninitialize   p_CoUninitialize;
static pfn_CoCreateInstance p_CoCreateInstance;

static volatile LONG g_started;
static volatile LONG g_stop;
static volatile LONG g_backend;
static volatile LONG64 g_written;
static HANDLE g_thread;
static int    g_tone;
static int    g_force_null;
static int    g_latency_ms = 40;
static double g_tone_phase;

/* ---- WAV dump ------------------------------------------------------------------------------- */

static FILE*    g_wav;
static uint64_t g_wav_frames;
static DWORD    g_wav_tick;

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void wav_header(void) {
    uint8_t h[44];
    uint64_t bytes64 = g_wav_frames * 4;
    uint32_t bytes = bytes64 > 0xFFFFFFD3u ? 0xFFFFFFD3u : (uint32_t)bytes64;
    memcpy(h, "RIFF", 4);      put32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1);          put16(h + 22, 2);           /* PCM, stereo */
    put32(h + 24, RATE);       put32(h + 28, RATE * 4);
    put16(h + 32, 4);          put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, bytes);
    long pos = ftell(g_wav);
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, g_wav);
    if (pos > (long)sizeof h) fseek(g_wav, pos, SEEK_SET);
    fflush(g_wav);
}

static void wav_write(const int16_t* s, uint32_t frames) {
    fwrite(s, 4, frames, g_wav);
    g_wav_frames += frames;
    DWORD now = GetTickCount();
    if (now - g_wav_tick >= 250) {       /* keep the header valid: the game may ExitProcess */
        g_wav_tick = now;
        wav_header();
    }
}

/* ---- sample production -------------------------------------------------------------------- */

static void tone_render(int16_t* out, uint32_t frames) {
    const double step = 2.0 * 3.14159265358979323846 * 440.0 / RATE;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(sin(g_tone_phase) * 8192.0);     /* -12 dBFS */
        out[2 * i] = out[2 * i + 1] = v;
        g_tone_phase += step;
        if (g_tone_phase >= 2.0 * 3.14159265358979323846) g_tone_phase -= 2.0 * 3.14159265358979323846;
    }
}

/* ---- health: underruns and render cost, reported every 10 s when something happened ---------- */

static volatile LONG g_underruns;          /* device ran dry (WASAPI) / null sink fell >100 ms behind */
static uint64_t g_render_ticks, g_status_frames;
static uint32_t g_status_underruns;
static int64_t  g_status_ticks;

static void status_tick(void) {
    uint64_t w = (uint64_t)g_written;
    if (w - g_status_frames < (uint64_t)RATE * 10) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double audio_s = (double)(w - g_status_frames) / RATE;
    double cpu = 100.0 * (double)(g_render_ticks - (uint64_t)g_status_ticks) / (double)f.QuadPart / audio_s;
    uint32_t u = (uint32_t)g_underruns;
    uint64_t overlaps, bad, holds, hold_frames;
    sd_health(&overlaps, &bad);
    sd_driver_holds(&holds, &hold_frames);
    static uint64_t last_trans;
    uint64_t trans = sd_voice_trans_writes();
    fprintf(stderr, "[audio] status: %.1f s of audio, %u underruns (%u in the last %.0f s), mixer %.1f%% of a core, "
                    "late/early stream refills %llu, invalid ADPCM blocks %llu, voices held for the IOP %llu times (%.2f s), "
                    "%llu voice transfers in the last %.0f s\n",
            (double)w / RATE, u, u - g_status_underruns, audio_s, cpu, (unsigned long long)overlaps, (unsigned long long)bad,
            (unsigned long long)holds, (double)hold_frames / RATE, (unsigned long long)(trans - last_trans), audio_s);
    last_trans = trans;
    g_status_frames = w;
    g_status_ticks = (int64_t)g_render_ticks;
    g_status_underruns = u;
}

static void produce(int16_t* out, uint32_t frames) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (g_tone) tone_render(out, frames);
    else        sd_render(out, frames);
    QueryPerformanceCounter(&t1);
    g_render_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
    if (g_wav) wav_write(out, frames);
    InterlockedExchangeAdd64(&g_written, frames);
    status_tick();
}

/* ---- null sink: real-time pacing without a device ----------------------------------------- */

static void null_run(DWORD retry_ms) {
    LARGE_INTEGER freq, t0, t;
    int16_t buf[2 * 1024];
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    uint64_t done = 0;
    DWORD start = GetTickCount();
    InterlockedExchange(&g_backend, AUDIO_NULL);
    while (!g_stop) {
        Sleep(5);
        QueryPerformanceCounter(&t);
        uint64_t due = (uint64_t)((double)(t.QuadPart - t0.QuadPart) * RATE / (double)freq.QuadPart);
        if (due > done + RATE / 10) {                            /* after a stall, skip ahead */
            done = due - RATE / 10;
            InterlockedIncrement(&g_underruns);
        }
        while (done < due && !g_stop) {
            uint32_t n = due - done > 1024 ? 1024 : (uint32_t)(due - done);
            produce(buf, n);
            done += n;
        }
        if (retry_ms && GetTickCount() - start >= retry_ms) return;
    }
}

/* ---- WASAPI ------------------------------------------------------------------------------- */

static int load_com(void) {
    static int tried, ok;
    if (tried) return ok;
    tried = 1;
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    if (!ole) return 0;
    p_CoInitializeEx   = (pfn_CoInitializeEx)(void*)GetProcAddress(ole, "CoInitializeEx");
    p_CoUninitialize   = (pfn_CoUninitialize)(void*)GetProcAddress(ole, "CoUninitialize");
    p_CoCreateInstance = (pfn_CoCreateInstance)(void*)GetProcAddress(ole, "CoCreateInstance");
    ok = p_CoInitializeEx && p_CoUninitialize && p_CoCreateInstance;
    return ok;
}

/* Returns 1 when stopped by audio_shutdown, -1 when the device was lost, 0 when it never started. */
static int wasapi_run(int quiet) {
    if (!load_com()) return 0;
    HRESULT hr = p_CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com = SUCCEEDED(hr);
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioClient* ac = NULL;
    IAudioRenderClient* rc = NULL;
    HANDLE ev = NULL;
    int result = 0;
    const char* step = "CoCreateInstance(MMDeviceEnumerator)";

    hr = p_CoCreateInstance(&k_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &k_IID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) goto out;
    step = "GetDefaultAudioEndpoint";
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) goto out;
    step = "Activate(IAudioClient)";
    hr = IMMDevice_Activate(dev, &k_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&ac);
    if (FAILED(hr)) goto out;

    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = RATE * 4;
    step = "IAudioClient::Initialize";
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 (REFERENCE_TIME)g_latency_ms * 10000, 0, &wf, NULL);
    if (FAILED(hr)) goto out;
    ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    step = "SetEventHandle";
    hr = IAudioClient_SetEventHandle(ac, ev);
    if (FAILED(hr)) goto out;
    UINT32 buf_frames = 0;
    step = "GetBufferSize";
    hr = IAudioClient_GetBufferSize(ac, &buf_frames);
    if (FAILED(hr)) goto out;
    step = "GetService(IAudioRenderClient)";
    hr = IAudioClient_GetService(ac, &k_IID_IAudioRenderClient, (void**)&rc);
    if (FAILED(hr)) goto out;

    UINT32 target = (UINT32)((uint64_t)g_latency_ms * RATE / 1000);
    if (target > buf_frames) target = buf_frames;
    BYTE* data;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(rc, target, &data))) {
        produce((int16_t*)data, target);
        IAudioRenderClient_ReleaseBuffer(rc, target, 0);
    }
    step = "Start";
    hr = IAudioClient_Start(ac);
    if (FAILED(hr)) goto out;
    InterlockedExchange(&g_backend, AUDIO_WASAPI);
    if (!quiet)
        fprintf(stderr, "[audio] WASAPI shared mode, 48000 Hz stereo, buffer %u frames, target latency %d ms%s\n",
                buf_frames, g_latency_ms, g_tone ? " (440 Hz test tone)" : "");
    else
        fprintf(stderr, "[audio] output device back\n");
    result = 1;
    while (!g_stop) {
        WaitForSingleObject(ev, 100);
        UINT32 pad = 0;
        hr = IAudioClient_GetCurrentPadding(ac, &pad);
        if (FAILED(hr)) break;
        if (pad == 0) {                            /* the device played everything we gave it */
            LONG n = InterlockedIncrement(&g_underruns);
            if (n <= 5) fprintf(stderr, "[audio] underrun at %.3f s of audio\n", (double)g_written / RATE);
        }
        if (pad >= target) continue;
        UINT32 n = target - pad;
        hr = IAudioRenderClient_GetBuffer(rc, n, &data);
        if (FAILED(hr)) break;
        produce((int16_t*)data, n);
        hr = IAudioRenderClient_ReleaseBuffer(rc, n, 0);
        if (FAILED(hr)) break;
    }
    if (!g_stop) {
        fprintf(stderr, "[audio] output device lost (0x%08lX); continuing without sound and retrying\n", (unsigned long)hr);
        result = -1;
    }
    IAudioClient_Stop(ac);
    step = NULL;
out:
    if (result == 0 && !quiet)
        fprintf(stderr, "[audio] no output device (%s failed, 0x%08lX): rendering in real time without sound\n",
                step ? step : "?", (unsigned long)hr);
    if (rc) IAudioRenderClient_Release(rc);
    if (ac) IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (ev) CloseHandle(ev);
    if (com) p_CoUninitialize();
    return result;
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    pfn_AvSetMmThreadCharacteristicsW mm = avrt ? (pfn_AvSetMmThreadCharacteristicsW)(void*)
        GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
    DWORD task = 0;
    if (!mm || !mm(L"Pro Audio", &task)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    if (g_force_null) {
        null_run(0);
        return 0;
    }
    int r = wasapi_run(0);
    if (r == 0) {                                  /* no device at all: real-time null sink */
        null_run(0);
        return 0;
    }
    while (!g_stop && r != 1) {                    /* device lost: keep going, retry every 3 s */
        null_run(3000);
        if (!g_stop) r = wasapi_run(1);
    }
    return 0;
}

/* ---- public ------------------------------------------------------------------------------- */

static int env_on(const char* name) {
    const char* e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}

void audio_init(void) {
    const char* e = getenv("PS2_AUDIO");
    if (e && strcmp(e, "0") == 0) return;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    g_stop = 0;
    g_tone = env_on("PS2_AUDIO_TEST");
    g_force_null = env_on("PS2_AUDIO_NULL");
    g_tone_phase = 0;
    InterlockedExchange64(&g_written, 0);
    const char* lat = getenv("PS2_AUDIO_LATENCY");
    if (lat && atoi(lat) >= 10 && atoi(lat) <= 500) g_latency_ms = atoi(lat);
    const char* wav = getenv("PS2_AUDIO_WAV");
    if (wav && *wav) {
        g_wav = fopen(wav, "wb");
        if (g_wav) {
            g_wav_frames = 0;
            g_wav_tick = GetTickCount();
            wav_header();
            fseek(g_wav, 44, SEEK_SET);
        } else {
            fprintf(stderr, "[audio] cannot create %s\n", wav);
        }
    }
    static int registered;
    if (!registered) { registered = 1; atexit(audio_shutdown); }
    g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    if (!g_thread) {
        fprintf(stderr, "[audio] cannot start the audio thread\n");
        g_started = 0;
    }
}

void audio_shutdown(void) {
    if (!g_started) return;
    InterlockedExchange(&g_stop, 1);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_wav) {
        wav_header();
        fclose(g_wav);
        g_wav = NULL;
    }
    InterlockedExchange(&g_backend, AUDIO_OFF);
    InterlockedExchange(&g_started, 0);
}

int audio_backend(void) { return (int)g_backend; }
uint64_t audio_frames_written(void) { return (uint64_t)g_written; }
