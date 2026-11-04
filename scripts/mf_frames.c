/* Video frames -> frame_NNNNNN.bmp (32-bit, top-down; what scripts/flicker_check.c reads) through Windows' own Media
 * Foundation decoder (no extra tools): for looking at the owner's screen recordings frame by frame.
 *
 *   mf_frames <video.mp4> <out dir> [--from N] [--to N] [--every K] [--info]
 * --info: print the size, frame rate and frame count only. Frames are numbered from 0 in decode order.
 * Build: clang -O2 -o build\mf_frames.exe scripts\mf_frames.c -lmfplat -lmfreadwrite -lmfuuid -lole32 */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_bmp(const char* path, const BYTE* px, LONG stride, UINT32 w, UINT32 h) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    BYTE hd[54] = { 'B', 'M' };
    UINT32 sz = 54 + w * h * 4, off = 54, hs = 40, img = w * h * 4;
    INT32 iw = (INT32)w, ih = -(INT32)h;
    WORD planes = 1, bpp = 32;
    memcpy(hd + 2, &sz, 4); memcpy(hd + 10, &off, 4); memcpy(hd + 14, &hs, 4); memcpy(hd + 18, &iw, 4);
    memcpy(hd + 22, &ih, 4); memcpy(hd + 26, &planes, 2); memcpy(hd + 28, &bpp, 2); memcpy(hd + 34, &img, 4);
    fwrite(hd, 1, 54, f);
    for (UINT32 y = 0; y < h; y++) fwrite(px + (LONG)y * stride, 4, w, f);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mf_frames <video> <out dir> [--from N] [--to N] [--every K] [--info]\n"); return 2; }
    long from = 0, to = 0x7FFFFFFF, every = 1;
    int info = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--info")) info = 1;
    }
    if (every < 1) every = 1;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { fprintf(stderr, "MFStartup failed\n"); return 2; }
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wpath, MAX_PATH);
    IMFAttributes* at = NULL;
    MFCreateAttributes(&at, 1);
    IMFAttributes_SetUINT32(at, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* rd = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath, at, &rd);
    if (FAILED(hr)) { fprintf(stderr, "cannot open %s (0x%08lX)\n", argv[1], hr); return 2; }
    const DWORD VS = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    IMFMediaType* mt = NULL;
    MFCreateMediaType(&mt);
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = IMFSourceReader_SetCurrentMediaType(rd, VS, NULL, mt);
    if (FAILED(hr)) { fprintf(stderr, "no RGB32 output (0x%08lX)\n", hr); return 2; }
    IMFMediaType* cur = NULL;
    IMFSourceReader_GetCurrentMediaType(rd, VS, &cur);
    UINT64 fs = 0, fr = 0;
    IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &fs);
    IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_RATE, &fr);
    UINT32 w = (UINT32)(fs >> 32), h = (UINT32)fs;
    UINT32 st32 = 0;
    LONG stride = SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &st32)) ? (LONG)(INT32)st32 : (LONG)w * 4;
    printf("video %ux%u, %.3f fps, stride %ld\n", w, h, (fr & 0xFFFFFFFF) ? (double)(fr >> 32) / (double)(fr & 0xFFFFFFFF) : 0.0, stride);
    if (!info) CreateDirectoryA(argv[2], NULL);
    long n = 0, written = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = NULL;
        hr = IMFSourceReader_ReadSample(rd, VS, 0, NULL, &flags, &ts, &s);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (s) IMFSample_Release(s); break; }
        if (!s) continue;
        if (!info && n >= from && n <= to && (n - from) % every == 0) {
            IMFMediaBuffer* b = NULL;
            IMFSample_ConvertToContiguousBuffer(s, &b);
            BYTE* p = NULL;
            DWORD len = 0;
            IMFMediaBuffer_Lock(b, &p, NULL, &len);
            const BYTE* top = stride < 0 ? p + (size_t)(h - 1) * (size_t)(-stride) : p;
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\frame_%06ld.bmp", argv[2], n);
            write_bmp(path, top, stride, w, h);
            IMFMediaBuffer_Unlock(b);
            IMFMediaBuffer_Release(b);
            written++;
        }
        IMFSample_Release(s);
        n++;
        if (n > to) break;
    }
    printf("%ld frames decoded, %ld written\n", n, written);
    IMFSourceReader_Release(rd);
    MFShutdown();
    return 0;
}
