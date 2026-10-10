/* High-level emulation of the 989snd sound library's IOP RPC servers (silent for now).
 *
 * The EE side batches sound commands into one RPC call on server 0x123456. The reply buffer
 * holds one result per command between two 0xFFFFFFFF sentinel words; the EE only accepts it
 * when both sentinels are present (proof the IOP's DMA arrived completely). Direct ("wait")
 * calls get a 12-byte reply: sentinel, result, sentinel.
 *
 * 989snd's own commands are answered with 0. Sucker Punch's IOP module spiop, which 989snd
 * reaches through its extern-call commands (76 and 104, procs registered under 'SUKR'), streams
 * music and dialogue from the disc. The game waits on those streams (a dialogue line's subtitle
 * is revealed as its voice plays, and the line ends when spiop reports the stream finished), so
 * until the native audio path replaces 989snd and spiop (notes/audio-native-plan.md) the stream
 * channels are modelled here without sound; see "spiop" below. */
#include "ps2_internal.h"

#include <stdlib.h>
#include <windows.h>

#define SND_SID_CMD   0x00123456u
#define SND_SID_OTHER 0x00123457u

#define SND_CMD_EXTERN      76     /* extern call: id, proc, 5 argument words */
#define SND_CMD_EXTERN_DATA 104    /* extern call with data: id, proc, size, data[size] */
#define SND_CMD_BATCH       77

static int log_snd = -1;           /* PS2_LOG_SND=1: list sound commands and stream events */

/* ---- spiop stream channels ----------------------------------------------------------------------
 * spiop (module "spiop", procs table at 0x54E8 of the IRX) has three stream channels. What the
 * game uses, read from spiop's code:
 *   proc 1 (cmd 104, 24 data bytes: sector, volume, ?, channel, mode, ?) opens a stream on a
 *          channel: 0 if the channel is in use, else a new handle (a counter, never 0). The
 *          stream's thread reads a header at `sector` ("VAGp" mono, or " KPV" interleaved with
 *          per-channel byte length at +4 and sample rate at +0x10) and plays it; mode bit 2 loops.
 *   proc 2 (handle) closes it and frees the channel.   proc 3 / 4 (handle) pause / resume.
 *   proc 7 (handle) is 1 once the stream is buffered and ready (or the handle is unknown).
 *   proc 8 (handle) is 1 while the stream is active, 0 once it has finished (or is unknown).
 *   proc 5, 6, 9-13: parameters and status words; answered 0.
 * The EE opens a stream, waits for proc 7, resumes it (proc 4), polls proc 8 every frame for
 * one-shot streams and closes it (proc 2) when proc 8 returns 0. Time is host time, like the
 * EE's COP0 Count the game measures dialogue with. */
#define SUKR 0x524B5553u

typedef struct { uint32_t handle; int loop, playing; double length, played, since; } sp_chan;
static sp_chan sp[3];
static uint32_t sp_next;

