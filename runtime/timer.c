/* EE timers T0-T3. STUB: to be implemented (owner: timer agent). */
#include "hw.h"

void timer_access(uint32_t phys, void* data, unsigned size, int is_write) {
    if (!is_write) memset(data, 0, size);
    hw_log_unhandled("timer", phys, data, size, is_write);
}

void timer_service(void) {}
