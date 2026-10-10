/* IOP controller service: high-level emulation of Sony's padman.irx and of a DualShock 2.
 *
 * The game links Sony's EE libpad 4.0 and ships padman.irx 4.16 (0x0410) in its executable. The
 * protocol below was read out of both binaries; notes/pad-protocol.md documents it in full.
 *
 *  - libpad calls RPC server 0x80000100 with a 128-byte buffer. Word 0 is a command code; padman
 *    answers in the same buffer (the reply is the request with result words filled in).
 *  - For every open port/slot, padman keeps a 128-byte record whose layout libpad knows, and once
 *    per frame DMAs it to the EE buffer given to scePadPortOpen: two copies, alternating, each
 *    carrying a frame counter; libpad reads whichever copy has the larger counter.
 *  - scePadSetActDirect does not use the RPC: libpad DMAs a 32-byte block (counter, "new" flag,
 *    length 6, 6 motor bytes) into IOP memory whose address padman returned from the open call.
 *  - padman runs a small state machine per slot (polling, detection, command execution), one
 *    controller transaction per frame. Mode changes go through the pad's configuration mode and
 *    are followed by a full re-detection, so a request completes some 15-30 frames later.
 *
 * The emulated controller is a DualShock 2 in port 0, slot 0 (no multitap); port 1 is empty.
 * Input comes from window_get_input(). Everything runs synchronously from pad_rpc() and from
 * pad_vsync(), which advances padman by one frame and writes the EE buffers.
 */
#include "hw.h"

#include <stdlib.h>

#define PAD_SID         0x80000100u    /* padman command server */
#define PAD_EXT_SID      0x80000101u    /* "extended service": not supported by padman 4.16 either */
#define PAD_MODULE_VER   0x0410u        /* padman.irx module version; libpad requires major 4 */
#define PAD_PORTS        2
#define PAD_SLOTS        4
/* IOP RAM where the EE's actuator blocks land (2 x 0x20 bytes per slot). Real padman uses its own
 * .bss; this HLE keeps nothing else below the SIF command buffer at 0x1F000. */
#define PAD_ACT_IOP_BASE 0x0001E000u
#define PAD_ACT_IOP_SIZE 0x40u

/* libpad/padman constants */
enum { STATE_DISCONN = 0, STATE_FINDPAD = 1, STATE_FINDCTP1 = 2, STATE_EXECCMD = 5, STATE_STABLE = 6,
       STATE_ERROR = 7 };
enum { RSTAT_COMPLETE = 0, RSTAT_FAILED = 1, RSTAT_BUSY = 2 };
/* padman's per-slot task (mirrored at EE offset 0x72; libpad treats 1 as "information valid") */
enum { TASK_NONE = 0, TASK_POLL = 1, TASK_DETECT = 2, TASK_CLOSE = 3, TASK_SETMODE = 4, TASK_ACTALIGN = 5,
       TASK_BTNINFO = 6, TASK_VREF = 7 };
/* RPC command codes (word 0 of the buffer) */
enum { CMD_OPEN = 0x01, CMD_INFO_ACT = 0x03, CMD_INFO_COMB = 0x04, CMD_INFO_MODE = 0x05,
       CMD_SET_MMODE = 0x06, CMD_SET_ACTDIR = 0x07, CMD_SET_ACTALIGN = 0x08, CMD_GET_BTNMASK = 0x09,
       CMD_SET_BTNINFO = 0x0A, CMD_SET_VREF = 0x0B, CMD_GET_PORTMAX = 0x0C, CMD_GET_SLOTMAX = 0x0D,
       CMD_CLOSE = 0x0E, CMD_END = 0x0F, CMD_INIT = 0x10, CMD_GET_MODVER = 0x12, CMD_NOP = 0x13,
       CMD_SET_DEBUG = 0x14, CMD_SET_NOTIFY = 0x18 };

/* ---- the DualShock 2 ---------------------------------------------------------------------------
 * What padman learns from it in configuration mode (commands 0x45/0x46/0x47/0x4C/0x41), as the
 * controller reports it. */
#define DS2_MODEL   3                   /* 0x45 byte 0: DualShock 2 (a DualShock reports 1) */
#define DS2_NMODES  2
#define DS2_NACT    2
#define DS2_NCOMB   1
static const uint16_t ds2_mode_table[DS2_NMODES] = { 0x0004, 0x0007 };    /* digital, analog */
static const uint8_t ds2_act_info[DS2_NACT][4] = {     /* function, sub-function, size, current */
    { 0x01, 0x02, 0x00, 0x0A },                        /* small motor: on/off, 10 units */
    { 0x01, 0x01, 0x01, 0x14 },                        /* large motor: 8-bit speed, 20 units */
};
static const uint8_t ds2_comb_info[DS2_NCOMB][4] = { { 0x02, 0x00, 0x01, 0x00 } };
#define DS2_FULL_MASK 0x0003FFFFu       /* 0x41 reply in analog mode: buttons, sticks, 12 pressures */
#define PAD_MAX_CURRENT 60              /* padman refuses motor requests beyond this total */
/* Entering configuration mode arms a watchdog in the controller: after about a second without
 * communication it resets to digital mode with the motors off and unmapped (psx-spx, 43h). */
