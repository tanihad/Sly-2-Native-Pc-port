"""(ai) Safe reader for the Real-ESRGAN model file and a pure-Python reference of its network (notes/ai-textures.md).

The model (RealESRGAN_x4plus_anime_6B.pth, BSD-3, xinntao) is a PyTorch zip whose data.pkl is a pickle. A pickle can
run arbitrary code, so this file NEVER calls pickle.load / torch.load: data.pkl is read by a restricted Unpickler whose
find_class only knows the few names a state dict needs (collections.OrderedDict, torch._utils._rebuild_tensor_v2, the
torch *Storage type tags) and maps them to inert stand-ins defined here; anything else is an error. Tensor bytes come
from the zip's data/<key> entries (raw little-endian arrays). Standard library only (the venv has no numpy). Run with
python -I (isolated: no script directory / user site on sys.path).

    python -I scripts/ai_model.py info   <model.pth>
    python -I scripts/ai_model.py export <model.pth> <weights.bin>     (the C / GPU code's weight file, below)
    python -I scripts/ai_model.py ref    <model.pth> <in.f32> <out.f32> (pure-Python forward pass: tiny images only)

weights.bin ("SLYAIW01"): u32 count, then per tensor: u16 name length, name (ASCII), u32 ndim, u32 dims[ndim], float32
data (C order). f32 image files: u32 width, u32 height, float32 RGB planes (3 x height x width), values 0..1.
"""
import array
import hashlib
import io
import pickle
import struct
import sys
import zipfile

MODEL_SHA256 = 'F872D837D3C90ED2E05227BED711AF5671A6FD1C9F7D7E91C911A61F155E99DA'   # (RealESRGAN_x4plus_anime_6B.pth)
# the owner-approved model files (both Real-ESRGAN, BSD-3): only these load
APPROVED = {
    MODEL_SHA256: 'RealESRGAN_x4plus_anime_6B.pth',
    '4FA0D38905F75AC06EB49A7951B426670021BE3018265FD191D2125DF9D682F1': 'RealESRGAN_x4plus.pth',
}
# The weights Setup and the update bundle ship (scripts/make_installer.py, notes/ai-textures.md "Shipping"): `export` of
# RealESRGAN_x4plus.pth is deterministic (fixed tensor order, raw float32), so the .bin has one SHA-256, recorded here.
# check_no_game_data.py allows exactly this file (it is third-party weights, nothing of the game).
X4PLUS_PTH_SHA256 = '4FA0D38905F75AC06EB49A7951B426670021BE3018265FD191D2125DF9D682F1'
X4PLUS_BIN_SHA256 = 'B3FF1E5FB8F9F5FC2A39BD5045EACFB43F9DF3A1163856DF6A8638E2364874D3'
X4PLUS_BIN_SIZE = 66819626


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest().upper()


def export_x4plus(pth, out):
    """x4plus.bin from the pinned RealESRGAN_x4plus.pth: checks the .pth's SHA-256 first and the result's after.
    An existing `out` with the pinned SHA-256 is kept (it is the same bytes). Returns the .bin's SHA-256."""
    import os
    if os.path.isfile(out) and os.path.getsize(out) == X4PLUS_BIN_SIZE and sha256_file(out) == X4PLUS_BIN_SHA256:
        return X4PLUS_BIN_SHA256
    if sha256_file(pth) != X4PLUS_PTH_SHA256:
        raise SystemExit(f'{pth}: not the pinned RealESRGAN_x4plus.pth (SHA-256 {X4PLUS_PTH_SHA256[:8]}...)')
    state = load_state(pth)
    arch = check_arch(state)
    tmp = str(out) + '.tmp'
    export(state, arch, tmp)
    got = sha256_file(tmp)
    if got != X4PLUS_BIN_SHA256:
        os.remove(tmp)
        raise SystemExit(f'export of {pth}: SHA-256 {got}, expected the pinned {X4PLUS_BIN_SHA256} (exporter changed?)')
    os.replace(tmp, out)
    return got


# ---- inert stand-ins for the only globals a state-dict pickle may name ------------------------------------------------
class _Dict(dict):
    """collections.OrderedDict: a plain dict (insertion-ordered)."""


class _StorageTag:
    def __init__(self, name, code, size):
        self.name, self.code, self.size = name, code, size


_STORAGES = {
    'FloatStorage': _StorageTag('FloatStorage', 'f', 4),
    'HalfStorage': _StorageTag('HalfStorage', 'e', 2),
}


class _StorageRef:
    def __init__(self, tag, key, numel):
        self.tag, self.key, self.numel = tag, key, numel


class _TensorRef:
    def __init__(self, storage, offset, size, stride):
        self.storage, self.offset, self.size, self.stride = storage, offset, tuple(size), tuple(stride)


def _rebuild_tensor_v2(storage, storage_offset, size, stride, requires_grad=False, backward_hooks=None, metadata=None):
    if not isinstance(storage, _StorageRef):
        raise pickle.UnpicklingError('tensor without a storage')
    if not all(isinstance(v, int) for v in (storage_offset, *size, *stride)):
        raise pickle.UnpicklingError('bad tensor geometry')
    return _TensorRef(storage, storage_offset, size, stride)


