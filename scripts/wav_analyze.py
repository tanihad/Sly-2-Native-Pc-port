"""Analyse a 16-bit stereo WAV recorded with PS2_AUDIO_WAV (pure Python, no numpy).

Usage:
    <python> scripts\\wav_analyze.py <file.wav> [--window 0.5] [--fft] [--from S] [--to S] [--json out.json]

Prints:
  - a timeline of levels per window (RMS and peak per channel in dBFS, L/R balance, clipped samples);
  - sound/silence segments (10 ms resolution, silence = RMS below --silence dBFS on both channels);
  - dropouts: runs of digital zero (>= 1 ms) or sudden level collapses inside sounding audio, the
    usual sign of an underrun or a stream that ran dry;
  - clicks: sample-to-sample jumps far above the local signal slope;
  - dominant frequencies per sounding segment (Hann-windowed FFT of the mid channel, peaks with
    parabolic interpolation) and stereo correlation/balance per segment.
The header may be stale when the game was stopped with ExitProcess (it is refreshed every 250 ms),
so the data length is taken from the file size.
"""
import argparse
import array
import cmath
import json
import math
import struct
import sys


def load(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(raw):
        cid, size = raw[pos:pos + 4], struct.unpack_from("<I", raw, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", raw, body)
        elif cid == b"data":
            end = len(raw) if body + size > len(raw) or size == 0 else body + size
            data = raw[body:len(raw)]      # header may lag behind: use everything after it
            break
        pos = body + size + (size & 1)
    if not fmt or data is None:
        sys.exit(f"{path}: no fmt/data chunk")
    tag, ch, rate, _, align, bits = fmt
    if tag != 1 or bits != 16 or ch != 2:
        sys.exit(f"{path}: expected 16-bit stereo PCM, got tag {tag}, {ch} ch, {bits} bits")
    n = len(data) // 4
    s = array.array("h")
    s.frombytes(data[:n * 4])
    if sys.byteorder != "little":
        s.byteswap()
    return rate, s[0::2], s[1::2]


def db(x):
    return 20 * math.log10(x / 32768.0) if x > 0 else -120.0


def rms(seg):
    return math.sqrt(sum(v * v for v in seg) / len(seg)) if len(seg) else 0.0


def fft(x):
    """Iterative radix-2 FFT of a list of complex numbers (len power of two)."""
    n = len(x)
    a = list(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        half = size // 2
        tw = [w ** k for k in range(half)]
        for start in range(0, n, size):
            for k in range(half):
                u, v = a[start + k], a[start + k + half] * tw[k]
                a[start + k], a[start + k + half] = u + v, u - v
        size *= 2
    return a


def peaks(mid, rate, n=8192, top=5):
    """Dominant frequencies (Hz, dB relative to the strongest) of `mid` (len >= n)."""
    hann = [0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1)) for i in range(n)]
    spec = fft([mid[i] * hann[i] for i in range(n)])
    mag = [abs(c) for c in spec[:n // 2]]
    found = []
    for k in range(2, n // 2 - 1):
        if mag[k] > mag[k - 1] and mag[k] >= mag[k + 1]:
            found.append(k)
    found.sort(key=lambda k: -mag[k])
    best = mag[found[0]] if found else 1.0
    out = []
    for k in found[:top]:
        a, b, c = (math.log(max(mag[k + d], 1e-9)) for d in (-1, 0, 1))
        p = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
        out.append((round((k + p) * rate / n, 1), round(20 * math.log10(mag[k] / best), 1)))
    total = sum(m * m for m in mag) or 1.0
    hi = sum(mag[k] ** 2 for k in range(int(10000 * n / rate), n // 2)) / total
    centroid = sum(k * mag[k] ** 2 for k in range(n // 2)) / total * rate / n
    return out, centroid, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--window", type=float, default=0.5, help="timeline window in seconds")
    ap.add_argument("--silence", type=float, default=-66.0, help="silence threshold, dBFS RMS")
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    ap.add_argument("--fft", action="store_true", help="dominant frequencies per sounding segment")
    ap.add_argument("--json", help="write the per-window levels and segments as JSON")
    args = ap.parse_args()

    rate, L, R = load(args.wav)
    a = int(args.t0 * rate)
    b = min(len(L), int(args.t1 * rate))
    L, R = L[a:b], R[a:b]
    n = len(L)
    print(f"{args.wav}: {rate} Hz, {n} frames = {n / rate:.2f} s (from {args.t0:.2f} s)")
    if not n:
        return

    # ---- whole file
    pl, pr = max(map(abs, L)), max(map(abs, R))
    clip_l = sum(1 for v in L if v >= 32767 or v <= -32768)
    clip_r = sum(1 for v in R if v >= 32767 or v <= -32768)
    rl, rr = rms(L), rms(R)
    dcl, dcr = sum(L) / n, sum(R) / n
    print(f"overall: RMS L {db(rl):6.1f} R {db(rr):6.1f} dBFS, peak L {db(pl):6.1f} R {db(pr):6.1f} dBFS, "
          f"clipped samples L {clip_l} R {clip_r}, DC L {dcl:.1f} R {dcr:.1f}")

    # ---- timeline
    w = max(1, int(args.window * rate))
    timeline = []
    print(f"\n{'time':>7} {'rmsL':>6} {'rmsR':>6} {'pkL':>6} {'pkR':>6} {'bal':>5} {'clip':>5}")
    for s in range(0, n, w):
        sl, sr = L[s:s + w], R[s:s + w]
        a_l, a_r = rms(sl), rms(sr)
        p_l, p_r = max(map(abs, sl)), max(map(abs, sr))
        c = sum(1 for v in sl if v >= 32767 or v <= -32768) + sum(1 for v in sr if v >= 32767 or v <= -32768)
        bal = db(a_l) - db(a_r) if a_l > 0 and a_r > 0 else 0.0
        t = args.t0 + s / rate
        timeline.append(dict(t=round(t, 3), rms_l=round(db(a_l), 1), rms_r=round(db(a_r), 1),
                             peak_l=round(db(p_l), 1), peak_r=round(db(p_r), 1), clip=c))
        print(f"{t:7.2f} {db(a_l):6.1f} {db(a_r):6.1f} {db(p_l):6.1f} {db(p_r):6.1f} {bal:5.1f} {c:5d}")

    # ---- sound / silence segments at 10 ms resolution
    step = rate // 100
    loud = []
    for s in range(0, n, step):
        loud.append(max(rms(L[s:s + step]), rms(R[s:s + step])) > 32768 * 10 ** (args.silence / 20))
    segs, start = [], 0
    for i in range(1, len(loud) + 1):
        if i == len(loud) or loud[i] != loud[start]:
            segs.append((loud[start], start * step, min(n, i * step)))
            start = i
    print("\nsegments (10 ms resolution):")
    for is_loud, s0, s1 in segs:
        if (s1 - s0) < rate // 50 and not is_loud:
            tag = "gap!"           # a <20 ms silence inside sound: likely a dropout
        else:
            tag = ""
        print(f"  {'sound  ' if is_loud else 'silence'} {args.t0 + s0 / rate:8.2f} - {args.t0 + s1 / rate:8.2f} "
              f"({(s1 - s0) / rate:6.2f} s) {tag}")

    # ---- dropouts: exact-zero runs (>= 1 ms) with sound on both sides within 50 ms
    print("\ndropouts (digital-zero runs >= 1 ms between sounding audio):")
    found = 0
    i = 0
    ms = rate // 1000
    while i < n:
        if L[i] == 0 and R[i] == 0:
            j = i
            while j < n and L[j] == 0 and R[j] == 0:
                j += 1
            if j - i >= ms and i > 0 and j < n:
                before = rms(L[max(0, i - 2400):i]) + rms(R[max(0, i - 2400):i])
                after = rms(L[j:j + 2400]) + rms(R[j:j + 2400])
                if before > 30 and after > 30:
                    found += 1
                    if found <= 40:
                        print(f"  {args.t0 + i / rate:8.3f} s: {(j - i) / ms:.1f} ms of zeros "
                              f"(level before {db(before / 2):.0f} dB, after {db(after / 2):.0f} dB)")
            i = j
        else:
            i += 1
    print(f"  {found} found")

    # ---- clicks: jumps much larger than the local slope
    print("\nclicks (|delta| > 8x local mean |delta| and > 4000):")
    clicks = 0
    for ch_name, X in (("L", L), ("R", R)):
        d = [abs(X[k + 1] - X[k]) for k in range(n - 1)]
        blk = 480
        for s in range(0, len(d), blk):
            seg = d[s:s + blk]
            mean = sum(seg) / len(seg)
            mx = max(seg)
            if mx > 4000 and mx > 8 * mean:
                clicks += 1
                if clicks <= 30:
                    k = s + seg.index(mx)
                    print(f"  {ch_name} {args.t0 + k / rate:8.3f} s: jump {mx} (local mean {mean:.0f})")
    print(f"  {clicks} found")

    # ---- per segment: stereo correlation, balance, dominant frequencies
    if args.fft:
        print("\nsounding segments: balance, correlation, dominant frequencies (mid channel):")
        for is_loud, s0, s1 in segs:
            if not is_loud or s1 - s0 < 8192:
                continue
            sl, sr = L[s0:s1], R[s0:s1]
            m = len(sl)
            sxy = sum(x * y for x, y in zip(sl, sr))
            sxx = sum(x * x for x in sl)
            syy = sum(y * y for y in sr)
            corr = sxy / math.sqrt(sxx * syy) if sxx and syy else 0.0
            print(f"  {args.t0 + s0 / rate:7.2f}-{args.t0 + s1 / rate:7.2f}: L {db(math.sqrt(sxx / m)):6.1f} "
                  f"R {db(math.sqrt(syy / m)):6.1f} dBFS, corr {corr:+.2f}")
            # up to 4 analysis points spread over the segment
            pts = min(4, max(1, (s1 - s0) // 8192))
            for p in range(pts):
                c = s0 + (s1 - s0 - 8192) * (p + 1) // (pts + 1)
                mid = [(L[c + i] + R[c + i]) * 0.5 for i in range(8192)]
                pk, cen, hi = peaks(mid, rate)
                print(f"      @{args.t0 + c / rate:7.2f} s: centroid {cen:6.0f} Hz, >10 kHz {hi * 100:4.1f}%, peaks "
                      + ", ".join(f"{f:.0f} Hz ({d:+.0f})" for f, d in pk))

    if args.json:
        with open(args.json, "w") as f:
            json.dump(dict(rate=rate, frames=n, timeline=timeline,
                           segments=[dict(sound=s[0], t0=args.t0 + s[1] / rate, t1=args.t0 + s[2] / rate) for s in segs]),
                      f, indent=1)


if __name__ == "__main__":
    main()
