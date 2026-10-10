/* Loading the recompiled IOP modules (what Sony's loadcore/modload do for an IRX).
 *
 * The EE copies each IRX file into IOP RAM and asks LOADFILE to load it from there. For a module
 * the recompiler translated (recomp/iop.py), the image is placed at the address the code was
 * recompiled for, relocated (R_MIPS_32/26/HI16/LO16, as loadcore would), and checked against the
 * CRCs recorded at build time: the recompiled C must match the module the game actually sends.
 * The module's start function then runs natively in a kernel thread with (argc, argv). */
#include "iop_internal.h"

#include <windows.h>

static uint32_t crc32_bytes(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* Apply the IRX's relocations to `img` (memsz bytes) linked at `base`. Returns 0 on bad input. */
static int relocate(uint8_t* img, uint32_t filesz, uint32_t base, const uint8_t* elf, uint32_t elf_size) {
    uint32_t shoff = rd32(elf + 0x20);
    uint16_t shentsize = rd16(elf + 0x2E), shnum = rd16(elf + 0x30);
    if ((uint64_t)shoff + (uint64_t)shentsize * shnum > elf_size) return 0;
    uint32_t pending[256];
    int npending = 0;
    for (uint16_t s = 0; s < shnum; s++) {
        const uint8_t* sh = elf + shoff + (uint32_t)s * shentsize;
        if (rd32(sh + 4) != 9) continue;                                /* SHT_REL */
        uint32_t off = rd32(sh + 16), size = rd32(sh + 20);
        if ((uint64_t)off + size > elf_size) return 0;
        for (uint32_t k = 0; k + 8 <= size; k += 8) {
            uint32_t addr = rd32(elf + off + k), type = rd32(elf + off + k + 4) & 0xFF;
            if (addr + 4 > filesz) return 0;
            uint8_t* w = img + addr;
            uint32_t v = rd32(w);
            switch (type) {
            case 2: v += base; break;                                   /* R_MIPS_32 */
            case 4: v = (v & 0xFC000000u) | ((((v & 0x03FFFFFFu) << 2) + base) >> 2 & 0x03FFFFFFu); break;  /* R_MIPS_26 */
            case 5: if (npending < 256) pending[npending++] = addr; continue;                               /* R_MIPS_HI16 */
            case 6: {                                                   /* R_MIPS_LO16 */
                int32_t lo = (int16_t)(v & 0xFFFF);
                for (int i = 0; i < npending; i++) {
                    uint32_t h = rd32(img + pending[i]);
                    uint32_t value = ((h & 0xFFFF) << 16) + (uint32_t)lo + base;
                    h = (h & 0xFFFF0000u) | (((value + 0x8000u) >> 16) & 0xFFFF);
                    memcpy(img + pending[i], &h, 4);
                }
                npending = 0;
                v = (v & 0xFFFF0000u) | (((uint32_t)lo + base) & 0xFFFF);
                break;
            }
            default:
                fprintf(stderr, "[iop] unsupported relocation type %u at 0x%X\n", type, addr);
                return 0;
            }
            memcpy(w, &v, 4);
        }
    }
    return npending == 0;
}

int iop_load_module(uint32_t image, const char* args, uint32_t args_len, int32_t* result) {
    if (!iop_native_audio()) return 0;
    const uint8_t* elf = iop_ram_ptr(image);
    uint32_t avail = IOP_RAM_SIZE - (image & (IOP_RAM_SIZE - 1));
    if (!elf || avail < 0x34 || memcmp(elf, "\x7f" "ELF", 4) != 0 || rd16(elf + 0x10) != 0xFF80) return 0;

    uint32_t phoff = rd32(elf + 0x1C);
    uint16_t phentsize = rd16(elf + 0x2A), phnum = rd16(elf + 0x2C);
    const char* name = NULL;
    uint32_t load_off = 0, filesz = 0, memsz = 0;
    for (uint16_t i = 0; i < phnum && phoff + (uint32_t)(i + 1) * phentsize <= avail; i++) {
        const uint8_t* ph = elf + phoff + (uint32_t)i * phentsize;
        uint32_t type = rd32(ph), off = rd32(ph + 4);
        if (type == 0x70000080u && off + 27 < avail) name = (const char*)elf + off + 26;
        else if (type == 1) { load_off = off; filesz = rd32(ph + 16); memsz = rd32(ph + 20); }
    }
    if (!name) return 0;
    const iop_module_desc* d = iop_module_by_name(name);
    if (!d) return 0;
    if (filesz != d->filesz || memsz != d->memsz || load_off + filesz > avail) {
        fprintf(stderr, "[iop] %s: image layout differs from the recompiled module; not run natively\n", name);
        return 0;
    }
    uint32_t crc = crc32_bytes(elf + load_off, filesz);
    if (crc != d->file_crc) {
        fprintf(stderr, "[iop] %s: CRC 0x%08X, recompiled build is 0x%08X; not run natively\n", name, crc, d->file_crc);
        return 0;
    }

    /* Copy, relocate, clear bss. The image buffer is the EE's IOPHEAP block, never our area. */
    static uint8_t tmp[IOP_RAM_SIZE];
    memcpy(tmp, elf + load_off, filesz);
    if (!relocate(tmp, filesz, d->base, elf, avail)) {
        fprintf(stderr, "[iop] %s: bad relocations; not run natively\n", name);
        return 0;
    }
    crc = crc32_bytes(tmp, filesz);
    if (crc != d->image_crc) {
        fprintf(stderr, "[iop] %s: relocated image CRC 0x%08X != 0x%08X; not run natively\n", name, crc, d->image_crc);
        return 0;
    }
    memcpy(iop_ram + d->base, tmp, filesz);
    memset(iop_ram + d->base + filesz, 0, memsz - filesz);

    /* argv: the module name, then the NUL-separated LOADFILE arguments */
    uint32_t strings = (uint32_t)strlen(name) + 1, slots = args_len + 2;   /* at most one arg per byte */
    uint32_t block = iop_alloc(4 * slots + strings + args_len + 1);
    if (!block) { fprintf(stderr, "[iop] %s: no IOP memory for argv\n", name); return 0; }
    uint32_t argv = block, str = block + 4 * slots, n = 0;
    iop_w32(argv + 4 * n++, str);
    memcpy(iop_ram + str, name, strings);
    str += strings;
    for (uint32_t i = 0; i < args_len;) {
        uint32_t len = 0;
        while (i + len < args_len && args[i + len]) len++;
        if (len) {
            iop_w32(argv + 4 * n++, str);
            memcpy(iop_ram + str, args + i, len);
            iop_ram[str + len] = 0;
            str += len + 1;
        }
        i += len + 1;
    }
    iop_w32(argv + 4 * n, 0);
    fprintf(stderr, "[iop] %s: running natively at 0x%08X (recompiled), %u args\n", name, d->base, n - 1);

    /* Start it the way modload does: in a thread of the loader, with (argc, argv). */
    int tid = iop_create_kernel_thread(d->entry, 8, 0x4000, n, argv, d->gp);
    if (tid <= 0) { fprintf(stderr, "[iop] %s: cannot create the start thread (%d)\n", name, tid); return 0; }
    uint32_t ret = 0;
    int64_t give_up = iop_now() + iop_usec_to_ticks(10u * 1000000u);
    for (;;) {
        iop_run();
        if (iop_thread_status_dormant(tid, &ret)) break;
        if (iop_now() > give_up) {
            fprintf(stderr, "[iop] %s: module start has not returned after 10 s; continuing\n", name);
            *result = 0;
            return 1;
        }
        Sleep(1);
    }
    iop_delete_kernel_thread(tid);                         /* argv stays: modules may keep pointers into it */
    *result = (int32_t)ret;
    fprintf(stderr, "[iop] %s: start returned %d\n", name, (int)ret);
    return 1;
}
