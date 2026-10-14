/* Native IOP SIF libraries (sifman, sifcmd: the RPC server side) and the IOP's CD/DVD driver
 * (cdvdman), for the recompiled sound modules.
 *
 * RPC: a guest thread registers a server (sceSifRegisterRpc) on a queue (sceSifSetRpcQueue) and
 * serves it in sceSifRpcLoop. sif.c hands every EE call for such a server to iop_rpc_request();
 * the serving thread wakes, runs the guest's server function and replies through sif_rpc_reply()
 * (result copied to the EE, RPC_END sent), possibly long after the request (bank loads read the
 * disc first). Structure layouts follow Sony's sifrpc.h (also in ps2sdk).
 *
 * cdvdman: sceCdRead copies from the disc image (cdvd.c) into IOP RAM at once; the command then
 * stays "busy" until the next interrupt delivery point, where the sceCdCallback handler runs in
 * interrupt context and sceCdSync(0) waiters wake, as after a real drive interrupt. */
#include "iop_internal.h"

#include <stdlib.h>

/* ---- sifcmd: RPC servers ------------------------------------------------------------------------- */

typedef struct { uint32_t sd, sid, func, buff, cfunc, cbuff, qd, calls; } rpc_server;
typedef struct {
    uint32_t qd;
    int thread;
    iop_rpc_req req[32];
    unsigned head, tail;
} rpc_queue;

static rpc_server servers[16];
static int nservers;
static rpc_queue queues[8];
static int nqueues;

static rpc_queue* queue_by_addr(uint32_t qd) {
    for (int i = 0; i < nqueues; i++) if (queues[i].qd == qd) return &queues[i];
    return NULL;
}

void iop_imp_sifcmd_sceSifInitRpc(iop_ctx* c) { (void)c; }

/* sceSifSetRpcQueue(SifRpcDataQueue_t* qd, int thread_id) */
void iop_imp_sifcmd_sceSifSetRpcQueue(iop_ctx* c) {
    uint32_t qd = c->r[4];
    rpc_queue* q = queue_by_addr(qd);
    if (!q) {
        if (nqueues == 8) { fprintf(stderr, "[iop] too many RPC queues\n"); return; }
        q = &queues[nqueues++];
    }
    memset(q, 0, sizeof *q);
    q->qd = qd;
    q->thread = (int)c->r[5];
    iop_w32(qd + 0, c->r[5]);          /* thread_id */
    iop_w32(qd + 4, 0);                /* active */
    for (uint32_t k = 8; k < 24; k += 4) iop_w32(qd + k, 0);   /* link, start, end, next */
}

/* sceSifRegisterRpc(SifRpcServerData_t* sd, int sid, func, buff, cfunc, cbuff, SifRpcDataQueue_t* qd) */
void iop_imp_sifcmd_sceSifRegisterRpc(iop_ctx* c) {
    uint32_t sd = c->r[4];
    rpc_server s = { sd, c->r[5], c->r[6], c->r[7], iop_arg(c, 4), iop_arg(c, 5), iop_arg(c, 6), 0 };
    for (uint32_t k = 0; k < 0x44; k += 4) iop_w32(sd + k, 0);
    iop_w32(sd + 0, s.sid);
    iop_w32(sd + 4, s.func);
    iop_w32(sd + 8, s.buff);
    iop_w32(sd + 16, s.cfunc);
    iop_w32(sd + 20, s.cbuff);
    iop_w32(sd + 64, s.qd);            /* base */
    int i = 0;
    while (i < nservers && servers[i].sid != s.sid) i++;
    if (i == 16) { fprintf(stderr, "[iop] too many RPC servers\n"); return; }
    if (i == nservers) nservers++;
    servers[i] = s;
    rpc_queue* q = queue_by_addr(s.qd);
    if (q && !iop_r32(s.qd + 12)) iop_w32(s.qd + 12, sd);   /* queue's first server */
    fprintf(stderr, "[iop] RPC server 0x%08X registered by the IOP (sd 0x%08X, buffer 0x%08X, thread %d)\n",
            s.sid, sd, s.buff, q ? q->thread : -1);
}