#define DS2_WATCHDOG_FRAMES 60

typedef struct {
    int      present;
    int      analog;                    /* main mode: 0 digital (0x41), 1 analog (0x73/0x79) */
    int      locked;                    /* mode lock: the ANALOG button is ignored */
    uint32_t resp_mask;                 /* 0x4F: which of the 18 analog bytes the poll returns */
    uint8_t  align[6];                  /* 0x4D: motor driven by each poll byte (0xFF = none) */
    uint8_t  motor[2];                  /* small (on/off), large (speed) */
    uint8_t  vref[12];                  /* 0x40: pressure sensor parameters */
    int      armed;                     /* watchdog running (configuration mode was entered) */
    int      idle;                      /* frames without communication */
} ds2_state;

/* Once per frame: `talked` says whether padman exchanged a transaction with the controller. */
static void ds2_watchdog(ds2_state* d, int talked) {
    if (talked) { d->idle = 0; return; }
    if (!d->armed || ++d->idle < DS2_WATCHDOG_FRAMES) return;
    d->analog = 0;
    d->locked = 0;
    d->resp_mask = 0x3F;
    memset(d->align, 0xFF, sizeof d->align);
    d->motor[0] = d->motor[1] = 0;
    d->armed = 0;
    d->idle = 0;
}

static uint8_t ds2_id(const ds2_state* d) {
    if (!d->analog) return 0x41;
    return (d->resp_mask & ~0x3Fu) ? 0x79 : 0x73;
}

/* Pressure byte order in the poll reply, and the button each one belongs to. */
static const uint16_t pressure_button[12] = {
    PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE, PAD_CROSS, PAD_SQUARE,
    PAD_L1, PAD_R1, PAD_L2, PAD_R2,
};

/* Reply to a poll (command 0x42): 0xFF, ID, 0x5A, then 2/6/18 data bytes. The bytes padman sent
 * along (tx[3..8], the motor values) drive the motors through the alignment table (0x4D): a motor
 * no byte is mapped to gets no drive value and stops, and the small motor is on/off and only
 * looks at bit 0 of its byte (psx-spx, commands 42h/4Dh). */
static void ds2_poll(ds2_state* d, const pad_input* in, const uint8_t tx[6], uint8_t rx[32]) {
    uint8_t motor[2] = { 0, 0 };
    for (int i = 0; i < 6; i++)
        if (d->align[i] < 2) motor[d->align[i]] = tx[i];
    d->motor[0] = motor[0] & 1;
    d->motor[1] = motor[1];
    memset(rx, 0, 32);
    uint8_t id = ds2_id(d);
    uint16_t b = in->buttons;
    if (!d->analog) b &= (uint16_t)~(PAD_L3 | PAD_R3);       /* no stick buttons in digital mode */
    rx[0] = 0xFF;
    rx[1] = id;
    rx[2] = 0x5A;
    rx[3] = (uint8_t)~b;                                      /* active low */
    rx[4] = (uint8_t)~(b >> 8);
    if (id == 0x41) return;
    rx[5] = in->rx; rx[6] = in->ry; rx[7] = in->lx; rx[8] = in->ly;
    if (id != 0x79) return;
    for (int i = 0; i < 12; i++) {
        uint8_t p = 0;
        if (d->resp_mask & (1u << (6 + i))) {
            p = in->pressure[i];
            if (!p && (in->buttons & pressure_button[i])) p = 0xFF;   /* digital-only input: full press */
        }
        rx[9 + i] = p;
    }
}

/* ---- padman ------------------------------------------------------------------------------------ */

typedef struct {
    int      open;
    uint32_t ee_addr;                   /* the two 128-byte buffers given to scePadPortOpen */
    uint32_t frame;                     /* next frame number (EE +0x58) */
    uint32_t err_count;                 /* failed polls since detection (EE +0x5C) */
    uint8_t  data[32];                  /* formatted controller data (EE +0x00), kept while invalid */
    uint8_t  mode_config;               /* 0 unknown, 1 no configuration mode, 2 configurable (EE +0x64) */
    uint8_t  cur_id;                    /* controller ID padman expects (EE +0x65) */
    uint8_t  model;                     /* EE +0x66 */
    uint8_t  data_valid;                /* EE +0x67 */
    uint8_t  n_modes, cur_offs, n_act, n_comb;              /* EE +0x68..0x6B */
    uint8_t  press_set;                 /* button info was set (EE +0x6C) */
    uint8_t  req_mode, req_lock;        /* EE +0x6D, +0x6E */
    uint8_t  act_direct_len;            /* EE +0x6F */
    uint8_t  state, req_state, task, pending;               /* EE +0x70..0x73 */
    uint8_t  act_direct[6];             /* EE +0x20 */
    uint8_t  act_align[6];              /* EE +0x28 */
    uint8_t  act_data[16];              /* 4 x (function, sub, size, current) (EE +0x30) */
    uint8_t  comb_data[16];             /* EE +0x40 */
    uint16_t mode_table[4];             /* EE +0x50 */
    uint8_t  btn_info[4];               /* response mask requested with 0x4F (EE +0x75) */
    uint8_t  btn_mask[4];               /* response mask the pad reported with 0x41 (EE +0x79) */
    uint8_t  vref[12];
    uint8_t  first;                     /* first detection since open: force digital mode */
    uint8_t  rx[32];                    /* last good poll reply */
    int      phase, steps;              /* progress of the running task */
    int      poll_fail;
} pad_slot;

