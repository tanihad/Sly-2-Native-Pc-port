/* (pc-fps2) PS2_CHAIN_DUMP=<vblank>[:<count>]:<file>: writes the VIF1 DMA chains kicked from that VBlank on (count chains,
 * default 2) as text - every DMA tag and the VIF codes in its data (UNPACK target, format, size; MSCAL; DIRECT; the
 * values of V4-32 unpacks) - to study what a frame uploads to VU1 (frame interpolation research, notes/frame-rate.md). */
#include "hw.h"

#include <stdlib.h>

static FILE* out;
static uint32_t from = ~0u, left;

static const char* vname(uint32_t cmd) {
    switch (cmd) {
    case 0x00: return "NOP"; case 0x01: return "STCYCL"; case 0x02: return "OFFSET"; case 0x03: return "BASE";
    case 0x04: return "ITOP"; case 0x05: return "STMOD"; case 0x06: return "MSKPATH3"; case 0x07: return "MARK";
    case 0x10: return "FLUSHE"; case 0x11: return "FLUSH"; case 0x13: return "FLUSHA"; case 0x14: return "MSCAL";
    case 0x15: return "MSCALF"; case 0x17: return "MSCNT"; case 0x20: return "STMASK"; case 0x30: return "STROW";
    case 0x31: return "STCOL"; case 0x4A: return "MPG"; case 0x50: return "DIRECT"; case 0x51: return "DIRECTHL";
    default: return cmd >= 0x60 ? "UNPACK" : "?";
    }
}

/* VIF codes in n words; *skip = data words of a command continuing from the previous block */
static void vif_words(const uint32_t* w, uint32_t n, uint32_t* skip, uint32_t* cycle) {
    uint32_t i = 0;
    if (*skip) { uint32_t s = *skip < n ? *skip : n; i = s; *skip -= s; }
    while (i < n) {
        uint32_t code = w[i++], cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF, data = 0;
        if (cmd >= 0x60) {
            uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, nn = num ? num : 256;
            uint32_t cl = *cycle & 0xFF, wl = (*cycle >> 8) & 0xFF;
            uint32_t nin = (!wl || wl <= cl) ? nn : (nn / wl) * cl + (nn % wl < cl ? nn % wl : cl);
            uint32_t vb = (vl == 3) ? 2 : (4u >> vl) * (vn + 1);
            data = (nin * vb + 3) / 4;
            fprintf(out, "      UNPACK V%u-%u%s%s addr 0x%03X%s num %u (%u words)", vn + 1, vl == 3 ? 5 : 32 >> vl,
                    (cmd & 0x10) ? " mask" : "", (imm & 0x4000) ? " usn" : "", imm & 0x3FF, (imm & 0x8000) ? "+tops" : "",
                    nn, data);
            if (vn == 3 && vl == 0 && nn <= 8 && i + data <= n) {
                const float* f = (const float*)(w + i);
                for (uint32_t k = 0; k < data; k++) fprintf(out, "%s%.4g", k % 4 ? " " : " | ", f[k]);
            }
            fprintf(out, "\n");
        } else {
            switch (cmd) {
            case 0x20: data = 1; break;
            case 0x30: case 0x31: data = 4; break;
            case 0x4A: data = (num ? num : 256) * 2; break;
            case 0x50: case 0x51: data = (imm ? imm : 65536) * 4; break;
            case 0x01: *cycle = imm; break;
            default: break;
            }
            if (cmd != 0x00)
                fprintf(out, "      %s imm 0x%04X num %u%s\n", vname(cmd), imm, num, (code >> 31) ? " [i]" : "");
        }
        uint32_t take = data < n - i ? data : n - i;
        i += take;
        *skip = data - take;
    }
}

/* (pc-research) PS2_CHAIN_STATS=<vblank>:<count>:<file>: one line per DMA tag of the kicked VIF1 chains - tag id, data
 * address, qwc, a hash of the data and its VIF payload split (unpack / direct / mpg / other words, MSCALs) - to measure
 * how much of a frame's VU1 input is the same from frame to frame (scripts/chain_stats.py). */
static void vif_count(const uint32_t* w, uint32_t n, uint32_t* skip, uint32_t* cycle, uint32_t* skind, uint32_t cnt[5]) {
    uint32_t i = 0;
    if (*skip) { uint32_t s = *skip < n ? *skip : n; i = s; *skip -= s; cnt[*skind] += s; }
    while (i < n) {
        uint32_t code = w[i++], cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF, data = 0, kind = 3;
        if (cmd >= 0x60) {
            uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, nn = num ? num : 256;
            uint32_t cl = *cycle & 0xFF, wl = (*cycle >> 8) & 0xFF;
            uint32_t nin = (!wl || wl <= cl) ? nn : (nn / wl) * cl + (nn % wl < cl ? nn % wl : cl);
            uint32_t vb = (vl == 3) ? 2 : (4u >> vl) * (vn + 1);
            data = (nin * vb + 3) / 4;
            kind = 0;
        } else {
            switch (cmd) {
            case 0x20: data = 1; break;
            case 0x30: case 0x31: data = 4; break;
            case 0x4A: data = (num ? num : 256) * 2; kind = 2; break;
            case 0x50: case 0x51: data = (imm ? imm : 65536) * 4; kind = 1; break;
            case 0x01: *cycle = imm; break;
            case 0x14: case 0x15: case 0x17: cnt[4]++; break;
            default: break;
            }
        }
        uint32_t take = data < n - i ? data : n - i;
        cnt[kind] += take;
        i += take;
        *skip = data - take;
        *skind = kind;
    }
}