/* The next three are called by the EE side (sif.c): under the IOP lock (the IOP thread). */
int iop_rpc_find_server(uint32_t sid, uint32_t* sd, uint32_t* buff) {
    int found = 0;
    iop_lock();
    for (int i = 0; i < nservers && !found; i++)
        if (servers[i].sid == sid) { *sd = servers[i].sd; *buff = servers[i].buff; found = 1; }
    iop_unlock();
    return found;
}

int iop_rpc_is_server(uint32_t sd) {
    int found = 0;
    iop_lock();
    for (int i = 0; i < nservers && !found; i++) found = servers[i].sd == sd;
    iop_unlock();
    return found;
}

static rpc_server* server_by_sd(uint32_t sd) {
    for (int i = 0; i < nservers; i++) if (servers[i].sd == sd) return &servers[i];
    return NULL;
}

static void rpc_request(const iop_rpc_req* r);

void iop_rpc_request(const iop_rpc_req* r) {
    iop_lock();
    rpc_request(r);
    iop_unlock();
}

static void rpc_request(const iop_rpc_req* r) {
    rpc_server* s = server_by_sd(r->server);
    rpc_queue* q = s ? queue_by_addr(s->qd) : NULL;
    if (!q) { fprintf(stderr, "[iop] RPC request for unknown server 0x%08X\n", r->server); return; }
    if (q->tail - q->head >= 32) { fprintf(stderr, "[iop] RPC queue full (sid 0x%08X)\n", s->sid); return; }
    /* The EE DMAs a call's arguments into the server's one receive buffer before the call packet,
     * so a second call while one is still queued has already overwritten the first one's data. */
    for (unsigned i = q->head; i != q->tail; i++)
        if (q->req[i & 31].server == r->server) {
            static int warned;
            if (warned++ < 5) fprintf(stderr, "[iop] RPC 0x%08X fn %u arrived while fn %u is still queued: arguments overwritten\n",
                                      s->sid, r->fno, q->req[i & 31].fno);
            break;
        }
    q->req[q->tail++ & 31] = *r;
    int t = iop_find_waiter(IOP_WAIT_RPC, q->qd, 1);
    if (t) iop_wake(t, KE_OK);
}

/* PS2_LOG_SND=1: the 989snd commands the EE sends (as runtime/snd.c logs them on the fallback
 * path) with their results, e.g. spiop's 'SUKR' stream procs through commands 76/104:
 * "[snd] v<vblank> cmd <n> (<bytes>): <words> -> <result>". The arguments are copied before the
 * call (the server may reuse its receive buffer); each line goes out in one write, so it neither
 * interleaves with other threads' output nor costs a system call per word. */
#define SND_SID 0x00123456u

static int snd_log_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS2_LOG_SND"); on = e && *e && *e != '0'; }
    return on;
}

static uint8_t snd_args[16384];

static void snd_log_args(uint32_t buff, uint32_t send_size) {
    uint32_t n = send_size < sizeof snd_args ? send_size : (uint32_t)sizeof snd_args;
    for (uint32_t k = 0; k < n; k++) snd_args[k] = iop_r8(buff + k);
}

static uint32_t snd_arg32(uint32_t off) {
    if (off + 4 > sizeof snd_args) return 0;
    uint32_t v;
    memcpy(&v, snd_args + off, 4);
    return v;
}

static void snd_log_line(int direct, int cmd, uint32_t data_off, uint32_t n, uint32_t result) {
    char line[256];
    int len = snprintf(line, sizeof line, "[snd] v%u %scmd %d (%u bytes):", ps2_vblank_count(), direct ? "direct " : "",
                       cmd, n);
    for (uint32_t k = 0; k + 4 <= n && k < 32; k += 4)
        len += snprintf(line + len, sizeof line - (size_t)len, " %08X", snd_arg32(data_off + k));
    snprintf(line + len, sizeof line - (size_t)len, " -> %08X\n", result);
    fputs(line, stderr);
}

