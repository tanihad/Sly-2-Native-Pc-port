/* PC input: the device table, the keyboard, XInput, rumble, the menu's navigation events, binding
 * capture and the [controls] options (input.h; notes/controls.md).
 *
 * Controllers live in one table, in connection order: XInput slots (polled here, when the game or
 * the menu asks, so there is no added latency) and HID controllers (input_hid.c's thread pushes
 * every report it reads). Ports: the first controller that shows activity drives PS2 port 0
 * (input_assign_ports). The keyboard always adds to port 0 while the game window has focus.
 *
 * Rumble: the game's DualShock 2 motor values (pad.c, every VBlank) are scaled by the options and
 * sent by the input thread to the controller of that port, only when they change; if the game
 * stops sending for INPUT_RUMBLE_TIMEOUT_MS (a long stall), the motors stop. */
#include "input_internal.h"
#include "settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xinput.h>

#define INPUT_RUMBLE_TIMEOUT_MS 300

static void ilog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[input] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

uint64_t input_now_ms(void) { return GetTickCount64(); }

/* ---- configuration ----------------------------------------------------------------------------------- */

static input_config cfg;
static SRWLOCK cfg_lock = SRWLOCK_INIT;
static volatile LONG cfg_ready;

void input_config_get(input_config* c) {
    if (!cfg_ready) input_config_reload();
    AcquireSRWLockShared(&cfg_lock);
    *c = cfg;
    ReleaseSRWLockShared(&cfg_lock);
}

static void cfg_line(void* user, const char* key, const char* value, int line) {
    input_config* c = (input_config*)user;
    int r = input_config_apply(c, key, value);
    if (r > 0) ilog("sly2.ini [controls] line %d: bad value '%s' for %s (default kept)", line, value, key);
    else if (r < 0) ilog("sly2.ini [controls] line %d: unknown key '%s' (ignored)", line, key);
}

void input_config_reload(void) {
    input_config c;
    input_config_defaults(&c);
    settings_read_section("controls", cfg_line, &c);
    AcquireSRWLockExclusive(&cfg_lock);
    cfg = c;
    InterlockedExchange(&cfg_ready, 1);
    ReleaseSRWLockExclusive(&cfg_lock);
    input_hid_kick();                                         /* light bar colour, vibration on/off */
}

int input_config_set(const input_config* c, int save) {
    input_config v = *c;
    if (v.vibration_strength < 0) v.vibration_strength = 0;
    if (v.vibration_strength > 100) v.vibration_strength = 100;
    if (v.deadzone_left < 0) v.deadzone_left = 0;
    if (v.deadzone_left > 50) v.deadzone_left = 50;
    if (v.deadzone_right < 0) v.deadzone_right = 0;
    if (v.deadzone_right > 50) v.deadzone_right = 50;
    AcquireSRWLockExclusive(&cfg_lock);
    cfg = v;
    InterlockedExchange(&cfg_ready, 1);
    ReleaseSRWLockExclusive(&cfg_lock);
    input_hid_kick();
    if (!save) return 1;
    enum { N = 64 };
    static char keys[N][40], vals[N][128];
    static const char* kv[N][3];
    int n = input_config_keys();
    if (n > N) n = N;
    for (int i = 0; i < n; i++) {
        input_config_key(&v, i, keys[i], sizeof keys[i], vals[i], sizeof vals[i]);
        kv[i][0] = "controls"; kv[i][1] = keys[i]; kv[i][2] = vals[i];
    }
    int ok = settings_set((const char* const (*)[3])kv, n);
    if (!ok) ilog("cannot write [controls] to sly2.ini");
    return ok;
}

/* ---- the device table -------------------------------------------------------------------------------- */

