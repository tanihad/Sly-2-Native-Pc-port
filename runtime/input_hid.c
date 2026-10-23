/* The input thread: HID controllers (DualSense, DualShock 4, generic gamepads) through Windows' own
 * HID API, hot-plug, and the per-tick service of input.c (XInput probing, XInput rumble).
 *
 * Only system DLLs, loaded at run time: hid.dll (HidD_* / HidP_*), setupapi.dll (enumeration of
 * HID interfaces) and cfgmgr32.dll (CM_Register_Notification: arrival / removal of HID interfaces,
 * USB and Bluetooth alike). Without cfgmgr32 notifications the thread re-enumerates every 2 s.
 *
 * Each open controller has one overlapped ReadFile pending; the thread waits on all of them plus a
 * stop, a "kick" (an output changed) and a hot-plug event, at most 50 ms at a time. A read that fails
 * (device gone: unplugged, Bluetooth off) closes the device; the game keeps running.
 *
 * PlayStation controllers: at open the thread reads feature reports 0x05 (calibration; over
 * Bluetooth this switches the controller to its full reports 0x31 / 0x11) and, on a DualSense,
 * 0x20 (firmware: chooses the rumble flag), then sends output reports: DualSense first "release the
 * LEDs", then light bar (Sly blue) + player LED (port 0: the middle LED) + motors; DualShock 4
 * light bar + motors. Bluetooth output reports carry a CRC-32 and (DualSense) a 4-bit sequence.
 * Outputs are sent when they change, and again 1.5 s and 11 s after connecting (the light bar
 * animation of a fresh Bluetooth connection would otherwise override the colour). */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "input_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#define HID_MAX 12
#define READ_MAX 1024
#define REJECT_MAX 256

static void hlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[input] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

/* ---- run-time linked system functions ------------------------------------------------------------------- */
typedef void (WINAPI* HidD_GetHidGuid_t)(LPGUID);
typedef BOOLEAN (WINAPI* HidD_GetAttributes_t)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN (WINAPI* HidD_GetPreparsedData_t)(HANDLE, PHIDP_PREPARSED_DATA*);
typedef BOOLEAN (WINAPI* HidD_FreePreparsedData_t)(PHIDP_PREPARSED_DATA);
typedef BOOLEAN (WINAPI* HidD_GetFeature_t)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI* HidD_SetOutputReport_t)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI* HidD_GetProductString_t)(HANDLE, PVOID, ULONG);
typedef NTSTATUS (NTAPI* HidP_GetCaps_t)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
typedef NTSTATUS (NTAPI* HidP_GetValueCaps_t)(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS, PUSHORT, PHIDP_PREPARSED_DATA);
typedef NTSTATUS (NTAPI* HidP_GetUsages_t)(HIDP_REPORT_TYPE, USAGE, USHORT, PUSAGE, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
typedef NTSTATUS (NTAPI* HidP_GetUsageValue_t)(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
typedef HDEVINFO (WINAPI* SetupDiGetClassDevsW_t)(const GUID*, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI* SetupDiEnumDeviceInterfaces_t)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI* SetupDiGetDeviceInterfaceDetailW_t)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W,
                                                          DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI* SetupDiDestroyDeviceInfoList_t)(HDEVINFO);
typedef CONFIGRET (WINAPI* CM_Register_Notification_t)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
typedef CONFIGRET (WINAPI* CM_Unregister_Notification_t)(HCMNOTIFICATION);

static struct {
    HidD_GetHidGuid_t GetHidGuid;
    HidD_GetAttributes_t GetAttributes;
    HidD_GetPreparsedData_t GetPreparsedData;
    HidD_FreePreparsedData_t FreePreparsedData;
    HidD_GetFeature_t GetFeature;
    HidD_SetOutputReport_t SetOutputReport;
    HidD_GetProductString_t GetProductString;
    HidP_GetCaps_t GetCaps;
    HidP_GetValueCaps_t GetValueCaps;
    HidP_GetUsages_t GetUsages;
    HidP_GetUsageValue_t GetUsageValue;
    SetupDiGetClassDevsW_t GetClassDevs;
    SetupDiEnumDeviceInterfaces_t EnumInterfaces;
    SetupDiGetDeviceInterfaceDetailW_t GetInterfaceDetail;
    SetupDiDestroyDeviceInfoList_t DestroyList;
    CM_Register_Notification_t RegisterNotification;
    CM_Unregister_Notification_t UnregisterNotification;
} api;