_ALLOWED = {
    ('collections', 'OrderedDict'): _Dict,
    ('torch._utils', '_rebuild_tensor_v2'): _rebuild_tensor_v2,
}


class _SafeUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if (module, name) in _ALLOWED:
            return _ALLOWED[(module, name)]
        if module == 'torch' and name in _STORAGES:
            return _STORAGES[name]
        raise pickle.UnpicklingError(f'refused global {module}.{name}')

    def persistent_load(self, pid):
        # ('storage', <storage type tag>, key, location, numel)
        if not (isinstance(pid, tuple) and len(pid) == 5 and pid[0] == 'storage' and isinstance(pid[1], _StorageTag)
                and isinstance(pid[2], str) and pid[2].isdigit() and isinstance(pid[4], int)):
            raise pickle.UnpicklingError(f'refused persistent id {pid!r}')
        return _StorageRef(pid[1], pid[2], pid[4])


def load_state(path, check_hash=True):
    """{name: (shape tuple, flat list of floats)} of the model's EMA weights."""
    raw = open(path, 'rb').read()
    if check_hash and hashlib.sha256(raw).hexdigest().upper() not in APPROVED:
        raise SystemExit(f'{path}: unexpected SHA-256 (not the approved model file)')
    z = zipfile.ZipFile(io.BytesIO(raw))
    names = z.namelist()
    pkl = [n for n in names if n.endswith('/data.pkl')]
    if len(pkl) != 1:
        raise SystemExit('no single data.pkl in the model zip')
    prefix = pkl[0][:-len('data.pkl')]
    obj = _SafeUnpickler(io.BytesIO(z.read(pkl[0]))).load()
    if isinstance(obj, dict) and 'params_ema' in obj:
        obj = obj['params_ema']
    elif isinstance(obj, dict) and 'params' in obj:
        obj = obj['params']
    if not isinstance(obj, dict):
        raise SystemExit('the pickle is not a state dict')
    cache, state = {}, {}
    for name, t in obj.items():
        if not isinstance(name, str) or not isinstance(t, _TensorRef):
            raise SystemExit(f'unexpected entry {name!r}')
        s = t.storage
        if s.key not in cache:
            data = z.read(prefix + 'data/' + s.key)
            if len(data) != s.numel * s.tag.size:
                raise SystemExit(f'storage {s.key}: {len(data)} bytes for {s.numel} elements')
            a = array.array(s.tag.code if s.tag.code == 'f' else 'f')
            if s.tag.code == 'f':
                a.frombytes(data)
                if sys.byteorder != 'little':
                    a.byteswap()
            else:
                a.extend(struct.unpack(f'<{s.numel}e', data))
            cache[s.key] = a
        flat = cache[s.key]
        n = 1
        for d in t.size:
            n *= d
        # gather in C order through the stride (contiguous in practice: one slice)
        cst, acc = [], 1
        for d in reversed(t.size):
            cst.insert(0, acc)
            acc *= d
        if tuple(t.stride) == tuple(cst) or n <= 1:
            if t.offset + n > len(flat):
                raise SystemExit(f'{name}: out of its storage')
            state[name] = (t.size, flat[t.offset:t.offset + n])
            continue
        out = array.array('f', bytes(4 * n))
        idx = [0] * len(t.size)
        for i in range(n):
            off = t.offset + sum(k * st for k, st in zip(idx, t.stride))
            out[i] = flat[off]
            for d in range(len(idx) - 1, -1, -1):
                idx[d] += 1
                if idx[d] < t.size[d]:
                    break
                idx[d] = 0
        state[name] = (t.size, out)
    return state


def check_arch(state):
    """RRDBNet geometry from the keys / shapes (raises on anything unexpected)."""
    def shp(k):
        return state[k][0]
    nf = shp('conv_first.weight')[0]
    ic = shp('conv_first.weight')[1]
    blocks = 1 + max(int(k.split('.')[1]) for k in state if k.startswith('body.'))
    gc = shp('body.0.rdb1.conv1.weight')[0]
    exp = {'conv_first.weight': (nf, ic, 3, 3), 'conv_first.bias': (nf,)}
    for b in range(blocks):
        for r in (1, 2, 3):
            for c in range(1, 6):
                o = nf if c == 5 else gc
                exp[f'body.{b}.rdb{r}.conv{c}.weight'] = (o, nf + (c - 1) * gc, 3, 3)
                exp[f'body.{b}.rdb{r}.conv{c}.bias'] = (o,)
    for n in ('conv_body', 'conv_up1', 'conv_up2', 'conv_hr'):
        exp[n + '.weight'] = (nf, nf, 3, 3)
        exp[n + '.bias'] = (nf,)
    exp['conv_last.weight'] = (3, nf, 3, 3)
    exp['conv_last.bias'] = (3,)
    if set(exp) != set(state):
        raise SystemExit(f'unexpected keys: extra {sorted(set(state) - set(exp))[:5]}, missing {sorted(set(exp) - set(state))[:5]}')
    for k, v in exp.items():
        if tuple(shp(k)) != v:
            raise SystemExit(f'{k}: shape {shp(k)}, expected {v}')
    if ic != 3:
        raise SystemExit(f'{ic} input channels: not the x4 (no pixel-unshuffle) variant')
    return {'num_feat': nf, 'num_block': blocks, 'num_grow_ch': gc, 'scale': 4, 'num_in_ch': ic}


