/* Unit tests for runtime/dma.c with fake devices (VIF, GIF, IPU), fake EE memory and D_STAT. */
#include "hw.h"

#include <stdlib.h>

/* ---- fakes ---------------------------------------------------------------------------------------- */

static uint8_t ram[PS2_RAM_SIZE + 16];
static uint8_t spr[PS2_SPR_SIZE];

uint8_t* hw_dma_ptr(uint32_t addr, uint32_t bytes) {          /* same mapping as hw.c */
    if (addr & 0x80000000u) {
        uint32_t off = addr & 0x3FF0u;
        return off + bytes <= PS2_SPR_SIZE ? spr + off : NULL;
    }
    uint32_t a = addr & 0x0FFFFFFFu;
    return a + bytes <= PS2_RAM_SIZE ? ram + a : NULL;
}

static uint32_t d_stat;
static int raised[16];
void dmac_raise(int ch) { d_stat |= 1u << ch; raised[ch]++; }
uint32_t dmac_stat_read(void) { return d_stat; }
void dmac_stat_write(uint32_t v) { d_stat &= ~(v & 0xFFFFu); d_stat ^= v & 0xFFFF0000u; }

static int unhandled;
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write) {
    (void)unit; (void)phys; (void)data; (void)size; (void)is_write;
    unhandled++;
}

/* Every qword a device sees, in order. dev: 'G' GIF, '0'/'1' VIF data, 'a'/'b' VIF0/1 TTE tags, 'I' IPU in */
typedef struct { char dev; uint32_t w[4]; } event;
static event evs[16384];
static int nev;
static uint32_t gif_budget, vif_budget[2], ipu_in_budget;   /* qwords accepted before stalling */
static int vif_tag_ok;
static uint32_t ipu_out_avail, vif1_out_avail, ipu_out_seq, vif1_out_seq;

static void record(char dev, const void* qw) {
    if (nev < (int)(sizeof evs / sizeof evs[0])) { evs[nev].dev = dev; memcpy(evs[nev].w, qw, 16); nev++; }
}
static uint32_t accept(char dev, uint32_t* budget, const void* qw, uint32_t n) {
    if (n > *budget) n = *budget;
    if (*budget != 0xFFFFFFFFu) *budget -= n;
    for (uint32_t i = 0; i < n; i++) record(dev, (const uint8_t*)qw + 16 * i);
    return n;
}
uint32_t gif_path3_dma(const void* qw, uint32_t qwc) { return accept('G', &gif_budget, qw, qwc); }
uint32_t vif_dma(int vif, const void* qw, uint32_t qwc) { return accept(vif ? '1' : '0', &vif_budget[vif], qw, qwc); }
int vif_dma_tag(int vif, const void* tag_qw) {
    if (!vif_tag_ok) return 0;
    record(vif ? 'b' : 'a', tag_qw);
    return 1;
}
/* ipu_fifo_mode: IPU_TO and IPU_FROM share an 8-qword FIFO, like a decoder that needs its output
 * drained before it takes more input (the two channels can only make progress in turns). */
static int ipu_fifo_mode;
static uint32_t ipu_fifo;
uint32_t ipu_dma_to(const void* qw, uint32_t qwc) {
    if (ipu_fifo_mode) { uint32_t room = 8 - ipu_fifo, n = accept('I', &room, qw, qwc); ipu_fifo += n; return n; }
    return accept('I', &ipu_in_budget, qw, qwc);
}
static uint32_t produce(void* qw, uint32_t n, uint32_t* avail, uint32_t* seq, uint32_t tag) {
    if (n > *avail) n = *avail;
    *avail -= n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w[4] = { tag | (*seq)++, 0x11111111u, 0x22222222u, 0x33333333u };
        memcpy((uint8_t*)qw + 16 * i, w, 16);
    }
    return n;
}
uint32_t ipu_dma_from(void* qw, uint32_t qwc) {
    return produce(qw, qwc, ipu_fifo_mode ? &ipu_fifo : &ipu_out_avail, &ipu_out_seq, 0x1F000000u);
}
uint32_t vif1_read(void* qw, uint32_t qwc) { return produce(qw, qwc, &vif1_out_avail, &vif1_out_seq, 0x71000000u); }

/* The IOP hook (ps2_runtime.c): records how many qwords the devices had received at each call. */
static int polls, poll_nev[64];
void ps2_poll_iop(void) { if (polls < 64) poll_nev[polls] = nev; polls++; }

/* ---- helpers -------------------------------------------------------------------------------------- */

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); checks++; if (_a != _b) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s == 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #a, (unsigned long long)_a, (unsigned long long)_b); } } while (0)

static const uint32_t base[10] = { 0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                   0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400 };
#define CHCR(c) (base[c] + 0x00)
#define MADR(c) (base[c] + 0x10)
#define QWC(c)  (base[c] + 0x20)
#define TADR(c) (base[c] + 0x30)
#define ASR0(c) (base[c] + 0x40)
#define ASR1(c) (base[c] + 0x50)
#define SADR(c) (base[c] + 0x80)
enum { D_CTRL = 0x1000E000, D_STAT = 0x1000E010, D_PCR = 0x1000E020, D_SQWC = 0x1000E030, D_RBSR = 0x1000E040,
       D_RBOR = 0x1000E050, D_STADR = 0x1000E060, D_ENABLER = 0x1000F520, D_ENABLEW = 0x1000F590 };

/* CHCR values */
#define STR 0x100u
#define DIR 0x1u
#define CHAIN 0x4u
#define INTERLEAVE 0x8u
#define TTE 0x40u
#define TIE 0x80u

static void w32(uint32_t a, uint32_t v) { dma_access(a, &v, 4, 1); }
static uint32_t r32(uint32_t a) { uint32_t v = 0xDEADBEEF; dma_access(a, &v, 4, 0); return v; }

static uint8_t* mp(uint32_t addr) { return hw_dma_ptr(addr, 16); }

/* qword at addr: { addr, 0xA5A5A5A5, i, ~addr } so devices' input can be traced to its address */
static void fill(uint32_t addr, uint32_t nqw) {
    for (uint32_t i = 0; i < nqw; i++) {
        uint32_t a = addr + 16 * i, w[4] = { a, 0xA5A5A5A5u, i, ~a };
        memcpy(mp(a), w, 16);
    }
}