typedef struct {
    int id;
    int kind, bus;
    uint16_t vid, pid;
    char name[64];
    const input_backend_ops* ops;
    void* backend;
    int can_rumble;
    input_state st;
    int battery, charging, enhanced;
    uint32_t reports, crc_errors;
    int port;
    uint64_t last_active;
    input_nav_tracker nav;
    uint32_t nav_latch;                          /* nav bits that went down since the last nav poll */
    uint32_t cap_latch;                          /* buttons that went down since the capture began */
    uint32_t hidden;                             /* held when the menu closed: hidden from the game until released */
    uint8_t sent_small, sent_large;              /* motors as last sent (input thread) */
    uint64_t test_until;
    uint8_t test_small, test_large;
} device;

static device devs[INPUT_MAX_DEVICES];
static int ndev;
static int next_id = 1;
static SRWLOCK dev_lock = SRWLOCK_INIT;

static device* dev_find(int id) {
    for (int i = 0; i < ndev; i++) if (devs[i].id == id) return &devs[i];
    return NULL;
}

/* Notices (connect / disconnect), a small ring. */
#define NOTICES 16
static input_notice notices[NOTICES];
static int notice_head, notice_count;

static void notice_push(int connected, const device* d) {      /* dev_lock held */
    input_notice* n = &notices[(notice_head + notice_count) % NOTICES];
    if (notice_count == NOTICES) notice_head = (notice_head + 1) % NOTICES;
    else notice_count++;
    n->connected = connected;
    n->device_id = d->id;
    n->port = d->port;
    snprintf(n->name, sizeof n->name, "%s", d->name);
}

int input_poll_notice(input_notice* out) {
    AcquireSRWLockExclusive(&dev_lock);
    int got = notice_count > 0;
    if (got) { *out = notices[notice_head]; notice_head = (notice_head + 1) % NOTICES; notice_count--; }
    ReleaseSRWLockExclusive(&dev_lock);
    return got;
}

/* Port assignment over the table; dev_lock held. */
static void assign_ports(uint64_t now) {
    input_port_dev pd[INPUT_MAX_DEVICES];
    for (int i = 0; i < ndev; i++) {
        pd[i].id = devs[i].id;
        pd[i].port = devs[i].port;
        input_state st = devs[i].st;
        st.buttons &= ~devs[i].hidden;
        pd[i].active = input_state_active(&st);
        pd[i].last_active = devs[i].last_active;
    }
    uint32_t changed = input_assign_ports(pd, ndev, now);
    for (int i = 0; i < ndev; i++) {
        devs[i].port = pd[i].port;
        devs[i].last_active = pd[i].last_active;
        if (changed >> i & 1) {
            if (devs[i].port >= 0) ilog("%s drives PS2 port %d", devs[i].name, devs[i].port);
            else ilog("%s no longer drives a port", devs[i].name);
        }
    }
}

int input_dev_add(int kind, int bus, uint16_t vid, uint16_t pid, const char* name, const input_backend_ops* ops,
                  void* backend, int can_rumble) {
    AcquireSRWLockExclusive(&dev_lock);
    int id = 0;
    if (ndev < INPUT_MAX_DEVICES) {
        device* d = &devs[ndev++];
        memset(d, 0, sizeof *d);
        d->id = id = next_id++;
        d->kind = kind;
        d->bus = bus;
        d->vid = vid;
        d->pid = pid;
        snprintf(d->name, sizeof d->name, "%s", name);
        d->ops = ops;
        d->backend = backend;
        d->can_rumble = can_rumble;
        d->battery = -1;
        d->port = -1;
        d->st.buttons = 0;
        notice_push(1, d);
        ilog("connected: %s (%04X:%04X)%s", d->name, vid, pid, can_rumble ? "" : ", no rumble");
    } else {
        ilog("too many controllers: %s ignored", name);
    }
    ReleaseSRWLockExclusive(&dev_lock);
    input_hid_kick();                                         /* light bar, motors */
    return id;
}

