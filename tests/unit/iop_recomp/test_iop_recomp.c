/* IOP recompiler test: synthetic IRX modules (gen.py) recompiled by recomp/iop.py, loaded by the
 * runtime's loader (relocation + CRC checks, module start in a kernel thread), then each test
 * function is called and its registers and memory are compared with independently computed
 * results. Links the native IOP kernel and libraries; the EE side, the disc and the sound
 * backend are fakes. */
#include "iop_internal.h"
#include "sd.h"
#include "cases.h"

#include <stdlib.h>
#include <windows.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fakes ------------------------------------------------------------------------------------- */

uint8_t* iop_mem(uint32_t a) { a &= 0x1FFFFFFFu; return a < IOP_RAM_SIZE ? iop_ram + a : NULL; }
void sif_rpc_reply(const iop_rpc_req* r, uint32_t a) { (void)r; (void)a; }
void sif_iop_to_ee(uint32_t ee, uint32_t iop, uint32_t size) { (void)ee; (void)iop; (void)size; }
int sif_ee_to_iop(uint32_t iop, uint32_t ee, uint32_t size) { (void)iop; (void)ee; (void)size; return 1; }
uint32_t ps2_vblank_count(void) { return 0; }
int cdvd_read_host(uint32_t lbn, uint32_t count, void* dst) { (void)lbn; memset(dst, 0, count * 2048u); return 1; }

static struct { int calls; int16_t ch; uint16_t mode; uint32_t iop, spu, size; } vt;
int sceSdInit(int flag) { (void)flag; return 0; }
void sceSdSetParam(uint16_t e, uint16_t v) { (void)e; (void)v; }
uint16_t sceSdGetParam(uint16_t e) { (void)e; return 0; }
void sceSdSetSwitch(uint16_t e, uint32_t v) { (void)e; (void)v; }
uint32_t sceSdGetSwitch(uint16_t e) { (void)e; return 0; }
void sceSdSetAddr(uint16_t e, uint32_t v) { (void)e; (void)v; }
uint32_t sceSdGetAddr(uint16_t e) { (void)e; return 0; }
void sceSdSetCoreAttr(uint16_t e, uint16_t v) { (void)e; (void)v; }
uint16_t sceSdNote2Pitch(uint16_t a, uint16_t b, uint16_t c, int16_t d) { (void)a; (void)b; (void)c; (void)d; return 0x1000; }
int sceSdVoiceTrans(int16_t ch, uint16_t mode, uint32_t iop, uint32_t spu, uint32_t size) {
    vt.calls++; vt.ch = ch; vt.mode = mode; vt.iop = iop; vt.spu = spu; vt.size = size;
    return (int)size;
}
uint32_t sceSdVoiceTransStatus(int16_t ch, int16_t f) { (void)ch; (void)f; return 1; }
int sceSdBlockTrans(int16_t ch, uint16_t m, uint32_t a, uint32_t s, uint32_t st) { (void)ch; (void)m; (void)a; (void)s; (void)st; return 0; }
uint32_t sceSdBlockTransStatus(int16_t ch, int16_t f) { (void)ch; (void)f; return 0; }
int sceSdSetEffectAttr(int core, const sd_effect_attr* a) { (void)core; (void)a; return 0; }
int sceSdClearEffectWorkArea(int core, int ch, int m) { (void)core; (void)ch; (void)m; return 0; }
void audio_init(void) {}

/* ---- tests -------------------------------------------------------------------------------------- */

static int32_t load(const unsigned char* irx, size_t size, const char* args, uint32_t args_len) {
    uint32_t buf = iop_alloc((uint32_t)size);
    memcpy(iop_ram + buf, irx, size);
    int32_t result = -12345;
    int native = iop_load_module(buf, args, args_len, &result);
    iop_free(buf);
    return native ? result : -99999;
}

int main(void) {
    ConvertThreadToFiber(NULL);
    iop_init();

    printf("module loading (relocation and CRC checks in runtime/iop_loader.c)\n");
    CHECK(load(test_lib_irx, sizeof test_lib_irx, "", 0) == 0, "test_lib did not load natively");
    CHECK(load(test_mod_irx, sizeof test_mod_irx, "xyz", 4) == 0, "test_mod did not load natively");
    CHECK(iop_r32(START_INFO) == 2, "module start: argc %u, want 2", iop_r32(START_INFO));
    CHECK(iop_r32(START_INFO + 4) == 'x', "module start: argv[1][0] 0x%X, want 'x'", iop_r32(START_INFO + 4));
    /* a modified image must be refused (CRC of the module as shipped) */
    unsigned char* bad = malloc(sizeof test_mod_irx);
    memcpy(bad, test_mod_irx, sizeof test_mod_irx);
    bad[sizeof test_mod_irx / 2] ^= 0x40;
    CHECK(load(bad, sizeof test_mod_irx, "", 0) == -99999, "a modified module was accepted");
    free(bad);

    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const test_case* t = &cases[k];
        iop_ctx c;
        memset(&c, 0, sizeof c);
        c.r[29] = 0x001F0000u;
        for (int i = 0; i < t->nin; i++) c.r[t->in[i][0]] = t->in[i][1];
        c.r[31] = 0xDEAD0000u;
        int before = failures;
        iop_call(&c, t->fn);
        for (int i = 0; i < t->nout; i++)
            CHECK(c.r[t->out[i][0]] == t->out[i][1], "%s: r%u = 0x%08X, want 0x%08X", t->name, t->out[i][0],
                  c.r[t->out[i][0]], t->out[i][1]);
        for (int i = 0; i < t->nmem; i++)
            CHECK(iop_r32(t->mem[i][0]) == t->mem[i][1], "%s: [0x%08X] = 0x%08X, want 0x%08X", t->name, t->mem[i][0],
                  iop_r32(t->mem[i][0]), t->mem[i][1]);
        CHECK(c.r[0] == 0, "%s: $zero = 0x%X", t->name, c.r[0]);
        printf("  %-14s %s\n", t->name, failures == before ? "ok" : "FAILED");
    }
    CHECK(vt.calls == 1 && vt.ch == 1 && vt.mode == 0 && vt.iop == 0x00123456u && vt.spu == 0x5000u && vt.size == 0x800u,
          "sceSdVoiceTrans args: calls %d ch %d mode %u iop 0x%X spu 0x%X size 0x%X", vt.calls, vt.ch, vt.mode, vt.iop,
          vt.spu, vt.size);

    printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