static int load_api(void) {
    HMODULE hid = LoadLibraryW(L"hid.dll"), sa = LoadLibraryW(L"setupapi.dll"), cm = LoadLibraryW(L"cfgmgr32.dll");
#define GET(m, field, name) api.field = (void*)GetProcAddress(m, name)
    if (hid) {
        GET(hid, GetHidGuid, "HidD_GetHidGuid"); GET(hid, GetAttributes, "HidD_GetAttributes");
        GET(hid, GetPreparsedData, "HidD_GetPreparsedData"); GET(hid, FreePreparsedData, "HidD_FreePreparsedData");
        GET(hid, GetFeature, "HidD_GetFeature"); GET(hid, SetOutputReport, "HidD_SetOutputReport");
        GET(hid, GetProductString, "HidD_GetProductString"); GET(hid, GetCaps, "HidP_GetCaps");
        GET(hid, GetValueCaps, "HidP_GetValueCaps"); GET(hid, GetUsages, "HidP_GetUsages");
        GET(hid, GetUsageValue, "HidP_GetUsageValue");
    }
    if (sa) {
        GET(sa, GetClassDevs, "SetupDiGetClassDevsW"); GET(sa, EnumInterfaces, "SetupDiEnumDeviceInterfaces");
        GET(sa, GetInterfaceDetail, "SetupDiGetDeviceInterfaceDetailW"); GET(sa, DestroyList, "SetupDiDestroyDeviceInfoList");
    }
    if (cm) { GET(cm, RegisterNotification, "CM_Register_Notification"); GET(cm, UnregisterNotification, "CM_Unregister_Notification"); }
#undef GET
    return api.GetHidGuid && api.GetAttributes && api.GetPreparsedData && api.FreePreparsedData && api.GetCaps &&
           api.GetUsages && api.GetUsageValue && api.GetValueCaps && api.GetClassDevs && api.EnumInterfaces &&
           api.GetInterfaceDetail && api.DestroyList;
}

/* ---- devices ------------------------------------------------------------------------------------------------ */

enum { AX_X, AX_Y, AX_Z, AX_RX, AX_RY, AX_RZ, AX_HAT, AX_N };
static const USAGE ax_usage[AX_N] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x39 };

typedef struct {
    int used, id;
    int kind, bt;                                /* INPUT_DEV_* */
    wchar_t path[512];
    HANDLE h;
    HIDP_CAPS caps;
    PHIDP_PREPARSED_DATA pp;                     /* generic pads */
    OVERLAPPED rov;
    uint8_t rbuf[READ_MAX];
    int can_write;
    uint8_t* wbuf;                               /* output report padded to caps.OutputReportByteLength */
    OVERLAPPED wov;
    /* PlayStation outputs */
    int vib_v2, released, write_errors;
    uint8_t seq;
    ps_output last;
    int have_last;
    uint64_t opened_at;
    int refresh_stage;
    int enhanced;
    uint32_t reports;
    /* generic: logical ranges and the last values */
    int has_ax[AX_N];
    LONG ax_min[AX_N], ax_max[AX_N];
    int ax_val[6], hat;
    uint32_t gbuttons;
} hid_dev;

static hid_dev hdev[HID_MAX];
static uint64_t rejected[REJECT_MAX];            /* path hashes of HID interfaces that are not controllers */
static int nrejected;
static GUID hid_guid;
static HANDLE ev_stop, ev_kick, ev_plug, thread;
static volatile LONG started;

static uint64_t path_hash(const wchar_t* p) {
    uint64_t h = 1469598103934665603ull;
    for (; *p; p++) { wchar_t c = (wchar_t)towlower(*p); h = (h ^ (uint64_t)c) * 1099511628211ull; }
    return h;
}

static int is_rejected(uint64_t h) {
    for (int i = 0; i < nrejected; i++) if (rejected[i] == h) return 1;
    return 0;
}

static void reject(uint64_t h) { if (nrejected < REJECT_MAX && !is_rejected(h)) rejected[nrejected++] = h; }

static void start_read(hid_dev* d);

static void close_dev(hid_dev* d, const char* why) {
    if (!d->used) return;
    if (d->id) input_dev_remove(d->id);
    if (why) hlog("closed HID controller %d: %s", d->id, why);
    CancelIoEx(d->h, NULL);
    DWORD n;
    GetOverlappedResult(d->h, &d->rov, &n, TRUE);             /* (the cancelled read finishes) */
    CloseHandle(d->h);
    if (d->rov.hEvent) CloseHandle(d->rov.hEvent);
    if (d->wov.hEvent) CloseHandle(d->wov.hEvent);
    if (d->pp) api.FreePreparsedData(d->pp);
    free(d->wbuf);
    memset(d, 0, sizeof *d);
}

