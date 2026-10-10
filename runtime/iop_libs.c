/* Native IOP libraries used by the recompiled sound modules: sysclib, stdio, ioman, the Logitech
 * USB headset driver (lgaud: reports no headset) and libsd, whose calls go to the native sound
 * backend declared in sd.h. Signatures follow Sony's library headers (also in ps2sdk). */
#include "iop_internal.h"
#include "sd.h"

#include <stdlib.h>
#include <windows.h>

/* Host pointer for [a, a+n) if it lies inside one view of IOP RAM, else NULL. */
static uint8_t* span(uint32_t a, uint32_t n) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p >= 0x00800000u) return NULL;
    uint32_t off = p & (IOP_RAM_SIZE - 1);
    return off + n <= IOP_RAM_SIZE ? iop_ram + off : NULL;
}

static void guest_memmove(uint32_t dst, uint32_t src, uint32_t n) {
    uint8_t *d = span(dst, n), *s = span(src, n);
    if (d && s) { memmove(d, s, n); return; }
    if (dst <= src) for (uint32_t i = 0; i < n; i++) iop_w8(dst + i, iop_r8(src + i));
    else for (uint32_t i = n; i-- > 0;) iop_w8(dst + i, iop_r8(src + i));
}

/* ---- sysclib ------------------------------------------------------------------------------------- */

void iop_imp_sysclib_memcpy(iop_ctx* c) { guest_memmove(c->r[4], c->r[5], c->r[6]); c->r[2] = c->r[4]; }
void iop_imp_sysclib_memmove(iop_ctx* c) { guest_memmove(c->r[4], c->r[5], c->r[6]); c->r[2] = c->r[4]; }

void iop_imp_sysclib_memset(iop_ctx* c) {
    uint32_t dst = c->r[4], n = c->r[6];
    uint8_t* d = span(dst, n);
    if (d) memset(d, (int)(c->r[5] & 0xFF), n);
    else for (uint32_t i = 0; i < n; i++) iop_w8(dst + i, (uint8_t)c->r[5]);
    c->r[2] = dst;
}

void iop_imp_sysclib_bzero(iop_ctx* c) {
    uint32_t dst = c->r[4], n = c->r[5];
    uint8_t* d = span(dst, n);
    if (d) memset(d, 0, n);
    else for (uint32_t i = 0; i < n; i++) iop_w8(dst + i, 0);
}

/* wmemcopy(u32* dest, const u32* src, size_t bytes): word copy */
void iop_imp_sysclib_wmemcopy(iop_ctx* c) {
    uint32_t dst = c->r[4], src = c->r[5], n = c->r[6];
    for (uint32_t i = 0; i < n; i += 4) iop_w32(dst + i, iop_r32(src + i));
    c->r[2] = dst + ((n + 3) & ~3u);
}

static int guest_strncmp(uint32_t a, uint32_t b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = iop_r8(a + i), y = iop_r8(b + i);
        if (x != y) return x < y ? -1 : 1;
        if (!x) break;
    }
    return 0;
}
void iop_imp_sysclib_strcmp(iop_ctx* c) { iop_ret(c, guest_strncmp(c->r[4], c->r[5], 0xFFFFFFFFu)); }
void iop_imp_sysclib_strncmp(iop_ctx* c) { iop_ret(c, guest_strncmp(c->r[4], c->r[5], c->r[6])); }

void iop_imp_sysclib_strtol(iop_ctx* c) {                  /* (str, char** end, base) */
    char buf[64];
    iop_strncpy_from(buf, c->r[4], sizeof buf);
    char* end;
    long v = strtol(buf, &end, (int)c->r[6]);
    if (c->r[5]) iop_w32(c->r[5], c->r[4] + (uint32_t)(end - buf));
    iop_ret(c, (int32_t)v);
}

/* longjmp: spiop only uses it on fatal error paths (with a jmp_buf nobody set). Stop the thread. */
void iop_imp_sysclib_longjmp(iop_ctx* c) {
    fprintf(stderr, "[iop] longjmp(0x%08X, %d) from 0x%08X: fatal error path in IOP thread %d; thread stopped\n",
            c->r[4], (int)c->r[5], c->r[31], iop_thread_current());
    iop_dump_trace("longjmp");
    for (;;) if (iop_block(IOP_WAIT_SLEEP, 0xDEAD) == KE_ILLEGAL_CONTEXT) return;
}

/* ---- stdio: printf with the guest's varargs (o32: a1-a3, then the stack) ----------------------- */

typedef struct { iop_ctx* c; int next; } va_guest;
static uint32_t next_arg(va_guest* va) { return iop_arg(va->c, va->next++); }

static char line[512];
static size_t line_len;

static void out_char(char ch) {
    if (ch == '\n' || line_len == sizeof line - 1) {
        line[line_len] = 0;
        fprintf(stderr, "[iop] %s\n", line);
        line_len = 0;
        if (ch == '\n') return;
    }
    line[line_len++] = ch;
}

