/* SIF (EE <-> IOP link) and a high-level-emulated IOP.
 *
 * The game's own libsif code runs unchanged on the EE side. It sends command packets to the
 * IOP with SifSetDma; we parse them here and answer the way the IOP's sifcmd/sifrpc modules
 * would: by writing reply packets into the EE's receive buffer and raising the SIF0 DMA
 * interrupt, whose handler (game code) dispatches them. Packet layouts follow Sony's sifcmd
 * protocol (also documented by ps2sdk).
 */
#include "hw.h"
#include "perf.h"

#include <stdlib.h>

int ps2_log_sif;

#define IOP_RAM_SIZE   0x00200000u     /* 2 MB */
#define IOP_CMD_BUF    0x0001F000u     /* where the IOP receives command packets (IOP address) */

/* system command ids */
#define SIF_CMD_CHANGE_SADDR 0x80000000u
#define SIF_CMD_SET_SREG     0x80000001u
#define SIF_CMD_INIT_CMD     0x80000002u
#define SIF_CMD_RPC_END      0x80000008u
#define SIF_CMD_RPC_BIND     0x80000009u
#define SIF_CMD_RPC_CALL     0x8000000Au

/* ---- RPC servers ------------------------------------------------------------------------------
 * Each bound server ID gets a fake server handle and a receive buffer in IOP RAM. Calls copy
 * their arguments there; the handler for that server (below) produces the reply data. */

#define RPC_BUF_BASE 0x00100000u            /* IOP RAM area for server receive buffers */
#define RPC_BUF_SIZE 0x00004000u            /* 16 KB each */

typedef struct {
    uint32_t sid, server, buff;
    uint32_t calls;
} rpc_server;
static rpc_server servers[32];
static int nservers;

static rpc_server* server_for_sid(uint32_t sid) {
    for (int i = 0; i < nservers; i++) if (servers[i].sid == sid) return &servers[i];
    if (nservers == 32) return NULL;
    rpc_server* s = &servers[nservers];
    s->sid = sid;
    s->server = 0x00080000u + 0x40u * (uint32_t)nservers;    /* opaque handle (IOP address) */
    s->buff = RPC_BUF_BASE + RPC_BUF_SIZE * (uint32_t)nservers;
    nservers++;
    fprintf(stderr, "[sif] RPC server 0x%08X bound (handle 0x%08X, buffer 0x%08X)\n", sid, s->server, s->buff);
    return s;
}

static rpc_server* server_by_handle(uint32_t handle) {
    for (int i = 0; i < nservers; i++) if (servers[i].server == handle) return &servers[i];
    return NULL;
}

static ps2_ctx* cur_ctx;                    /* for EE memory access while handling a command */

/* Fill `out` (recv_size bytes, initially zero) with the reply to one RPC call. */
static void rpc_dispatch(rpc_server* s, uint32_t fno, const uint8_t* args, uint32_t send_size,
                         uint8_t* out, uint32_t recv_size) {
    if (cdvd_rpc(cur_ctx, s->sid, fno, args, send_size, out, recv_size)) return;
    if (iop_modules_rpc(cur_ctx, s->sid, fno, args, send_size, out, recv_size)) return;
    if (snd_rpc(cur_ctx, s->sid, fno, args, send_size, out, recv_size)) return;
    if (pad_rpc(cur_ctx, s->sid, fno, args, send_size, out, recv_size)) return;
    if (mc_rpc(cur_ctx, s->sid, fno, args, send_size, out, recv_size)) return;
    if (s->calls++ < 4 || ps2_log_sif)
        fprintf(stderr, "[sif] RPC call sid 0x%08X fn %u (send %u, recv %u): no handler, replying zeros\n",
                s->sid, fno, send_size, recv_size);
}

static uint8_t  iop_ram[IOP_RAM_SIZE];