/* Write an output report (padded to the device's output report length); 1 if sent. */
static int write_report(hid_dev* d, const uint8_t* rep, size_t len) {
    size_t cap = d->caps.OutputReportByteLength;
    if (!d->can_write || !cap || !d->wbuf) return 0;
    memset(d->wbuf, 0, cap);
    memcpy(d->wbuf, rep, len < cap ? len : cap);
    DWORD n = 0;
    ResetEvent(d->wov.hEvent);
    BOOL ok = WriteFile(d->h, d->wbuf, (DWORD)cap, NULL, &d->wov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(d->wov.hEvent, 200) != WAIT_OBJECT_0) {
            CancelIoEx(d->h, &d->wov);
            GetOverlappedResult(d->h, &d->wov, &n, TRUE);
            ok = FALSE;
        } else {
            ok = GetOverlappedResult(d->h, &d->wov, &n, FALSE);
        }
    }
    if (!ok && api.SetOutputReport) ok = api.SetOutputReport(d->h, d->wbuf, (ULONG)cap);   /* control pipe */
    if (!ok && d->write_errors++ < 3)
        hlog("controller %d: output report 0x%02X not sent (error %lu)", d->id, rep[0], (unsigned long)GetLastError());
    return ok != 0;
}

static int get_feature(hid_dev* d, uint8_t id, uint8_t* buf, size_t cap) {
    size_t len = d->caps.FeatureReportByteLength;
    if (!api.GetFeature || !len || len > cap) return 0;
    memset(buf, 0, len);
    buf[0] = id;
    return api.GetFeature(d->h, buf, (ULONG)len) ? (int)len : 0;
}

/* PlayStation output: the state the device should show now (motors, light bar, player LED). */
static void ps_update_output(hid_dev* d, int force) {
    if (!d->can_write || (d->kind != INPUT_DEV_DUALSENSE && d->kind != INPUT_DEV_DUALSENSE_EDGE && d->kind != INPUT_DEV_DS4))
        return;
    input_config c;
    input_config_get(&c);
    ps_output o;
    memset(&o, 0, sizeof o);
    if (!input_dev_motors(d->id, &o.motor_small, &o.motor_large)) return;
    o.lightbar_on = c.lightbar_on;
    memcpy(o.rgb, c.lightbar, 3);
    int port = input_dev_port(d->id);
    o.player_leds = port == 0 ? 0x04 : port == 1 ? 0x0A : 0x00;   /* middle LED / the two around it */
    o.vibration_v2 = d->vib_v2;
    uint64_t now = input_now_ms();
    if (d->refresh_stage == 0 && now - d->opened_at >= 1500) { d->refresh_stage = 1; force = 1; }
    if (d->refresh_stage == 1 && now - d->opened_at >= 11000) { d->refresh_stage = 2; force = 1; }
    if (!force && d->have_last && !memcmp(&o, &d->last, sizeof o)) return;
    uint8_t rep[PS_OUTPUT_MAX];
    size_t len;
    if (d->kind == INPUT_DEV_DS4) {
        len = ps_build_ds4(&o, d->bt, rep);
    } else {
        if (!d->released) {                                   /* first: take the light bar over */
            ps_output r = o;
            r.release_leds = 1;
            r.seq = d->seq++;
            len = ps_build_dualsense(&r, d->bt, rep);
            write_report(d, rep, len);
            d->released = 1;
        }
        o.seq = d->seq++;
        len = ps_build_dualsense(&o, d->bt, rep);
        o.seq = 0;
    }
    if (write_report(d, rep, len)) input_dev_sent(d->id, o.motor_small, o.motor_large);
    d->last = o;
    d->have_last = 1;
}