enum { REFE, CNT, NEXT, REF, REFS, CALL, RET, END };
static uint64_t mktag(uint32_t id, uint32_t qwc, uint32_t addr, int irq) {
    return qwc | ((uint64_t)id << 28) | ((uint64_t)(irq ? 1 : 0) << 31) | ((uint64_t)addr << 32);
}
/* Write a DMAtag at `at`; the upper 64 bits are a recognisable VIF payload. */
static void tag(uint32_t at, uint32_t id, uint32_t qwc, uint32_t addr, int irq) {
    uint64_t lo = mktag(id, qwc, addr, irq), hi = 0x5600000000000000ull | at;
    memcpy(mp(at), &lo, 8);
    memcpy(mp(at) + 8, &hi, 8);
}

static void reset(void) {
    gif_budget = vif_budget[0] = vif_budget[1] = ipu_in_budget = 0;   /* quiesce while stopping */
    ipu_out_avail = vif1_out_avail = 0;
    ipu_fifo_mode = 0; ipu_fifo = 0;
    w32(D_CTRL, 1);
    w32(D_PCR, 0);
    w32(D_ENABLEW, 0x1201);
    for (int c = 0; c < 10; c++) {
        w32(CHCR(c), 0);            /* stops a busy channel (only STR changes) */
        w32(CHCR(c), 0);            /* clears the other fields */
        w32(MADR(c), 0); w32(QWC(c), 0); w32(TADR(c), 0); w32(ASR0(c), 0); w32(ASR1(c), 0); w32(SADR(c), 0);
    }
    w32(D_SQWC, 0); w32(D_RBSR, 0); w32(D_RBOR, 0); w32(D_STADR, 0);
    d_stat = 0;
    memset(raised, 0, sizeof raised);
    nev = 0;
    unhandled = 0;
    gif_budget = vif_budget[0] = vif_budget[1] = ipu_in_budget = 0xFFFFFFFFu;
    vif_tag_ok = 1;
    ipu_out_seq = vif1_out_seq = 0;
}

static int count(char dev) { int n = 0; for (int i = 0; i < nev; i++) n += evs[i].dev == dev; return n; }
static uint32_t ev_w0(char dev, int k) {          /* first word of the k-th qword `dev` received */
    for (int i = 0; i < nev; i++) if (evs[i].dev == dev && k-- == 0) return evs[i].w[0];
    return 0xFFFFFFFFu;
}

/* ---- tests ---------------------------------------------------------------------------------------- */

static void test_registers(void) {
    reset();
    w32(MADR(1), 0x12345678); CHECK_EQ(r32(MADR(1)), 0x12345670);
    w32(QWC(1), 0x12345);     CHECK_EQ(r32(QWC(1)), 0x2345);
    w32(TADR(1), 0x80001234); CHECK_EQ(r32(TADR(1)), 0x80001230);
    w32(ASR0(1), 0x00ABCDEF); CHECK_EQ(r32(ASR0(1)), 0x00ABCDE0);
    w32(ASR1(1), 0x00FEDCBA); CHECK_EQ(r32(ASR1(1)), 0x00FEDCB0);
    w32(SADR(1), 0x1230);     CHECK_EQ(r32(SADR(1)), 0);              /* VIF1 has no SADR */
    w32(CHCR(1), 0xFFFF00F7); CHECK_EQ(r32(CHCR(1)), 0xFFFF00F5);     /* bit 1 reserved, STR not set */
    w32(SADR(8), 0xFFFFFFFF); CHECK_EQ(r32(SADR(8)), 0x3FF0);
    w32(TADR(8), 0x1000);     CHECK_EQ(r32(TADR(8)), 0);              /* SPR_FROM has no TADR */
    w32(TADR(9), 0x1000);     CHECK_EQ(r32(TADR(9)), 0x1000);
    w32(TADR(3), 0x1000);     CHECK_EQ(r32(TADR(3)), 0);              /* IPU_FROM has no TADR */
    w32(ASR0(4), 0x1000);     CHECK_EQ(r32(ASR0(4)), 0);              /* IPU_TO has no ASR */
    CHECK_EQ(unhandled, 0);

    w32(D_CTRL, 0xFFFFFFFF);  CHECK_EQ(r32(D_CTRL), 0x7FF);
    w32(D_CTRL, 1);
    w32(D_PCR, 0xFFFFFFFF);   CHECK_EQ(r32(D_PCR), 0x83FF03FF);
    w32(D_PCR, 0);
    w32(D_SQWC, 0xFFFFFFFF);  CHECK_EQ(r32(D_SQWC), 0x00FF00FF);
    w32(D_RBSR, 0xFFFFFFFF);  CHECK_EQ(r32(D_RBSR), 0x7FFFFFF0);
    w32(D_RBOR, 0x12345678);  CHECK_EQ(r32(D_RBOR), 0x12345670);
    w32(D_STADR, 0x00100018); CHECK_EQ(r32(D_STADR), 0x00100010);
    CHECK_EQ(r32(D_ENABLER), 0x1201);
    w32(D_ENABLEW, 0x11201);  CHECK_EQ(r32(D_ENABLER), 0x11201);
    w32(D_ENABLEW, 0x1201);   CHECK_EQ(r32(D_ENABLER), 0x1201);

    /* D_STAT forwarding: write 1 clears status, toggles masks; sub-word writes only touch their bytes */
    d_stat = 0x00000105;
    w32(D_STAT, 0x1);          CHECK_EQ(r32(D_STAT), 0x104);
    w32(D_STAT, 0x40000);      CHECK_EQ(r32(D_STAT), 0x40104);
    uint8_t b = 0x01;
    dma_access(D_STAT + 1, &b, 1, 1);  CHECK_EQ(d_stat, 0x40004);    /* byte 1: clears bit 8 only */
    uint16_t h = 0x0004;
    dma_access(D_STAT + 2, &h, 2, 1);  CHECK_EQ(d_stat, 0x00004);    /* halfword 2: toggles mask bit 18 */
    dma_access(D_STAT, &h, 2, 0);      CHECK_EQ(h, 0x0004);

    /* sub-word channel access: byte write to CHCR+1 sets STR */
    fill(0x1000, 1);
    w32(MADR(2), 0x1000); w32(QWC(2), 1);
    b = 0x01;
    dma_access(CHCR(2) + 1, &b, 1, 1);
    CHECK_EQ(count('G'), 1);
    CHECK_EQ(r32(CHCR(2)) & STR, 0);
    w32(QWC(2), 0xBEEF);
    dma_access(QWC(2) + 1, &b, 1, 0);  CHECK_EQ(b, 0xBE);
    uint64_t q = 0;
    dma_access(QWC(2), &q, 8, 0);      CHECK_EQ(q, 0xBEEF);
    uint8_t big[16]; memset(big, 0xFF, sizeof big);
    dma_access(MADR(2), big, 16, 0);
    CHECK_EQ(big[0], 0x10); CHECK_EQ(big[1], 0x10); CHECK_EQ(big[4], 0); CHECK_EQ(big[15], 0);

    /* unknown registers are logged; padding inside a register slot is silent */
    r32(0x1000E070); CHECK_EQ(unhandled, 1);
    r32(0x10008060); CHECK_EQ(unhandled, 2);
    CHECK_EQ(r32(CHCR(0) + 4), 0); CHECK_EQ(unhandled, 2);

    /* SIF channels keep registers, never transfer, never interrupt */
    w32(MADR(5), 0x2000); w32(QWC(5), 4); w32(CHCR(5), 0x184 | STR);
    CHECK_EQ(r32(CHCR(5)), 0x184 & ~STR);
    CHECK_EQ(r32(MADR(5)), 0x2000); CHECK_EQ(r32(QWC(5)), 4);
    w32(CHCR(6), 0x105); CHECK_EQ(r32(CHCR(6)), 0x005);
    w32(CHCR(7), 0x101); CHECK_EQ(r32(CHCR(7)), 0x001);
    CHECK_EQ(raised[5] + raised[6] + raised[7], 0);
}