uint8_t* iop_mem(uint32_t a) {
    a &= 0x1FFFFFFFu;
    return a < IOP_RAM_SIZE ? iop_ram + a : NULL;
}
static uint32_t ee_pktbuf;             /* EE address of the EE's command receive buffer */
static uint32_t sysregs[8];            /* SIF_SYSREG_* (0x8000000x), set by the EE */
static uint32_t regs[8];               /* SIF_REG_* mailbox registers */
static uint32_t next_dma_id = 1;

/* ---- outgoing (IOP -> EE) packets ---------------------------------------------------------- */

typedef struct { uint8_t data[128]; uint32_t size; } packet;
static packet   queue[64];
static unsigned q_head, q_tail;

static void send_to_ee(uint32_t cid, const void* body, uint32_t body_size);

/* RPC_END tells the EE a bind or call finished. Layout after the 16-byte command header:
 * rec_id, pkt_addr, rpc_id, client, cid, server, buff, cbuf. The first four are echoed back
 * from the request so the EE can match it up. */
static void rpc_end(const uint8_t* req, uint32_t cid, uint32_t server, uint32_t buff) {
    uint32_t body[8];
    memcpy(body, req + 16, 16);              /* rec_id, pkt_addr, rpc_id, client */
    body[4] = cid;
    body[5] = server;
    body[6] = buff;
    body[7] = 0;
    send_to_ee(SIF_CMD_RPC_END, body, sizeof body);
}

static void send_to_ee(uint32_t cid, const void* body, uint32_t body_size) {
    if (q_tail - q_head >= 64) { fprintf(stderr, "[sif] IOP->EE queue full, dropping cid 0x%08X\n", cid); return; }
    packet* p = &queue[q_tail++ & 63];
    uint32_t size = 16 + body_size;
    uint32_t hdr[4] = { size & 0xFF, 0, cid, 0 };     /* psize, dsize/dest unused, cid, opt */
    memcpy(p->data, hdr, 16);
    memcpy(p->data + 16, body, body_size);
    p->size = size;
}

/* Hand the next queued packet to the EE: copy it into the receive buffer and raise the SIF0
 * DMA interrupt. One packet at a time, like the hardware: the next one waits until the game's
 * handler has taken this one (its D_STAT bit acknowledged) and SIF0 interrupts are enabled. */
void sif_deliver(ps2_ctx* ctx) {
    (void)ctx;
    if (q_head == q_tail || !ee_pktbuf) return;
    if (!dmac_mask_enabled(DMA_SIF0) || (dmac_stat_read() & (1u << DMA_SIF0))) return;
    packet* p = &queue[q_head & 63];
    uint8_t* dst = ps2_fast(ee_pktbuf);
    if (!dst) { fprintf(stderr, "[sif] EE receive buffer 0x%08X not in RAM\n", ee_pktbuf); return; }
    memcpy(dst, p->data, p->size);
    q_head++;
    if (ps2_log_sif) {
        uint32_t cid; memcpy(&cid, p->data + 8, 4);
        fprintf(stderr, "[sif] IOP->EE cid 0x%08X (%u bytes)\n", cid, p->size);
    }
    dmac_raise(DMA_SIF0);
}

/* ---- incoming (EE -> IOP) commands ---------------------------------------------------------- */

static void handle_rpc_call(const uint8_t* pkt) {
    uint32_t f[10];
    memcpy(f, pkt + 16, sizeof f);
    /* rec_id, pkt_addr, rpc_id, client, rpc_number, send_size, receive, recv_size, rmode, server */
    uint32_t fno = f[4], send_size = f[5], receive = f[6], recv_size = f[7], handle = f[9];
    rpc_server* s = server_by_handle(handle);
    if (!s) { fprintf(stderr, "[sif] RPC call to unknown server 0x%08X\n", handle); return; }

    static uint8_t out[0x10000];
    if (recv_size > sizeof out) recv_size = sizeof out;
    memset(out, 0, recv_size);
    rpc_dispatch(s, fno, iop_ram + (s->buff & (IOP_RAM_SIZE - 1)), send_size, out, recv_size);
    for (uint32_t k = 0; k < recv_size; k++)                    /* results go straight to EE RAM */
        ps2_w8(cur_ctx, receive + k, out[k]);
    rpc_end(pkt, SIF_CMD_RPC_CALL, handle, s->buff);
}