/* Generic pad: buttons, hat and axes from one input report. */
static void generic_parse(hid_dev* d, uint8_t* r, ULONG n) {
    USAGE usages[64];
    ULONG count = 64;
    if (api.GetUsages(HidP_Input, 0x09, 0, usages, &count, d->pp, (PCHAR)r, n) == HIDP_STATUS_SUCCESS) {
        d->gbuttons = 0;
        for (ULONG i = 0; i < count; i++) if (usages[i] >= 1 && usages[i] <= 32) d->gbuttons |= 1u << (usages[i] - 1);
    }
    for (int a = 0; a < AX_N; a++) {
        if (!d->has_ax[a]) continue;
        ULONG v;
        if (api.GetUsageValue(HidP_Input, 0x01, 0, ax_usage[a], &v, d->pp, (PCHAR)r, n) != HIDP_STATUS_SUCCESS) continue;
        LONG lo = d->ax_min[a], hi = d->ax_max[a], x = (LONG)v;
        if (a == AX_HAT) {
            LONG span = hi - lo + 1;
            d->hat = x < lo || x > hi ? -1 : span == 8 ? (int)(x - lo) : span == 4 ? (int)(x - lo) * 2 : -1;
            continue;
        }
        if (hi <= lo) continue;
        long long s = ((long long)(x - lo) * 65535) / (hi - lo) - 32768;
        d->ax_val[a] = (int)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
    }
    int axes[6];
    for (int a = 0; a < 6; a++) axes[a] = d->has_ax[a] ? d->ax_val[a] : INPUT_AXIS_ABSENT;
    input_state st;
    input_generic_to_state(d->gbuttons, d->has_ax[AX_HAT] ? d->hat : -1, axes, &st);
    input_dev_update(d->id, &st, -1, 0, 0, 1);
}

static void handle_report(hid_dev* d, DWORD n) {
    d->reports++;
    if (d->kind == INPUT_DEV_GENERIC) { generic_parse(d, d->rbuf, n); return; }
    ps_report rep;
    int ok = d->kind == INPUT_DEV_DS4 ? ps_parse_ds4(d->rbuf, n, d->bt, &rep) : ps_parse_dualsense(d->rbuf, n, d->bt, &rep);
    if (!ok) return;
    if (rep.full && d->bt && !d->enhanced) { d->enhanced = 1; hlog("controller %d: full Bluetooth reports", d->id); }
    input_dev_update(d->id, &rep.st, rep.battery, rep.charging, d->bt ? d->enhanced : 1, rep.crc_ok);
}

static void start_read(hid_dev* d) {
    for (int tries = 0; tries < 64 && d->used; tries++) {
        ResetEvent(d->rov.hEvent);
        DWORD len = d->caps.InputReportByteLength;
        if (len > READ_MAX) len = READ_MAX;
        if (ReadFile(d->h, d->rbuf, len, NULL, &d->rov)) {     /* completed at once: take it, read on */
            DWORD n = 0;
            if (GetOverlappedResult(d->h, &d->rov, &n, FALSE)) { handle_report(d, n); continue; }
        }
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) return;
        char why[64];
        snprintf(why, sizeof why, "read error %lu", (unsigned long)e);
        close_dev(d, why);
        return;
    }
}

static void wide_to_utf8(const wchar_t* w, char* out, int cap) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}