static double now_seconds(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Playing time of the stream whose header is at `lbn`, in seconds (0 if unreadable). PS2 ADPCM
 * packs 28 samples into 16 bytes. */
static double stream_length(uint32_t lbn) {
    static uint8_t hdr[2048];
    if (!cdvd_read_host(lbn, 1, hdr)) return 0;
    uint32_t bytes = 0, rate = 0;
    if (le32(hdr) == 0x70474156u) { bytes = be32(hdr + 12); rate = be32(hdr + 16); }        /* "VAGp" */
    else if (le32(hdr) == 0x56504B20u) { bytes = le32(hdr + 4); rate = le32(hdr + 16); }    /* " KPV" */
    return rate ? (double)(bytes / 16) * 28.0 / (double)rate : 0;
}

static sp_chan* sp_find(uint32_t handle) {
    for (int i = 0; i < 3; i++) if (handle && sp[i].handle == handle) return &sp[i];
    return NULL;
}

static void sp_advance(sp_chan* c) {
    if (!c->playing) return;
    double t = now_seconds();
    c->played += t - c->since;
    c->since = t;
}

static uint32_t spiop_proc(uint32_t proc, const uint32_t* arg) {
    sp_chan* c = (proc >= 2 && proc <= 9) ? sp_find(arg[0]) : NULL;
    uint32_t r = 0;
    switch (proc) {
    case 1: {                                          /* open: arg[0] = size, data follows */
        const uint32_t* d = arg + 1;
        uint32_t ch = d[3];
        if (arg[0] != 0x18 || ch >= 3 || sp[ch].handle) break;
        if (++sp_next == 0) sp_next = 1;
        sp[ch] = (sp_chan){ sp_next, (int)((d[4] >> 2) & 1), 0, stream_length(d[0]), 0, 0 };
        r = sp_next;
        if (log_snd) fprintf(stderr, "[spiop] v%u open sector %u channel %u mode %u -> handle %u (%.2f s%s)\n",
                             ps2_vblank_count(), d[0], ch, d[4], r, sp[ch].length, sp[ch].loop ? ", loops" : "");
        break;
    }
    case 2:                                            /* close */
        if (c) {
            if (log_snd) fprintf(stderr, "[spiop] v%u close handle %u\n", ps2_vblank_count(), c->handle);
            memset(c, 0, sizeof *c);
        }
        break;
    case 3:                                            /* pause */
        if (c) { sp_advance(c); c->playing = 0; }
        break;
    case 4:                                            /* resume */
        if (c && !c->playing) { c->playing = 1; c->since = now_seconds(); }
        break;
    case 7:                                            /* ready to play */
        r = 1;
        break;
    case 8:                                            /* still active */
        if (c) {
            sp_advance(c);
            r = c->loop || c->played < c->length;
            if (!r && log_snd) fprintf(stderr, "[spiop] v%u handle %u finished\n", ps2_vblank_count(), c->handle);
        }
        break;
    default:
        break;
    }
    return r;
}

/* Result of one extern-call command (cmd 76 or 104); `w` = its data words, `nw` = their count. */
static uint32_t extern_call(int cmd, const uint32_t* w, uint32_t nw) {
    if (nw < 3 || w[0] != SUKR) return 0;
    uint32_t arg[8] = { 0 };
    for (uint32_t k = 2; k < nw && k - 2 < 8; k++) arg[k - 2] = w[k];   /* cmd 104: size, data... */
    (void)cmd;
    return spiop_proc(w[1], arg);
}

static void log_words(const char* what, int cmd, const uint8_t* p, uint32_t size) {
    fprintf(stderr, "[snd] v%u %s %d (%u bytes):", ps2_vblank_count(), what, cmd, size);
    for (uint32_t k = 0; k + 4 <= size && k < 32; k += 4) fprintf(stderr, " %08X", le32(p + k));
    fprintf(stderr, "\n");
}

int snd_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
            uint8_t* out, uint32_t recv_size) {
    (void)ctx;
    if (log_snd < 0) log_snd = getenv("PS2_LOG_SND") != NULL;
    if (sid == SND_SID_CMD) {
        if (recv_size >= 8) {
            uint32_t sentinel = 0xFFFFFFFFu;
            memcpy(out, &sentinel, 4);
            memcpy(out + (recv_size & ~3u) - 4, &sentinel, 4);
        }
        if (ps2_log_sif) fprintf(stderr, "[snd] batch fn %u (%u bytes of results)\n", fno, recv_size);
        if (fno == SND_CMD_BATCH && send_size >= 4) {
            /* int count, then per command: s16 cmd, s16 size, data rounded up to 4 bytes */
            uint32_t count = le32(args), pos = 4;
            for (uint32_t i = 0; i < count && pos + 4 <= send_size; i++) {
                int16_t cmd, size;
                memcpy(&cmd, args + pos, 2);
                memcpy(&size, args + pos + 2, 2);
                uint32_t n = size > 0 ? (uint32_t)size : 0;
                if (pos + 4 + n > send_size) break;
                if (log_snd) log_words("cmd", cmd, args + pos + 4, n);
                if (cmd == SND_CMD_EXTERN || cmd == SND_CMD_EXTERN_DATA) {
                    uint32_t w[16] = { 0 };
                    memcpy(w, args + pos + 4, n < sizeof w ? n : sizeof w);
                    uint32_t r = extern_call(cmd, w, (n < sizeof w ? n : sizeof w) / 4);
                    if (8 + 4 * i <= recv_size) memcpy(out + 4 + 4 * i, &r, 4);
                }
                pos += 4 + ((n + 3) & ~3u);
            }
        } else {
            if (log_snd) log_words("direct cmd", (int)fno, args, send_size);
            if (fno == SND_CMD_EXTERN || fno == SND_CMD_EXTERN_DATA) {
                uint32_t w[16] = { 0 };
                uint32_t n = send_size < sizeof w ? send_size : sizeof w;
                memcpy(w, args, n);
                uint32_t r = extern_call((int)fno, w, n / 4);
                if (recv_size >= 12) memcpy(out + 4, &r, 4);
            }
        }
        return 1;
    }
    if (sid == SND_SID_OTHER) {
        if (ps2_log_sif) fprintf(stderr, "[snd] 0x123457 fn %u\n", fno);
        return 1;                                   /* zeros */
    }
    return 0;
}
