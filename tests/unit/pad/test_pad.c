/* Unit tests for runtime/pad.c (padman + DualShock 2 HLE).
 *
 * The EE side is modelled on the game's own libpad (SCUS_973.16, 0x242D08-0x243CA8): the same
 * buffer selection, state and information functions, reading the buffers pad.c writes into a
 * fake EE RAM. The game's joystick setup sequence (0x1871C0) is replayed end to end. */
#include "hw.h"

#include <stdlib.h>

/* ---- fakes for the neighbouring subsystems ------------------------------------------------------ */

uint8_t* ps2_ram;
uint8_t* ps2_spr;
int ps2_log_sif;
static uint8_t iop_ram[0x200000];

uint8_t* iop_mem(uint32_t a) { a &= 0x1FFFFFFFu; return a < sizeof iop_ram ? iop_ram + a : NULL; }
void ps2_hw_read(ps2_ctx* ctx, uint32_t addr, void* out, unsigned size) { (void)ctx; (void)addr; memset(out, 0, size); }
void ps2_hw_write(ps2_ctx* ctx, uint32_t addr, const void* in, unsigned size) { (void)ctx; (void)addr; (void)in; (void)size; }

static pad_input input;                 /* what the host controller reports for port 0 */
static int input_calls[2];
void window_get_input(int port, pad_input* out) {
    input_calls[port & 1]++;
    if (port == 0) { *out = input; return; }
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = 0x80;
}

void pad_get_motors(int port, uint8_t motors[2]);

