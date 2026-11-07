"""(ai) AI texture upscaling prototype, phase 1 (notes/ai-textures.md): a texture dump -> an HD-pack folder.

    python -I scripts/ai_textures.py build                           build\\tools\\ai_up.exe (+ build\\ai\\x4anime6b.bin)
    python -I scripts/ai_textures.py selftest [--size 5x4]           pure-Python reference vs C reference vs GPU
    python -I scripts/ai_textures.py upscale <dump dir> <pack dir> [--max-pixels N] [--keep-tmp]

upscale: every <hash>.png of a PS2_TEX_DUMP folder -> <pack dir>\\<hash>.png at 4x (the HD-pack hook's names; sizes
stay powers of two). Per texture (RGBA, alpha 255 = opaque):
  1. transparent texels take their opaque neighbours' colour (repeated dilation): no dark fringe gets into the net;
  2. dithered palette textures (few colours, many isolated +-1-level texels in flat areas) are smoothed in the flat areas
     only (a 3x3 range-limited mean): the net would otherwise sharpen the dither into grain;
  3. RGB padded by 16 texels on each side: wrapped where the texture tiles (its opposite edges continue each other),
     edge-replicated where it does not; > 128 texels: tiles of 128 + 16 context. The net (build\\tools\\ai_up.exe,
     Real-ESRGAN x4 anime 6B on D3D11) upscales; the padding is cropped (seamless when tiled);
  4. alpha is not the net's: bicubic 4x (wrapped / clamped like the RGB), then for cut-out textures (alpha mostly 0 or
     255) a contrast ramp around 50 % that keeps the coverage and turns the staircase into smooth contours.
Standard library only (the venv has no numpy / PIL): PNGs via scripts/png_tool.py's reader and zlib.
"""
import array
import os
import random
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))    # (ai_model: our own code; -I keeps everything else off the path)

MAIN = ROOT
if not (ROOT / 'tools' / 'llvm-mingw').exists():          # worktree: tools live in the main checkout
    g = (ROOT / '.git').read_text().split('gitdir:')[1].strip()
    MAIN = Path(g).parent.parent.parent
CLANG = MAIN / 'tools' / 'llvm-mingw' / 'bin' / 'clang.exe'
MODELS = {'6b': ('RealESRGAN_x4plus_anime_6B.pth', 'x4anime6b.bin'), 'x4plus': ('RealESRGAN_x4plus.pth', 'x4plus.bin')}
AI_MODEL = os.environ.get('AI_MODEL', '6b')        # (6b: the phase-1 anime model; x4plus: the general 23-block one)
MODEL = MAIN / 'tools' / 'models' / 'realesrgan' / MODELS[AI_MODEL][0]
EXE = ROOT / 'build' / 'tools' / 'ai_up.exe'
WEIGHTS = ROOT / 'build' / 'ai' / MODELS[AI_MODEL][1]
HLSL = ROOT / 'runtime' / 'gpu' / 'shaders' / 'ai_up.hlsl'
PAD = int(os.environ.get('AI_PAD', '16'))
TILE = 128
DITHER = float(os.environ.get('AI_DITHER', '9'))   # (dither score above which flat areas are smoothed first: off -
# the net removes dither by itself and the pre-smoothing cost the textures' grain; notes/ai-textures.md)
os.environ.setdefault('AI_UP_DETAIL', '1.0')        # (the source's grain added back under the net's lines; 0 = the net alone)
os.environ.setdefault('AI_UP_CACHE', str(ROOT / 'build' / 'ai' / 'shcache'))   # (compiled shader permutations)