static void out_str(const char* s, size_t n) { for (size_t i = 0; i < n; i++) out_char(s[i]); }

/* Format guest `fmt` with arguments starting at argument index `first`; returns characters written. */
static int guest_printf(iop_ctx* c, uint32_t fmt, int first) {
    va_guest va = { c, first };
    int total = 0;
    for (uint32_t p = fmt;; p++) {
        char ch = (char)iop_r8(p);
        if (!ch) break;
        if (ch != '%') { out_char(ch); total++; continue; }
        char spec[32] = "%";
        size_t k = 1;
        for (ch = (char)iop_r8(++p); ch && strchr("-+ #0", ch) && k < 8; ch = (char)iop_r8(++p)) spec[k++] = ch;
        if (ch == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)next_arg(&va)); ch = (char)iop_r8(++p); }
        for (; ch >= '0' && ch <= '9' && k < 16; ch = (char)iop_r8(++p)) spec[k++] = ch;
        if (ch == '.') {
            spec[k++] = ch;
            ch = (char)iop_r8(++p);
            if (ch == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)next_arg(&va)); ch = (char)iop_r8(++p); }
            for (; ch >= '0' && ch <= '9' && k < 24; ch = (char)iop_r8(++p)) spec[k++] = ch;
        }
        while (ch == 'l' || ch == 'h' || ch == 'q' || ch == 'L') ch = (char)iop_r8(++p);   /* 32-bit anyway */
        if (!ch) break;
        char buf[256];
        int n = 0;
        switch (ch) {
        case 'd': case 'i': spec[k++] = 'd'; spec[k] = 0; n = snprintf(buf, sizeof buf, spec, (int32_t)next_arg(&va)); break;
        case 'u': case 'x': case 'X': case 'o':
            spec[k++] = ch; spec[k] = 0; n = snprintf(buf, sizeof buf, spec, next_arg(&va)); break;
        case 'p': n = snprintf(buf, sizeof buf, "%08x", next_arg(&va)); break;
        case 'c': spec[k++] = 'c'; spec[k] = 0; n = snprintf(buf, sizeof buf, spec, (int)(next_arg(&va) & 0xFF)); break;
        case 's': {
            char str[200];
            uint32_t a = next_arg(&va);
            if (a) iop_strncpy_from(str, a, sizeof str); else strcpy(str, "(null)");
            spec[k++] = 's'; spec[k] = 0;
            n = snprintf(buf, sizeof buf, spec, str);
            break;
        }
        case '%': buf[0] = '%'; n = 1; break;
        default: buf[0] = '%'; buf[1] = ch; n = 2; break;
        }
        if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
        if (n > 0) { out_str(buf, (size_t)n); total += n; }
    }
    return total;
}

void iop_imp_stdio_printf(iop_ctx* c) { iop_ret(c, guest_printf(c, c->r[4], 1)); }
void iop_imp_sysmem_Kprintf(iop_ctx* c) { iop_ret(c, guest_printf(c, c->r[4], 1)); }

/* ---- ioman: loads by file name are not used by Sly 2; nothing to open ---------------------------- */

static void ioman_unsupported(iop_ctx* c, const char* what) {
    static int logged;
    if (logged++ < 4) {
        char name[128] = "";
        if (strcmp(what, "open") == 0) iop_strncpy_from(name, c->r[4], sizeof name);
        fprintf(stderr, "[iop] ioman %s(%s) not supported\n", what, name);
    }
    iop_ret(c, -2);                                   /* -ENOENT */
}
void iop_imp_ioman_open(iop_ctx* c) { ioman_unsupported(c, "open"); }
void iop_imp_ioman_close(iop_ctx* c) { ioman_unsupported(c, "close"); }
void iop_imp_ioman_read(iop_ctx* c) { ioman_unsupported(c, "read"); }
void iop_imp_ioman_lseek(iop_ctx* c) { ioman_unsupported(c, "lseek"); }

/* ---- lgaud (Logitech USB headset): no device ---------------------------------------------------------
 * spiop's headset thread polls #18 (device count, written through a0) every 500 ms and only uses
 * the other functions once a device is present, so #18 reports 0 devices and the rest fail. */
void iop_imp_lgaud_18(iop_ctx* c) { if (c->r[4]) iop_w32(c->r[4], 0); iop_ret(c, 0); }
#define LGAUD_FAIL(n) void iop_imp_lgaud_##n(iop_ctx* c) { iop_ret(c, -1); }
LGAUD_FAIL(4) LGAUD_FAIL(5) LGAUD_FAIL(6) LGAUD_FAIL(7) LGAUD_FAIL(11) LGAUD_FAIL(13) LGAUD_FAIL(14)
LGAUD_FAIL(15) LGAUD_FAIL(16) LGAUD_FAIL(19) LGAUD_FAIL(22)