static int        initialized;
static int        notify;               /* command 0x18: per-frame SIF notification (not emulated) */
static pad_slot   slots[PAD_PORTS][PAD_SLOTS];
static ds2_state  pads[PAD_PORTS];
static int        pads_ready;

static const char* const cmd_names[0x19] = {
    [CMD_OPEN] = "PortOpen", [CMD_INFO_ACT] = "InfoAct", [CMD_INFO_COMB] = "InfoComb",
    [CMD_INFO_MODE] = "InfoMode", [CMD_SET_MMODE] = "SetMainMode", [CMD_SET_ACTDIR] = "SetActDirect",
    [CMD_SET_ACTALIGN] = "SetActAlign", [CMD_GET_BTNMASK] = "GetButtonMask",
    [CMD_SET_BTNINFO] = "SetButtonInfo", [CMD_SET_VREF] = "SetVrefParam", [CMD_GET_PORTMAX] = "GetPortMax",
    [CMD_GET_SLOTMAX] = "GetSlotMax", [CMD_CLOSE] = "PortClose", [CMD_END] = "End", [CMD_INIT] = "Init",
    [CMD_GET_MODVER] = "GetModVersion", [CMD_NOP] = "Nop", [CMD_SET_DEBUG] = "SetDebug",
    [CMD_SET_NOTIFY] = "SetNotify",
};

static void pads_reset(void) {
    memset(pads, 0, sizeof pads);
    pads[0].present = 1;                /* DualShock 2 in port 0; port 1 empty */
    for (int p = 0; p < PAD_PORTS; p++) {
        pads[p].resp_mask = 0x3F;
        memset(pads[p].align, 0xFF, sizeof pads[p].align);
    }
    pads_ready = 1;
}

static ds2_state* pad_at(int port, int slot) {
    if (!pads_ready) pads_reset();
    return (slot == 0 && pads[port].present) ? &pads[port] : NULL;   /* slots 1-3 need a multitap */
}

static pad_slot* slot_at(uint32_t port, uint32_t slot) {
    return (port < PAD_PORTS && slot < PAD_SLOTS) ? &slots[port][slot] : NULL;
}

/* -- tasks -- */

/* Detection (re)starts: padman forgets everything it knew about the controller. The state is
 * left to the caller: the detection thread itself only sets it after its first transaction. */
static void detect_start(pad_slot* s) {
    s->task = TASK_DETECT;
    s->data_valid = 0;
    s->cur_id = s->mode_config = s->model = 0;
    s->n_modes = s->n_act = s->n_comb = 0;
    s->act_direct_len = 0;
    s->err_count = 0;
    s->poll_fail = 0;
    memset(s->btn_info, 0, sizeof s->btn_info);
    memset(s->btn_mask, 0, sizeof s->btn_mask);
    memset(s->act_direct, 0, sizeof s->act_direct);
    static const uint8_t default_align[6] = { 0, 1, 0xFF, 0xFF, 0xFF, 0xFF };
    memcpy(s->act_align, default_align, sizeof default_align);
    s->phase = 0;                       /* 0: looking for a controller, 1: configuration transactions */
    s->steps = 0;
}

/* One detection transaction per frame: find the pad, enter configuration mode, query model,
 * actuators, combinations and modes, send the actuator alignment, query the button mask, leave
 * configuration mode, read the ID. The information is published when the sequence completes. */