void input_dev_remove(int id) {
    AcquireSRWLockExclusive(&dev_lock);
    device* d = dev_find(id);
    if (d) {
        notice_push(0, d);
        if (d->port >= 0) ilog("disconnected: %s (it drove PS2 port %d; the next controller used takes over)", d->name, d->port);
        else ilog("disconnected: %s", d->name);
        int i = (int)(d - devs);
        memmove(&devs[i], &devs[i + 1], (size_t)(ndev - i - 1) * sizeof devs[0]);
        ndev--;
    }
    ReleaseSRWLockExclusive(&dev_lock);
}

static void dev_set_state(device* d, const input_state* st) {    /* dev_lock held */
    uint32_t before = d->st.buttons, nav_before = input_state_nav_bits(&d->st);
    d->st = *st;
    d->cap_latch |= st->buttons & ~before;
    d->nav_latch |= input_state_nav_bits(st) & ~nav_before;
    d->hidden &= st->buttons;
    d->reports++;
}

void input_dev_update(int id, const input_state* st, int battery, int charging, int enhanced, int crc_ok) {
    AcquireSRWLockExclusive(&dev_lock);
    device* d = dev_find(id);
    if (d) {
        dev_set_state(d, st);
        d->battery = battery;
        d->charging = charging;
        d->enhanced = enhanced;
        if (!crc_ok) d->crc_errors++;
        assign_ports(input_now_ms());
    }
    ReleaseSRWLockExclusive(&dev_lock);
}

int input_devices(input_device_info* out, int max) {
    AcquireSRWLockShared(&dev_lock);
    int n = 0;
    for (int i = 0; i < ndev && n < max; i++, n++) {
        const device* d = &devs[i];
        input_device_info* o = &out[n];
        memset(o, 0, sizeof *o);
        o->id = d->id; o->kind = d->kind; o->bus = d->bus; o->port = d->port; o->vid = d->vid; o->pid = d->pid;
        snprintf(o->name, sizeof o->name, "%s", d->name);
        o->state = d->st; o->battery = d->battery; o->charging = d->charging; o->reports = d->reports;
        o->crc_errors = d->crc_errors; o->enhanced = d->enhanced; o->can_rumble = d->can_rumble;
        o->motor_small = d->sent_small; o->motor_large = d->sent_large;
    }
    ReleaseSRWLockShared(&dev_lock);
    return n;
}

/* ---- XInput ------------------------------------------------------------------------------------------- */

typedef DWORD (WINAPI* xi_get_fn)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI* xi_set_fn)(DWORD, XINPUT_VIBRATION*);
static xi_get_fn xi_get;
static xi_set_fn xi_set;
static volatile LONG xi_dev[XUSER_MAX_COUNT];            /* device id of each slot, 0 = empty */
static SRWLOCK xi_lock[XUSER_MAX_COUNT];                 /* serialises calls per slot (zero = SRWLOCK_INIT) */

static int xi_read(DWORD slot, input_state* st) {
    XINPUT_STATE s;
    AcquireSRWLockExclusive(&xi_lock[slot]);
    DWORD r = xi_get(slot, &s);
    ReleaseSRWLockExclusive(&xi_lock[slot]);
    if (r != ERROR_SUCCESS) return 0;
    if (st) input_xinput_to_state(s.Gamepad.wButtons, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger,
                                  s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY, st);
    return 1;
}

static int xi_poll_op(void* backend, input_state* st) { return xi_read((DWORD)(intptr_t)backend, st); }

static void xi_output_op(void* backend, uint8_t small_motor, uint8_t large_motor) {
    DWORD slot = (DWORD)(intptr_t)backend;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = (WORD)(large_motor * 257);            /* left: the heavy low-frequency motor */
    v.wRightMotorSpeed = (WORD)(small_motor * 257);
    AcquireSRWLockExclusive(&xi_lock[slot]);
    xi_set(slot, &v);
    ReleaseSRWLockExclusive(&xi_lock[slot]);
}

static const input_backend_ops xi_ops = { xi_output_op, xi_poll_op };