static void test_normal(void) {
    reset();
    fill(0x10000, 4);
    w32(MADR(2), 0x10000); w32(QWC(2), 4); w32(CHCR(2), STR);
    CHECK_EQ(count('G'), 4);
    for (int i = 0; i < 4; i++) CHECK_EQ(ev_w0('G', i), 0x10000 + 16 * i);
    CHECK_EQ(r32(CHCR(2)), 0);
    CHECK_EQ(r32(MADR(2)), 0x10040);
    CHECK_EQ(r32(QWC(2)), 0);
    CHECK_EQ(raised[2], 1);
    CHECK(d_stat & (1u << 2));

    /* uncached alias addresses reach the same RAM */
    nev = 0;
    w32(MADR(2), 0x20010000); w32(QWC(2), 1); w32(CHCR(2), STR);
    CHECK_EQ(ev_w0('G', 0), 0x10000);

    /* VIF0, VIF1 (to VIF), IPU_TO */
    nev = 0;
    w32(MADR(0), 0x10000); w32(QWC(0), 2); w32(CHCR(0), STR);
    w32(MADR(1), 0x10010); w32(QWC(1), 2); w32(CHCR(1), STR | DIR);
    w32(MADR(4), 0x10020); w32(QWC(4), 2); w32(CHCR(4), STR | DIR);
    CHECK_EQ(count('0'), 2); CHECK_EQ(ev_w0('0', 1), 0x10010);
    CHECK_EQ(count('1'), 2); CHECK_EQ(ev_w0('1', 0), 0x10010);
    CHECK_EQ(count('I'), 2); CHECK_EQ(ev_w0('I', 1), 0x10030);
    CHECK_EQ(raised[0], 1); CHECK_EQ(raised[1], 1); CHECK_EQ(raised[4], 1);

    /* VIF1 towards memory (DIR = 0) and IPU_FROM */
    vif1_out_avail = 3;
    w32(MADR(1), 0x30000); w32(QWC(1), 3); w32(CHCR(1), STR);
    CHECK_EQ(r32(CHCR(1)) & STR, 0);
    uint32_t w;
    memcpy(&w, mp(0x30020), 4); CHECK_EQ(w, 0x71000002);
    ipu_out_avail = 2;
    w32(MADR(3), 0x31000); w32(QWC(3), 2); w32(CHCR(3), STR);
    memcpy(&w, mp(0x31010), 4); CHECK_EQ(w, 0x1F000001);
    CHECK_EQ(raised[3], 1);

    /* QWC = 0 in normal mode ends at once */
    w32(MADR(2), 0x10000); w32(QWC(2), 0); w32(CHCR(2), STR);
    CHECK_EQ(r32(CHCR(2)) & STR, 0);
    CHECK_EQ(raised[2], 3);
}

static void test_enable_suspend_priority(void) {
    reset();
    fill(0x10000, 2);
    w32(D_CTRL, 0);                                         /* DMAE off: held */
    w32(MADR(2), 0x10000); w32(QWC(2), 2); w32(CHCR(2), STR);
    CHECK_EQ(nev, 0); CHECK(r32(CHCR(2)) & STR);
    w32(D_CTRL, 1);
    CHECK_EQ(count('G'), 2); CHECK_EQ(r32(CHCR(2)) & STR, 0);

    nev = 0;                                                /* D_ENABLEW bit 16: suspended */
    w32(D_ENABLEW, r32(D_ENABLER) | 0x10000);
    w32(MADR(2), 0x10000); w32(QWC(2), 2); w32(CHCR(2), STR);
    dma_service();
    CHECK_EQ(nev, 0); CHECK(r32(CHCR(2)) & STR);
    w32(D_ENABLEW, r32(D_ENABLER) & ~0x10000u);
    CHECK_EQ(count('G'), 2); CHECK_EQ(raised[2], 2);

    nev = 0;                                                /* D_PCR.PCE with CDE clear: held */
    w32(D_PCR, 0x80000000u);
    w32(MADR(2), 0x10000); w32(QWC(2), 2); w32(CHCR(2), STR);
    CHECK_EQ(nev, 0);
    w32(D_PCR, 0x80000000u | (1u << (16 + 2)));
    CHECK_EQ(count('G'), 2);
    w32(D_PCR, 0);
}