def build():
    WEIGHTS.parent.mkdir(parents=True, exist_ok=True)
    Path(os.environ['AI_UP_CACHE']).mkdir(parents=True, exist_ok=True)
    EXE.parent.mkdir(parents=True, exist_ok=True)
    if not WEIGHTS.exists():
        import ai_model
        st = ai_model.load_state(str(MODEL))
        ai_model.export(st, ai_model.check_arch(st), str(WEIGHTS))
    src = [ROOT / 'scripts' / 'ai_up_tool.c', ROOT / 'runtime' / 'gpu' / 'ai_up.c', ROOT / 'runtime' / 'gpu' / 'ai_up.h']
    if not EXE.exists() or max(p.stat().st_mtime for p in src) > EXE.stat().st_mtime:
        subprocess.check_call([str(CLANG), '-O2', '-std=c11', '-Wall', '-Wextra', '-o', str(EXE), str(src[0]), str(src[1]),
                               '-ld3d11', '-ldxgi', '-ldxguid'])


# ---- small image helpers (lists of floats) ------------------------------------------------------------------------
def write_png(path, w, h, px, ch):
    """px: bytes / bytearray of w*h*ch (ch 3 or 4)."""
    raw = bytearray()
    st = w * ch
    for y in range(h):
        raw.append(0)
        raw += px[y * st:(y + 1) * st]

    def chunk(t, d):
        c = zlib.crc32(t + d) & 0xFFFFFFFF
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', c)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if ch == 4 else 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def read_rgba(path):
    """8-bit RGB / RGBA PNG -> (w, h, RGBA bytearray)"""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat = 8, []
    while pos < len(data):
        n, = struct.unpack('>I', data[pos:pos + 4])
        typ, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype in (2, 6) and body[12] == 0, (path, depth, ctype)
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b''.join(idat))
    st = w * bpp
    out = bytearray(w * h * 4)
    prev = bytearray(st)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + st])
        p += 1 + st
        if f == 1:
            for i in range(bpp, st):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(st):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(st):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(st):
                a = line[i - bpp] if i >= bpp else 0
                b, c = prev[i], prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        o = y * w * 4
        if bpp == 4:
            out[o:o + w * 4] = line
        else:
            out[o:o + w * 4:4], out[o + 1:o + w * 4:4], out[o + 2:o + w * 4:4] = line[0::3], line[1::3], line[2::3]
            out[o + 3:o + w * 4:4] = b'\xff' * w
    return w, h, out


# ---- 1. colour under transparent texels ----------------------------------------------------------------------------
def bleed(w, h, rgba):
    """RGB planes (floats 0..255) where every texel with alpha < 8 takes the mean colour of known neighbours, repeated
    outwards (fully transparent images keep their colour)."""
    n = w * h
    r = [float(rgba[i * 4]) for i in range(n)]
    g = [float(rgba[i * 4 + 1]) for i in range(n)]
    b = [float(rgba[i * 4 + 2]) for i in range(n)]
    known = [rgba[i * 4 + 3] >= 8 for i in range(n)]
    if all(known) or not any(known):
        return r, g, b
    todo = [i for i in range(n) if not known[i]]
    while todo:
        nxt, upd = [], []
        for i in todo:
            x, y = i % w, i // w
            sr = sg = sb = 0.0
            k = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    j = ((y + dy) % h) * w + (x + dx) % w
                    if known[j]:
                        sr += r[j]; sg += g[j]; sb += b[j]; k += 1
            if k:
                upd.append((i, sr / k, sg / k, sb / k))
            else:
                nxt.append(i)
        if not upd:
            break
        for i, a, c, d in upd:
            r[i], g[i], b[i] = a, c, d
            known[i] = True
        todo = nxt
    return r, g, b


# ---- 2. dither ------------------------------------------------------------------------------------------------------
def dither_score(w, h, planes):
    """share of texels in flat areas that are a small isolated extreme against both horizontal or both vertical
    neighbours (ordered / error-diffusion dither in 4-bit palettes)."""
    lum = [0.3 * a + 0.59 * b + 0.11 * c for a, b, c in zip(*planes)]
    flat = iso = 0
    for y in range(h):
        for x in range(w):
            v = lum[y * w + x]
            l, r_ = lum[y * w + (x - 1) % w], lum[y * w + (x + 1) % w]
            u, d = lum[((y - 1) % h) * w + x], lum[((y + 1) % h) * w + x]
            rng = max(v, l, r_, u, d) - min(v, l, r_, u, d)
            if rng > 40:
                continue
            flat += 1
            if rng >= 2 and (((v - l) * (v - r_) > 0 and abs(v - l) <= 24 and abs(v - r_) <= 24) or
                             ((v - u) * (v - d) > 0 and abs(v - u) <= 24 and abs(v - d) <= 24)):
                iso += 1
    return iso / flat if flat else 0.0