/* A HID interface that appeared: open it if it is a controller. */
static void probe(const wchar_t* path) {
    uint64_t hash = path_hash(path);
    if (is_rejected(hash)) return;
    for (int i = 0; i < HID_MAX; i++) if (hdev[i].used && !_wcsicmp(hdev[i].path, path)) return;
    if (wcsstr(path, L"IG_") || wcsstr(path, L"ig_")) { reject(hash); return; }   /* XInput's own HID side */
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;                      /* (not ready yet: the next scan retries) */
    HIDD_ATTRIBUTES at;
    at.Size = sizeof at;
    PHIDP_PREPARSED_DATA pp = NULL;
    HIDP_CAPS caps;
    int ok = api.GetAttributes(h, &at) && api.GetPreparsedData(h, &pp);
    if (ok) ok = api.GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
    CloseHandle(h);
    if (!ok) { if (pp) api.FreePreparsedData(pp); reject(hash); return; }
    int kind = ps_kind(at.VendorID, at.ProductID);
    int pad = caps.UsagePage == 0x01 && (caps.Usage == 0x04 || caps.Usage == 0x05);
    if (!pad) {
        api.FreePreparsedData(pp);
        reject(hash);
        return;
    }
    if (kind == INPUT_DEV_NONE) kind = INPUT_DEV_GENERIC;
    hid_dev* d = NULL;
    for (int i = 0; i < HID_MAX && !d; i++) if (!hdev[i].used) d = &hdev[i];
    if (!d) { api.FreePreparsedData(pp); return; }
    HANDLE rw = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, NULL);
    int can_write = rw != INVALID_HANDLE_VALUE;
    if (!can_write)
        rw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (rw == INVALID_HANDLE_VALUE) {
        hlog("cannot open controller %04X:%04X (error %lu)", at.VendorID, at.ProductID, (unsigned long)GetLastError());
        api.FreePreparsedData(pp);
        return;
    }
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->kind = kind;
    d->h = rw;
    d->caps = caps;
    d->can_write = can_write && caps.OutputReportByteLength > 0 && kind != INPUT_DEV_GENERIC;
    wcsncpy(d->path, path, 511);
    d->rov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->wov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (d->can_write) d->wbuf = (uint8_t*)calloc(1, caps.OutputReportByteLength + 1);
    /* USB PlayStation controllers report 64-byte input reports; over Bluetooth they are longer. */
    d->bt = kind != INPUT_DEV_GENERIC && caps.InputReportByteLength != 64;
    d->opened_at = input_now_ms();
    char name[64];
    if (kind == INPUT_DEV_GENERIC) {
        d->pp = pp;
        HIDP_VALUE_CAPS vc[32];
        USHORT nvc = 32;
        if (api.GetValueCaps(HidP_Input, vc, &nvc, pp) == HIDP_STATUS_SUCCESS)
            for (USHORT i = 0; i < nvc; i++) {
                if (vc[i].UsagePage != 0x01) continue;
                for (int a = 0; a < AX_N; a++) {
                    USAGE u = vc[i].IsRange ? vc[i].Range.UsageMin : vc[i].NotRange.Usage;
                    USAGE u2 = vc[i].IsRange ? vc[i].Range.UsageMax : u;
                    if (ax_usage[a] < u || ax_usage[a] > u2 || d->has_ax[a]) continue;
                    d->has_ax[a] = 1;
                    d->ax_min[a] = vc[i].LogicalMin;
                    d->ax_max[a] = vc[i].LogicalMax;
                    if (d->ax_max[a] <= d->ax_min[a] && vc[i].BitSize > 0 && vc[i].BitSize < 32) {   /* unsigned range */
                        d->ax_min[a] = 0;
                        d->ax_max[a] = (LONG)((1u << vc[i].BitSize) - 1);
                    }
                }
            }
        for (int a = 0; a < 6; a++) d->ax_val[a] = 0;
        d->hat = -1;
        wchar_t prod[64] = L"";
        char p8[48] = "";
        if (api.GetProductString && api.GetProductString(rw, prod, sizeof prod)) wide_to_utf8(prod, p8, sizeof p8);
        snprintf(name, sizeof name, "%s", p8[0] ? p8 : "Gamepad (HID)");
    } else {
        api.FreePreparsedData(pp);
        const char* model = kind == INPUT_DEV_DS4 ? "DualShock 4" : kind == INPUT_DEV_DUALSENSE_EDGE ? "DualSense Edge" : "DualSense";
        snprintf(name, sizeof name, "%s (%s)", model, d->bt ? "Bluetooth" : "USB");
    }
    d->id = input_dev_add(kind, kind == INPUT_DEV_GENERIC ? INPUT_BUS_OTHER : d->bt ? INPUT_BUS_BT : INPUT_BUS_USB,
                          at.VendorID, at.ProductID, name, NULL, NULL, d->can_write);
    if (!d->id) { close_dev(d, NULL); return; }
    hlog("  HID: input report %u bytes, output %u, feature %u%s", caps.InputReportByteLength, caps.OutputReportByteLength,
         caps.FeatureReportByteLength, can_write ? "" : ", read-only (no rumble)");
    if (kind != INPUT_DEV_GENERIC) {
        uint8_t feat[1024];
        int n = get_feature(d, 0x05, feat, sizeof feat);       /* calibration: Bluetooth -> full reports */
        if (!n && d->bt && kind == INPUT_DEV_DS4) n = get_feature(d, 0x02, feat, sizeof feat);
        if (kind != INPUT_DEV_DS4) {
            n = get_feature(d, 0x20, feat, sizeof feat);
            unsigned fw = ps_dualsense_fw_version(feat, (size_t)n);
            d->vib_v2 = !n || fw == 0 || fw >= PS_DS_FW_VIBRATION_V2;
            hlog("  DualSense firmware update version 0x%04X: %s rumble", fw, d->vib_v2 ? "improved" : "classic");
        }
        ps_update_output(d, 1);
    }
    start_read(d);
}