/* Every source chain tag type including nested call/ret. */
static void test_source_chain(void) {
    reset();
    tag(0x1000, CNT, 1, 0, 0);          fill(0x1010, 1);    /* -> 0x1020 */
    tag(0x1020, NEXT, 1, 0x2000, 0);    fill(0x1030, 1);    /* -> 0x2000 */
    tag(0x2000, REF, 2, 0x3000, 0);     fill(0x3000, 2);    /* -> 0x2010 */
    tag(0x2010, REFS, 1, 0x3100, 0);    fill(0x3100, 1);    /* -> 0x2020 (no stall control set up) */
    tag(0x2020, CALL, 1, 0x4000, 0);    fill(0x2030, 1);    /* push 0x2040 -> 0x4000 */
    tag(0x4000, CALL, 0, 0x5000, 0);                        /* push 0x4010 -> 0x5000 */
    tag(0x5000, RET, 1, 0, 0);          fill(0x5010, 1);    /* pop -> 0x4010 */
    tag(0x4010, RET, 0, 0, 0);                              /* pop -> 0x2040 */
    tag(0x2040, REFE, 1, 0x3200, 0);    fill(0x3200, 1);    /* end */
    w32(TADR(2), 0x1000); w32(QWC(2), 0); w32(CHCR(2), STR | CHAIN);
    static const uint32_t want[] = { 0x1010, 0x1030, 0x3000, 0x3010, 0x3100, 0x2030, 0x5010, 0x3200 };
    CHECK_EQ(count('G'), 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(ev_w0('G', i), want[i]);
    uint32_t chcr = r32(CHCR(2));
    CHECK_EQ(chcr & STR, 0);
    CHECK_EQ(chcr & 0x30, 0);                                /* ASP back to 0 */
    CHECK_EQ(chcr >> 16, (uint32_t)(mktag(REFE, 1, 0x3200, 0) >> 16) & 0xFFFF);
    CHECK_EQ(r32(TADR(2)), 0x2050);
    CHECK_EQ(r32(MADR(2)), 0x3210);
    CHECK_EQ(r32(QWC(2)), 0);
    CHECK_EQ(r32(ASR0(2)), 0x2040);
    CHECK_EQ(r32(ASR1(2)), 0x4010);
    CHECK_EQ(raised[2], 1);

    /* end: data follows the tag; TADR stays on the end tag */
    reset();
    tag(0x1000, CNT, 1, 0, 0); fill(0x1010, 1);
    tag(0x1020, END, 2, 0, 0); fill(0x1030, 2);
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 3); CHECK_EQ(ev_w0('G', 2), 0x1040);
    CHECK_EQ(r32(TADR(2)), 0x1020); CHECK_EQ(r32(MADR(2)), 0x1050);
    CHECK_EQ(r32(CHCR(2)) & STR, 0); CHECK_EQ(raised[2], 1);

    /* ret with an empty address stack ends the chain after its data */
    reset();
    tag(0x1000, RET, 1, 0, 0); fill(0x1010, 1);
    tag(0x1020, CNT, 1, 0, 0); fill(0x1030, 1);
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 1); CHECK_EQ(r32(CHCR(2)) & STR, 0); CHECK_EQ(raised[2], 1);

    /* tags and data in the scratchpad (bit 31) */
    reset();
    tag(0x80000100, CNT, 1, 0, 0); fill(0x80000110, 1);
    tag(0x80000120, REFE, 1, 0x80000200, 0); fill(0x80000200, 1);
    w32(TADR(2), 0x80000100); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 2); CHECK_EQ(ev_w0('G', 0), 0x80000110); CHECK_EQ(ev_w0('G', 1), 0x80000200);

    /* tag PCE field: 3 sets D_PCR.PCE, 2 clears it */
    reset();
    w32(D_PCR, 1u << (16 + 2));
    uint64_t t = mktag(CNT, 0, 0, 0) | (3ull << 26); memcpy(mp(0x1000), &t, 8);
    w32(TADR(2), 0x1000);
    t = mktag(END, 0, 0, 0); memcpy(mp(0x1010), &t, 8);
    t = mktag(CNT, 0, 0, 0) | (2ull << 26);
    w32(CHCR(2), STR | CHAIN);
    CHECK(r32(D_PCR) & 0x80000000u);
    memcpy(mp(0x1000), &t, 8);
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(r32(D_PCR) & 0x80000000u, 0);
    w32(D_PCR, 0);
}

static void test_tte_vif(void) {
    reset();
    tag(0x1000, CNT, 2, 0, 0); fill(0x1010, 2);
    tag(0x1030, END, 1, 0, 0); fill(0x1040, 1);
    w32(TADR(1), 0x1000); w32(CHCR(1), STR | CHAIN | DIR | TTE);
    CHECK_EQ(nev, 5);
    CHECK(evs[0].dev == 'b'); CHECK_EQ(evs[0].w[2], 0x1000); CHECK_EQ(evs[0].w[3], 0x56000000);
    CHECK(evs[1].dev == '1'); CHECK_EQ(evs[1].w[0], 0x1010);
    CHECK(evs[2].dev == '1'); CHECK_EQ(evs[2].w[0], 0x1020);
    CHECK(evs[3].dev == 'b'); CHECK_EQ(evs[3].w[2], 0x1030);
    CHECK(evs[4].dev == '1'); CHECK_EQ(evs[4].w[0], 0x1040);

    /* VIF0 too */
    reset();
    tag(0x1000, END, 1, 0, 0); fill(0x1010, 1);
    w32(TADR(0), 0x1000); w32(CHCR(0), STR | CHAIN | TTE);
    CHECK_EQ(nev, 2); CHECK(evs[0].dev == 'a'); CHECK(evs[1].dev == '0');

    /* SPR_TO with TTE stores the tag in the scratchpad before the data */
    reset();
    tag(0x1000, END, 1, 0, 0); fill(0x1010, 1);
    w32(TADR(9), 0x1000); w32(SADR(9), 0x200); w32(CHCR(9), STR | CHAIN | TTE);
    CHECK_EQ(memcmp(spr + 0x200, mp(0x1000), 16), 0);
    CHECK_EQ(memcmp(spr + 0x210, mp(0x1010), 16), 0);
    CHECK_EQ(r32(SADR(9)), 0x220);
}

static void test_irq_tie(void) {
    reset();
    tag(0x1000, CNT, 1, 0, 1); fill(0x1010, 1);
    tag(0x1020, CNT, 1, 0, 0); fill(0x1030, 1);
    tag(0x1040, END, 0, 0, 0);
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN | TIE);
    CHECK_EQ(count('G'), 1);
    CHECK_EQ(r32(CHCR(2)) & STR, 0);
    CHECK(r32(CHCR(2)) & 0x80000000u);                     /* TAG field holds the IRQ bit */
    CHECK_EQ(r32(TADR(2)), 0x1020);
    CHECK_EQ(raised[2], 1);

    nev = 0;                                                /* TIE off: IRQ bit ignored */
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 2); CHECK_EQ(raised[2], 2);
}