def dedither(w, h, planes, lim=28.0):
    """3x3 mean of the neighbours within lim of the centre (per channel max), wrapped: removes dither, keeps edges."""
    out = [list(p) for p in planes]
    for y in range(h):
        for x in range(w):
            i = y * w + x
            c = [p[i] for p in planes]
            s = [0.0, 0.0, 0.0]
            k = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    j = ((y + dy) % h) * w + (x + dx) % w
                    v = [p[j] for p in planes]
                    if max(abs(v[0] - c[0]), abs(v[1] - c[1]), abs(v[2] - c[2])) <= lim:
                        s[0] += v[0]; s[1] += v[1]; s[2] += v[2]; k += 1
            for ch in range(3):
                out[ch][i] = s[ch] / k
    return out


# ---- 3. tiling detection / padding ------------------------------------------------------------------------------------
def tiles_axis(w, h, planes, axis):
    """True when the texture continues across its own edge on this axis (wrap): the seam's step is no larger than
    typical steps inside."""
    lum = [0.3 * a + 0.59 * b + 0.11 * c for a, b, c in zip(*planes)]
    if axis == 0:
        if w < 4:
            return True
        seam = sum(abs(lum[y * w] - lum[y * w + w - 1]) for y in range(h)) / h
        inner = sum(abs(lum[y * w + x] - lum[y * w + x + 1]) for y in range(h) for x in range(w - 1)) / (h * (w - 1))
        edge = sum(abs(lum[y * w + w - 2] - lum[y * w + w - 1]) + abs(lum[y * w] - lum[y * w + 1]) for y in range(h)) / (2 * h)
    else:
        if h < 4:
            return True
        seam = sum(abs(lum[x] - lum[(h - 1) * w + x]) for x in range(w)) / w
        inner = sum(abs(lum[y * w + x] - lum[(y + 1) * w + x]) for y in range(h - 1) for x in range(w)) / (w * (h - 1))
        edge = sum(abs(lum[(h - 2) * w + x] - lum[(h - 1) * w + x]) + abs(lum[x] - lum[w + x]) for x in range(w)) / (2 * w)
    return seam <= max(2.0 * max(inner, edge), 6.0)


def sample_idx(i, n, wrap):
    if wrap:
        return i % n
    return 0 if i < 0 else n - 1 if i >= n else i


def crop_pad(w, h, planes, x0, y0, cw, ch, wx, wy):
    """a cw x ch window at (x0, y0) of the texture extended by wrap / replicate: interleaved RGB bytes"""
    out = bytearray(cw * ch * 3)
    xs = [sample_idx(x0 + x, w, wx) for x in range(cw)]
    for y in range(ch):
        sy = sample_idx(y0 + y, h, wy) * w
        o = y * cw * 3
        for x in range(cw):
            j = sy + xs[x]
            out[o] = min(255, max(0, int(planes[0][j] + 0.5)))
            out[o + 1] = min(255, max(0, int(planes[1][j] + 0.5)))
            out[o + 2] = min(255, max(0, int(planes[2][j] + 0.5)))
            o += 3
    return out


# ---- 4. alpha (ai_up.exe alpha: Catmull-Rom 4x, wrapped / clamped like the RGB, a steep ramp for cut-outs) ------------
def is_cutout(a):
    """mostly 0 / 255 (alpha-tested leaves, railings, moss): sharpen the interpolated contour"""
    return sum(1 for v in a if 16 < v < 239) < 0.1 * len(a) and min(a) < 128 <= max(a)

