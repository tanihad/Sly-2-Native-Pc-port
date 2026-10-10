/* IOP controller service (padman). STUB: to be implemented (owner: pad agent). */
#include "hw.h"

int pad_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
            uint8_t* out, uint32_t recv_size) {
    (void)ctx; (void)sid; (void)fno; (void)args; (void)send_size; (void)out; (void)recv_size;
    return 0;
}

void pad_vsync(ps2_ctx* ctx) { (void)ctx; }