/* ---- test helpers ------------------------------------------------------------------------------- */

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
    if (_a != _b) { failures++; fprintf(stderr, "FAIL %s:%d: %s == %s (0x%llX vs 0x%llX)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void vsync(int n) { for (int i = 0; i < n; i++) pad_vsync(NULL); }

/* ---- EE libpad, as the game implements it -------------------------------------------------------- */

#define EE_PADBUF 0x002DFC00u           /* the game's 256-byte pad buffer (64-byte aligned) */
static uint8_t rpcbuf[128];             /* libpad sends and receives in the same buffer */

static void rpc(uint32_t cmd, uint32_t port, uint32_t slot, const uint32_t* extra, int nextra) {
    wr32(rpcbuf, cmd);
    wr32(rpcbuf + 4, port);
    wr32(rpcbuf + 8, slot);
    for (int i = 0; i < nextra; i++) wr32(rpcbuf + 0x0C + 4 * i, extra[i]);
    uint8_t out[128];
    memset(out, 0, sizeof out);
    CHECK_EQ(pad_rpc(NULL, 0x80000100u, 1, rpcbuf, 128, out, 128), 1);
    memcpy(rpcbuf, out, 128);
}

static uint32_t lp_get_mod_version(void) { rpc(0x12, 0, 0, NULL, 0); return rd32(rpcbuf + 0x0C); }
static uint32_t lp_init(void) { uint32_t z = 0; wr32(rpcbuf + 0x10, 0); rpc(0x10, 0, 0, &z, 0); return rd32(rpcbuf + 0x0C); }

static uint32_t act_iop_addr[2][4];
static uint32_t act_counter[2][4];

static uint32_t lp_port_open(int port, int slot, uint32_t addr) {
    uint8_t* b = ps2_ram + addr;
    for (int k = 0; k < 2; k++, b += 0x80) {        /* what scePadPortOpen does before the call */
        wr32(b + 0x58, 0);
        b[0x70] = 5;
        b[0x71] = 2;
        b[0x67] = 0;
        memset(b, 0xFF, 0x20);
        wr32(b + 0x60, 0);
    }
    uint32_t extra[2] = { 0, addr };
    rpc(0x01, (uint32_t)port, (uint32_t)slot, extra, 2);
    act_iop_addr[port][slot] = rd32(rpcbuf + 0x14);
    act_counter[port][slot] = 0;
    return rd32(rpcbuf + 0x0C);
}

static uint8_t* lp_dma_str(uint32_t addr) {     /* the copy with the larger frame counter */
    uint8_t* b = ps2_ram + addr;
    return (int32_t)rd32(b + 0x58) < (int32_t)rd32(b + 0xD8) ? b + 0x80 : b;
}
static uint8_t* P(void) { return lp_dma_str(EE_PADBUF); }

static int lp_get_state(void) {
    uint8_t* p = P();
    if (p[0x70] == 6 && p[0x71] == 2) return 5;
    return p[0x70];
}
static int lp_get_req_state(void) { return P()[0x71]; }
static void lp_set_req_state(int s) { P()[0x71] = (uint8_t)s; }

static int lp_read(uint8_t data[32]) {
    uint8_t* p = P();
    uint32_t len = rd32(p + 0x60);
    memcpy(data, p, len);
    return (int)len;
}

static int lp_info_act(int act, int term) {
    uint8_t* p = P();
    if (p[0x72] != 1 || p[0x64] < 2) return 0;
    if (act >= p[0x6A]) return -1;
    if (act == -1) return p[0x6A];
    if (term < 1 || term > 4) return 0;
    return p[0x30 + 4 * act + term - 1];
}

static int lp_info_mode(int term, int offs) {
    uint8_t* p = P();
    if (p[0x72] != 1 || p[0x71] == 2) return 0;
    switch (term) {
    case 1: return p[0x65] == 0xF3 ? 0 : p[0x65] >> 4;
    case 2: return p[0x64] == 1 ? 0 : rd32(p + 0x50 + 2 * p[0x69]) & 0xFFFF;
    case 3: return p[0x64] == 1 ? 0 : p[0x69];
    case 4:
        if (p[0x64] == 1) return 0;
        if (offs == -1) return p[0x68];
        return offs < p[0x68] ? (int)(rd32(p + 0x50 + 2 * offs) & 0xFFFF) : 0;
    }
    return 0;
}

static uint32_t lp_button_mask(void) {
    uint8_t* p = P();
    if (p[0x72] != 1 || p[0x64] < 2 || p[0x66] < 2) return 0;
    return (uint32_t)p[0x79] | (uint32_t)p[0x7A] << 8 | (uint32_t)p[0x7B] << 16 | (uint32_t)p[0x7C] << 24;
}
static int lp_info_press_mode(void) { return lp_button_mask() == 0x3FFFF; }

static int lp_set_main_mode(int mode, int lock) {
    uint32_t extra[2] = { (uint32_t)mode, (uint32_t)lock };
    rpc(0x06, 0, 0, extra, 2);
    if (rd32(rpcbuf + 0x14) == 1) lp_set_req_state(2);
    return (int)rd32(rpcbuf + 0x14);
}

static int lp_set_act_align(const uint8_t align[6]) {
    wr32(rpcbuf, 0x08); wr32(rpcbuf + 4, 0); wr32(rpcbuf + 8, 0);
    memcpy(rpcbuf + 0x0C, align, 6);
    rpc(0x08, 0, 0, NULL, 0);
    if (rd32(rpcbuf + 0x14) == 1) lp_set_req_state(2);
    return (int)rd32(rpcbuf + 0x14);
}

static int lp_set_button_info(uint32_t info) {
    rpc(0x0A, 0, 0, &info, 1);
    if (rd32(rpcbuf + 0x10) == 1) lp_set_req_state(2);
    return (int)rd32(rpcbuf + 0x10);
}

/* scePadSetActDirect: copy into the slot's block and SIF-DMA it to the IOP address padman gave. */
static int lp_set_act_direct(int port, int slot, const uint8_t data[6]) {
    if (P()[0x72] != 1) return 0;
    uint8_t blk[0x20];
    memset(blk, 0, sizeof blk);
    uint32_t c = ++act_counter[port][slot];
    wr32(blk, c);
    wr32(blk + 4, 1);
    wr32(blk + 8, 6);
    memcpy(blk + 0x0C, data, 6);
    memcpy(iop_mem(act_iop_addr[port][slot] + ((c & 1) ? 0x20 : 0)), blk, sizeof blk);
    return 1;
}

static int wait_complete(int max_frames, int* saw_execcmd) {
    for (int f = 1; f <= max_frames; f++) {
        vsync(1);
        if (saw_execcmd && lp_get_state() == 5) *saw_execcmd = 1;
        if (lp_get_req_state() == 0 && lp_get_state() == 6) return f;
    }
    return -1;
}

static int wait_state(int want, int max_frames) {
    for (int f = 1; f <= max_frames; f++) { vsync(1); if (lp_get_state() == want) return f; }
    return -1;
}

static void reset_world(void) {
    memset(ps2_ram, 0, 0x02000000);
    memset(iop_ram, 0, sizeof iop_ram);
    memset(rpcbuf, 0, sizeof rpcbuf);
    memset(&input, 0, sizeof input);
    input.lx = input.ly = input.rx = input.ry = 0x80;
    CHECK_EQ(lp_init(), 1);             /* closes anything a previous test left open */
}

/* ---- tests -------------------------------------------------------------------------------------- */

static void test_rpc_basics(void) {
    uint8_t in[128] = { 0 }, out[128];
    CHECK_EQ(pad_rpc(NULL, 0x80000400u, 1, in, 128, out, 128), 0);    /* someone else's server */
    CHECK_EQ(pad_rpc(NULL, 0x80000101u, 1, in, 128, out, 128), 1);    /* extended service: echo */

    uint32_t v = lp_get_mod_version();
    CHECK_EQ(v, 0x0410);
    CHECK_EQ(v >> 8, 4);                /* scePadInit refuses anything but major version 4 */
    CHECK_EQ(lp_init(), 1);
    rpc(0x0C, 0, 0, NULL, 0); CHECK_EQ(rd32(rpcbuf + 0x0C), 2);         /* GetPortMax */
    rpc(0x0D, 0, 0, NULL, 0); CHECK_EQ(rd32(rpcbuf + 0x0C), 1);         /* GetSlotMax: no multitap */

    /* the reply is the request buffer with results filled in */
    wr32(rpcbuf + 0x40, 0xDEADBEEF);
    rpc(0x12, 0, 0, NULL, 0);
    CHECK_EQ(rd32(rpcbuf + 0x40), 0xDEADBEEF);
    CHECK_EQ(rd32(rpcbuf), 0x12);
    rpc(0x02, 0, 0, NULL, 0);           /* invalid function code: buffer unchanged */
    CHECK_EQ(rd32(rpcbuf), 0x02);
}

static void test_no_processing_before_init(void) {
    rpc(0x0F, 0, 0, NULL, 0);           /* End */
    CHECK_EQ(rd32(rpcbuf + 0x0C), 1);
    memset(ps2_ram + EE_PADBUF, 0xAB, 0x100);
    vsync(5);
    CHECK_EQ(ps2_ram[EE_PADBUF + 0x70], 0xAB);
}

static void test_open_and_detect(void) {
    reset_world();
    CHECK_EQ(lp_port_open(0, 0, EE_PADBUF), 1);
    CHECK_EQ(act_iop_addr[0][0] & 0xF, 0);
    CHECK(iop_mem(act_iop_addr[0][0]) != NULL);
    CHECK_EQ(lp_port_open(0, 0, EE_PADBUF), 0);                        /* already open */
    CHECK_EQ(lp_port_open(2, 0, EE_PADBUF + 0x200), 0);                /* no such port */
    CHECK_EQ(lp_get_state(), 5);

    /* frame counters: frame n goes to copy n & 1 */
    vsync(1);
    CHECK_EQ(rd32(ps2_ram + EE_PADBUF + 0x58), 0);
    CHECK_EQ(lp_get_state(), 5);
    CHECK_EQ(P()[0x72], 2);             /* detecting: information not valid */
    vsync(1);
    CHECK_EQ(rd32(ps2_ram + EE_PADBUF + 0xD8), 1);
    CHECK(P() == ps2_ram + EE_PADBUF + 0x80);

    int f = wait_state(6, 60);
    CHECK(f > 0 && f < 30);
    uint8_t* p = P();
    CHECK_EQ(p[0x64], 2);               /* configurable */
    CHECK_EQ(p[0x65], 0x41);            /* DualShock 2 powers up in digital mode */
    CHECK_EQ(p[0x66], 3);
    CHECK_EQ(p[0x67], 1);
    CHECK_EQ(p[0x68], 2);
    CHECK_EQ(p[0x69], 0);
    CHECK_EQ(p[0x6A], 2);
    CHECK_EQ(p[0x6B], 1);
    CHECK_EQ(p[0x71], 0);
    CHECK_EQ(p[0x72], 1);
    CHECK_EQ(rd32(p + 0x60), 32);
    CHECK_EQ(lp_info_mode(1, 0), 4);    /* PAD_TYPE_DIGITAL */
    CHECK_EQ(lp_info_mode(2, 0), 4);
    CHECK_EQ(lp_info_mode(3, 0), 0);
    CHECK_EQ(lp_info_mode(4, -1), 2);
    CHECK_EQ(lp_info_mode(4, 0), 4);
    CHECK_EQ(lp_info_mode(4, 1), 7);
    CHECK_EQ(lp_info_mode(4, 2), 0);
    CHECK_EQ(lp_button_mask(), 0);      /* no analog bytes in digital mode */
    CHECK_EQ(lp_info_press_mode(), 0);

    /* the same answers through padman's own information commands */
    uint32_t e[2] = { 1, 0 };
    rpc(0x05, 0, 0, e, 2); CHECK_EQ(rd32(rpcbuf + 0x14), 4);
    e[0] = 4; e[1] = (uint32_t)-1;
    rpc(0x05, 0, 0, e, 2); CHECK_EQ(rd32(rpcbuf + 0x14), 2);
    e[0] = 1; e[1] = 4;
    rpc(0x03, 0, 0, e, 2); CHECK_EQ(rd32(rpcbuf + 0x14), 0x14);        /* large motor current */
    e[0] = 0; e[1] = 0;
    rpc(0x04, 0, 0, e, 2); CHECK_EQ(rd32(rpcbuf + 0x14), 0);

    /* frames keep alternating, both copies one apart */
    vsync(3);
    uint32_t f0 = rd32(ps2_ram + EE_PADBUF + 0x58), f1 = rd32(ps2_ram + EE_PADBUF + 0xD8);
    CHECK_EQ(f0 > f1 ? f0 - f1 : f1 - f0, 1);
}

static void test_digital_data(void) {
    reset_world();
    lp_port_open(0, 0, EE_PADBUF);
    wait_state(6, 60);
    input.buttons = PAD_CROSS | PAD_START | PAD_UP | PAD_L3;
    input.lx = 0x10;
    vsync(1);
    uint8_t d[32];
    memset(d, 0xEE, sizeof d);
    CHECK_EQ(lp_read(d), 32);
    CHECK_EQ(d[0], 0);
    CHECK_EQ(d[1], 0x41);
    CHECK_EQ(d[2], (uint8_t)~(PAD_START | PAD_UP));      /* L3 does not exist in digital mode */
    CHECK_EQ(d[3], (uint8_t)~(PAD_CROSS >> 8));
    CHECK_EQ(d[4], 0);                  /* no stick bytes */
    CHECK_EQ(d[31], 0x5A);
    /* the game's decoding: (d[2] << 8 | d[3]) ^ 0xFFFF; 0x1000 = up, 0x0040 = cross */
    uint16_t g = (uint16_t)((d[2] << 8 | d[3]) ^ 0xFFFF);
    CHECK(g & 0x1000);
    CHECK(g & 0x0040);
    CHECK(g & 0x0800);                  /* start */
}

/* The game's setup sequence: analog + lock, actuator alignment, pressure mode. */
static void test_game_sequence(void) {
    reset_world();
    CHECK_EQ(lp_port_open(0, 0, EE_PADBUF), 1);
    CHECK(wait_state(6, 60) > 0);

    /* (1,0): DualShock in digital mode */
    int id = lp_info_mode(1, 0), ex = lp_info_mode(2, 0);
    CHECK_EQ(id, 4);
    CHECK_EQ(ex, 4);
    /* (1,1): analog, locked */
    CHECK_EQ(lp_set_main_mode(1, 3), 1);
    CHECK_EQ(lp_get_state(), 5);        /* STABLE + BUSY reads as EXECCMD */
    CHECK_EQ(lp_set_main_mode(1, 3), 0);/* one request at a time */
    int busy = 0;
    int f = wait_complete(120, &busy);  /* the game gives up after 2 seconds */
    CHECK(f > 0);
    CHECK(busy);
    fprintf(stderr, "  SetMainMode completed after %d frames\n", f);
    uint8_t* p = P();
    CHECK_EQ(p[0x65], 0x73);
    CHECK_EQ(p[0x69], 1);
    CHECK_EQ(p[0x6D], 1);
    CHECK_EQ(p[0x6E], 3);
    CHECK_EQ(lp_info_mode(1, 0), 7);    /* PAD_TYPE_DUALSHOCK */
    CHECK_EQ(lp_info_mode(2, 0), 7);
    CHECK_EQ(lp_info_mode(3, 0), 1);

    /* (1,2): the actuators the game insists on */
    CHECK_EQ(lp_info_act(-1, 0), 2);
    CHECK_EQ(lp_info_act(0, 1), 1);
    CHECK_EQ(lp_info_act(0, 2), 2);
    CHECK_EQ(lp_info_act(0, 3), 0);
    CHECK_EQ(lp_info_act(1, 1), 1);
    CHECK_EQ(lp_info_act(1, 2), 1);
    CHECK_EQ(lp_info_act(1, 3), 1);
    CHECK_EQ(lp_info_act(2, 1), -1);
    static const uint8_t align[6] = { 0, 1, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK_EQ(lp_set_act_align(align), 1);
    f = wait_complete(120, NULL);
    CHECK(f > 0 && f < 10);
    CHECK_EQ(memcmp(P() + 0x28, align, 6), 0);

    /* (1,3): pressure mode */
    CHECK_EQ(lp_button_mask(), 0x3FFFF);
    CHECK_EQ(lp_info_press_mode(), 1);
    CHECK_EQ(lp_set_button_info(0xFFF), 1);
    f = wait_complete(120, NULL);
    CHECK(f > 0);
    fprintf(stderr, "  EnterPressMode completed after %d frames\n", f);
    p = P();
    CHECK_EQ(p[0x65], 0x79);
    CHECK_EQ(p[0x6C], 1);
    CHECK_EQ(lp_info_mode(1, 0), 7);

    /* (3,4): reading */
    memset(input.pressure, 0, sizeof input.pressure);
    input.buttons = PAD_CROSS | PAD_RIGHT | PAD_R2 | PAD_R3;
    input.pressure[6] = 0x40;           /* cross: partial press */
    input.pressure[1] = 0x30;           /* left: pressure but no button -> padman reports 0 */
    input.lx = 0x00; input.ly = 0xFF; input.rx = 0x12; input.ry = 0x34;
    vsync(1);
    uint8_t d[32];
    CHECK_EQ(lp_read(d), 32);
    CHECK_EQ(d[1], 0x79);
    uint16_t g = (uint16_t)((d[2] << 8 | d[3]) ^ 0xFFFF);
    CHECK(g & 0x2000);                  /* right */
    CHECK(g & 0x0040);                  /* cross */
    CHECK(g & 0x0002);                  /* R2 */
    CHECK(g & 0x0400);                  /* R3 exists in analog mode */
    CHECK_EQ(d[4], 0x12);               /* right stick x, y */
    CHECK_EQ(d[5], 0x34);
    CHECK_EQ(d[6], 0x00);               /* left stick x, y */
    CHECK_EQ(d[7], 0xFF);
    CHECK_EQ(d[8], 0xFF);               /* right: digital-only press reads as full pressure */
    CHECK_EQ(d[9], 0x00);               /* left: not pressed */
    CHECK_EQ(d[0xE], 0x40);             /* cross */
    CHECK_EQ(d[0x13], 0xFF);            /* R2 */
    CHECK_EQ(d[0x10], 0);               /* L1 */
    CHECK_EQ(d[31], 0x5A);

    /* vibration through the DMA'd actuator block */
    uint8_t m[2];
    static const uint8_t act[6] = { 1, 0xC0, 0, 0, 0, 0 };
    CHECK_EQ(lp_set_act_direct(0, 0, act), 1);
    vsync(1);
    p = P();
    CHECK_EQ(p[0x6F], 6);
    CHECK_EQ(p[0x20], 1);
    CHECK_EQ(p[0x21], 0xC0);
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 1);
    CHECK_EQ(m[1], 0xC0);
    uint8_t* blk = iop_mem(act_iop_addr[0][0] + 0x20);  /* first send: counter 1 -> second block */
    CHECK_EQ(rd32(blk + 4), 0);         /* padman clears the "new" flag */
    static const uint8_t stop[6] = { 0 };
    lp_set_act_direct(0, 0, stop);
    vsync(1);
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 0);
    CHECK_EQ(m[1], 0);
    /* the RPC variant (command 7) takes effect on the next poll */
    wr32(rpcbuf + 0x0C, 0); memcpy(rpcbuf + 0x0C, act, 6);
    rpc(0x07, 0, 0, NULL, 0);
    CHECK_EQ(rd32(rpcbuf + 0x14), 1);
    vsync(1);
    pad_get_motors(0, m);
    CHECK_EQ(m[1], 0xC0);

    /* back to digital: pressure and stick bytes disappear, ID changes */
    CHECK_EQ(lp_set_main_mode(0, 0), 1);
    CHECK(wait_complete(120, NULL) > 0);
    CHECK_EQ(P()[0x65], 0x41);
    CHECK_EQ(lp_button_mask(), 0);
}

static void test_requests_need_ready_pad(void) {
    reset_world();
    lp_port_open(0, 0, EE_PADBUF);
    vsync(2);                           /* still detecting */
    uint32_t e[2] = { 1, 3 };
    rpc(0x06, 0, 0, e, 2);
    CHECK_EQ(rd32(rpcbuf + 0x14), 0);
    wait_state(6, 60);
    e[0] = 2;                           /* only two modes */
    rpc(0x06, 0, 0, e, 2);
    CHECK_EQ(rd32(rpcbuf + 0x14), 0);
    rpc(0x09, 0, 0, NULL, 0);           /* GetButtonMask via RPC */
    CHECK_EQ(rd32(rpcbuf + 0x0C), 0);
    rpc(0x06, 1, 0, e, 2);              /* port 1 is not open */
    CHECK_EQ(rd32(rpcbuf + 0x14), 0);
}

static void test_empty_port(void) {
    reset_world();
    CHECK_EQ(lp_port_open(1, 0, EE_PADBUF), 1);
    vsync(1);
    CHECK_EQ(P()[0x70], 7);             /* no answer: error */
    CHECK_EQ(rd32(P() + 0x60), 0);
    int f = wait_state(0, 40);          /* after ten failed polls padman looks for a controller */
    CHECK(f > 0);
    CHECK_EQ(input_calls[1], 0);        /* nothing to read */
    vsync(10);
    CHECK_EQ(lp_get_state(), 0);
    CHECK_EQ(P()[0x72], 2);
    /* slots 1-3 need a multitap */
    CHECK_EQ(lp_port_open(0, 1, EE_PADBUF + 0x100), 1);
}

static void test_close_and_reopen_forces_digital(void) {
    reset_world();
    lp_port_open(0, 0, EE_PADBUF);
    wait_state(6, 60);
    CHECK_EQ(lp_set_main_mode(1, 3), 1);
    CHECK(wait_complete(120, NULL) > 0);
    CHECK_EQ(P()[0x65], 0x73);

    rpc(0x0E, 0, 0, NULL, 0);           /* PortClose */
    CHECK_EQ(rd32(rpcbuf + 0x0C), 1);
    rpc(0x0E, 0, 0, NULL, 0);
    CHECK_EQ(rd32(rpcbuf + 0x0C), 0);
    uint32_t frame = rd32(ps2_ram + EE_PADBUF + 0x58);
    vsync(3);
    CHECK_EQ(rd32(ps2_ram + EE_PADBUF + 0x58), frame);                /* no more updates */

    /* the controller is still analog; a new session puts it back into digital mode */
    CHECK_EQ(lp_port_open(0, 0, EE_PADBUF), 1);
    int f = wait_state(6, 120);
    CHECK(f > 0);
    CHECK_EQ(P()[0x65], 0x41);
    CHECK_EQ(lp_info_mode(3, 0), 0);
}

/* padman's poll thread (padman+0x2674): every failed poll sets ERROR and counts; on the tenth it
 * starts the detection thread but leaves the state at ERROR. Detection only changes the state
 * after its first transaction (DISCONN when nobody answers, padman+0x27C4). So an empty port reads
 * ERROR for ten frames and DISCONN afterwards, never EXECCMD. */
static void test_empty_port_state_sequence(void) {
    reset_world();
    CHECK_EQ(lp_port_open(1, 0, EE_PADBUF), 1);
    for (int f = 1; f <= 10; f++) {
        vsync(1);
        if (lp_get_state() != 7) fprintf(stderr, "  empty port, frame %d: state %d\n", f, lp_get_state());
        CHECK_EQ(lp_get_state(), 7);
        CHECK_EQ(P()[0x72], f < 10 ? 1 : 2);                      /* detection starts on the tenth */
    }
    vsync(1);
    CHECK_EQ(lp_get_state(), 0);
}

/* The small motor is on/off and only looks at bit 0 of its byte (psx-spx, config command 4Dh:
 * "Map Right/small Motor (Motor M2) to bit0 of this byte"; command 42h: "xx.bit0 ---> Right/small
 * Motor M2 (digital on/off) (0=off, 1=on)"). The large motor takes the whole byte as its speed. */
static void test_small_motor_uses_bit0(void) {
    reset_world();
    lp_port_open(0, 0, EE_PADBUF);
    CHECK(wait_state(6, 60) > 0);
    CHECK_EQ(lp_set_main_mode(1, 3), 1);
    CHECK(wait_complete(120, NULL) > 0);
    static const uint8_t align[6] = { 0, 1, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK_EQ(lp_set_act_align(align), 1);
    CHECK(wait_complete(120, NULL) > 0);

    uint8_t m[2];
    static const uint8_t even[6] = { 0xFE, 0x80, 0, 0, 0, 0 };
    CHECK_EQ(lp_set_act_direct(0, 0, even), 1);
    vsync(1);
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 0);                  /* bit 0 clear: small motor off */
    CHECK_EQ(m[1], 0x80);
    static const uint8_t odd[6] = { 0x03, 0, 0, 0, 0, 0 };
    CHECK_EQ(lp_set_act_direct(0, 0, odd), 1);
    vsync(1);
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 1);                  /* on */
    CHECK_EQ(m[1], 0);
}

