/* Sly2-Setup: downloads over HTTP(S) with WinHTTP, resumable (Range requests), with clear errors. */
#include "setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <winhttp.h>


static void net_error(setup_t* s, const wchar_t* host, DWORD err) {
    const wchar_t* why;
    switch (err) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
        why = L"could not connect: this PC seems to be offline (or a firewall blocks Setup)"; break;
    case ERROR_WINHTTP_TIMEOUT: why = L"the connection timed out"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: why = L"the connection was interrupted"; break;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        why = L"the secure connection failed (check the PC's date and time, or a proxy / antivirus that intercepts HTTPS)"; break;
    default: why = L"network error"; break;
    }
    wfmt(s->error, sizeof s->error / sizeof s->error[0], L"Downloading from %ls failed: %ls (error %lu).", host, why, err);
}

/* One attempt. 1 = complete, 0 = failed (s->error), -2 = cancelled, 2 = failed but worth retrying. */
static int attempt(setup_t* s, const wchar_t* url, const wchar_t* part, uint64_t expect, const wchar_t* label) {
    URL_COMPONENTSW uc;
    wchar_t host[256], path[2048];
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        wfmt(s->error, sizeof s->error / sizeof s->error[0], L"Bad download address: %ls", url);
        return 0;
    }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) wcsncat(path, uc.lpszExtraInfo, uc.dwExtraInfoLength);

    uint64_t have = file_size(part);
    if (have == (uint64_t)-1) have = 0;
    if (expect && have > expect) { DeleteFileW(part); have = 0; }
    if (expect && have == expect) return 1;          /* complete from an earlier run (the hash decides) */

    HINTERNET ses = WinHttpOpen(L"Sly2-Setup/" SETUP_VERSION, 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 8.1+ */,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"Sly2-Setup/" SETUP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { net_error(s, host, GetLastError()); return 0; }
    WinHttpSetTimeouts(ses, 30000, 30000, 30000, 60000);
    DWORD protos = 0x00000800 | 0x00002000;          /* TLS 1.2, TLS 1.3 */
    WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : NULL;
    int result = 2;
    HANDLE f = INVALID_HANDLE_VALUE;
    uint8_t* buf = NULL;
    if (!req) { net_error(s, host, GetLastError()); goto done; }
    wchar_t range[64] = L"";
    if (have) wfmt(range, 64, L"Range: bytes=%llu-", (unsigned long long)have);
    if (!WinHttpSendRequest(req, have ? range : WINHTTP_NO_ADDITIONAL_HEADERS, have ? (DWORD)-1L : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        DWORD e = GetLastError();
        net_error(s, host, e);
        result = (e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT) ? 0 : 2;
        goto done;
    }
    DWORD status = 0, n = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &n, WINHTTP_NO_HEADER_INDEX);
    if (status == 416 && have && (!expect || have == expect)) { result = 1; goto done; }   /* already complete */
    if (status == 416) { DeleteFileW(part); have = 0; goto done; }                       /* stale part: again */
    if (status != 200 && status != 206) {
        wfmt(s->error, sizeof s->error / sizeof s->error[0],
             L"Downloading %ls failed: the server %ls answered %lu%ls.", label, host, status,
             status == 404 ? L" (file not found)" : L"");
        result = status >= 500 ? 2 : 0;
        goto done;
    }
    uint64_t total = 0;
    wchar_t clen[32];
    n = sizeof clen;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen, &n, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(clen, NULL, 10);
    if (status == 200) {                             /* no resume support: start over */
        if (have) loga("  the server does not resume: downloading from the start");
        have = 0;
    } else {
        loga("  resuming at %llu bytes", (unsigned long long)have);
        total += have;
    }
    if (!total) total = expect;
    f = CreateFileW(part, GENERIC_WRITE, FILE_SHARE_READ, NULL, have ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        wfmt(s->error, sizeof s->error / sizeof s->error[0], L"Cannot write %ls (error %lu).", part, GetLastError());
        result = 0;
        goto done;
    }
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)have;
    SetFilePointerEx(f, li, NULL, FILE_BEGIN);
    SetEndOfFile(f);
    enum { CHUNK = 256 << 10 };
    buf = malloc(CHUNK);
    uint64_t done = have;
    DWORD last = 0;
    for (;;) {
        if (s->cancel) { result = -2; goto done; }
        DWORD got = 0, w = 0;
        if (!WinHttpReadData(req, buf, CHUNK, &got)) { net_error(s, host, GetLastError()); result = 2; goto done; }
        if (!got) break;
        if (!WriteFile(f, buf, got, &w, NULL) || w != got) {
            wfmt(s->error, sizeof s->error / sizeof s->error[0], L"Cannot write %ls (error %lu: is the disk full?).", part,
                 GetLastError());
            result = 0;
            goto done;
        }
        done += got;
        if (GetTickCount() - last > 100) { last = GetTickCount(); setup_bytes(s, label, done, total); }
    }
    setup_bytes(s, label, done, total);
    if (total && done != total) {
        wfmt(s->error, sizeof s->error / sizeof s->error[0], L"The download of %ls ended early (%llu of %llu bytes).", label,
             (unsigned long long)done, (unsigned long long)total);
        result = 2;
        goto done;
    }
    result = 1;
done:
    free(buf);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return result;
}

int net_download(setup_t* s, const wchar_t* url, const wchar_t* part, uint64_t expect, const wchar_t* label, int tries) {
    for (int i = 0; i < tries; i++) {
        if (i) {
            loga("  retrying (%d of %d)", i + 1, tries);
            for (int t = 0; t < 20 && !s->cancel; t++) Sleep(100 * (i < 3 ? i : 3));
        }
        s->error[0] = 0;
        int r = attempt(s, url, part, expect, label);
        if (r == 1) return 0;
        if (r == -2 || s->cancel) return -2;
        logw(L"  %ls", s->error);
        if (r == 0) return 1;
    }
    return 1;
}
