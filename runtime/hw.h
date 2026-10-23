/* Interfaces between the emulated PS2 hardware subsystems.
 *
 * FROZEN INTERFACE: subsystem implementers must not change these declarations. If an interface
 * genuinely needs to change, say so in your report instead of editing this file.
 *
 * Conventions
 *  - All functions run on the emulation thread (the game's fibers). No locking is needed except
 *    in window.c, which owns a separate OS thread.
 *  - `phys` addresses have the kseg/uncached bits stripped (addr & 0x1FFFFFFF).
 *  - Register/FIFO access handlers get a host buffer `data` of `size` bytes (1, 2, 4, 8 or 16),
 *    little-endian. For reads, fill `data`; for writes, consume it.
 *  - Everything is synchronous: a DMA transfer, VIF command, VU microprogram or GS primitive runs
 *    to completion inside the call that starts it, unless the hardware would genuinely stall
 *    (then return how much was consumed and the DMAC retries later via dma_service()).
 *  - Interrupts are never delivered from inside these functions. Call intc_raise()/dmac_raise();
 *    the game's handlers run at the next safe point (loop head or syscall return).
 *  - Hardware reference: ps2tek (https://psi-rockin.github.io/ps2tek/), Sony's EE/GS/VU user
 *    manuals. Implement from the documented behaviour; do not copy code from GPL emulators.
 */
#pragma once
#include "ps2_internal.h"

typedef void (*hw_access_fn)(uint32_t phys, void* data, unsigned size, int is_write);

/* ---- hw.c: address dispatch, interrupt controller, debug UART ---------------------------------- */

/* Log an access nobody handles yet; prints only the first access to each address. */
void hw_log_unhandled(const char* unit, uint32_t phys, const void* data, unsigned size, int is_write);

/* INTC causes */
enum { INTC_GS = 0, INTC_SBUS = 1, INTC_VBLANK_S = 2, INTC_VBLANK_E = 3, INTC_VIF0 = 4, INTC_VIF1 = 5,
       INTC_VU0 = 6, INTC_VU1 = 7, INTC_IPU = 8, INTC_TIM0 = 9, INTC_TIM1 = 10, INTC_TIM2 = 11,
       INTC_TIM3 = 12, INTC_SFIFO = 13, INTC_VU0WD = 14 };
/* DMAC channels */
enum { DMA_VIF0 = 0, DMA_VIF1 = 1, DMA_GIF = 2, DMA_IPU_FROM = 3, DMA_IPU_TO = 4, DMA_SIF0 = 5,
       DMA_SIF1 = 6, DMA_SIF2 = 7, DMA_SPR_FROM = 8, DMA_SPR_TO = 9 };

void intc_raise(int cause);              /* I_STAT |= 1 << cause */
void dmac_raise(int channel);            /* D_STAT.CIS |= 1 << channel */
/* D_STAT (0x1000E010) storage lives in hw.c so the interrupt logic has one source of truth.
 * dma.c forwards reads/writes of D_STAT here: writing 1 clears a CIS/SIS/MEIS/BEIS bit and
 * toggles a CIM/SIM/MEIM mask bit, as on hardware. */
uint32_t dmac_stat_read(void);
void     dmac_stat_write(uint32_t v);

/* ---- EE memory as seen by DMA -------------------------------------------------------------------- */

/* Host pointer for a DMA memory address (MADR/TADR/SADR style: bit 31 set = scratchpad), or NULL
 * if the range [addr, addr+bytes) is not entirely in RAM or scratchpad. */
uint8_t* hw_dma_ptr(uint32_t addr, uint32_t bytes);

/* ---- timer.c: EE timers T0-T3 (0x10000000-0x10001FFF) ------------------------------------------- */

void timer_access(uint32_t phys, void* data, unsigned size, int is_write);
void timer_service(void);                /* called often (>= 1 kHz): raise INTC_TIM* for due events */
void timer_hold(int on);                 /* debug pause: EE timer time stands still while on */
uint32_t timer_cop0_count(void);         /* COP0 Count (294.912 MHz) on the timers' held time base */