/* Command 4Dh decides which poll byte drives which motor; a motor no byte is mapped to gets no
 * drive value any more and stops (psx-spx: 4Dh with FFh bytes "disables motor control"). Here the
 * game turns the large motor on, then unmaps both motors with SetActAlign. */
static void test_unmapped_motor_stops(void) {
    reset_world();
    lp_port_open(0, 0, EE_PADBUF);
    CHECK(wait_state(6, 60) > 0);
    CHECK_EQ(lp_set_main_mode(1, 3), 1);
    CHECK(wait_complete(120, NULL) > 0);
    static const uint8_t align[6] = { 0, 1, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK_EQ(lp_set_act_align(align), 1);
    CHECK(wait_complete(120, NULL) > 0);
    static const uint8_t act[6] = { 1, 0xC0, 0, 0, 0, 0 };
    CHECK_EQ(lp_set_act_direct(0, 0, act), 1);
    vsync(1);
    uint8_t m[2];
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 1);
    CHECK_EQ(m[1], 0xC0);

    static const uint8_t none[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK_EQ(lp_set_act_align(none), 1);
    CHECK(wait_complete(120, NULL) > 0);
    vsync(2);
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 0);
    CHECK_EQ(m[1], 0);
}

/* Entering configuration mode arms the controller's watchdog: after about a second without any
 * communication it resets, which "does disable and lock rumble motors, and switches the
 * controller to Digital Mode" (psx-spx, command 43h). Here the game ends its session (Init closes
 * the ports) while the motors run; nobody polls the pad any more. */