static void detect_step(pad_slot* s, ds2_state* d) {
    if (s->phase == 0) {
        if (!d) { s->state = STATE_DISCONN; s->first = 1; return; }
        s->state = STATE_EXECCMD;
        if (s->first) memset(s->act_align, 0xFF, sizeof s->act_align);
        s->phase = 1;                   /* next: enter configuration mode */
        d->armed = 1;
        if (s->first && d->analog)
            s->steps = 3;               /* enter config, query model, set digital mode */
        else
            s->steps = 2 + DS2_NACT + DS2_NCOMB + DS2_NMODES + 1 + ((DS2_MODEL & 2) ? 1 : 0) + 1 + 1;
        return;
    }
    if (--s->steps > 0) return;
    s->task = TASK_POLL;
    if (s->first && d->analog) {
        /* A newly connected controller is put into digital mode, unlocked; padman then sees an
         * unexpected ID on the next poll and detects again. */
        d->analog = 0;
        d->locked = 0;
        d->resp_mask = 0x3F;
        s->first = 0;
        s->cur_id = 0;
        return;
    }
    s->mode_config = 2;
    s->model = DS2_MODEL;
    s->n_modes = DS2_NMODES;
    s->cur_offs = (uint8_t)d->analog;
    s->n_act = DS2_NACT;
    s->n_comb = DS2_NCOMB;
    memset(s->act_data, 0, sizeof s->act_data);
    memset(s->comb_data, 0, sizeof s->comb_data);
    memcpy(s->act_data, ds2_act_info, sizeof ds2_act_info);
    memcpy(s->comb_data, ds2_comb_info, sizeof ds2_comb_info);
    memset(s->mode_table, 0, sizeof s->mode_table);
    for (int i = 0; i < DS2_NMODES; i++) s->mode_table[i] = ds2_mode_table[i];
    memcpy(d->align, s->act_align, sizeof d->align);       /* 0x4D during detection */
    uint32_t mask = d->analog ? DS2_FULL_MASK : 0;         /* 0x41 answers zeros in digital mode */
    for (int i = 0; i < 4; i++) s->btn_mask[i] = (uint8_t)(mask >> (8 * i));
    s->cur_id = ds2_id(d);
    s->first = 0;
    s->state = STATE_EXECCMD;
}

static void log_mode(int port, const ds2_state* d) {
    static int n;
    if (n < 16 && ++n)
        fprintf(stderr, "[pad] port %d: controller now %s (ID 0x%02X)%s\n", port,
                !d->analog ? "digital" : ds2_id(d) == 0x79 ? "analog with pressure" : "analog", ds2_id(d),
                d->locked ? ", mode locked" : "");
}

/* Commands run inside the controller's configuration mode: enter, the command itself (one
 * transaction, or 12 for the pressure parameters), leave. */
static void command_start(pad_slot* s) {
    s->state = STATE_EXECCMD;
    s->data_valid = 0;
    s->phase = 0;
    switch (s->task) {
    case TASK_SETMODE:  s->steps = 3; break;
    case TASK_ACTALIGN: s->steps = 3; break;
    case TASK_BTNINFO:  s->steps = 1 + 1 + 12 + 1; break;
    case TASK_VREF:     s->steps = 1 + 12 + 1; break;
    default:            s->steps = 1; break;
    }
}

static void command_step(pad_slot* s, ds2_state* d, int port) {
    s->phase++;
    if (s->phase == 1 && d) d->armed = 1;   /* enter configuration mode */
    if (s->phase == 2 && d) {           /* the transaction after "enter configuration mode" */
        switch (s->task) {
        case TASK_SETMODE:
            d->analog = s->req_mode == 1;
            d->locked = s->req_lock == 3;
            d->resp_mask = 0x3F;        /* a new main mode starts without pressure bytes */
            log_mode(port, d);
            break;
        case TASK_ACTALIGN:
            memcpy(d->align, s->act_align, sizeof d->align);
            break;
        case TASK_BTNINFO:
            d->resp_mask = ((uint32_t)s->btn_info[0] | (uint32_t)s->btn_info[1] << 8 |
                            (uint32_t)s->btn_info[2] << 16) & DS2_FULL_MASK;
            if (d->analog) log_mode(port, d);
            break;
        case TASK_VREF:
            memcpy(d->vref, s->vref, sizeof d->vref);
            break;
        }
    }
    if (s->task == TASK_BTNINFO && s->phase == 3 && d) memcpy(d->vref, s->vref, sizeof d->vref);
    if (--s->steps > 0) return;
    if (s->task == TASK_BTNINFO) s->press_set = 1;
    s->task = TASK_POLL;                /* the next poll completes the request (or re-detects) */
}

/* The EE's SetActDirect block: libpad bumps a counter and alternates between two 0x20-byte
 * blocks; padman takes the newer one if its "new" flag is set. */
static int set_act_direct(int port, int slot, pad_slot* s, uint8_t data[6]);

static void act_blocks(int port, int slot, pad_slot* s) {
    uint8_t* b = iop_mem(PAD_ACT_IOP_BASE + (uint32_t)(port * PAD_SLOTS + slot) * PAD_ACT_IOP_SIZE);
    if (!b) return;
    uint32_t c0, c1, flag;
    memcpy(&c0, b, 4);
    memcpy(&c1, b + 0x20, 4);
    uint8_t* blk = b + (c0 < c1 ? 0x20 : 0);
    memcpy(&flag, blk + 4, 4);
    if (!flag) return;
    uint8_t data[6];
    memcpy(data, blk + 0x0C, 6);
    set_act_direct(port, slot, s, data);
    memset(blk + 4, 0, 4);
}