static void xi_connect(DWORD slot) {
    char name[64];
    snprintf(name, sizeof name, "Xbox controller (XInput %lu)", (unsigned long)slot + 1);
    int id = input_dev_add(INPUT_DEV_XINPUT, INPUT_BUS_OTHER, 0x045E, 0, name, &xi_ops, (void*)(intptr_t)slot, xi_set != NULL);
    InterlockedExchange(&xi_dev[slot], id);
}

static void xi_disconnect(DWORD slot) {
    LONG id = InterlockedExchange(&xi_dev[slot], 0);
    if (id) input_dev_remove((int)id);
}

/* Read every connected slot into the table (the game or the menu asks). */
static void xi_refresh(void) {
    if (!xi_get) return;
    for (DWORD s = 0; s < XUSER_MAX_COUNT; s++) {
        LONG id = xi_dev[s];
        if (!id) continue;
        input_state st;
        if (!xi_read(s, &st)) { xi_disconnect(s); continue; }
        AcquireSRWLockExclusive(&dev_lock);
        device* d = dev_find((int)id);
        if (d) { dev_set_state(d, &st); assign_ports(input_now_ms()); }
        ReleaseSRWLockExclusive(&dev_lock);
    }
}

/* Probe one empty slot every 500 ms (probing an empty slot is slow; Microsoft recommends doing it
 * rarely). Input thread. */
static void xi_probe_step(void) {
    static uint64_t next;
    static DWORD slot;
    uint64_t now = input_now_ms();
    if (!xi_get || now < next) return;
    next = now + 500;
    for (int k = 0; k < XUSER_MAX_COUNT; k++) {
        DWORD s = (slot + (DWORD)k) % XUSER_MAX_COUNT;
        if (xi_dev[s]) continue;
        if (xi_read(s, NULL)) xi_connect(s);
        slot = s + 1;
        break;
    }
}

void input_xinput_override(input_xi_get_fn get, input_xi_set_fn set) {   /* (unit tests) */
    xi_get = (xi_get_fn)(void*)get;
    xi_set = (xi_set_fn)(void*)set;
}

static void xi_load(void) {
    if (xi_get) return;
    static const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (size_t i = 0; i < sizeof dlls / sizeof dlls[0] && !xi_get; i++) {
        HMODULE m = LoadLibraryW(dlls[i]);
        if (!m) continue;
        xi_get = (xi_get_fn)(void*)GetProcAddress(m, "XInputGetState");
        xi_set = (xi_set_fn)(void*)GetProcAddress(m, "XInputSetState");
    }
    if (!xi_get) ilog("XInput unavailable: Xbox controllers disabled");
}

/* ---- life cycle --------------------------------------------------------------------------------------- */

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK do_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    if (!cfg_ready) input_config_reload();
    xi_load();
    if (xi_get)
        for (DWORD s = 0; s < XUSER_MAX_COUNT; s++) if (xi_read(s, NULL)) xi_connect(s);   /* ready from the first frame */
    input_hid_start();
    return TRUE;
}

void input_init(void) { InitOnceExecuteOnce(&init_once, do_init, NULL, NULL); }

static void all_motors_off(void) {
    if (xi_set)
        for (DWORD s = 0; s < XUSER_MAX_COUNT; s++) if (xi_dev[s]) xi_output_op((void*)(intptr_t)s, 0, 0);
}

void input_shutdown(void) {
    static volatile LONG done;
    if (InterlockedExchange(&done, 1)) return;
    all_motors_off();
    input_hid_stop();
}

static HWND volatile game_hwnd;
void input_set_window(void* hwnd) { game_hwnd = (HWND)hwnd; }

/* ---- rumble ---------------------------------------------------------------------------------------------- */

static struct { uint8_t small_on, large; uint64_t at; } want[2];
static volatile LONG menu_on;

void input_set_rumble(int port, uint8_t small_on, uint8_t large_speed) {
    if (port < 0 || port > 1) return;
    int changed = want[port].small_on != (small_on != 0) || want[port].large != large_speed;
    want[port].small_on = small_on != 0;
    want[port].large = large_speed;
    want[port].at = input_now_ms();
    if (changed) input_hid_kick();
}