/* ---- dma.c: DMA controller (0x10008000-0x1000EFFF, 0x1000F500-0x1000F5FF) ------------------------ */

void dma_access(uint32_t phys, void* data, unsigned size, int is_write);
void dma_service(void);                  /* retry channels that stalled on a busy/empty device */

/* ---- gif.c: GIF (0x10003000-0x100037FF regs, 0x10006000 FIFO) ----------------------------------- */

void     gif_access(uint32_t phys, void* data, unsigned size, int is_write);
uint32_t gif_path3_dma(const void* qw, uint32_t qwc);   /* DMA ch2; returns qwords consumed (0 while PATH3 is masked) */
uint32_t gif_path2(const void* qw, uint32_t qwc);       /* VIF1 DIRECT/DIRECTHL */
void     gif_path1_kick(const uint8_t* vu1_data, uint32_t qw_addr);   /* VU1 XGKICK; packet wraps at 16 KB */
void     gif_set_path3_mask(int masked);                /* VIF1 MSKPATH3 */

/* ---- vif.c: VIF0/VIF1 (0x10003800-0x10003FFF regs, 0x10004000/0x10005000 FIFOs) ------------------ */

void     vif_access(uint32_t phys, void* data, unsigned size, int is_write);
uint32_t vif_dma(int vif, const void* qw, uint32_t qwc);   /* DMA ch0/ch1 data; returns qwords consumed */
int      vif_dma_tag(int vif, const void* tag_qw);         /* DMA tag with CHCR.TTE: upper 64 bits are VIF data; 1 = consumed, 0 = stalled */
uint32_t vif1_read(void* qw, uint32_t qwc);                /* VIF1 FIFO towards memory (GS downloads); returns qwords produced */

/* ---- vu.c: VU0/VU1 micro mode and VU memory (0x11000000-0x1100FFFF) ------------------------------ */

void     vu_mem_access(uint32_t phys, void* data, unsigned size, int is_write);
uint8_t* vu_data_mem(int vu);            /* VU0: 4 KB, VU1: 16 KB */
uint8_t* vu_micro_mem(int vu);           /* VU0: 4 KB, VU1: 16 KB */
void     vu1_start(uint32_t addr_dw);    /* run a VU1 microprogram from instruction address (64-bit units) until its E bit */
int      vu1_busy(void);
uint32_t vu_get_tpc(int vu);             /* where the last microprogram stopped (MSCNT continues there) */
void     vu_set_vif_tops(int vu, uint32_t top, uint32_t itop);  /* VIF TOP/ITOP values for XTOP/XITOP */
/* VU0 from EE code (macro mode registers live in ps2_ctx: vf, vacc, vi, vq, vi_reg, vr) */
void     vu0_callms(ps2_ctx* ctx, uint32_t addr_dw);  /* VCALLMS / VCALLMSR */
uint32_t vu0_cfc2_ctrl(ps2_ctx* ctx, int reg);        /* CFC2 for control registers 16-31 */
void     vu0_ctc2_ctrl(ps2_ctx* ctx, int reg, uint32_t v);

/* ---- ipu.c: IPU MPEG decoder (0x10002000-0x10002FFF regs, 0x10007000 FIFOs) ---------------------- */

void     ipu_access(uint32_t phys, void* data, unsigned size, int is_write);
uint32_t ipu_dma_to(const void* qw, uint32_t qwc);     /* DMA ch4 into the input FIFO; returns qwords consumed */
uint32_t ipu_dma_from(void* qw, uint32_t qwc);         /* DMA ch3 out of the output FIFO; returns qwords produced */

/* ---- gs.c / gs_draw.c / gs_mem.c: Graphics Synthesizer ------------------------------------------ */