static void poll_step(pad_slot* s, ds2_state* d, int port, int slot, const pad_input* in) {
    act_blocks(port, slot, s);
    if (!d) {                           /* nobody answers: count errors, then look for a controller */
        s->data_valid = 0;
        s->state = STATE_ERROR;         /* stays ERROR in the frame detection is started */
        s->err_count++;
        if (++s->poll_fail >= 10) detect_start(s);
        return;
    }
    uint8_t tx[6] = { 0 };
    for (int i = 0; i < s->act_direct_len && i < 6; i++)
        if (s->act_align[i] != 0xFF) tx[i] = s->act_direct[i];
    uint8_t rx[32];
    ds2_poll(d, in, tx, rx);
    s->poll_fail = 0;
    static int log_pad = -1;
    static uint16_t last_buttons;
    if (log_pad < 0) log_pad = getenv("PS2_LOG_PAD") != NULL;
    if (log_pad && in->buttons != last_buttons) {
        fprintf(stderr, "[pad] frame %u: buttons 0x%04X (id 0x%02X)\n", s->frame, in->buttons, rx[1]);
        last_buttons = in->buttons;
    }
    if (rx[2] != 0x5A || rx[1] != s->cur_id || rx[1] == 0xF3) {
        detect_start(s);
        s->state = STATE_EXECCMD;       /* padman+0x25C8: unexpected answer */
        return;
    }
    memcpy(s->rx, rx, sizeof rx);
    s->data_valid = 1;
    s->state = s->mode_config == 1 ? STATE_FINDCTP1 : STATE_STABLE;
    if (s->req_state == RSTAT_BUSY && !s->pending) s->req_state = RSTAT_COMPLETE;
}

/* The controller data libpad's scePadRead returns: status 0, ID, the poll reply after its
 * header, 0x5A in the last byte; always 32 bytes. Pressures are made consistent with the
 * button bits (pressed: at least 1, released: 0). */
static void format_data(pad_slot* s) {
    uint8_t* o = s->data;
    o[0] = 0;
    o[1] = s->cur_id;
    for (int k = 2; k < 31; k++) o[k] = s->rx[k + 1];
    o[31] = s->rx[2];
    if (s->cur_id == 0x12) { o[2] = o[3] = 0xFF; o[4] = o[5] = 0; }      /* mouse */
    if (s->cur_id == 0x79) {
        uint16_t pressed = (uint16_t)~(o[2] << 8 | o[3]);
        static const uint16_t bit[12] = { 0x2000, 0x8000, 0x1000, 0x4000, 0x0010, 0x0020, 0x0040, 0x0080,
                                          0x0004, 0x0008, 0x0001, 0x0002 };
        for (int i = 0; i < 12; i++) {
            if (pressed & bit[i]) { if (!o[8 + i]) o[8 + i] = 1; }
            else o[8 + i] = 0;
        }
    }
}

static void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* padman's per-frame DMA of the slot record to the EE. */
static void write_ee(pad_slot* s, int port, int slot) {
    uint8_t r[128];
    memset(r, 0, sizeof r);
    uint32_t len = 0;
    if (s->data_valid) { format_data(s); len = 32; }
    memcpy(r + 0x00, s->data, 32);
    memcpy(r + 0x20, s->act_direct, 6);
    memcpy(r + 0x28, s->act_align, 6);
    put16(r + 0x2E, s->state);          /* padman copies 8 bytes from the align field: the state follows */
    memcpy(r + 0x30, s->act_data, 16);
    memcpy(r + 0x40, s->comb_data, 16);
    for (int i = 0; i < 4; i++) put16(r + 0x50 + 2 * i, s->mode_table[i]);
    put32(r + 0x58, s->frame);
    put32(r + 0x5C, s->err_count);
    put32(r + 0x60, len);
    r[0x64] = s->mode_config;
    r[0x65] = s->cur_id;
    r[0x66] = s->model;
    r[0x67] = s->data_valid;
    r[0x68] = s->n_modes;
    r[0x69] = s->cur_offs;
    r[0x6A] = s->n_act;
    r[0x6B] = s->n_comb;
    r[0x6C] = s->press_set;
    r[0x6D] = s->req_mode;
    r[0x6E] = s->req_lock;
    r[0x6F] = s->act_direct_len;
    r[0x70] = s->state;
    r[0x71] = s->req_state;
    r[0x72] = s->task;
    r[0x73] = s->pending;
    r[0x74] = 0;                        /* multitap slot adjustment: never needed without a multitap */
    memcpy(r + 0x75, s->btn_info, 4);
    memcpy(r + 0x79, s->btn_mask, 4);

    uint32_t dst = s->ee_addr + ((s->frame & 1) ? 0x80u : 0u);
    uint8_t* p = ps2_fast(dst);
    if (p && ps2_fast(dst + 0x7F) == p + 0x7F) memcpy(p, r, sizeof r);
    else {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "[pad] port %d slot %d: EE buffer 0x%08X is not in RAM\n", port, slot, dst); }
    }
    s->frame++;
}