def order(arch):
    names = ['conv_first']
    for b in range(arch['num_block']):
        for r in (1, 2, 3):
            names += [f'body.{b}.rdb{r}.conv{c}' for c in range(1, 6)]
    return names + ['conv_body', 'conv_up1', 'conv_up2', 'conv_hr', 'conv_last']


def export(state, arch, out):
    with open(out, 'wb') as f:
        names = []
        for n in order(arch):
            names += [n + '.weight', n + '.bias']
        f.write(b'SLYAIW01' + struct.pack('<I', len(names)))
        for n in names:
            size, data = state[n]
            f.write(struct.pack('<H', len(n)) + n.encode('ascii') + struct.pack('<I', len(size)))
            f.write(struct.pack(f'<{len(size)}I', *size))
            f.write(data.tobytes())       # (little-endian host: Windows x86-64)


# ---- pure-Python reference (slow: ~6 M multiply-adds per input pixel; a few pixels only) ------------------------------
def conv3(x, w, b, cin, cout, h, wd):
    """x: list of cin planes (lists of h*wd floats), zero padding, 3x3, stride 1."""
    out = []
    # im2col per pixel, shared by all output channels
    cols = []
    for y in range(h):
        for xx in range(wd):
            col = []
            for c in range(cin):
                p = x[c]
                for dy in (-1, 0, 1):
                    yy = y + dy
                    for dx in (-1, 0, 1):
                        x2 = xx + dx
                        col.append(p[yy * wd + x2] if 0 <= yy < h and 0 <= x2 < wd else 0.0)
            cols.append(col)
    k = cin * 9
    for o in range(cout):
        wr = w[o * k:(o + 1) * k]
        bo = b[o]
        out.append([bo + sum(map(float.__mul__, wr, col)) for col in cols])
    return out


def lrelu(p):
    return [v if v >= 0 else 0.2 * v for v in p]


def forward(state, arch, img, h, w):
    nf, gc = arch['num_feat'], arch['num_grow_ch']

    def L(name, x, cin, cout, hh, ww):
        return conv3(x, list(state[name + '.weight'][1]), list(state[name + '.bias'][1]), cin, cout, hh, ww)

    feat = L('conv_first', img, 3, nf, h, w)
    x = feat
    for bi in range(arch['num_block']):
        rin = x
        for r in (1, 2, 3):
            dense = list(x)
            for c in range(1, 5):
                dense += lrelu_planes(L(f'body.{bi}.rdb{r}.conv{c}', dense, nf + (c - 1) * gc, gc, h, w))
            x5 = L(f'body.{bi}.rdb{r}.conv5', dense, nf + 4 * gc, nf, h, w)
            x = [[a * 0.2 + bb for a, bb in zip(p5, p0)] for p5, p0 in zip(x5, x)]
        x = [[a * 0.2 + bb for a, bb in zip(p, p0)] for p, p0 in zip(x, rin)]
    body = L('conv_body', x, nf, nf, h, w)
    feat = [[a + bb for a, bb in zip(p, p0)] for p, p0 in zip(body, feat)]
    for name in ('conv_up1', 'conv_up2'):
        feat = [up2(p, h, w) for p in feat]
        h, w = h * 2, w * 2
        feat = lrelu_planes(L(name, feat, nf, nf, h, w))
    feat = lrelu_planes(L('conv_hr', feat, nf, nf, h, w))
    return L('conv_last', feat, nf, 3, h, w), h, w


def lrelu_planes(ps):
    return [lrelu(p) for p in ps]


def up2(p, h, w):
    out = []
    for y in range(2 * h):
        row = p[(y >> 1) * w:(y >> 1) * w + w]
        for v in row:
            out += (v, v)
    return out


def read_f32(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<II', d[:8])
    a = array.array('f')
    a.frombytes(d[8:8 + 12 * w * h])
    return [list(a[c * w * h:(c + 1) * w * h]) for c in range(3)], w, h


def write_f32(path, planes, w, h):
    with open(path, 'wb') as f:
        f.write(struct.pack('<II', w, h))
        for p in planes:
            f.write(array.array('f', p).tobytes())


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, model = argv[1], argv[2]
    state = load_state(model)
    arch = check_arch(state)
    if cmd == 'info':
        n = sum(len(v[1]) for v in state.values())
        print(f'RRDBNet {arch}: {len(state)} tensors, {n} parameters')
    elif cmd == 'export':
        export(state, arch, argv[3])
        print(f'wrote {argv[3]} ({arch})')
    elif cmd == 'ref':
        img, w, h = read_f32(argv[3])
        out, ow, oh = forward(state, arch, img, h, w)
        write_f32(argv[4], out, ow, oh)
        print(f'{w}x{h} -> {ow}x{oh}')
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