# ---- the pipeline ------------------------------------------------------------------------------------------------------
def prepare(path):
    w, h, rgba = read_rgba(path)
    a = [rgba[i * 4 + 3] for i in range(w * h)]
    planes = list(bleed(w, h, rgba))
    ncol = len(set(bytes(rgba[i * 4:i * 4 + 3]) for i in range(w * h)))
    ds = dither_score(w, h, planes) if ncol <= 256 else 0.0
    dith = ncol <= 256 and ds > DITHER
    if dith:
        planes = dedither(w, h, planes)
    wx, wy = tiles_axis(w, h, planes, 0), tiles_axis(w, h, planes, 1)
    jobs = []
    for ty in range(0, h, TILE):
        for tx in range(0, w, TILE):
            cw, ch = min(TILE, w - tx), min(TILE, h - ty)
            jobs.append((tx, ty, cw, ch, crop_pad(w, h, planes, tx - PAD, ty - PAD, cw + 2 * PAD, ch + 2 * PAD, wx, wy)))
    return {'w': w, 'h': h, 'a': a, 'wx': wx, 'wy': wy, 'dith': dith, 'ncol': ncol, 'jobs': jobs,
            'opaque': min(a) >= 250}


def run_net(blobs, weights, max_pixels, tmp, detail=None):
    """blobs: (tx, ty, cw, ch, padded RGB bytes) -> the out.bin bytes"""
    with open(tmp / 'in.bin', 'wb') as f:
        f.write(b'AIB1' + struct.pack('<I', len(blobs)))
        for (tx, ty, cw, ch, px) in blobs:
            f.write(struct.pack('<II', cw + 2 * PAD, ch + 2 * PAD) + px)
    env = dict(os.environ)
    if detail is not None:
        env['AI_UP_DETAIL'] = detail
    r = subprocess.run([str(EXE), 'batch', str(weights), str(HLSL), str(tmp / 'in.bin'), str(tmp / 'out.bin'), str(max_pixels)],
                       capture_output=True, text=True, env=env)
    print(r.stdout.strip())
    if r.returncode:
        print(r.stderr)
        raise SystemExit('ai_up failed')
    return open(tmp / 'out.bin', 'rb').read()


def chain_clean(infos, data):
    """(AI_CHAIN=1) the first pass's 4x RGB (anime 6B: dither and palette noise gone) boxed back down to the source size:
    the cleaned source the second model upscales"""
    pos = 8
    for inf in infos:
        w, h = inf['w'], inf['h']
        planes = [[0.0] * (w * h) for _ in range(3)]
        for (tx, ty, cw, ch, _) in inf['jobs']:
            ow, oh = struct.unpack('<II', data[pos:pos + 8])
            img = data[pos + 8:pos + 8 + ow * oh * 3]
            pos += 8 + ow * oh * 3
            for y in range(ch):
                for x in range(cw):
                    s = [0, 0, 0]
                    for yy in range(4):
                        o = ((4 * (y + PAD) + yy) * ow + 4 * (x + PAD)) * 3
                        for xx in range(4):
                            s[0] += img[o]; s[1] += img[o + 1]; s[2] += img[o + 2]
                            o += 3
                    i = (ty + y) * w + tx + x
                    planes[0][i], planes[1][i], planes[2][i] = s[0] / 16.0, s[1] / 16.0, s[2] / 16.0
        wx, wy = inf['wx'], inf['wy']
        inf['jobs'] = [(tx, ty, cw, ch, crop_pad(w, h, planes, tx - PAD, ty - PAD, cw + 2 * PAD, ch + 2 * PAD, wx, wy))
                       for (tx, ty, cw, ch, _) in inf['jobs']]


