/* Window, presentation and input. STUB: to be implemented (owner: window agent). */
#include "hw.h"

void window_init(void) {}
void window_present(const uint32_t* bgrx, int w, int h, int stride) { (void)bgrx; (void)w; (void)h; (void)stride; }
void window_get_input(int port, pad_input* out) {
    (void)port;
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = 0x80;
}