static void test_stalls(void) {
    /* GIF accepts 3 qwords, then PATH3 is "masked" */
    reset();
    tag(0x1000, CNT, 2, 0, 0); fill(0x1010, 2);
    tag(0x1030, REF, 3, 0x3000, 0); fill(0x3000, 3);
    tag(0x1040, END, 1, 0, 0); fill(0x1050, 1);
    gif_budget = 3;
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 3);
    CHECK(r32(CHCR(2)) & STR);
    CHECK_EQ(r32(QWC(2)), 2);
    CHECK_EQ(r32(MADR(2)), 0x3010);
    CHECK_EQ(r32(TADR(2)), 0x1040);
    CHECK_EQ(raised[2], 0);
    dma_service();                                          /* still masked: nothing changes */
    CHECK_EQ(count('G'), 3); CHECK_EQ(raised[2], 0);
    gif_budget = 0xFFFFFFFFu;
    dma_service();
    CHECK_EQ(count('G'), 6);
    CHECK_EQ(ev_w0('G', 3), 0x3010); CHECK_EQ(ev_w0('G', 5), 0x1050);
    CHECK_EQ(r32(CHCR(2)) & STR, 0); CHECK_EQ(raised[2], 1);

    /* polling CHCR lets a stalled channel continue once the device frees up */
    reset();
    fill(0x10000, 4);
    ipu_in_budget = 1;
    w32(MADR(4), 0x10000); w32(QWC(4), 4); w32(CHCR(4), STR | DIR);
    CHECK_EQ(count('I'), 1); CHECK_EQ(r32(QWC(4)), 3); CHECK_EQ(r32(MADR(4)), 0x10010);
    ipu_in_budget = 0xFFFFFFFFu;
    CHECK_EQ(r32(CHCR(4)) & STR, 0);
    CHECK_EQ(count('I'), 4); CHECK_EQ(raised[4], 1);

    /* IPU_FROM waiting for decoded output */
    reset();
    w32(MADR(3), 0x20000); w32(QWC(3), 4); w32(CHCR(3), STR);
    CHECK(r32(CHCR(3)) & STR);
    ipu_out_avail = 3; dma_service();
    CHECK_EQ(r32(QWC(3)), 1); CHECK_EQ(r32(MADR(3)), 0x20030); CHECK(r32(CHCR(3)) & STR);
    ipu_out_avail = 5; dma_service();
    CHECK_EQ(r32(CHCR(3)) & STR, 0); CHECK_EQ(ipu_out_avail, 4); CHECK_EQ(raised[3], 1);

    /* a TTE tag the VIF refuses is retried without re-reading the tag (call pushes once) */
    reset();
    tag(0x1000, CALL, 1, 0x2000, 0); fill(0x1010, 1);
    tag(0x2000, RET, 1, 0, 0); fill(0x2010, 1);
    tag(0x1020, END, 1, 0, 0); fill(0x1030, 1);
    vif_tag_ok = 0;
    w32(TADR(1), 0x1000); w32(CHCR(1), STR | CHAIN | DIR | TTE);
    CHECK_EQ(nev, 0);
    CHECK_EQ((r32(CHCR(1)) >> 4) & 3, 1);
    CHECK_EQ(r32(TADR(1)), 0x2000);
    vif_tag_ok = 1;
    dma_service();
    CHECK_EQ(nev, 6);
    CHECK(evs[0].dev == 'b'); CHECK_EQ(evs[0].w[2], 0x1000);
    CHECK(evs[1].dev == '1'); CHECK_EQ(evs[1].w[0], 0x1010);
    CHECK(evs[2].dev == 'b'); CHECK_EQ(evs[2].w[2], 0x2000);
    CHECK(evs[3].dev == '1'); CHECK_EQ(evs[3].w[0], 0x2010);
    CHECK(evs[4].dev == 'b'); CHECK_EQ(evs[4].w[2], 0x1020);
    CHECK(evs[5].dev == '1'); CHECK_EQ(evs[5].w[0], 0x1030);
    CHECK_EQ((r32(CHCR(1)) >> 4) & 3, 0);
    CHECK_EQ(r32(CHCR(1)) & STR, 0); CHECK_EQ(raised[1], 1);
}