def upscale(dump, pack, max_pixels=131072, keep_tmp=False):
    build()
    dump, pack = Path(dump), Path(pack)
    pack.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in dump.glob('*.png') if len(p.stem) == 16)
    t0 = time.perf_counter()
    infos, blobs = [], []
    for p in files:
        inf = prepare(p)
        inf['name'] = p.stem
        infos.append(inf)
        for j in inf['jobs']:
            blobs.append(j)
    t1 = time.perf_counter()
    tmp = ROOT / 'build' / 'ai' / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    t2 = time.perf_counter()
    if os.environ.get('AI_CHAIN') == '1':    # 2 passes: anime 6B cleans, then the selected model (x4plus) upscales
        first = ROOT / 'build' / 'ai' / MODELS['6b'][1]
        chain_clean(infos, run_net(blobs, first, max_pixels, tmp, detail='0'))
        blobs = [j for inf in infos for j in inf['jobs']]
        print(f'chain: first pass done in {time.perf_counter() - t2:.1f} s')
    data = run_net(blobs, WEIGHTS, max_pixels, tmp)
    t3 = time.perf_counter()
    with open(tmp / 'ain.bin', 'wb') as f:
        al = [inf for inf in infos if not inf['opaque']]
        f.write(b'AIA1' + struct.pack('<I', len(al)))
        for inf in al:
            inf['cut'] = is_cutout(inf['a'])
            f.write(struct.pack('<III', inf['w'], inf['h'], inf['wx'] | inf['wy'] << 1 | inf['cut'] << 2) + bytes(inf['a']))
    subprocess.check_call([str(EXE), 'alpha', str(tmp / 'ain.bin'), str(tmp / 'aout.bin')])
    adata, apos = open(tmp / 'aout.bin', 'rb').read(), 0
    t3b = time.perf_counter()
    data = open(tmp / 'out.bin', 'rb').read()
    pos = 8
    k = 0
    stats = {'dith': 0, 'cut': 0, 'wrap_both': 0, 'clamp_any': 0}
    for inf in infos:
        w, h = inf['w'], inf['h']
        W, H = 4 * w, 4 * h
        rgb = bytearray(W * H * 3)
        for (tx, ty, cw, ch, _) in inf['jobs']:
            ow, oh = struct.unpack('<II', data[pos:pos + 8])
            img = memoryview(data)[pos + 8:pos + 8 + ow * oh * 3]
            pos += 8 + ow * oh * 3
            k += 1
            for y in range(4 * ch):
                s = ((y + 4 * PAD) * ow + 4 * PAD) * 3
                d = ((4 * ty + y) * W + 4 * tx) * 3
                rgb[d:d + 4 * cw * 3] = img[s:s + 4 * cw * 3]
        out = bytearray(W * H * 4)
        out[0::4], out[1::4], out[2::4] = rgb[0::3], rgb[1::3], rgb[2::3]
        if inf['opaque']:
            out[3::4] = b'\xff' * (W * H)
        else:
            out[3::4] = adata[apos:apos + W * H]
            apos += W * H
            stats['cut'] += inf['cut']
        stats['dith'] += inf['dith']
        stats['wrap_both'] += inf['wx'] and inf['wy']
        stats['clamp_any'] += not (inf['wx'] and inf['wy'])
        write_png(pack / (inf['name'] + '.png'), W, H, out, 4)
    t4 = time.perf_counter()
    print(f'{len(infos)} textures ({k} net tiles): prepare {t1 - t0:.1f} s, write batch {t2 - t1:.1f} s, net (ai_up) {t3 - t2:.1f} s, '
          f'alpha {t3b - t3:.1f} s, finish + PNGs {t4 - t3b:.1f} s, total {t4 - t0:.1f} s')
    print(f"  dithered (smoothed first) {stats['dith']}, cut-out alpha {stats['cut']}, tiling both axes {stats['wrap_both']}, "
          f"clamped on an axis {stats['clamp_any']}")
    if not keep_tmp:
        for p in ('in.bin', 'out.bin', 'ain.bin', 'aout.bin'):
            (tmp / p).unlink(missing_ok=True)


