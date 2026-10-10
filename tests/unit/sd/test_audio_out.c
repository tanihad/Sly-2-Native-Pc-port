/* Tests for runtime/audio_out.c (with runtime/sd.c behind it):
 *   1. PS2_AUDIO_TEST=1 through the real-time null sink: the WAV holds a clean 440 Hz tone.
 *   2. The sound backend through the null sink: a voice's 1 kHz sine reaches the WAV.
 *   3. The device path (WASAPI) starts and is paced by the device; silent unless --audible,
 *      which plays the test tone through the speakers for two seconds.
 * usage: test_audio_out <out dir> [--audible] */
#include "audio_out.h"
#include "sd_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static uint8_t g_iop[2 << 20];
uint8_t* iop_mem(uint32_t a) {
    a &= 0x1FFFFFFFu;
    return a < sizeof g_iop ? g_iop + a : NULL;
}
void iop_sd_transfer_done(int channel) { (void)channel; }

static int g_failed, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int16_t* s; uint32_t frames; int ok; } wav;

static wav wav_read(const char* path) {
    wav w = { 0 };
    FILE* f = fopen(path, "rb");
    if (!f) return w;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t h[44];
    if (size >= 44 && fread(h, 1, 44, f) == 44) {
        uint32_t rate, data;
        uint16_t fmt, ch, bits;
        memcpy(&fmt, h + 20, 2); memcpy(&ch, h + 22, 2); memcpy(&rate, h + 24, 4);
        memcpy(&bits, h + 34, 2); memcpy(&data, h + 40, 4);
        w.ok = !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4) &&
               fmt == 1 && ch == 2 && rate == 48000 && bits == 16 && data == (uint32_t)(size - 44);
        w.frames = (uint32_t)(size - 44) / 4;
        w.s = malloc((size_t)w.frames * 4 + 4);
        w.frames = (uint32_t)fread(w.s, 4, w.frames, f);
    }
    fclose(f);
    return w;
}

static double measure_freq(const int16_t* s, int n) {
    double first = -1, last = -1;
    int count = 0;
    for (int i = 1; i < n; i++) {
        int a = s[2 * (i - 1)], b = s[2 * i];
        if (a < 0 && b >= 0) {
            double t = (i - 1) + (double)(-a) / (double)(b - a);
            if (first < 0) first = t;
            last = t;
            count++;
        }
    }
    return count > 1 ? (count - 1) * 48000.0 / (last - first) : 0;
}

static void run_for(DWORD ms) {
    audio_init();
    Sleep(ms);
    audio_shutdown();
}