static void chain_stats(uint32_t tadr) {
    static int init;
    static FILE* so;
    static uint32_t sfrom = ~0u, sleft;
    if (!init) {
        init = 1;
        const char* e = getenv("PS2_CHAIN_STATS");
        char path[400];
        unsigned f = 0, c = 2;
        if (e && sscanf(e, "%u:%u:%399s", &f, &c, path) == 3) { so = fopen(path, "w"); sfrom = f; sleft = c; }
    }
    if (!so || !sleft || ps2_vblank_count() < sfrom) return;
    sleft--;
    fprintf(so, "C %08X %u\n", tadr, ps2_vblank_count());
    uint32_t stack[2], sp = 0, skip = 0, skind = 0, cycle = 0x0101;
    for (int n = 0; n < 400000; n++) {
        const uint8_t* p = hw_dma_ptr(tadr, 16);
        if (!p) break;
        uint64_t t;
        memcpy(&t, p, 8);
        uint32_t qwc = (uint32_t)t & 0xFFFF, id = (uint32_t)(t >> 28) & 7, addr = (uint32_t)(t >> 32) & 0xFFFFFFF0u;
        uint32_t data = (id == 0 || id == 3 || id == 4) ? addr : tadr + 16, next = tadr + 16 + qwc * 16;
        const uint8_t* d = qwc ? hw_dma_ptr(data, qwc * 16) : NULL;
        uint64_t h = 1469598103934665603ull;
        uint32_t cnt[5] = { 0, 0, 0, 0, 0 };
        if (d) {
            for (uint32_t k = 0; k < qwc * 16; k++) h = (h ^ d[k]) * 1099511628211ull;
            vif_count((const uint32_t*)d, qwc * 4, &skip, &cycle, &skind, cnt);
        }
        fprintf(so, "T %u %08X %u %016llX %u %u %u %u %u\n", id, data, qwc, (unsigned long long)h, cnt[0], cnt[1], cnt[2],
                cnt[3], cnt[4]);
        int stop = 0;
        switch (id) {
        case 0: stop = 1; break;
        case 1: tadr = next; break;
        case 2: tadr = addr; break;
        case 3: case 4: tadr += 16; break;
        case 5: if (sp < 2) stack[sp++] = next; tadr = addr; break;
        case 6: if (sp) tadr = stack[--sp]; else stop = 1; break;
        default: stop = 1; break;
        }
        if (stop) break;
    }
    fflush(so);
    if (!sleft) { fclose(so); so = NULL; }
}

void chain_dump(uint32_t tadr) {
    chain_stats(tadr);
    static int init;
    if (!init) {
        init = 1;
        const char* e = getenv("PS2_CHAIN_DUMP");
        if (!e) return;
        char path[400];
        unsigned f = 0, c = 2;
        if (sscanf(e, "%u:%u:%399s", &f, &c, path) == 3 || (c = 2, sscanf(e, "%u:%399s", &f, path) == 2)) {
            out = fopen(path, "w");
            from = f;
            left = c;
        }
    }
    if (!out || !left || ps2_vblank_count() < from) return;
    left--;
    fprintf(out, "==== VIF1 chain at 0x%08X, VBlank %u\n", tadr, ps2_vblank_count());
    uint32_t stack[2], sp = 0, skip = 0, cycle = 0x0101;
    for (int n = 0; n < 200000; n++) {
        const uint8_t* p = hw_dma_ptr(tadr, 16);
        if (!p) { fprintf(out, "  bad tag address 0x%08X\n", tadr); break; }
        uint64_t t;
        memcpy(&t, p, 8);
        uint32_t qwc = (uint32_t)t & 0xFFFF, id = (uint32_t)(t >> 28) & 7, addr = (uint32_t)(t >> 32) & 0xFFFFFFF0u;
        static const char* ids[8] = { "refe", "cnt", "next", "ref", "refs", "call", "ret", "end" };
        fprintf(out, "  %08X %-4s qwc %4u addr %08X\n", tadr, ids[id], qwc, addr);
        uint32_t data = (id == 0 || id == 3 || id == 4) ? addr : tadr + 16, next = tadr + 16 + qwc * 16;
        const uint8_t* d = qwc ? hw_dma_ptr(data, qwc * 16) : NULL;
        if (d) vif_words((const uint32_t*)d, qwc * 4, &skip, &cycle);
        int stop = 0;
        switch (id) {
        case 0: stop = 1; break;                                  /* refe */
        case 1: tadr = next; break;                               /* cnt */
        case 2: tadr = addr; break;                               /* next */
        case 3: case 4: tadr += 16; break;                        /* ref, refs */
        case 5: if (sp < 2) stack[sp++] = next; tadr = addr; break;   /* call */
        case 6: if (sp) tadr = stack[--sp]; else stop = 1; break; /* ret */
        default: stop = 1; break;                                 /* end */
        }
        if (stop) break;
    }
    fflush(out);
    if (!left) { fclose(out); out = NULL; }
}