void input_test_rumble(int device_id, uint8_t small_motor, uint8_t large_motor, int ms) {
    AcquireSRWLockExclusive(&dev_lock);
    device* d = dev_find(device_id);
    if (d) { d->test_small = small_motor; d->test_large = large_motor; d->test_until = input_now_ms() + (uint64_t)(ms > 0 ? ms : 0); }
    ReleaseSRWLockExclusive(&dev_lock);
    input_hid_kick();
}

int input_dev_port(int id) {
    AcquireSRWLockShared(&dev_lock);
    const device* d = dev_find(id);
    int port = d ? d->port : -1;
    ReleaseSRWLockShared(&dev_lock);
    return port;
}

static void motors_for(const device* d, const input_config* c, uint64_t now, uint8_t* s, uint8_t* l) {
    *s = *l = 0;
    int strength = c->vibration_strength;
    if (now < d->test_until) {
        *s = (uint8_t)(d->test_small * strength / 100);
        *l = (uint8_t)(d->test_large * strength / 100);
        return;
    }
    if (menu_on || !c->vibration || d->port < 0 || d->port > 1) return;
    if (now - want[d->port].at > INPUT_RUMBLE_TIMEOUT_MS) return;          /* the game stopped sending */
    *s = want[d->port].small_on ? (uint8_t)(255 * strength / 100) : 0;
    *l = (uint8_t)(want[d->port].large * strength / 100);
}

int input_dev_motors(int id, uint8_t* s, uint8_t* l) {
    input_config c;
    input_config_get(&c);
    AcquireSRWLockShared(&dev_lock);
    const device* d = dev_find(id);
    if (d) motors_for(d, &c, input_now_ms(), s, l);
    ReleaseSRWLockShared(&dev_lock);
    return d != NULL;
}

/* Input thread, every wake-up (at least every 50 ms): probe XInput, send changed motor values. */
void input_service(void) {
    xi_probe_step();
    input_config c;
    input_config_get(&c);
    uint64_t now = input_now_ms();
    struct { int id; const input_backend_ops* ops; void* backend; uint8_t s, l; } todo[INPUT_MAX_DEVICES];
    int n = 0;
    AcquireSRWLockExclusive(&dev_lock);
    for (int i = 0; i < ndev; i++) {
        device* d = &devs[i];
        if (!d->can_rumble || d->ops != &xi_ops) continue;   /* (HID controllers: input_hid.c sends) */
        uint8_t s, l;
        motors_for(d, &c, now, &s, &l);
        if (s == d->sent_small && l == d->sent_large) continue;
        d->sent_small = s;
        d->sent_large = l;
        todo[n].id = d->id; todo[n].ops = d->ops; todo[n].backend = d->backend; todo[n].s = s; todo[n].l = l; n++;
    }
    ReleaseSRWLockExclusive(&dev_lock);
    for (int i = 0; i < n; i++) todo[i].ops->output(todo[i].backend, todo[i].s, todo[i].l);
}

/* HID backend: note what it sent (pad test display). */
void input_dev_sent(int id, uint8_t s, uint8_t l) {
    AcquireSRWLockExclusive(&dev_lock);
    device* d = dev_find(id);
    if (d) { d->sent_small = s; d->sent_large = l; }
    ReleaseSRWLockExclusive(&dev_lock);
}

/* ---- keyboard -------------------------------------------------------------------------------------------- */

static uint8_t kb_hidden[256];                   /* held when the menu closed: ignored until released */

static int kb_focused(void) {
    HWND h = game_hwnd;
    return h && GetForegroundWindow() == h && GetAsyncKeyState(VK_MENU) >= 0;   /* Alt held: window shortcut */
}