static void snd_log(uint32_t fno, uint32_t send_size, uint32_t reply) {
    if (send_size > sizeof snd_args) send_size = sizeof snd_args;
    if (fno != 77) {                                   /* a direct ("wait") command: 12-byte reply */
        snd_log_line(1, (int)fno, 0, send_size, reply ? iop_r32(reply + 4) : 0);
        return;
    }
    uint32_t count = snd_arg32(0), pos = 4;            /* batch: s16 cmd, s16 size, data */
    for (uint32_t i = 0; i < count && pos + 4 <= send_size; i++) {
        uint32_t w = snd_arg32(pos);
        int16_t cmd = (int16_t)(w & 0xFFFF), size = (int16_t)(w >> 16);
        uint32_t n = size > 0 ? (uint32_t)size : 0;
        if (pos + 4 + n > send_size) n = send_size - pos - 4;
        snd_log_line(0, cmd, pos + 4, n, reply ? iop_r32(reply + 4 + 4 * i) : 0);
        pos += 4 + ((n + 3) & ~3u);
    }
}

/* sceSifRpcLoop(SifRpcDataQueue_t* qd): serve requests forever. */
void iop_imp_sifcmd_sceSifRpcLoop(iop_ctx* c) {
    uint32_t qd = c->r[4];
    for (;;) {
        rpc_queue* q = queue_by_addr(qd);
        if (!q) { fprintf(stderr, "[iop] sceSifRpcLoop on an unknown queue 0x%08X\n", qd); return; }
        if (q->head == q->tail) {
            if (iop_block(IOP_WAIT_RPC, qd) != KE_OK) return;
            continue;
        }
        iop_rpc_req r = q->req[q->head++ & 31];
        rpc_server* s = server_by_sd(r.server);
        if (!s) continue;
        iop_w32(s->sd + 12, r.send_size);    /* size */
        iop_w32(s->sd + 28, r.pkt[3]);       /* client */
        iop_w32(s->sd + 32, r.pkt[1]);       /* pkt_addr */
        iop_w32(s->sd + 36, r.fno);          /* rpc_number */
        iop_w32(s->sd + 40, r.receive);      /* receive */
        iop_w32(s->sd + 44, r.recv_size);    /* rsize */
        iop_w32(s->sd + 48, r.rmode);        /* rmode */
        iop_w32(s->sd + 52, r.pkt[0]);       /* rid */
        int log_snd = s->sid == SND_SID && snd_log_on();
        if (log_snd) snd_log_args(s->buff, r.send_size);
        uint32_t reply = iop_guest_call(c, s->func, r.fno, s->buff, r.send_size, 0);
        if (log_snd) snd_log(r.fno, r.send_size, reply);
        s = server_by_sd(r.server);                       /* (the table may have changed meanwhile) */
        if (s) s->calls++;
        if (iop_log || (s && s->calls <= 16))             /* the first calls of each server, e.g. bank loads */
            fprintf(stderr, "[iop] RPC 0x%08X fn %u (%u bytes) -> reply at 0x%08X, first word 0x%08X\n",
                    s ? s->sid : 0, r.fno, r.send_size, reply, reply ? iop_r32(reply) : 0);
        sif_rpc_reply(&r, reply);
    }
}

/* sceSifGetOtherData(SifRpcReceiveData_t* rd, void* src (EE), void* dest (IOP), int size, int mode) */
void iop_imp_sifcmd_sceSifGetOtherData(iop_ctx* c) {
    uint32_t src = c->r[5], dst = c->r[6], size = c->r[7];
    iop_ret(c, sif_ee_to_iop(dst, src, size) ? 0 : -1);
}

/* ---- sifman: IOP -> EE DMA ---------------------------------------------------------------------- */

static uint32_t dma_id = 1;
static struct { uint32_t fn, arg, gp; } dma_cb[16];
static unsigned cb_head, cb_tail;

static uint32_t do_dma(uint32_t list, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {     /* SifDmaTransfer_t: src (IOP), dest (EE), size, attr */
        uint32_t d = list + 16 * i;
        sif_iop_to_ee(iop_r32(d + 4), iop_r32(d), iop_r32(d + 8));
    }
    if (++dma_id == 0) dma_id = 1;
    return dma_id;
}

void iop_imp_sifman_sceSifInit(iop_ctx* c) { (void)c; }
void iop_imp_sifman_sceSifCheckInit(iop_ctx* c) { iop_ret(c, 1); }
void iop_imp_sifman_sceSifSetDma(iop_ctx* c) { iop_ret(c, (int32_t)do_dma(c->r[4], c->r[5])); }
void iop_imp_sifman_sceSifDmaStat(iop_ctx* c) { iop_ret(c, -1); }   /* every transfer is already done */