void     gs_priv_access(uint32_t phys, void* data, unsigned size, int is_write);   /* 0x12000000-0x12001FFF */
void     gs_write_reg(uint32_t reg, uint64_t value);       /* general register 0x00-0x63 (A+D / REGLIST / packed after conversion) */
void     gs_image_write(const void* data, uint32_t bytes); /* IMAGE-mode data for the current host->local transfer */
uint32_t gs_image_read(void* data, uint32_t bytes);        /* local->host transfer data; returns bytes produced */
void     gs_vsync(void);                                   /* start of vertical blank: CSR events, field flip */
/* Compose the currently displayed picture (PMODE/DISPFB/DISPLAY, both circuits) as 32-bit BGRX
 * into dst (stride in pixels, at least 1024 wide x 1024 tall available). Returns 0 if the display
 * is off. *w and *h receive the output size in pixels. */
int      gs_display(uint32_t* dst, int stride, int* w, int* h);

/* ---- gs_thread.c: the GS on its own host thread ---------------------------------------------------
 * After gs_thread_start(), gs_write_reg/gs_image_write only queue (in order) and a host thread
 * executes the stream, sharing large primitives with `pixel_workers` more threads. The picture of
 * each VBlank is composed there too: gs_queue_vsync() marks the VBlank in the stream, and when the
 * GS reaches it `present` is called (on the GS thread) with the picture a synchronous GS would have
 * shown at that VBlank (ok = 0: display off). The EE waits only where it observes the GS (local->host
 * transfers, CSR.RESET) and at a VBlank while the previous picture is still pending. Without
 * gs_thread_start() everything stays synchronous (unit tests). */
typedef void (*gs_present_fn)(const uint32_t* frame, int ok, int w, int h, uint32_t vblank);
void     gs_thread_start(int pixel_workers, gs_present_fn present);
int      gs_async(void);                 /* the GS thread is running */
void     gs_queue_vsync(uint32_t vblank);
/* (pc-pacing) when the next queued VBlank was due (QPC ticks, the host timer's schedule): set by the EE before
 * gs_queue_vsync, carried in the stream with it; gs_vsync_qpc() is that time for the VBlank whose picture the GS thread
 * is handing to the vblank hook (0: unknown) - the paced present shows the picture at this + the present delay. */
void     gs_set_vsync_time(int64_t qpc);
int64_t  gs_vsync_qpc(void);
/* Called on the EE thread about every millisecond while it waits for the GS thread (the runtime
 * keeps the IOP running there; NULL: none). */
void     gs_set_wait_hook(void (*fn)(void));

/* ---- window.c: presentation + input (own OS thread) --------------------------------------------- */

/* Controller state, active-high bits in DualShock 2 order. */
enum { PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3, PAD_UP = 1 << 4,
       PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7, PAD_L2 = 1 << 8, PAD_R2 = 1 << 9,
       PAD_L1 = 1 << 10, PAD_R1 = 1 << 11, PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13,
       PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15 };
typedef struct {
    uint16_t buttons;                    /* PAD_* bits, 1 = pressed */
    uint8_t  lx, ly, rx, ry;             /* analog sticks, 0x80 = centre */
    uint8_t  pressure[12];               /* right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 */
} pad_input;

void window_init(void);                  /* create the window and its thread; returns immediately */
void window_present(const uint32_t* bgrx, int w, int h, int stride);   /* copy a finished frame; never blocks for long */
void window_get_input(int port, pad_input* out);   /* port 0/1; zeroed buttons + centred sticks if absent */
/* input.c: the DualShock 2 motors of a port (small on/off, large speed), every VBlank -> PC controller rumble */
void input_set_rumble(int port, uint8_t small_on, uint8_t large_speed);

/* ---- pad.c, mc.c: IOP controller and memory card services --------------------------------------- */

int  pad_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
             uint8_t* out, uint32_t recv_size);
void pad_vsync(ps2_ctx* ctx);            /* once per VBlank: push controller state to the game */
int  mc_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
            uint8_t* out, uint32_t recv_size);