static int kb_read(const input_config* c, uint8_t down[256]) {
    if (!kb_focused()) return 0;
    memset(down, 0, 256);
    for (int a = 0; a < INPUT_ACT_COUNT; a++)
        for (int k = 0; k < INPUT_BIND_SLOTS; k++) {
            int vk = c->key[a][k];
            if (!vk) continue;
            int held = GetAsyncKeyState(vk) < 0;
            if (!held) kb_hidden[vk] = 0;
            down[vk] = (uint8_t)(held && !kb_hidden[vk]);
        }
    return 1;
}

/* ---- the game --------------------------------------------------------------------------------------------- */

void input_get_pad(int port, pad_input* out) {
    input_pad_clear(out);
    if (port < 0 || port > 1) return;
    input_init();
    xi_refresh();
    input_config c;
    input_config_get(&c);
    if (menu_on) return;
    AcquireSRWLockShared(&dev_lock);
    for (int i = 0; i < ndev; i++) {
        if (devs[i].port != port) continue;
        input_state st = devs[i].st;
        st.buttons &= ~devs[i].hidden;
        if (devs[i].hidden >> INPUT_BTN_L2 & 1) st.l2 = 0;
        if (devs[i].hidden >> INPUT_BTN_R2 & 1) st.r2 = 0;
        input_state_to_pad(&st, &c, out);
        break;
    }
    ReleaseSRWLockShared(&dev_lock);
    uint8_t down[256];
    if (port == 0 && kb_read(&c, down)) input_keys_to_pad(down, &c, out);
    input_invert_camera(out, &c);
}

/* ---- the menu ------------------------------------------------------------------------------------------- */

#define NAVQ 64
static input_nav_event navq[NAVQ];
static int navq_head, navq_count;
static SRWLOCK nav_lock = SRWLOCK_INIT;
static volatile LONG capture_what, capture_state = INPUT_CAPTURE_IDLE, capture_code;
static volatile LONG swallow_vk;                 /* keyboard: the captured key, ignored until released */

static void nav_push(int nav, int source, int device_id, int repeat, int x, int y) {
    if (capture_what) return;                                 /* capturing: no navigation */
    if (!menu_on && nav != INPUT_NAV_MENU) return;            /* closed: only "open the menu" */
    AcquireSRWLockExclusive(&nav_lock);
    if (nav == INPUT_NAV_POINTER && navq_count &&
        navq[(navq_head + navq_count - 1) % NAVQ].nav == INPUT_NAV_POINTER) {   /* coalesce moves */
        input_nav_event* e = &navq[(navq_head + navq_count - 1) % NAVQ];
        e->x = x; e->y = y;
    } else {
        if (navq_count == NAVQ) { navq_head = (navq_head + 1) % NAVQ; navq_count--; }
        input_nav_event* e = &navq[(navq_head + navq_count++) % NAVQ];
        e->nav = nav; e->source = source; e->device_id = device_id; e->repeat = repeat; e->x = x; e->y = y;
    }
    ReleaseSRWLockExclusive(&nav_lock);
}

/* Controllers: edge detection and auto-repeat (the menu calls input_nav_poll every frame). */
static void nav_controllers(void) {
    xi_refresh();
    uint64_t now = input_now_ms();
    struct { int nav, repeat, id; } ev[64];
    int n = 0;
    AcquireSRWLockExclusive(&dev_lock);
    for (int i = 0; i < ndev; i++) {
        device* d = &devs[i];
        uint32_t bits = input_state_nav_bits(&d->st), pressed = d->nav_latch;
        d->nav_latch = 0;
        uint32_t start_menu = (d->st.buttons >> INPUT_BTN_START & 1) && !(d->st.buttons >> INPUT_BTN_HOME & 1);
        if (!menu_on && start_menu) { bits &= ~(1u << INPUT_NAV_MENU); pressed &= ~(1u << INPUT_NAV_MENU); }   /* START is the game's */
        int navs[16], reps[16];
        int k = input_nav_step(&d->nav, bits, pressed, now, navs, reps, 16);
        for (int j = 0; j < k && n < 64; j++, n++) { ev[n].nav = navs[j]; ev[n].repeat = reps[j]; ev[n].id = d->id; }
    }
    ReleaseSRWLockExclusive(&dev_lock);
    for (int i = 0; i < n; i++) nav_push(ev[i].nav, INPUT_SRC_CONTROLLER, ev[i].id, ev[i].repeat, 0, 0);
}

