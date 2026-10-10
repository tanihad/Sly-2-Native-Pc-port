/* Linked instead of fake_gs_mem.c when the tests run against the real runtime/gs_mem*.c: the
 * fake's CLUT load counter is not available there (negative = skip that check). */
int fake_clut_loads = -1;