def selftest(size):
    build()
    import ai_model
    w, h = (int(v) for v in size.split('x'))
    rnd = random.Random(7)
    # a smooth pattern with an edge (not noise: the net's behaviour on noise is atypical)
    planes = [[min(1.0, max(0.0, 0.5 + 0.4 * ((x + c) % 3 - 1) * (1 if y < h // 2 else -1) + 0.05 * rnd.random()))
               for y in range(h) for x in range(w)] for c in range(3)]
    tmp = ROOT / 'build' / 'ai' / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    ai_model.write_f32(tmp / 'st_in.f32', planes, w, h)
    t = time.perf_counter()
    st = ai_model.load_state(str(MODEL))
    arch = ai_model.check_arch(st)
    out, ow, oh = ai_model.forward(st, arch, planes, h, w)
    print(f'python reference {w}x{h}: {time.perf_counter() - t:.1f} s')
    ai_model.write_f32(tmp / 'st_py.f32', out, ow, oh)
    subprocess.check_call([str(EXE), 'cpu', str(WEIGHTS), str(tmp / 'st_in.f32'), str(tmp / 'st_c.f32')])
    subprocess.check_call([str(EXE), 'gpu', str(WEIGHTS), str(HLSL), str(tmp / 'st_in.f32'), str(tmp / 'st_gpu.f32')])
    py = [v for p in out for v in p]
    c, _, _ = ai_model.read_f32(tmp / 'st_c.f32')
    gp, _, _ = ai_model.read_f32(tmp / 'st_gpu.f32')
    c = [v for p in c for v in p]
    gp = [v for p in gp for v in p]

    def cmp(a, b):
        d = [abs(x - y) for x, y in zip(a, b)]
        return max(d), sum(d) / len(d)
    print('output range %.3f .. %.3f' % (min(py), max(py)))
    print('python vs C:   max %.2e mean %.2e' % cmp(py, c))
    print('C vs GPU:      max %.2e mean %.2e' % cmp(c, gp))
    ok = cmp(py, c)[0] < 1e-4 and cmp(c, gp)[0] < 1e-3
    print('selftest', 'OK' if ok else 'FAILED')
    return 0 if ok else 1


def gpu_check(size):
    """C reference vs GPU on a bigger image (multi-tile, multiple oc blocks)"""
    build()
    import ai_model
    w, h = (int(v) for v in size.split('x'))
    rnd = random.Random(3)
    planes = [[0.5 + 0.3 * ((x // 3 + y // 5 + c) % 2) + 0.1 * rnd.random() for y in range(h) for x in range(w)] for c in range(3)]
    tmp = ROOT / 'build' / 'ai' / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    ai_model.write_f32(tmp / 'gc_in.f32', planes, w, h)
    subprocess.check_call([str(EXE), 'cpu', str(WEIGHTS), str(tmp / 'gc_in.f32'), str(tmp / 'gc_c.f32')])
    subprocess.check_call([str(EXE), 'gpu', str(WEIGHTS), str(HLSL), str(tmp / 'gc_in.f32'), str(tmp / 'gc_gpu.f32')])
    c, _, _ = ai_model.read_f32(tmp / 'gc_c.f32')
    gp, _, _ = ai_model.read_f32(tmp / 'gc_gpu.f32')
    d = [abs(x - y) for p, q in zip(c, gp) for x, y in zip(p, q)]
    print(f'{w}x{h}: C vs GPU max {max(d):.2e} mean {sum(d) / len(d):.2e}; >1/255: {sum(1 for v in d if v > 1 / 255)}')
    return 0 if max(d) < 2e-3 else 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == 'build':
        build()
        return 0
    if argv[1] == 'selftest':
        return selftest(argv[3] if len(argv) > 3 and argv[2] == '--size' else '5x4')
    if argv[1] == 'gpucheck':
        return gpu_check(argv[2] if len(argv) > 2 else '40x24')
    if argv[1] == 'upscale':
        mp = 131072
        if '--max-pixels' in argv:
            mp = int(argv[argv.index('--max-pixels') + 1])
        upscale(argv[2], argv[3], mp, '--keep-tmp' in argv)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