int input_nav_poll(input_nav_event* out) {
    nav_controllers();
    AcquireSRWLockExclusive(&nav_lock);
    int got = navq_count > 0;
    if (got) { *out = navq[navq_head]; navq_head = (navq_head + 1) % NAVQ; navq_count--; }
    ReleaseSRWLockExclusive(&nav_lock);
    return got;
}

void input_nav_flush(void) {
    nav_controllers();
    AcquireSRWLockExclusive(&nav_lock);
    navq_count = 0;
    ReleaseSRWLockExclusive(&nav_lock);
}

void input_set_menu_active(int on) {
    on = on != 0;
    if (InterlockedExchange(&menu_on, on) == on) return;
    if (!on) {                                                /* hide what is still held from the game */
        AcquireSRWLockExclusive(&dev_lock);
        for (int i = 0; i < ndev; i++) devs[i].hidden = devs[i].st.buttons;
        ReleaseSRWLockExclusive(&dev_lock);
        for (int vk = 1; vk < 256; vk++) kb_hidden[vk] = GetAsyncKeyState(vk) < 0;
    }
    input_hid_kick();                                         /* motors off while the menu is open */
}

int input_menu_active(void) { return menu_on != 0; }

void input_capture_begin(int what) {
    xi_refresh();
    AcquireSRWLockExclusive(&dev_lock);
    for (int i = 0; i < ndev; i++) devs[i].cap_latch = 0;
    ReleaseSRWLockExclusive(&dev_lock);
    InterlockedExchange(&capture_state, INPUT_CAPTURE_PENDING);
    InterlockedExchange(&capture_what, what & (INPUT_CAPTURE_KEYBOARD | INPUT_CAPTURE_CONTROLLER));
}

void input_capture_cancel(void) {
    InterlockedExchange(&capture_what, 0);
    InterlockedExchange(&capture_state, INPUT_CAPTURE_IDLE);
}

static void capture_done(int state, int code) {
    InterlockedExchange(&capture_code, code);
    InterlockedExchange(&capture_state, state);
    InterlockedExchange(&capture_what, 0);
}

int input_capture_poll(int* code) {
    if (capture_what & INPUT_CAPTURE_CONTROLLER) {
        xi_refresh();
        int found = -1;
        AcquireSRWLockExclusive(&dev_lock);
        for (int i = 0; i < ndev && found < 0; i++)
            for (int b = 0; b < INPUT_BTN_COUNT; b++)
                if (devs[i].cap_latch >> b & 1) { found = b; break; }
        if (found >= 0)                                       /* keep the nav trackers in step: no CONFIRM later */
            for (int i = 0; i < ndev; i++) { devs[i].nav.held = input_state_nav_bits(&devs[i].st); devs[i].nav_latch = 0; }
        ReleaseSRWLockExclusive(&dev_lock);
        if (found == INPUT_BTN_HOME) capture_done(INPUT_CAPTURE_CANCELLED, 0);
        else if (found >= 0) capture_done(INPUT_CAPTURE_BUTTON, found);
    }
    int s = (int)capture_state;
    if (code) *code = (int)capture_code;
    if (s != INPUT_CAPTURE_PENDING) InterlockedExchange(&capture_state, INPUT_CAPTURE_IDLE);
    return s;
}

/* The left/right variant of a Shift/Ctrl key from WM_KEYDOWN's lParam. */
static int key_side(int vk, intptr_t lp) {
    if (vk == VK_CONTROL) return (lp >> 24 & 1) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_SHIFT) {
        UINT sc = (UINT)(lp >> 16 & 0xFF);
        return MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    }
    return vk;
}

