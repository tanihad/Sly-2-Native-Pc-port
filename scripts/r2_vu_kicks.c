/* (r2-re) What the game's VU1 microprograms output: replays a VU recording (PS2_VU_RECORD, runtime/vu.c) with the
 * interpreter and decodes every XGKICK packet (GIF tags, PRIM, registers, the first vertices), so a program's output can
 * be compared with its input (the mesh data the VIF unpacked). Built by scripts/r2_vu_kicks.ps1.
 *   r2_vu_kicks <recording> [first program] [count] [dump program]
 * Prints one block per VU1 program: entry address, kicks; per GIF tag: FLG, NLOOP, NREG, REGS, PRIM, EOP and up to 4
 * loops decoded (PACKED: ST/UV, RGBAQ, XYZF2/XYZ2, A+D register writes). With [dump program], that program's input data
 * memory is also written as r2_vu_in_<n>.bin and its output memory as r2_vu_out_<n>.bin (16 KB each). */
#include "vu_internal.h"
#include "vu_record.h"

#include <stdlib.h>
#include <windows.h>
#include <xmmintrin.h>

static const uint8_t* cur_data;
static uint32_t kicks, verbose_loops = 4;
static uint64_t total_verts, total_prims, total_ad;

static const char* regname(unsigned r) {
    static const char* n[16] = { "PRIM", "RGBAQ", "ST", "UV", "XYZF2", "XYZ2", "TEX0_1", "TEX0_2", "CLAMP_1", "CLAMP_2",
                                 "FOG", "?B", "XYZF3", "XYZ3", "A+D", "NOP" };
    return n[r & 15];
}
static const char* adname(unsigned r) {
    switch (r) {
    case 0x00: return "PRIM"; case 0x01: return "RGBAQ"; case 0x06: return "TEX0_1"; case 0x08: return "CLAMP_1";
    case 0x14: return "TEX1_1"; case 0x3B: return "TEXA"; case 0x3D: return "FOGCOL"; case 0x3F: return "TEXFLUSH";
    case 0x42: return "ALPHA_1"; case 0x47: return "TEST_1"; case 0x4C: return "FRAME_1"; case 0x4E: return "ZBUF_1";
    case 0x49: return "FBA_1"; case 0x46: return "COLCLAMP"; case 0x40: return "SCISSOR_1"; case 0x18: return "XYOFFSET_1";
    case 0x1A: return "PRMODECONT"; case 0x1B: return "PRMODE"; case 0x44: return "PABE"; case 0x45: return "DTHE";
    default: return "?";
    }
}

void gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr) {
    kicks++;
    for (int guard = 0; guard < 64; guard++) {
        const uint64_t* q = (const uint64_t*)(vu1_data + (qw_addr & 0x3FF) * 16);
        uint64_t lo = q[0], hi = q[1];
        uint32_t nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, pre = (lo >> 46) & 1, prim = (lo >> 47) & 0x7FF,
                 flg = (lo >> 58) & 3, nreg = (uint32_t)(lo >> 60);
        if (!nreg) nreg = 16;
        printf("    tag @%03X %s NLOOP %u NREG %u REGS %016llX%s", qw_addr & 0x3FF, flg == 0 ? "PACKED" : flg == 1 ? "REGLIST" : "IMAGE",
               nloop, nreg, (unsigned long long)hi, eop ? " EOP" : "");
        if (pre) printf(" PRIM %03X (type %u IIP %u TME %u FGE %u ABE %u AA1 %u FST %u CTXT %u)", prim, prim & 7, (prim >> 3) & 1,
                        (prim >> 4) & 1, (prim >> 5) & 1, (prim >> 6) & 1, (prim >> 7) & 1, (prim >> 8) & 1, (prim >> 9) & 1);
        printf("\n");
        uint32_t a = (qw_addr + 1) & 0x3FF;
        if (flg == 0) {
            int is_ad = nreg == 1 && (hi & 15) == 14;
            if (is_ad) total_ad += nloop; else if (pre) { total_prims++; total_verts += nloop; }
            for (uint32_t l = 0; l < nloop; l++) {
                if (l < verbose_loops || (is_ad && l < 16)) {
                    printf("      ");
                    for (uint32_t r = 0; r < nreg; r++) {
                        const uint32_t* w = (const uint32_t*)(vu1_data + ((a + r) & 0x3FF) * 16);
                        const float* f = (const float*)w;
                        unsigned reg = (unsigned)(hi >> (4 * r)) & 15;
                        switch (reg) {
                        case 2: printf("ST(%.4f %.4f Q %.4g) ", f[0], f[1], f[2]); break;
                        case 1: printf("RGBA(%u %u %u %u) ", w[0] & 0xFF, w[1] & 0xFF, w[2] & 0xFF, w[3] & 0xFF); break;
                        case 4: case 5: printf("%s(%.2f %.2f z %u f %u adc %u) ", regname(reg), (w[0] & 0xFFFF) / 16.0,
                                               (w[1] & 0xFFFF) / 16.0, reg == 4 ? (w[2] >> 4) & 0xFFFFFF : w[2],
                                               (w[3] >> 4) & 0xFF, (w[3] >> 15) & 1); break;
                        case 14: printf("%s(%02X)=%08X%08X ", adname(w[2] & 0xFF), w[2] & 0xFF, w[1], w[0]); break;
                        case 15: break;
                        default: printf("%s[%08X %08X %08X %08X] ", regname(reg), w[0], w[1], w[2], w[3]);
                        }
                    }
                    printf("\n");
                }
                a = (a + nreg) & 0x3FF;
            }
        } else if (flg == 1) {
            a = (a + (nloop * nreg + 1) / 2) & 0x3FF;
        } else {
            a = (a + nloop) & 0x3FF;
        }
        if (eop) break;
        qw_addr = a;
    }
}
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
}
uint32_t ps2_vblank_count(void) { return 0; }

