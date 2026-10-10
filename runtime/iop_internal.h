/* Declarations shared by the native IOP kernel and libraries (iop_kernel.c, iop_libs.c,
 * iop_sif.c, iop_loader.c). Not used by generated code. */
#pragma once
#include "iop.h"

#include <stdio.h>

/* Kernel error codes (Sony's kerr.h values, also in ps2sdk) */
enum {
    KE_OK = 0, KE_ERROR = -1, KE_ILLEGAL_CONTEXT = -100, KE_CPUDI = -102,
    KE_NO_TIMER = -150, KE_ILLEGAL_TIMERID = -151,
    KE_NO_MEMORY = -400, KE_ILLEGAL_ATTR = -401, KE_ILLEGAL_ENTRY = -402, KE_ILLEGAL_PRIORITY = -403,
    KE_ILLEGAL_STACK_SIZE = -404, KE_ILLEGAL_MODE = -405, KE_ILLEGAL_THID = -406,
    KE_UNKNOWN_THID = -407, KE_UNKNOWN_SEMID = -408, KE_UNKNOWN_EVFID = -409,
    KE_DORMANT = -413, KE_NOT_DORMANT = -414, KE_NOT_WAIT = -416, KE_RELEASE_WAIT = -418,
    KE_SEMA_ZERO = -419, KE_SEMA_OVF = -420, KE_EVF_COND = -421, KE_EVF_MULTI = -422,
    KE_EVF_ILPAT = -423, KE_WAIT_DELETE = -425,
};

/* Arguments of a call from guest code (o32: a0-a3, then the caller's stack at sp+16...). */
static inline uint32_t iop_arg(const iop_ctx* c, int n) {
    return n < 4 ? c->r[4 + n] : iop_r32(c->r[29] + 4u * (uint32_t)n);
}
static inline void iop_ret(iop_ctx* c, int32_t v) { c->r[2] = (uint32_t)v; }

/* Guest strings: copy at most cap-1 bytes of a NUL-terminated IOP string. */
void iop_strncpy_from(char* dst, uint32_t src, size_t cap);

/* ---- threads (iop_kernel.c) ------------------------------------------------------------------- */

enum { IOP_WAIT_SEMA = 1, IOP_WAIT_EVENT, IOP_WAIT_SLEEP, IOP_WAIT_DELAY, IOP_WAIT_RPC, IOP_WAIT_CD };

int      iop_thread_current(void);                 /* running thread id; 0 outside thread context */
int      iop_thread_priority(int tid);
/* Block the running thread until iop_wake(); returns the wake code (KE_OK, KE_WAIT_DELETE...).
 * Returns KE_ILLEGAL_CONTEXT without blocking in interrupt context or outside a thread. */
int      iop_block(int wait_type, uint32_t wait_obj);
void     iop_wake(int tid, int code);
/* Highest-priority thread waiting on (type, obj), or the longest-waiting one if fifo; 0 if none. */
int      iop_find_waiter(int wait_type, uint32_t wait_obj, int fifo);
uint32_t iop_waiter_data(int tid, int k);          /* per-wait scratch values (event flag mode...) */
void     iop_set_waiter_data(int tid, int k, uint32_t v);
void     iop_check_preempt(void);                  /* switch if a higher-priority thread is ready */
int      iop_create_kernel_thread(uint32_t entry, int prio, uint32_t stack_size, uint32_t a0, uint32_t a1,
                                  uint32_t gp);   /* created and started; returns tid */
int      iop_thread_status_dormant(int tid, uint32_t* exit_value);
void     iop_delete_kernel_thread(int tid);

/* Call guest code from native code running in thread context; preserves $ra/$sp. */
uint32_t iop_guest_call(iop_ctx* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
/* Run a guest handler in interrupt context (on the interrupt stack); returns its $v0. */
uint32_t iop_intr_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t gp);

/* Recompiled modules registered by the generated code (iop_kernel.c) */
const iop_module_desc* iop_module_by_name(const char* name);
void     iop_dump_trace(const char* why);

/* Time: host performance counter */
int64_t  iop_now(void);
int64_t  iop_usec_to_ticks(uint64_t usec);

/* Interrupt sources polled by the kernel (each runs due handlers via iop_intr_call). */
void     iop_cd_service(int64_t now);              /* iop_sif.c */
void     iop_sif_service(void);                    /* iop_sif.c */
void     iop_sd_service(void);                     /* iop_libs.c */
int      iop_cd_busy(void);
