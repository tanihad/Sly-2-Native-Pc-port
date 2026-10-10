/* Declarations shared between runtime source files (not used by generated code). */
#pragma once
#include "ps2_runtime.h"

#include <stdio.h>

/* Debug output of rendered frames (debug.c): PS2_DUMP_FRAMES=<dir> */
void debug_write_png(const char* path, const uint32_t* bgrx, int w, int h, int stride);
uint32_t ps2_vblank_count(void);           /* VBlanks serviced so far (ps2_runtime.c) */

/* Register numbers */
enum { REG_V0 = 2, REG_V1 = 3, REG_A0 = 4, REG_A1 = 5, REG_A2 = 6, REG_A3 = 7, REG_T0 = 8,
       REG_GP = 28, REG_SP = 29, REG_RA = 31 };

/* Interrupts (ps2_runtime.c runs handlers; hw.c holds INTC/DMAC state) */
int      ps2_in_interrupt(void);
uint32_t intc_pending(void);               /* I_STAT & I_MASK */
void     intc_ack(int cause);
void     intc_set_mask(int cause, int on); /* EnableIntc / DisableIntc */
uint32_t dmac_pending(void);               /* CIS & CIM */
void     dmac_ack(int channel);
void     dmac_set_mask(int channel, int on);   /* EnableDmac / DisableDmac */
int      dmac_mask_enabled(int channel);

/* Threads and semaphores (threads.c) */
void     threads_init(ps2_ctx* main_ctx);
ps2_ctx* thread_ctx(void);                 /* registers of the running thread */
int      thread_syscall(ps2_ctx* ctx, int n, int64_t* ret);
void     thread_reschedule(void);          /* switch if a higher-priority thread is ready */
int      thread_current_id(void);
void     threads_debug_dump(void);         /* PS2_LOG_THREADS: print every thread's state */

/* SIF: EE <-> IOP link and the high-level-emulated IOP (sif.c) */
uint32_t sif_get_reg(uint32_t reg);
void     sif_set_reg(uint32_t reg, uint32_t value);
uint32_t sif_set_dma(ps2_ctx* ctx, uint32_t desc, uint32_t count);
void     sif_deliver(ps2_ctx* ctx);        /* hand queued IOP->EE packets to the game */

extern int ps2_log_sif;                    /* PS2_LOG_SIF=1 */

/* IOP-side RPC servers. Each returns 1 if it handled (sid, fno), writing up to recv_size
 * bytes of reply into `out` (pre-zeroed). */
int cdvd_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
             uint8_t* out, uint32_t recv_size);
int iop_modules_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
                    uint8_t* out, uint32_t recv_size);

int snd_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
            uint8_t* out, uint32_t recv_size);

/* IOP RAM (sif.c): host pointer for an IOP address, or NULL if out of range */
uint8_t* iop_mem(uint32_t iop_addr);
