/* IOP memory card service (mcserv). STUB: to be implemented (owner: mc agent). */
#include "hw.h"

int mc_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
           uint8_t* out, uint32_t recv_size) {
    (void)ctx; (void)sid; (void)fno; (void)args; (void)send_size; (void)out; (void)recv_size;
    return 0;
}