static void test_watchdog_after_polling_stops(void) {
    reset_world();
    lp_port_open(0, 0, EE_PADBUF);
    CHECK(wait_state(6, 60) > 0);
    CHECK_EQ(lp_set_main_mode(1, 3), 1);
    CHECK(wait_complete(120, NULL) > 0);
    static const uint8_t align[6] = { 0, 1, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK_EQ(lp_set_act_align(align), 1);
    CHECK(wait_complete(120, NULL) > 0);
    static const uint8_t act[6] = { 1, 0xC0, 0, 0, 0, 0 };
    CHECK_EQ(lp_set_act_direct(0, 0, act), 1);
    vsync(1);
    uint8_t m[2];
    pad_get_motors(0, m);
    CHECK_EQ(m[1], 0xC0);

    CHECK_EQ(lp_init(), 1);             /* ports closed: no more transactions */
    vsync(30);                          /* half a second: still running */
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 1);
    CHECK_EQ(m[1], 0xC0);
    vsync(60);                          /* well past a second: the controller has reset */
    pad_get_motors(0, m);
    CHECK_EQ(m[0], 0);
    CHECK_EQ(m[1], 0);

    /* it comes back in digital mode, so the first detection needs no forced mode change */
    CHECK_EQ(lp_port_open(0, 0, EE_PADBUF), 1);
    int f = wait_state(6, 60);
    CHECK_EQ(f, 14);
    CHECK_EQ(P()[0x65], 0x41);

    /* while the game keeps polling, nothing resets, however long it runs */
    CHECK_EQ(lp_set_main_mode(1, 3), 1);
    CHECK(wait_complete(120, NULL) > 0);
    vsync(600);
    CHECK_EQ(P()[0x65], 0x73);
    CHECK_EQ(lp_get_state(), 6);
}

static void test_init_closes_ports(void) {
    reset_world();
    lp_port_open(0, 0, EE_PADBUF);
    vsync(2);
    lp_init();
    uint32_t frame = rd32(ps2_ram + EE_PADBUF + 0x58);
    vsync(2);
    CHECK_EQ(rd32(ps2_ram + EE_PADBUF + 0x58), frame);
    CHECK_EQ(lp_port_open(0, 0, EE_PADBUF), 1);
}

int main(void) {
    ps2_ram = calloc(1, 0x02000000 + 16);
    ps2_spr = calloc(1, 0x4000 + 16);
    if (!ps2_ram || !ps2_spr) return 2;
    test_rpc_basics();
    test_no_processing_before_init();
    test_open_and_detect();
    test_digital_data();
    test_game_sequence();
    test_requests_need_ready_pad();
    test_empty_port();
    test_close_and_reopen_forces_digital();
    test_init_closes_ports();
    test_empty_port_state_sequence();
    test_small_motor_uses_bit0();
    test_unmapped_motor_stops();
    test_watchdog_after_polling_stops();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