/* sceSifSetDmaIntr(dmat, count, void (*func)(void*), void* data): func runs as the DMA interrupt. */
void iop_imp_sifman_sceSifSetDmaIntr(iop_ctx* c) {
    uint32_t id = do_dma(c->r[4], c->r[5]);
    if (c->r[6]) {
        if (cb_tail - cb_head < 16) {
            dma_cb[cb_tail & 15].fn = c->r[6];
            dma_cb[cb_tail & 15].arg = c->r[7];
            dma_cb[cb_tail & 15].gp = c->r[28];
            cb_tail++;
            iop_poll_flag = 1;
        } else fprintf(stderr, "[iop] SIF DMA callback queue full\n");
    }
    iop_ret(c, (int32_t)id);
}

void iop_sif_service(void) {
    while (cb_head != cb_tail) {
        unsigned k = cb_head++ & 15;
        iop_intr_call(dma_cb[k].fn, dma_cb[k].arg, 0, dma_cb[k].gp);
    }
}

/* ---- cdvdman ---------------------------------------------------------------------------------------- */

enum { SCECdStatPause = 0x0A, SCECdStatRead = 0x06, SCECdFuncRead = 1, SCECdErNO = 0, SCECdErREAD = 0x30 };

static int cd_busy, cd_error;
static uint32_t cd_callback, cd_callback_gp;
static int64_t cd_done_at;
static unsigned cd_reads, cd_sectors;

int iop_cd_busy(void) { return cd_busy; }

void iop_sif_report(char* buf, size_t cap) {
    size_t n = (size_t)snprintf(buf, cap, "RPC calls served:");
    for (int i = 0; i < nservers && n < cap; i++)
        n += (size_t)snprintf(buf + n, cap - n, " 0x%08X %u,", servers[i].sid, servers[i].calls);
    if (n < cap) snprintf(buf + n, cap - n, " CD reads %u (%u sectors)", cd_reads, cd_sectors);
}

/* sceCdRead(lbn, sectors, buf, sceCdRMode*) */
void iop_imp_cdvdman_sceCdRead(iop_ctx* c) {
    uint32_t lbn = c->r[4], n = c->r[5], buf = c->r[6];
    if (cd_busy) { iop_ret(c, 0); return; }
    uint8_t* dst = iop_ram_ptr(buf);
    int ok = dst && (buf & 0x1FFFFFFFu) + n * 2048u <= 0x00800000u &&
             ((buf & (IOP_RAM_SIZE - 1)) + n * 2048u <= IOP_RAM_SIZE) && cdvd_read_host(lbn, n, dst);
    if (iop_log) fprintf(stderr, "[iop] sceCdRead(lbn %u, %u sectors) -> 0x%08X%s\n", lbn, n, buf, ok ? "" : " FAILED");
    cd_error = ok ? SCECdErNO : SCECdErREAD;
    cd_reads++;
    cd_sectors += n;
    cd_busy = 1;
    cd_done_at = iop_now();            /* completes at the next interrupt delivery point */
    iop_ret(c, 1);
}

void iop_cd_service(int64_t now) {
    if (!cd_busy || now < cd_done_at) return;
    cd_busy = 0;
    for (int t; (t = iop_find_waiter(IOP_WAIT_CD, 0, 1)) != 0;) iop_wake(t, KE_OK);
    if (cd_callback) iop_intr_call(cd_callback, SCECdFuncRead, 0, cd_callback_gp);
}

/* sceCdSync(mode): 0 waits for the command to finish; 1 polls (1 = busy, 0 = done). */
void iop_imp_cdvdman_sceCdSync(iop_ctx* c) {
    if (c->r[4] & 1) { iop_ret(c, cd_busy); return; }
    while (cd_busy) if (iop_block(IOP_WAIT_CD, 0) != KE_OK) break;
    iop_ret(c, 0);
}