static int key_nav(int vk, int shift) {
    switch (vk) {
    case VK_UP: case 'W': return INPUT_NAV_UP;
    case VK_DOWN: case 'S': return INPUT_NAV_DOWN;
    case VK_LEFT: case 'A': return INPUT_NAV_LEFT;
    case VK_RIGHT: case 'D': return INPUT_NAV_RIGHT;
    case VK_RETURN: case VK_SPACE: return INPUT_NAV_CONFIRM;
    case VK_ESCAPE: case VK_BACK: return INPUT_NAV_BACK;
    case VK_TAB: return shift ? INPUT_NAV_TAB_PREV : INPUT_NAV_TAB_NEXT;
    case 'E': case VK_NEXT: return INPUT_NAV_TAB_NEXT;
    case 'Q': case VK_PRIOR: return INPUT_NAV_TAB_PREV;
    default: return INPUT_NAV_NONE;
    }
}

int input_window_message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    (void)hwnd;
    int capturing_keys = (capture_what & INPUT_CAPTURE_KEYBOARD) != 0;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int vk = key_side((int)wp, lp), repeat = (lp >> 30 & 1) != 0;
        if (msg == WM_SYSKEYDOWN) return 0;                   /* Alt+key: window shortcuts */
        if ((LONG)vk == swallow_vk) return menu_on != 0;      /* the key just captured, still held */
        if (capture_what) {
            if (!capturing_keys) return (int)wp != VK_F11;
            if (repeat) return 1;
            if (vk == VK_ESCAPE) { capture_done(INPUT_CAPTURE_CANCELLED, 0); return 1; }
            if (!input_key_ini(vk)) return (int)wp != VK_F11;     /* not bindable: ignored, keep waiting */
            InterlockedExchange(&swallow_vk, vk);
            capture_done(INPUT_CAPTURE_KEY, vk);
            return 1;
        }
        if (vk == VK_ESCAPE && !menu_on) { if (!repeat) nav_push(INPUT_NAV_MENU, INPUT_SRC_KEYBOARD, 0, 0, 0, 0); return 0; }
        if (!menu_on) return 0;
        int nav = key_nav((int)wp, GetKeyState(VK_SHIFT) < 0);
        if (nav) nav_push(nav, INPUT_SRC_KEYBOARD, 0, repeat, 0, 0);
        return (int)wp != VK_F11;                             /* menu open: keys are the menu's (F11 still toggles fullscreen) */
    }
    case WM_KEYUP: {
        if ((LONG)key_side((int)wp, lp) == swallow_vk) InterlockedExchange(&swallow_vk, 0);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        if (capturing_keys) {
            int vk = msg == WM_LBUTTONDOWN ? VK_LBUTTON : msg == WM_RBUTTONDOWN ? VK_RBUTTON : msg == WM_MBUTTONDOWN
                     ? VK_MBUTTON : (HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
            capture_done(INPUT_CAPTURE_KEY, vk);
            return 1;
        }
        if (!menu_on || capture_what) return 0;
        if (msg == WM_LBUTTONDOWN) nav_push(INPUT_NAV_CLICK, INPUT_SRC_MOUSE, 0, 0, x, y);
        else if (msg == WM_RBUTTONDOWN) nav_push(INPUT_NAV_BACK, INPUT_SRC_MOUSE, 0, 0, x, y);
        return 1;
    }
    case WM_MOUSEMOVE:
        if (menu_on && !capture_what) nav_push(INPUT_NAV_POINTER, INPUT_SRC_MOUSE, 0, 0, (int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        return 0;
    case WM_MOUSEWHEEL:
        if (!menu_on || capture_what) return 0;
        nav_push((short)HIWORD(wp) > 0 ? INPUT_NAV_UP : INPUT_NAV_DOWN, INPUT_SRC_MOUSE, 0, 0, 0, 0);
        return 1;
    case WM_ACTIVATEAPP:
        if (!wp) InterlockedExchange(&swallow_vk, 0);
        return 0;
    }
    return 0;
}