static void put_env(const char* k, const char* v) { _putenv_s(k, v); }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_audio_out <out dir> [--audible]\n"); return 2; }
    int audible = argc > 2 && !strcmp(argv[2], "--audible");
    char path[MAX_PATH];

    /* 1. test tone, no device */
    printf("PS2_AUDIO_TEST=1 through the null sink\n");
    snprintf(path, sizeof path, "%s\\tone.wav", argv[1]);
    put_env("PS2_AUDIO_TEST", "1");
    put_env("PS2_AUDIO_NULL", "1");
    put_env("PS2_AUDIO_WAV", path);
    audio_init();
    Sleep(100);
    CHECK(audio_backend() == AUDIO_NULL, "backend %d", audio_backend());
    Sleep(1400);
    audio_shutdown();
    wav w = wav_read(path);
    CHECK(w.ok, "%s: bad WAV header", path);
    printf("  %u frames (%.2f s) in %s\n", w.frames, w.frames / 48000.0, path);
    CHECK(w.frames > 48000 * 1.2 && w.frames < 48000 * 1.8, "real-time pacing: %u frames for 1.5 s", w.frames);
    if (w.frames > 4800) {
        double f = measure_freq(w.s, (int)w.frames);
        int peak = 0, maxstep = 0, lr = 1;
        for (uint32_t i = 0; i < w.frames; i++) {
            int l = w.s[2 * i];
            peak = abs(l) > peak ? abs(l) : peak;
            lr &= w.s[2 * i] == w.s[2 * i + 1];
            if (i) { int d = abs(l - w.s[2 * i - 2]); maxstep = d > maxstep ? d : maxstep; }
        }
        printf("  %.3f Hz, peak %d, largest sample step %d\n", f, peak, maxstep);
        CHECK(fabs(f - 440.0) < 0.05, "tone frequency %.3f", f);
        CHECK(peak >= 8190 && peak <= 8192, "tone level %d", peak);
        CHECK(maxstep <= 8192 * 2 * 3.14159265 * 440 / 48000 + 2, "discontinuity: step %d", maxstep);
        CHECK(lr, "left != right");
    }
    free(w.s);

    /* 2. the sound backend behind the output thread */
    printf("sd_render through the null sink\n");
    snprintf(path, sizeof path, "%s\\voice.wav", argv[1]);
    put_env("PS2_AUDIO_TEST", "0");
    put_env("PS2_AUDIO_WAV", path);
    sceSdInit(0);
    sceSdSetParam(0x0800, 0xF00);
    sceSdSetParam(0x0801, 0xF0C);
    for (int c = 0; c < 2; c++) { sceSdSetParam((uint16_t)(0x0980 | c), 0x3FFF); sceSdSetParam((uint16_t)(0x0A80 | c), 0x3FFF); }
    /* a 1 kHz sine as 16-bit samples -> filter 0 ADPCM is not exact, so use 12 blocks of a
     * shift-0 square-ish wave instead: +4000h for 24 samples, -4000h for 24 */
    uint8_t* ram = (uint8_t*)sd_ram();
    for (int b = 0; b < 12; b++) {
        uint8_t* blk = ram + 0x20000 + 16 * b;
        memset(blk, 0, 16);
        blk[1] = b == 0 ? 4 : b == 11 ? 3 : 0;
        for (int i = 0; i < 28; i++) {
            int n = 28 * b + i;
            int nib = (n % 48) < 24 ? 4 : 0xC;
            blk[2 + i / 2] |= (uint8_t)(nib << ((i & 1) * 4));
        }
    }
    sceSdSetParam(0x0000, 0x3FFF);  sceSdSetParam(0x0100, 0x3FFF);
    sceSdSetParam(0x0200, 0x1000);  sceSdSetParam(0x0300, 0x000F);  sceSdSetParam(0x0400, 0x1FC0);
    sceSdSetAddr(0x2040, 0x20000);
    sceSdSetSwitch(0x1500, 1);
    uint64_t before = sd_frames_rendered();
    run_for(1000);
    w = wav_read(path);
    CHECK(w.ok, "%s: bad WAV header", path);
    CHECK(sd_frames_rendered() - before == w.frames, "every rendered frame written (%llu vs %u)",
          (unsigned long long)(sd_frames_rendered() - before), w.frames);
    if (w.frames > 9600) {
        double f = measure_freq(w.s + 2 * 4800, (int)w.frames - 4800);
        printf("  %u frames, %.2f Hz\n", w.frames, f);
        CHECK(fabs(f - 1000.0) < 1.0, "voice frequency %.2f", f);
    }
    free(w.s);
    sceSdSetSwitch(0x1600, 1);

    /* 3. device path */
    printf("device output (WASAPI)%s\n", audible ? ": you should hear a 440 Hz tone for 2 s" : "");
    snprintf(path, sizeof path, "%s\\device.wav", argv[1]);
    put_env("PS2_AUDIO_NULL", "0");
    put_env("PS2_AUDIO_TEST", audible ? "1" : "0");
    put_env("PS2_AUDIO_WAV", path);
    DWORD ms = audible ? 2000 : 1000;
    LARGE_INTEGER qf, q0, q1;
    QueryPerformanceFrequency(&qf);
    audio_init();
    for (int i = 0; i < 300 && audio_backend() == AUDIO_OFF; i++) Sleep(10);   /* device start-up */
    int backend = audio_backend();
    uint64_t f0 = audio_frames_written();
    QueryPerformanceCounter(&q0);
    Sleep(ms);
    uint64_t frames = audio_frames_written() - f0;
    QueryPerformanceCounter(&q1);
    audio_shutdown();
    double secs = (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart;
    printf("  backend: %s, %llu frames in %.3f s\n", backend == AUDIO_WASAPI ? "WASAPI" : backend == AUDIO_NULL ? "null (no device)" : "off",
           (unsigned long long)frames, secs);
    CHECK(backend == AUDIO_WASAPI || backend == AUDIO_NULL, "no backend running");
    CHECK(frames > 48000 * secs * 0.9 && frames < 48000 * secs * 1.1, "device pacing: %llu frames in %.3f s",
          (unsigned long long)frames, secs);

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