/* ---- libsd -> native sound backend (sd.h) ------------------------------------------------------------ */

void iop_imp_libsd_sceSdInit(iop_ctx* c) { iop_ret(c, sceSdInit((int)c->r[4])); }
void iop_imp_libsd_sceSdSetParam(iop_ctx* c) { sceSdSetParam((uint16_t)c->r[4], (uint16_t)c->r[5]); }
void iop_imp_libsd_sceSdGetParam(iop_ctx* c) { iop_ret(c, sceSdGetParam((uint16_t)c->r[4])); }
void iop_imp_libsd_sceSdSetSwitch(iop_ctx* c) { sceSdSetSwitch((uint16_t)c->r[4], c->r[5]); }
void iop_imp_libsd_sceSdGetSwitch(iop_ctx* c) { iop_ret(c, (int32_t)sceSdGetSwitch((uint16_t)c->r[4])); }
void iop_imp_libsd_sceSdSetAddr(iop_ctx* c) { sceSdSetAddr((uint16_t)c->r[4], c->r[5]); }
void iop_imp_libsd_sceSdGetAddr(iop_ctx* c) { iop_ret(c, (int32_t)sceSdGetAddr((uint16_t)c->r[4])); }
void iop_imp_libsd_sceSdSetCoreAttr(iop_ctx* c) { sceSdSetCoreAttr((uint16_t)c->r[4], (uint16_t)c->r[5]); }
void iop_imp_libsd_sceSdNote2Pitch(iop_ctx* c) {
    iop_ret(c, sceSdNote2Pitch((uint16_t)c->r[4], (uint16_t)c->r[5], (uint16_t)c->r[6], (int16_t)c->r[7]));
}
void iop_imp_libsd_sceSdVoiceTrans(iop_ctx* c) {          /* (channel, mode, iop addr, spu addr, size) */
    iop_ret(c, sceSdVoiceTrans((int16_t)c->r[4], (uint16_t)c->r[5], c->r[6], c->r[7], iop_arg(c, 4)));
}
void iop_imp_libsd_sceSdVoiceTransStatus(iop_ctx* c) { iop_ret(c, (int32_t)sceSdVoiceTransStatus((int16_t)c->r[4], (int16_t)c->r[5])); }
void iop_imp_libsd_sceSdBlockTrans(iop_ctx* c) {          /* (channel, mode, iop addr, size, [start addr]) */
    iop_ret(c, sceSdBlockTrans((int16_t)c->r[4], (uint16_t)c->r[5], c->r[6], c->r[7], iop_arg(c, 4)));
}
void iop_imp_libsd_sceSdBlockTransStatus(iop_ctx* c) { iop_ret(c, (int32_t)sceSdBlockTransStatus((int16_t)c->r[4], (int16_t)c->r[5])); }
void iop_imp_libsd_sceSdSetEffectAttr(iop_ctx* c) {       /* (core, sceSdEffectAttr*) */
    uint32_t p = c->r[5];
    sd_effect_attr a = { (uint16_t)iop_r16(p), (int32_t)iop_r32(p + 4), (int16_t)iop_r16(p + 8),
                         (int16_t)iop_r16(p + 10), (int32_t)iop_r32(p + 12), (int32_t)iop_r32(p + 16) };
    iop_ret(c, sceSdSetEffectAttr((int)c->r[4], &a));
}
void iop_imp_libsd_sceSdClearEffectWorkArea(iop_ctx* c) {
    iop_ret(c, sceSdClearEffectWorkArea((int)c->r[4], (int)c->r[5], (int)c->r[6]));
}

/* Transfer-completion interrupt: sceSdSetTransIntrHandler(channel, handler, arg) registers the
 * guest handler; the backend reports completions through iop_sd_transfer_done() (any thread),
 * and the handler runs in interrupt context at the next interrupt delivery point. */
static struct { uint32_t fn, arg, gp; } trans_handler[2];
static volatile LONG sd_pending;

void iop_imp_libsd_sceSdSetTransIntrHandler(iop_ctx* c) {
    int ch = (int)(c->r[4] & 1);
    uint32_t old = trans_handler[ch].fn;
    trans_handler[ch].fn = c->r[5];
    trans_handler[ch].arg = c->r[6];
    trans_handler[ch].gp = c->r[28];
    iop_ret(c, (int32_t)old);
}

void iop_sd_transfer_done(int channel) {
    InterlockedOr(&sd_pending, 1 << (channel & 1));
    iop_poll_flag = 1;
}

void iop_sd_service(void) {
    LONG p = InterlockedExchange(&sd_pending, 0);
    for (int ch = 0; ch < 2; ch++)
        if ((p & (1 << ch)) && trans_handler[ch].fn)
            iop_intr_call(trans_handler[ch].fn, (uint32_t)ch, trans_handler[ch].arg, trans_handler[ch].gp);
}