void pad_vsync(ps2_ctx* ctx) {
    (void)ctx;
    int talked[PAD_PORTS] = { 0 };      /* a transaction reached the controller this frame */
    for (int port = 0; port < PAD_PORTS && initialized; port++) {
        pad_input in = { 0 };
        int have_input = 0;
        for (int slot = 0; slot < PAD_SLOTS; slot++) {
            pad_slot* s = &slots[port][slot];
            if (!s->open) continue;
            ds2_state* d = pad_at(port, slot);
            if (d) talked[port] = 1;    /* every task runs one transaction per frame */
            /* scheduler: hand a queued request to its task (only while polling) */
            if (s->pending) {
                if (s->task == TASK_POLL) {
                    s->task = s->pending;
                    s->req_state = RSTAT_BUSY;
                    command_start(s);
                } else {
                    s->req_state = RSTAT_FAILED;
                }
                s->pending = 0;
            }
            {
                static int log_pad = -1, n;
                if (log_pad < 0) log_pad = getenv("PS2_LOG_PAD") != NULL;
                if (log_pad && n < 40 && (s->frame % 30) == 0) { n++; fprintf(stderr, "[pad] frame %u port %d slot %d task %d d=%p\n", s->frame, port, slot, s->task, (void*)d); }
            }
            switch (s->task) {
            case TASK_POLL:
                if (d && !have_input) { window_get_input(port, &in); have_input = 1; }
                poll_step(s, d, port, slot, &in);
                break;
            case TASK_DETECT:
                detect_step(s, d);
                break;
            default:
                s->data_valid = 0;
                command_step(s, d, port);
                break;
            }
        }
    }
    for (int port = 0; port < PAD_PORTS && initialized; port++)
        for (int slot = 0; slot < PAD_SLOTS; slot++)
            if (slots[port][slot].open) write_ee(&slots[port][slot], port, slot);
    /* the controllers' own watchdogs keep running when padman is idle or gone */
    for (int port = 0; port < PAD_PORTS && pads_ready; port++)
        if (pads[port].present) ds2_watchdog(&pads[port], talked[port]);
}

/* -- RPC commands -- */

static int port_open(uint32_t port, uint32_t slot, uint32_t ee_addr, uint32_t* act_addr) {
    pad_slot* s = slot_at(port, slot);
    if (!s) return 0;
    *act_addr = PAD_ACT_IOP_BASE + (port * PAD_SLOTS + slot) * PAD_ACT_IOP_SIZE;
    uint8_t* b = iop_mem(*act_addr);
    if (b) memset(b, 0, PAD_ACT_IOP_SIZE);
    if (s->open) return 0;
    memset(s, 0, sizeof *s);
    pad_at((int)port, (int)slot);       /* make sure the controllers exist */
    s->open = 1;
    s->ee_addr = ee_addr;
    s->state = STATE_EXECCMD;
    s->req_state = RSTAT_COMPLETE;
    s->task = TASK_POLL;                /* the first poll finds an unexpected ID and starts detection */
    s->first = 1;
    memset(s->act_align, 0xFF, sizeof s->act_align);
    fprintf(stderr, "[pad] port %u slot %u opened, EE buffer 0x%08X%s\n", port, slot, ee_addr,
            pad_at((int)port, (int)slot) ? "" : " (no controller)");
    return 1;
}

static int port_close(uint32_t port, uint32_t slot) {
    pad_slot* s = slot_at(port, slot);
    if (!s || !s->open || s->req_state == RSTAT_BUSY) return 0;
    s->open = 0;
    s->task = TASK_NONE;
    fprintf(stderr, "[pad] port %u slot %u closed\n", port, slot);
    return 1;
}

static void close_all(void) {
    for (int p = 0; p < PAD_PORTS; p++)
        for (int k = 0; k < PAD_SLOTS; k++) slots[p][k].open = 0;
}

/* "Ready" checks padman makes before answering information queries and accepting requests. */
static pad_slot* ready_slot(uint32_t port, uint32_t slot) {
    pad_slot* s = slot_at(port, slot);
    return (s && s->open && s->task == TASK_POLL) ? s : NULL;
}

static int32_t info_act(uint32_t port, uint32_t slot, int32_t act, int32_t term) {
    pad_slot* s = ready_slot(port, slot);
    if (!s || s->mode_config < 2) return 0;
    if (act == -1) return s->n_act;
    if (act < 0 || act >= s->n_act) return -1;
    if (term < 1 || term > 4) return -1;
    return s->act_data[4 * act + term - 1];
}

static int32_t info_comb(uint32_t port, uint32_t slot, int32_t comb, int32_t term) {
    pad_slot* s = ready_slot(port, slot);
    if (!s) return 0;
    if (s->mode_config < 2) return -1;
    if (comb == -1) return s->n_comb;
    if (comb < 0 || comb >= s->n_comb) return -1;
    if (term < -1 || term > 2) return -1;
    return s->comb_data[4 * comb + term + 1];
}