/* Stopping a busy channel keeps its state; restarting with QWC > 0 uses CHCR.TAG. */
static void test_stop_resume(void) {
    reset();
    fill(0x10000, 4);
    tag(0x1000, END, 1, 0, 0); fill(0x1010, 1);
    /* QWC > 0 with CHCR.TAG = refe (0): transfer QWC, then end without reading TADR */
    w32(MADR(2), 0x10000); w32(QWC(2), 2); w32(TADR(2), 0x1000);
    w32(CHCR(2), STR | CHAIN | (uint32_t)(mktag(REFE, 0, 0, 0) & 0xFFFF0000u));
    CHECK_EQ(count('G'), 2); CHECK_EQ(r32(CHCR(2)) & STR, 0); CHECK_EQ(r32(TADR(2)), 0x1000);
    /* ... with CHCR.TAG = cnt: then continue at TADR */
    nev = 0;
    w32(MADR(2), 0x10000); w32(QWC(2), 2); w32(TADR(2), 0x1000);
    w32(CHCR(2), STR | CHAIN | (uint32_t)(mktag(CNT, 0, 0, 0) & 0xFFFF0000u));
    CHECK_EQ(count('G'), 3); CHECK_EQ(ev_w0('G', 2), 0x1010);
    /* ... with CHCR.TAG = end + nothing else: end */
    nev = 0;
    w32(MADR(2), 0x10000); w32(QWC(2), 1); w32(TADR(2), 0x1000);
    w32(CHCR(2), STR | CHAIN | (uint32_t)(mktag(END, 0, 0, 0) & 0xFFFF0000u));
    CHECK_EQ(count('G'), 1);
    /* ... a tag with IRQ and TIE: end */
    nev = 0;
    w32(MADR(2), 0x10000); w32(QWC(2), 1); w32(TADR(2), 0x1000);
    w32(CHCR(2), STR | CHAIN | TIE | (uint32_t)(mktag(CNT, 0, 0, 1) & 0xFFFF0000u));
    CHECK_EQ(count('G'), 1);

    /* the movie library's pattern: stop with CHCR = 1 while busy, read back, restart with |STR */
    reset();
    tag(0x1000, CNT, 3, 0, 0); fill(0x1010, 3);
    tag(0x1040, END, 1, 0, 0); fill(0x1050, 1);
    ipu_in_budget = 1;
    w32(TADR(4), 0x1000); w32(CHCR(4), STR | CHAIN | DIR);
    CHECK_EQ(count('I'), 1);
    w32(D_ENABLEW, r32(D_ENABLER) | 0x10000);
    w32(CHCR(4), 1);
    w32(D_ENABLEW, r32(D_ENABLER) & ~0x10000u);
    uint32_t chcr = r32(CHCR(4));
    CHECK_EQ(chcr & STR, 0);
    CHECK_EQ(chcr & 0xC, CHAIN);                           /* mode kept */
    CHECK_EQ(chcr >> 28, CNT);                              /* TAG kept */
    CHECK_EQ(r32(QWC(4)), 2); CHECK_EQ(r32(MADR(4)), 0x1020); CHECK_EQ(r32(TADR(4)), 0x1040);
    CHECK_EQ(raised[4], 0);
    ipu_in_budget = 0xFFFFFFFFu;
    dma_service();
    CHECK_EQ(count('I'), 1);                                /* stopped: no progress */
    w32(CHCR(4), chcr | STR);
    CHECK_EQ(count('I'), 4);
    CHECK_EQ(ev_w0('I', 1), 0x1020); CHECK_EQ(ev_w0('I', 2), 0x1030); CHECK_EQ(ev_w0('I', 3), 0x1050);
    CHECK_EQ(r32(CHCR(4)) & STR, 0); CHECK_EQ(raised[4], 1);

    /* a write while busy cannot change mode/TAG */
    reset();
    fill(0x10000, 2);
    gif_budget = 0;
    w32(MADR(2), 0x10000); w32(QWC(2), 2); w32(CHCR(2), STR);
    w32(CHCR(2), STR | CHAIN | 0xFFFF0000u);
    CHECK_EQ(r32(CHCR(2)), STR);
    gif_budget = 0xFFFFFFFFu;
    dma_service();
    CHECK_EQ(count('G'), 2); CHECK_EQ(r32(CHCR(2)), 0);
}

static void test_interleave_spr(void) {
    reset();
    fill(0x20000, 9);
    w32(D_SQWC, (2u << 16) | 1);                            /* transfer 2, skip 1 */
    w32(MADR(9), 0x20000); w32(QWC(9), 6); w32(SADR(9), 0); w32(CHCR(9), STR | INTERLEAVE);
    static const uint32_t src[6] = { 0, 1, 3, 4, 6, 7 };
    for (int i = 0; i < 6; i++) CHECK_EQ(memcmp(spr + 16 * i, mp(0x20000 + 16 * src[i]), 16), 0);
    CHECK_EQ(r32(MADR(9)), 0x20090);
    CHECK_EQ(r32(SADR(9)), 0x60);
    CHECK_EQ(r32(CHCR(9)) & STR, 0); CHECK_EQ(raised[9], 1);

    /* SPR_FROM interleave: scratchpad -> memory, skipping in memory */
    memset(ram + 0x30000, 0, 0x100);
    w32(D_SQWC, (1u << 16) | 2);                            /* transfer 1, skip 2 */
    w32(MADR(8), 0x30000); w32(QWC(8), 3); w32(SADR(8), 0); w32(CHCR(8), STR | INTERLEAVE);
    CHECK_EQ(memcmp(mp(0x30000), spr + 0x00, 16), 0);
    CHECK_EQ(memcmp(mp(0x30030), spr + 0x10, 16), 0);
    CHECK_EQ(memcmp(mp(0x30060), spr + 0x20, 16), 0);
    uint8_t zero[16] = { 0 };
    CHECK_EQ(memcmp(mp(0x30010), zero, 16), 0);
    CHECK_EQ(raised[8], 1);

    /* scratchpad addresses wrap at 16 KB */
    reset();
    fill(0x40000, 4);
    w32(MADR(9), 0x40000); w32(QWC(9), 4); w32(SADR(9), 0x3FE0); w32(CHCR(9), STR);
    CHECK_EQ(memcmp(spr + 0x3FE0, mp(0x40000), 16), 0);
    CHECK_EQ(memcmp(spr + 0x3FF0, mp(0x40010), 16), 0);
    CHECK_EQ(memcmp(spr + 0x0000, mp(0x40020), 16), 0);
    CHECK_EQ(memcmp(spr + 0x0010, mp(0x40030), 16), 0);
    CHECK_EQ(r32(SADR(9)), 0x20);
    memset(ram + 0x50000, 0, 0x40);
    w32(MADR(8), 0x50000); w32(QWC(8), 4); w32(SADR(8), 0x3FE0); w32(CHCR(8), STR);
    for (int i = 0; i < 4; i++) CHECK_EQ(memcmp(mp(0x50000 + 16 * i), mp(0x40000 + 16 * i), 16), 0);
    CHECK_EQ(r32(SADR(8)), 0x20);

    /* MADR in the scratchpad also wraps (SPR_TO from scratchpad memory) */
    reset();
    fill(0x80003FF0, 1); fill(0x80000000, 1);
    w32(MADR(2), 0x80003FF0); w32(QWC(2), 2); w32(CHCR(2), STR);
    CHECK_EQ(ev_w0('G', 0), 0x80003FF0); CHECK_EQ(ev_w0('G', 1), 0x80000000);
}