void iop_imp_cdvdman_sceCdGetError(iop_ctx* c) { iop_ret(c, cd_error); }
void iop_imp_cdvdman_sceCdStatus(iop_ctx* c) { iop_ret(c, cd_busy ? SCECdStatRead : SCECdStatPause); }
void iop_imp_cdvdman_sceCdBreak(iop_ctx* c) { iop_ret(c, 1); }
void iop_imp_cdvdman_sceCdStop(iop_ctx* c) { iop_ret(c, 1); }

/* sceCdTrayReq(mode, u32* traycheck): the tray never opened. */
void iop_imp_cdvdman_sceCdTrayReq(iop_ctx* c) {
    if (c->r[5]) iop_w32(c->r[5], 0);
    iop_ret(c, 1);
}

/* sceCdSearchFile(sceCdlFILE* fp, const char* name): look a path ("\DIR\FILE.EXT;1") up in the
 * disc's ISO 9660 directories. Fills fp = { u32 lsn, u32 size, char name[16], u8 date[8] } and
 * returns 1, or 0 if there is no such file. (Sly 2 finds its data by sector; 989snd only uses
 * this for loads by file name.) */
static int iso_find(const char* path, uint32_t* lsn, uint32_t* size, uint8_t date[8], char* leaf, size_t leaf_cap) {
    static uint8_t sec[2048];
    if (!cdvd_read_host(16, 1, sec) || sec[0] != 1 || memcmp(sec + 1, "CD001", 5) != 0) return 0;
    uint32_t dir_lsn, dir_size;
    memcpy(&dir_lsn, sec + 156 + 2, 4);
    memcpy(&dir_size, sec + 156 + 10, 4);
    while (*path == '\\' || *path == '/') path++;
    while (*path) {
        size_t n = strcspn(path, "\\/");
        char want[64];
        snprintf(want, sizeof want, "%.*s", (int)n, path);
        if (!strchr(want, ';') && path[n] == 0 && strlen(want) + 2 < sizeof want) strcat(want, ";1");
        int found = 0;
        for (uint32_t off = 0; off < dir_size && !found; off += 2048) {
            if (!cdvd_read_host(dir_lsn + off / 2048, 1, sec)) return 0;
            for (uint32_t p = 0; p + 33 < 2048 && sec[p];) {
                uint8_t len = sec[p], nlen = sec[p + 32];
                if (p + 33 + nlen > 2048) break;
                char name[64];
                snprintf(name, sizeof name, "%.*s", (int)nlen, (const char*)sec + p + 33);
                if (_stricmp(name, want) == 0) {
                    memcpy(&dir_lsn, sec + p + 2, 4);
                    memcpy(&dir_size, sec + p + 10, 4);
                    if (date) memcpy(date, sec + p + 18, 7), date[7] = 0;
                    if (leaf) snprintf(leaf, leaf_cap, "%s", name);
                    found = 1;
                    break;
                }
                p += len;
            }
        }
        if (!found) return 0;
        path += n;
        while (*path == '\\' || *path == '/') path++;
    }
    *lsn = dir_lsn;
    *size = dir_size;
    return 1;
}

void iop_imp_cdvdman_sceCdSearchFile(iop_ctx* c) {
    char name[128], leaf[64] = "";
    uint8_t date[8] = { 0 };
    uint32_t lsn = 0, size = 0;
    iop_strncpy_from(name, c->r[5], sizeof name);
    int ok = iso_find(name, &lsn, &size, date, leaf, sizeof leaf);
    if (ok && c->r[4]) {
        iop_w32(c->r[4], lsn);
        iop_w32(c->r[4] + 4, size);
        for (uint32_t i = 0; i < 16; i++) iop_w8(c->r[4] + 8 + i, i < strlen(leaf) ? (uint8_t)leaf[i] : 0);
        for (uint32_t i = 0; i < 8; i++) iop_w8(c->r[4] + 24 + i, date[i]);
    }
    fprintf(stderr, "[iop] sceCdSearchFile(%s) -> %s (lsn %u, %u bytes)\n", name, ok ? "found" : "not found", lsn, size);
    iop_ret(c, ok);
}

/* sceCdCallback(func): returns the previous callback. */
void iop_imp_cdvdman_sceCdCallback(iop_ctx* c) {
    uint32_t old = cd_callback;
    cd_callback = c->r[4];
    cd_callback_gp = c->r[28];
    iop_ret(c, (int32_t)old);
}