static int32_t info_mode(uint32_t port, uint32_t slot, int32_t term, int32_t offs) {
    pad_slot* s = ready_slot(port, slot);
    if (!s || s->req_state == RSTAT_BUSY) return 0;
    switch (term) {
    case 1: return s->cur_id == 0xF3 ? 0 : s->cur_id >> 4;                       /* PAD_MODECURID */
    case 2: return s->mode_config == 1 ? 0 : s->mode_table[s->cur_offs & 3];    /* PAD_MODECUREXID */
    case 3: return s->mode_config == 1 ? 0 : s->cur_offs;                       /* PAD_MODECUROFFS */
    case 4:                                                                     /* PAD_MODETABLE */
        if (s->mode_config == 1) return 0;
        if (offs == -1) return s->n_modes;
        return (offs >= 0 && offs < s->n_modes) ? s->mode_table[offs] : 0;
    default: return -1;
    }
}

static int set_main_mode(uint32_t port, uint32_t slot, int32_t mode, int32_t lock) {
    pad_slot* s = ready_slot(port, slot);
    if (!s || s->mode_config < 2 || mode < 0 || mode >= s->n_modes || s->req_state == RSTAT_BUSY) return 0;
    s->req_mode = (uint8_t)mode;
    s->req_lock = (uint8_t)lock;
    s->pending = TASK_SETMODE;
    s->req_state = RSTAT_BUSY;
    return 1;
}

/* Motor bytes are checked against the power budget: each active actuator draws its "current"
 * from the actuator table, and requests that would exceed the total over all pads are dropped. */
static int motor_current(const pad_slot* s, const uint8_t data[6]) {
    int total = 0;
    for (int i = 0; i < 6; i++) {
        uint8_t m = s->act_align[i];
        if (m < s->n_act && m < 6 && data[m]) total += s->act_data[4 * m + 3];
    }
    return total;
}

static int set_act_direct(int port, int slot, pad_slot* s, uint8_t data[6]) {
    if (s->mode_config == 1 || s->task != TASK_POLL) return 0;
    int used = 0;
    for (int p = 0; p < PAD_PORTS; p++)
        for (int k = 0; k < PAD_SLOTS; k++) {
            const pad_slot* o = &slots[p][k];
            if ((p != port || k != slot) && o->open && o->cur_id && o->act_direct_len)
                used += motor_current(o, o->act_direct);
        }
    for (int i = 0; i < 6; i++) {
        uint8_t m = s->act_align[i];
        if (m >= s->n_act || m >= 6 || !data[m]) continue;
        int c = used + s->act_data[4 * m + 3];
        if (c > PAD_MAX_CURRENT) {
            static int warned;
            if (!warned) { warned = 1; fprintf(stderr, "[pad] motor request over the power budget [%d][%d]\n", port, slot); }
            data[m] = 0;
        } else used = c;
    }
    static int logged;
    if (!logged && (data[0] || data[1])) { logged = 1; fprintf(stderr, "[pad] first vibration request (%02X %02X)\n", data[0], data[1]); }
    memcpy(s->act_direct, data, 6);
    s->act_direct_len = 6;
    return 1;
}

static int set_act_align(uint32_t port, uint32_t slot, const uint8_t data[6]) {
    pad_slot* s = ready_slot(port, slot);
    if (!s || s->mode_config < 2 || s->req_state == RSTAT_BUSY) return 0;
    memcpy(s->act_align, data, 6);
    s->pending = TASK_ACTALIGN;
    s->req_state = RSTAT_BUSY;
    return 1;
}

static int set_vref(uint32_t port, uint32_t slot, const uint8_t data[12]) {
    pad_slot* s = ready_slot(port, slot);
    if (!s || s->mode_config < 2 || s->model < 2 || s->req_state == RSTAT_BUSY) return 0;
    memcpy(s->vref, data, 12);
    s->pending = TASK_VREF;
    s->req_state = RSTAT_BUSY;
    return 1;
}

static int set_button_info(uint32_t port, uint32_t slot, uint32_t info) {
    pad_slot* s = ready_slot(port, slot);
    if (!s || s->mode_config < 2 || !(s->model & 2) || s->req_state == RSTAT_BUSY) return 0;
    uint32_t mask = info << 6 | 0x3F;
    for (int i = 0; i < 4; i++) s->btn_info[i] = (uint8_t)(mask >> (8 * i));
    memset(s->vref, 2, sizeof s->vref);
    s->pending = TASK_BTNINFO;
    s->req_state = RSTAT_BUSY;
    return 1;
}

static uint32_t get_button_mask(uint32_t port, uint32_t slot) {
    pad_slot* s = ready_slot(port, slot);
    if (!s || s->mode_config < 2 || s->model < 2) return 0;
    return get32(s->btn_mask);
}