static void handle_command(const uint8_t* pkt) {
    uint32_t hdr[4];
    memcpy(hdr, pkt, 16);
    uint32_t cid = hdr[2];
    if (ps2_log_sif) fprintf(stderr, "[sif] EE->IOP cid 0x%08X (psize %u)\n", cid, hdr[0] & 0xFF);

    switch (cid) {
    case SIF_CMD_CHANGE_SADDR:
        memcpy(&ee_pktbuf, pkt + 16, 4);
        if (ps2_log_sif) fprintf(stderr, "[sif] EE receive buffer at 0x%08X\n", ee_pktbuf);
        break;
    case SIF_CMD_INIT_CMD:
        if ((hdr[0] & 0xFF) >= 20) {
            /* from sceSifInitCmd: the body carries the EE's receive buffer address */
            memcpy(&ee_pktbuf, pkt + 16, 4);
            if (ps2_log_sif) fprintf(stderr, "[sif] EE receive buffer at 0x%08X (opt %u)\n", ee_pktbuf, hdr[3]);
        } else {
            /* from sceSifInitRpc: the IOP acknowledges by setting software register 0 */
            uint32_t body[2] = { 0, 1 };
            send_to_ee(SIF_CMD_SET_SREG, body, sizeof body);
        }
        break;
    case SIF_CMD_RPC_BIND: {
        uint32_t sid;
        memcpy(&sid, pkt + 32, 4);           /* after rec_id, pkt_addr, rpc_id, client */
        rpc_server* s = server_for_sid(sid);
        rpc_end(pkt, SIF_CMD_RPC_BIND, s ? s->server : 0, s ? s->buff : 0);
        break;
    }
    case SIF_CMD_RPC_CALL:
        handle_rpc_call(pkt);
        break;
    default:
        fprintf(stderr, "[sif] EE->IOP command 0x%08X not handled yet\n", cid);
        break;
    }
}

uint32_t sif_set_dma(ps2_ctx* ctx, uint32_t desc, uint32_t count) {
    int perf_prev = perf_enter(PERF_IOP);                         /* PS2_PERF: IOP services */
    cur_ctx = ctx;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t d = desc + 16 * i;
        uint32_t src = ps2_r32(ctx, d), dest = ps2_r32(ctx, d + 4), size = ps2_r32(ctx, d + 8);
        uint32_t off = dest & (IOP_RAM_SIZE - 1);
        if (off + size > IOP_RAM_SIZE) { fprintf(stderr, "[sif] DMA to IOP 0x%08X+%u out of range\n", dest, size); continue; }
        for (uint32_t k = 0; k < size; k += 4) {                  /* source may span RAM views */
            uint32_t w = ps2_r32(ctx, src + k);
            memcpy(iop_ram + off + k, &w, size - k < 4 ? size - k : 4);
        }
        if (off == IOP_CMD_BUF) handle_command(iop_ram + off);
    }
    perf_leave(perf_prev);
    return next_dma_id++;
}

/* ---- registers ------------------------------------------------------------------------------ */

uint32_t sif_get_reg(uint32_t reg) {
    if (reg & 0x80000000u) return sysregs[reg & 7];
    switch (reg) {
    case 2: return IOP_CMD_BUF;                 /* SIF_REG_SUBADDR: IOP's command buffer */
    case 4: return 0x00070000u;                 /* SIF_REG_SMFLAG: IOP booted, SIF + CMD ready */
    default: return regs[reg & 7];
    }
}

void sif_set_reg(uint32_t reg, uint32_t value) {
    if (reg & 0x80000000u) sysregs[reg & 7] = value;
    else regs[reg & 7] = value;
}