static void test_dest_chain(void) {
    reset();
    uint32_t s = 0x100;
    uint64_t t;
    t = mktag(0, 1, 0x50000, 0); memcpy(spr + s, &t, 8); s += 16;   /* cnt */
    fill(0x80000000 | s, 1); s += 16;
    t = mktag(1, 2, 0x51000, 0); memcpy(spr + s, &t, 8); s += 16;   /* cnts */
    fill(0x80000000 | s, 2); s += 32;
    t = mktag(7, 1, 0x52000, 0); memcpy(spr + s, &t, 8); s += 16;   /* end */
    fill(0x80000000 | s, 1); s += 16;
    w32(D_CTRL, 1 | (2u << 4));                             /* stall source = SPR_FROM */
    w32(SADR(8), 0x100); w32(CHCR(8), STR | CHAIN);
    CHECK_EQ(memcmp(mp(0x50000), spr + 0x110, 16), 0);
    CHECK_EQ(memcmp(mp(0x51000), spr + 0x130, 16), 0);
    CHECK_EQ(memcmp(mp(0x51010), spr + 0x140, 16), 0);
    CHECK_EQ(memcmp(mp(0x52000), spr + 0x160, 16), 0);
    CHECK_EQ(r32(D_STADR), 0x51020);                        /* updated by cnts only */
    CHECK_EQ(r32(SADR(8)), s);
    CHECK_EQ(r32(MADR(8)), 0x52010);
    CHECK_EQ(r32(CHCR(8)) >> 28, 7);
    CHECK_EQ(r32(CHCR(8)) & STR, 0); CHECK_EQ(raised[8], 1);

    /* IRQ + TIE ends a destination chain too */
    reset();
    t = mktag(0, 1, 0x50000, 1); memcpy(spr, &t, 8);
    t = mktag(7, 1, 0x52000, 0); memcpy(spr + 0x20, &t, 8);
    w32(CHCR(8), STR | CHAIN | TIE);
    CHECK_EQ(r32(SADR(8)), 0x20); CHECK_EQ(r32(CHCR(8)) & STR, 0);
}

static void test_mfifo(void) {
    reset();
    const uint32_t rbor = 0x100000;
    w32(D_RBOR, rbor); w32(D_RBSR, 0xFF0);                 /* 4 KB ring */
    w32(D_CTRL, 1 | (3u << 2));                             /* MFD = GIF */
    /* packet 1 in the scratchpad: cnt(2) + 2 data; lands at ring 0xFE0..0x000 (data wraps) */
    uint64_t t = mktag(CNT, 2, 0, 0);
    memset(spr, 0, 0x100);
    memcpy(spr, &t, 8);
    uint32_t d0[4] = { 0xD0, 0, 0, 0 }, d1[4] = { 0xD1, 0, 0, 0 }, d2[4] = { 0xD2, 0, 0, 0 }, d3[4] = { 0xD3, 0, 0, 0 };
    memcpy(spr + 0x10, d0, 16); memcpy(spr + 0x20, d1, 16);
    w32(SADR(8), 0); w32(MADR(8), rbor + 0xFE0); w32(QWC(8), 3); w32(CHCR(8), STR);
    CHECK_EQ(r32(MADR(8)), rbor + 0x10);                    /* write pointer wrapped */
    CHECK_EQ(memcmp(mp(rbor + 0xFE0), spr, 16), 0);
    CHECK_EQ(memcmp(mp(rbor), spr + 0x20, 16), 0);
    CHECK_EQ(raised[8], 1);

    /* drain: GIF chain from the ring */
    w32(TADR(2), rbor + 0xFE0); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 2); CHECK_EQ(ev_w0('G', 0), 0xD0); CHECK_EQ(ev_w0('G', 1), 0xD1);
    CHECK(r32(CHCR(2)) & STR);                              /* ring empty: waiting */
    CHECK_EQ(r32(TADR(2)), rbor + 0x10);
    CHECK(d_stat & (1u << 14));                             /* MEIS */
    CHECK_EQ(raised[14], 1);
    dma_service();
    CHECK_EQ(raised[14], 1);                                /* raised once per wait */

    /* packet 2: end(2), but only tag + 1 data arrive first */
    t = mktag(END, 2, 0, 0);
    memcpy(spr + 0x30, &t, 8); memcpy(spr + 0x40, d2, 16); memcpy(spr + 0x50, d3, 16);
    w32(SADR(8), 0x30); w32(QWC(8), 2); w32(CHCR(8), STR);
    CHECK_EQ(count('G'), 3); CHECK_EQ(ev_w0('G', 2), 0xD2);
    CHECK(r32(CHCR(2)) & STR);
    CHECK_EQ(r32(QWC(2)), 1);
    w32(QWC(8), 1); w32(CHCR(8), STR);
    CHECK_EQ(count('G'), 4); CHECK_EQ(ev_w0('G', 3), 0xD3);
    CHECK_EQ(r32(CHCR(2)) & STR, 0);
    CHECK_EQ(raised[2], 1);

    /* ref tags in the ring point outside it; MEIS fires but the data still flows */
    reset();
    w32(D_RBOR, rbor); w32(D_RBSR, 0xFF0); w32(D_CTRL, 1 | (2u << 2));   /* MFD = VIF1 */
    fill(0x60000, 2);
    t = mktag(REF, 2, 0x60000, 0); memcpy(spr, &t, 8);
    w32(SADR(8), 0); w32(MADR(8), rbor); w32(QWC(8), 1); w32(CHCR(8), STR);
    w32(TADR(1), rbor); w32(CHCR(1), STR | CHAIN | DIR);
    CHECK_EQ(count('1'), 2); CHECK_EQ(ev_w0('1', 1), 0x60010);
    CHECK_EQ(raised[14], 2);                                /* at the ref tag, and at the next tag read */
    CHECK(r32(CHCR(1)) & STR);
    w32(D_CTRL, 1);
}

static void test_stall_control(void) {
    reset();
    w32(D_CTRL, 1 | (3u << 4) | (2u << 6));                 /* source IPU_FROM, drain GIF */
    w32(MADR(3), 0x40000); w32(QWC(3), 4); w32(CHCR(3), STR);
    CHECK_EQ(r32(D_STADR), 0);
    tag(0x30000, REFS, 4, 0x40000, 0);
    tag(0x30010, END, 0, 0, 0);
    w32(TADR(2), 0x30000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 0);
    CHECK(r32(CHCR(2)) & STR);
    CHECK(d_stat & (1u << 13));                             /* SIS */
    CHECK_EQ(raised[13], 1);
    ipu_out_avail = 2;
    dma_service();
    CHECK_EQ(r32(D_STADR), 0x40020);
    CHECK_EQ(count('G'), 2); CHECK_EQ(ev_w0('G', 1), 0x1F000001);
    CHECK_EQ(raised[13], 2);                                /* stalled again at the new D_STADR */
    ipu_out_avail = 2;
    dma_service();
    CHECK_EQ(count('G'), 4); CHECK_EQ(ev_w0('G', 3), 0x1F000003);
    CHECK_EQ(r32(CHCR(2)) & STR, 0); CHECK_EQ(r32(CHCR(3)) & STR, 0);
    CHECK_EQ(raised[2], 1); CHECK_EQ(raised[3], 1);

    /* plain ref tags ignore stall control */
    reset();
    w32(D_CTRL, 1 | (3u << 4) | (2u << 6));
    fill(0x40000, 2);
    tag(0x30000, REF, 2, 0x40000, 0);
    tag(0x30010, END, 0, 0, 0);
    w32(TADR(2), 0x30000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 2); CHECK_EQ(raised[13], 0);
    w32(D_CTRL, 1);
}