/* Execute one command in place, like padman's RPC handler: results go into the same buffer. */
static void pad_command(uint8_t* b) {
    uint32_t cmd = get32(b), port = get32(b + 4), slot = get32(b + 8);
    uint32_t res = 0;
    int res_off = -1;
    switch (cmd) {
    case CMD_OPEN: {
        uint32_t act = get32(b + 0x14);
        res = (uint32_t)port_open(port, slot, get32(b + 0x10), &act);
        put32(b + 0x14, act);
        res_off = 0x0C;
        break;
    }
    case CMD_INFO_ACT:   res = (uint32_t)info_act(port, slot, (int32_t)get32(b + 0x0C), (int32_t)get32(b + 0x10)); res_off = 0x14; break;
    case CMD_INFO_COMB:  res = (uint32_t)info_comb(port, slot, (int32_t)get32(b + 0x0C), (int32_t)get32(b + 0x10)); res_off = 0x14; break;
    case CMD_INFO_MODE:  res = (uint32_t)info_mode(port, slot, (int32_t)get32(b + 0x0C), (int32_t)get32(b + 0x10)); res_off = 0x14; break;
    case CMD_SET_MMODE:  res = (uint32_t)set_main_mode(port, slot, (int32_t)get32(b + 0x0C), (int32_t)get32(b + 0x10)); res_off = 0x14; break;
    case CMD_SET_ACTDIR: {
        pad_slot* s = slot_at(port, slot);
        uint8_t data[6];
        memcpy(data, b + 0x0C, 6);
        res = (s && s->open) ? (uint32_t)set_act_direct((int)port, (int)slot, s, data) : 0;
        res_off = 0x14;
        break;
    }
    case CMD_SET_ACTALIGN: res = (uint32_t)set_act_align(port, slot, b + 0x0C); res_off = 0x14; break;
    case CMD_GET_BTNMASK:  res = get_button_mask(port, slot); res_off = 0x0C; break;
    case CMD_SET_BTNINFO:  res = (uint32_t)set_button_info(port, slot, get32(b + 0x0C)); res_off = 0x10; break;
    case CMD_SET_VREF:     res = (uint32_t)set_vref(port, slot, b + 0x0C); res_off = 0x1C; break;
    case CMD_GET_PORTMAX:  res = PAD_PORTS; res_off = 0x0C; break;
    case CMD_GET_SLOTMAX:  res = 1; res_off = 0x0C; break;                 /* no multitap */
    case CMD_CLOSE:        res = (uint32_t)port_close(port, slot); res_off = 0x0C; break;
    case CMD_END:          close_all(); initialized = 0; res = 1; res_off = 0x0C; break;
    case CMD_INIT:
        close_all();                    /* padman ends a previous session first */
        initialized = 1;
        notify = 0;
        res = 1;
        res_off = 0x0C;
        break;
    case CMD_GET_MODVER:   res = PAD_MODULE_VER; res_off = 0x0C; break;
    case CMD_NOP:          break;
    case CMD_SET_DEBUG:    res = 1; res_off = 0x08; break;
    case CMD_SET_NOTIFY:
        notify = (int)port;
        if (notify) fprintf(stderr, "[pad] per-frame SIF notification requested: not emulated\n");
        res = 1;
        res_off = 0x08;
        break;
    default: {
        static uint32_t seen;
        if (cmd < 32 ? !(seen & (1u << cmd)) : !(seen & 1u)) {
            seen |= cmd < 32 ? 1u << cmd : 1u;
            fprintf(stderr, "[pad] invalid function code (%03x)\n", cmd);
        }
        return;
    }
    }
    if (res_off >= 0) put32(b + res_off, res);

    static uint8_t logged[0x19];         /* the first two calls of each command */
    if (cmd < 0x19 && (ps2_log_sif || (logged[cmd] < 2 && ++logged[cmd])) && cmd != CMD_OPEN && cmd != CMD_CLOSE)
        fprintf(stderr, "[pad] %s(%u, %u) -> 0x%X\n", cmd_names[cmd] ? cmd_names[cmd] : "?", port, slot, res);
}

int pad_rpc(ps2_ctx* ctx, uint32_t sid, uint32_t fno, const uint8_t* args, uint32_t send_size,
            uint8_t* out, uint32_t recv_size) {
    (void)ctx; (void)fno;               /* libpad always calls function 1; padman ignores it */
    if (sid != PAD_SID && sid != PAD_EXT_SID) return 0;
    uint8_t buf[128];
    memset(buf, 0, sizeof buf);
    memcpy(buf, args, send_size < sizeof buf ? send_size : sizeof buf);
    if (sid == PAD_SID) pad_command(buf);
    else {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "[pad] extended service (0x80000101) is not supported\n"); }
    }
    memcpy(out, buf, recv_size < sizeof buf ? recv_size : sizeof buf);
    return 1;
}

/* Current motor values of the controller in `port` (small motor on/off, large motor speed), for
 * a host rumble implementation. Not part of hw.h yet. */
void pad_get_motors(int port, uint8_t motors[2]) {
    motors[0] = motors[1] = 0;
    if (port < 0 || port >= PAD_PORTS || !pads_ready) return;
    motors[0] = pads[port].motor[0];
    motors[1] = pads[port].motor[1];
}
