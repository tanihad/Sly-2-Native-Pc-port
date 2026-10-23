/* Weak stand-ins for menu_ext.h until pc-pacing provides it (its strong definitions replace these at link time):
 * a pacing change applies at the next start, exclusive fullscreen falls back to borderless. */
#include "menu_ext.h"

#define WEAK __attribute__((weak))

WEAK int pacing_apply_mode(int pacing) { (void)pacing; return 0; }
WEAK int pacing_exclusive_fullscreen(int on) { (void)on; return 0; }
