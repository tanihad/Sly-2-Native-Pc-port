/* Command chunks and data arena from the GS side (producer) to the render thread (consumer).
 *
 * Two single-producer/single-consumer byte rings with free-running 64-bit positions, the scheme of
 * gs_thread.c: commands (16-byte header + payload, 16-byte aligned, never wrapping: a SKIP command
 * pads to the end of the ring) and bulk data (pictures, later vertices/records/uploads). The
 * producer publishes in chunks of GPU_CMD_CHUNK bytes and on explicit flushes; both sides spin
 * briefly, then block in WaitOnAddress. A command's data is allocated before the command, and the
 * consumer frees data up to the command's data_end once the command has executed. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define GPU_CMD_RING   (4u << 20)         /* command ring bytes */
#define GPU_CMD_CHUNK  (64u << 10)        /* publish at least every this many bytes */
#define GPU_DATA_RING  (64u << 20)        /* data arena bytes */

enum {
    GPU_OP_SKIP = 0,                      /* padding to the end of the ring */
    GPU_OP_NOP,
    GPU_OP_CALL,                          /* { fn, arg, done* } run a function on the render thread */
    GPU_OP_FENCE,                         /* { id } render thread reached this point */
    GPU_OP_PICTURE,                       /* gpu_op_picture */
    GPU_OP_PRESENT,                       /* { vblank } */
    GPU_OP_READBACK,                      /* { gpu_readback* } */
    GPU_OP_STOP,                          /* end the render thread */
    GPU_OP_COUNT
};

typedef struct {
    uint32_t op, size;                    /* size: whole command in bytes, multiple of 16 */
    uint64_t data_end;                    /* data ring position freed once this command ran */
} gpu_cmd;

int   gpu_cmd_init(void);                 /* allocate the rings (idempotent); 0 on failure */
void  gpu_cmd_free(void);
void  gpu_cmd_reset(void);                /* both sides idle: empty the rings */

/* ---- producer ---- */
void* gpu_data_alloc(size_t bytes);       /* contiguous, 16-byte aligned; for the next command */
void* gpu_cmd_alloc(uint32_t op, uint32_t payload_bytes);   /* returns the payload */
void  gpu_cmd_commit(void);               /* the command allocated last is complete */
void  gpu_cmd_publish(void);              /* make everything committed visible now */
uint64_t gpu_cmd_written(void);           /* bytes of commands committed so far (for tests) */
void  gpu_cmd_debug(char* buf, size_t n); /* ring positions, for hang diagnostics */

/* ---- consumer ---- */
/* Next command, or NULL when none arrives within wait_ms (0: poll, < 0: wait forever). */
const gpu_cmd* gpu_cmd_next(int wait_ms);
void  gpu_cmd_done(const gpu_cmd* c);     /* the command (and its data) can be reused */

/* counters for tests: times the producer waited for space, chunks published */
typedef struct { uint64_t cmd_waits, data_waits, publishes, commands; } gpu_cmd_stats;
extern gpu_cmd_stats gpu_cmd_counters;