/* Enumerate every present HID interface (setupapi). */
static void scan(void) {
    HDEVINFO set = api.GetClassDevs(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof ifd;
    union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; char raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024 * sizeof(wchar_t)]; } det;
    for (DWORD i = 0; api.EnumInterfaces(set, NULL, &hid_guid, i, &ifd); i++) {
        det.d.cbSize = sizeof det.d;
        if (!api.GetInterfaceDetail(set, &ifd, &det.d, sizeof det, NULL, NULL)) continue;
        probe(det.d.DevicePath);
    }
    api.DestroyList(set);
}

static DWORD CALLBACK on_plug(HCMNOTIFICATION n, PVOID ctx, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD size) {
    (void)n; (void)ctx; (void)data; (void)size;
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) SetEvent(ev_plug);
    return ERROR_SUCCESS;
}

static DWORD WINAPI input_thread(LPVOID arg) {
    (void)arg;
    int have_hid = load_api();
    HCMNOTIFICATION note = NULL;
    if (!have_hid) hlog("hid.dll / setupapi.dll unavailable: only XInput controllers and the keyboard");
    else {
        api.GetHidGuid(&hid_guid);
        if (api.RegisterNotification) {
            CM_NOTIFY_FILTER f;
            memset(&f, 0, sizeof f);
            f.cbSize = sizeof f;
            f.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
            f.u.DeviceInterface.ClassGuid = hid_guid;
            if (api.RegisterNotification(&f, NULL, on_plug, &note) != CR_SUCCESS) note = NULL;
        }
        if (!note) hlog("no device notifications: looking for controllers every 2 s");
        scan();
    }
    uint64_t next_scan = input_now_ms() + (note ? 0x7FFFFFFF : 2000), rescan_at = 0;
    for (;;) {
        HANDLE w[3 + HID_MAX];
        hid_dev* wd[3 + HID_MAX];
        DWORD n = 0;
        w[n++] = ev_stop; w[n++] = ev_kick; w[n++] = ev_plug;
        for (int i = 0; i < HID_MAX; i++) if (hdev[i].used) { wd[n] = &hdev[i]; w[n++] = hdev[i].rov.hEvent; }
        DWORD r = WaitForMultipleObjects(n, w, FALSE, 50);
        if (r == WAIT_OBJECT_0) break;
        uint64_t now = input_now_ms();
        if (have_hid && (r == WAIT_OBJECT_0 + 2 || now >= next_scan || (rescan_at && now >= rescan_at))) {
            rescan_at = r == WAIT_OBJECT_0 + 2 ? now + 750 : 0;  /* once more shortly: interfaces can be slow to open */
            if (!note) next_scan = now + 2000;
            if (r == WAIT_OBJECT_0 + 2) nrejected = 0;          /* (a new interface may reuse a path) */
            scan();
        }
        for (DWORD k = 3; k < n; k++) {                          /* every completed read, not only the first */
            hid_dev* d = wd[k];
            if (!d->used || WaitForSingleObject(d->rov.hEvent, 0) != WAIT_OBJECT_0) continue;
            DWORD got = 0;
            if (!GetOverlappedResult(d->h, &d->rov, &got, FALSE)) {
                char why[64];
                snprintf(why, sizeof why, "disconnected (error %lu)", (unsigned long)GetLastError());
                close_dev(d, why);
                continue;
            }
            handle_report(d, got);
            start_read(d);
        }
        input_service();
        for (int i = 0; i < HID_MAX; i++) if (hdev[i].used) ps_update_output(&hdev[i], 0);
    }
    /* stopping: motors off */
    for (int i = 0; i < HID_MAX; i++) {
        hid_dev* d = &hdev[i];
        if (!d->used || !d->can_write || !d->have_last) continue;
        ps_output o = d->last;
        o.motor_small = o.motor_large = 0;
        o.seq = d->seq++;
        uint8_t rep[PS_OUTPUT_MAX];
        size_t len = d->kind == INPUT_DEV_DS4 ? ps_build_ds4(&o, d->bt, rep) : ps_build_dualsense(&o, d->bt, rep);
        write_report(d, rep, len);
    }
    if (note && api.UnregisterNotification) api.UnregisterNotification(note);
    return 0;
}

void input_hid_start(void) {
    if (InterlockedExchange(&started, 1)) return;
    ev_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    ev_kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    ev_plug = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    if (!thread) hlog("cannot start the input thread (error %lu)", (unsigned long)GetLastError());
}

void input_hid_kick(void) { if (ev_kick) SetEvent(ev_kick); }

void input_hid_stop(void) {
    if (!thread) return;
    SetEvent(ev_stop);
    WaitForSingleObject(thread, 500);
}
