#pragma once
#include "hw.h"

extern uint8_t  fake_vu_data[2][16384];
extern uint8_t  fake_vu_micro[2][16384];
extern int      fake_vu1_starts;
extern uint32_t fake_vu1_start_addr[64];
extern uint32_t fake_top_at_start[64];
extern int      fake_vu1_busy;
extern uint32_t fake_tops_top[2], fake_tops_itop[2];
extern int      fake_tops_calls[2];
extern uint8_t  fake_gif[1 << 20];
extern uint32_t fake_gif_qwc;
extern int      fake_gif_limit;
extern int      fake_gif_misaligned;
extern int      fake_path3_mask;
extern uint32_t fake_gs_avail, fake_gs_sent;
extern uint32_t fake_intc;
extern int      fake_intc_count[16];
extern int      fake_unhandled;

void fakes_reset(void);