typedef struct { const vu_rec_head* h; const uint8_t* micro; const uint8_t* data; const vu_rec_tail* t; } rec;

static void load_state(vu_state* st, const rec* r) {
    memcpy(vu_data_mem((int)r->h->idx), r->data, r->h->data_size);
    memcpy(st->vf, r->h->vf, sizeof st->vf);
    st->acc = r->h->acc;
    memcpy(st->vi, r->h->vi, sizeof st->vi);
    st->I = r->h->I; st->Q = r->h->Q; st->P = r->h->P; st->R = r->h->R;
    st->status = r->h->status; st->mac = r->h->mac; st->clip = r->h->clip;
    st->top = r->h->top; st->itop = r->h->itop;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: r2_vu_kicks <recording> [first] [count] [dump]\n"); return 2; }
    size_t first = argc > 2 ? (size_t)atoi(argv[2]) : 0, count = argc > 3 ? (size_t)atoi(argv[3]) : 50;
    long dump = argc > 4 ? atol(argv[4]) : -1;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);
    vu_set_vif_tops(0, 0, 0);
    vu_set_vif_tops(1, 0, 0);
    const uint8_t* last_micro[2] = { NULL, NULL };
    const uint8_t* loaded[2] = { NULL, NULL };
    size_t i = 0;
    const unsigned csr = _mm_getcsr();
    for (long pos = 0; pos + (long)sizeof(vu_rec_head) <= size; i++) {
        rec r;
        r.h = (const vu_rec_head*)(buf + pos);
        if (memcmp(r.h->magic, "VUP1", 4)) break;
        pos += sizeof(vu_rec_head);
        if (r.h->micro_size) { last_micro[r.h->idx] = buf + pos; pos += r.h->micro_size; }
        r.micro = last_micro[r.h->idx];
        r.data = buf + pos;
        pos += r.h->data_size;
        r.t = (const vu_rec_tail*)(buf + pos);
        pos += sizeof(vu_rec_tail);
        if (pos > size || !r.micro) break;
        if (i < first || i >= first + count || r.h->idx != 1) continue;
        vu_state* st = &vu_st[1];
        if (loaded[1] != r.micro) { memcpy(vu_micro_mem(1), r.micro, (st->pcmask + 1) * 8u); loaded[1] = r.micro; }
        load_state(st, &r);
        if ((long)i == dump) {
            FILE* o = fopen("r2_vu_in.bin", "wb");
            fwrite(r.data, 1, r.h->data_size, o);
            fclose(o);
        }
        printf("program %zu: VU1 0x%04X vblank %u top %03X itop %03X micro-hash %08X\n", i, r.h->addr * 8u, r.h->vblank,
               r.h->top, r.h->itop, (uint32_t)vu_hash_words(VU_HASH_SEED, r.micro, 64));
        kicks = 0;
        _mm_setcsr(VU_MXCSR);
        uint32_t np = vu_run(st, r.h->addr);
        _mm_setcsr(csr);
        printf("  -> %u pairs, %u kicks\n", np, kicks);
        if ((long)i == dump) {
            FILE* o = fopen("r2_vu_out.bin", "wb");
            fwrite(st->data, 1, r.h->data_size, o);
            fclose(o);
        }
    }
    printf("total: %llu primitives (GIF tags with PRIM), %llu vertices, %llu A+D writes\n", (unsigned long long)total_prims,
           (unsigned long long)total_verts, (unsigned long long)total_ad);
    return 0;
}