static void test_bus_error(void) {
    reset();
    fill(0x01FFFFF0, 1);
    w32(MADR(2), 0x01FFFFF0); w32(QWC(2), 2); w32(CHCR(2), STR);
    CHECK_EQ(count('G'), 1);
    CHECK_EQ(r32(CHCR(2)) & STR, 0);
    CHECK(d_stat & (1u << 15));
    CHECK_EQ(raised[2], 0);
    /* bad tag address */
    w32(TADR(2), 0x03000000); w32(QWC(2), 0); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(r32(CHCR(2)) & STR, 0); CHECK_EQ(raised[15], 2);
}

static void test_looping_chain(void) {
    reset();
    tag(0x1000, NEXT, 0, 0x1000, 0);                        /* loops forever */
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN);
    CHECK(r32(CHCR(2)) & STR);                              /* returns; the channel stays busy */
    tag(0x1000, END, 0, 0, 0);                              /* the game breaks the loop */
    dma_service();
    CHECK_EQ(r32(CHCR(2)) & STR, 0); CHECK_EQ(raised[2], 1);

    /* a looping VIF0 chain does not starve other channels */
    reset();
    tag(0x1000, NEXT, 0, 0x1000, 0);
    tag(0x2000, END, 1, 0, 0); fill(0x2010, 1);
    w32(TADR(0), 0x1000); w32(CHCR(0), STR | CHAIN);
    w32(TADR(2), 0x2000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 1); CHECK_EQ(r32(CHCR(2)) & STR, 0);
    CHECK(r32(CHCR(0)) & STR);
    w32(CHCR(0), 0);                                        /* the game stops it */
    CHECK_EQ(r32(CHCR(0)) & STR, 0); CHECK_EQ(raised[0], 0);
}

static void test_poll_dstat(void) {
    reset();
    w32(MADR(3), 0x20000); w32(QWC(3), 2); w32(CHCR(3), STR);
    CHECK_EQ(r32(D_STAT) & (1u << 3), 0);
    ipu_out_avail = 2;                                      /* the IPU decodes; the game polls D_STAT */
    CHECK(r32(D_STAT) & (1u << 3));
    CHECK_EQ(r32(CHCR(3)) & STR, 0);
}

/* ret with CHCR.ASP = 3 (a value the CPU can write): ps2tek pops ASR1 only for ASP 2 and ASR0 only
 * for ASP 1; anything else ends the chain after the ret's data. */
static void test_ret_asp3(void) {
    reset();
    tag(0x1000, RET, 1, 0, 0); fill(0x1010, 1);
    tag(0x0000, CNT, 1, 0, 0); fill(0x0010, 1);            /* where a bogus pop to 0 would lead */
    tag(0x0020, END, 0, 0, 0);
    w32(TADR(2), 0x1000); w32(QWC(2), 0);
    w32(CHCR(2), STR | CHAIN | 0x30);
    CHECK_EQ(count('G'), 1); CHECK_EQ(ev_w0('G', 0), 0x1010);
    CHECK_EQ(r32(CHCR(2)) & STR, 0);
    CHECK_EQ((r32(CHCR(2)) >> 4) & 3, 3);                   /* ASP untouched */
    CHECK_EQ(r32(TADR(2)), 0x1000);                         /* stays on the ret tag, as for ASP 0 */
    CHECK_EQ(raised[2], 1);
}

/* Channels that feed each other through a device (IPU_TO -> decoder -> IPU_FROM, 8-qword FIFO) both
 * finish within the retry that a single register write triggers: rounds go on until nothing moves. */
static void test_coupled_channels(void) {
    reset();
    ipu_fifo_mode = 1;
    fill(0x100000, 1000);
    w32(MADR(3), 0x200000); w32(QWC(3), 1000); w32(CHCR(3), STR);
    w32(MADR(4), 0x100000); w32(QWC(4), 1000);
    w32(CHCR(4), STR | DIR);                                /* one write; no polling afterwards */
    CHECK_EQ(count('I'), 1000);
    CHECK_EQ(raised[4], 1);
    CHECK_EQ(raised[3], 1);
    uint32_t w;
    memcpy(&w, mp(0x200000 + 16 * 999), 4); CHECK_EQ(w, 0x1F000000u + 999);
}

/* A chain that draws a whole frame runs for a long time inside one register write (up to ~150 ms with
 * the software GS); the IOP, which runs on the same thread, gets a chance between the tags. */
static void test_poll_iop_between_tags(void) {
    reset();
    for (uint32_t i = 0; i < 4; i++) { tag(0x1000 + 0x20 * i, CNT, 1, 0, 0); fill(0x1010 + 0x20 * i, 1); }
    tag(0x1080, END, 0, 0, 0);
    polls = 0;
    w32(TADR(2), 0x1000); w32(CHCR(2), STR | CHAIN);
    CHECK_EQ(count('G'), 4);
    int between = 0;                                        /* calls made after some but not all data */
    for (int i = 0; i < polls && i < 64; i++) between += poll_nev[i] > 0 && poll_nev[i] < 4;
    CHECK(between >= 3);

    reset();                                                /* also between the rounds of a normal transfer */
    polls = 0;
    w32(MADR(2), 0x20000); w32(QWC(2), 3); w32(CHCR(2), STR);
    CHECK_EQ(count('G'), 3);
    CHECK(polls >= 1);
}

int main(void) {
    test_registers();
    test_normal();
    test_enable_suspend_priority();
    test_source_chain();
    test_tte_vif();
    test_irq_tie();
    test_stalls();
    test_stop_resume();
    test_interleave_spr();
    test_dest_chain();
    test_mfifo();
    test_stall_control();
    test_bus_error();
    test_looping_chain();
    test_poll_dstat();
    test_ret_asp3();
    test_coupled_channels();
    test_poll_iop_between_tags();
    printf("dma: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
