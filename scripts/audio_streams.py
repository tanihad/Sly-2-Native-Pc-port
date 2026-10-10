"""Check spiop's disc streams (music, dialogue) in a recorded run against the disc.

Needs a run with PS2_LOG_SND=1 (989snd commands with results), frame dumps (the
"[dump] frame_N.png at audio T s" lines map VBlanks to WAV time) and PS2_AUDIO_WAV:

    $env:PS2_LOG_SND="1"; scripts\\run_game.ps1 -Seconds 300 -Wav build\\audio\\x.wav -Dump build\\audio\\x \\
        -DumpEvery 90 -Pad "600:square:12,t95:start:10"
    <python> scripts\\audio_streams.py build\\sly2\\run.log build\\audio\\x.wav [--correlate]

For every stream the game opened through spiop ('SUKR' proc 1) it prints the header from the disc
(VAGp mono or " KPV" interleaved, size, rate, length) and, for one-shot streams, the time between
resume (proc 4) and the first "finished" answer (proc 8 = 0) against the header length. The game
ends a dialogue line on that answer, so the two must agree. It also prints the WAV level while each
one-shot stream plays and in the gaps between them.

--correlate: decodes each VAGp stream from the disc with an independent ADPCM decoder, resamples
it to 48 kHz and compares it with the WAV in 0.2 s windows (best lag within +-3 ms, normalised
correlation). Expect ~0.98 where the voice dominates; the lag drifts by the pitch quantisation
(22050 Hz plays at pitch 0759h = 22043 Hz, 15 samples/s), as on the PS2.
"""
import argparse
import array
import bisect
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ISO_DIR  # noqa: E402

SND = re.compile(r"^\[snd\] v(\d+) (direct )?cmd (-?\d+) \((\d+) bytes\):((?: [0-9A-F]{8})*) -> ([0-9A-F]{8})")
DUMP = re.compile(r"^\[dump\] frame_(\d+)\.png at audio ([\d.]+) s")
SUKR = "524B5553"
F0 = [0, 60, 115, 98, 122]
F1 = [0, 0, -52, -55, -60]


def parse_log(path):
    vt, opens, resumed, finished = [], {}, {}, {}
    for line in open(path, errors="replace"):
        d = DUMP.match(line)
        if d:
            vt.append((int(d.group(1)), float(d.group(2))))
            continue
        m = SND.match(line)
        if not m:
            continue
        v, cmd, words, res = int(m.group(1)), int(m.group(3)), m.group(5).split(), int(m.group(6), 16)
        if cmd not in (76, 104) or len(words) < 3 or words[0] != SUKR:
            continue
        proc = int(words[1], 16)
        if proc == 1 and res and len(words) >= 8:       # size, sector, volume, ?, channel, mode
            opens[res] = dict(v=v, sector=int(words[3], 16), channel=int(words[6], 16), mode=int(words[7], 16))
        elif proc == 4:
            resumed.setdefault(int(words[2], 16), v)
        elif proc == 8 and res == 0:
            finished.setdefault(int(words[2], 16), v)
    return vt, opens, resumed, finished


def header(iso, sector):
    iso.seek(sector * 2048)
    h = iso.read(64)
    if h[:4] == b"VAGp":
        return "VAGp", struct.unpack(">I", h[12:16])[0], struct.unpack(">I", h[16:20])[0]
    if h[:4] == b" KPV":
        return "KPV", struct.unpack("<I", h[4:8])[0], struct.unpack("<I", h[16:20])[0]
    return repr(h[:4]), 0, 0


def decode_vag(iso, sector, size):
    iso.seek(sector * 2048 + 48)
    data = iso.read(size)
    out, h0, h1 = [], 0, 0
    for b in range(0, len(data) - 15, 16):
        blk = data[b:b + 16]
        sh, flt = blk[0] & 15, (blk[0] >> 4) & 7
        sh = 9 if sh > 12 else sh
        flt = 0 if flt > 4 else flt
        for i in range(28):
            nib = (blk[2 + i // 2] >> (4 * (i & 1))) & 15
            t = ((nib << 12) - (0x10000 if nib & 8 else 0)) >> sh
            s = max(-32768, min(32767, t + ((h0 * F0[flt] + h1 * F1[flt] + 32) >> 6)))
            h1, h0 = h0, s
            out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("wav")
    ap.add_argument("--iso", default=None)
    ap.add_argument("--correlate", action="store_true")
    args = ap.parse_args()
    iso_path = args.iso or next(iter(sorted(ISO_DIR.glob("*.iso"))), None)
    if not iso_path:
        sys.exit("no disc image: pass --iso")
    iso = open(iso_path, "rb")
    vt, opens, resumed, finished = parse_log(args.log)
    if len(vt) < 2:
        sys.exit("need [dump] lines (run with -Dump) to map VBlanks to WAV time")
    vs = [a for a, _ in vt]

    def t_of(v):
        i = min(max(bisect.bisect_left(vs, v), 1), len(vt) - 1)
        (v0, t0), (v1, t1) = vt[i - 1], vt[i]
        return t0 + (v - v0) * (t1 - t0) / (v1 - v0)

    raw = open(args.wav, "rb").read()
    p = raw.find(b"data") + 8
    s = array.array("h")
    s.frombytes(raw[p:p + (len(raw) - p) // 4 * 4])

    def level(a, b):
        a, b = int(a * 48000), int(b * 48000)
        seg = s[2 * a:2 * b]
        if not seg:
            return -120.0
        r = math.sqrt(sum(x * x for x in seg) / len(seg))
        return 20 * math.log10(r / 32768) if r else -120.0

    print(f"{len(opens)} streams opened")
    prev_end = None
    for h, o in sorted(opens.items()):
        kind, size, rate = header(iso, o["sector"])
        length = (size // 16) * 28 / rate if rate else 0.0
        loops = " (loops)" if o["mode"] & 4 else ""
        print(f"handle {h:3d} v{o['v']:5d} channel {o['channel']} sector {o['sector']} {kind} {size} bytes "
              f"@ {rate} Hz = {length:6.2f} s{loops}")
        if h not in resumed or h not in finished:
            continue
        ta, tb = t_of(resumed[h]), t_of(finished[h])
        if prev_end is not None and ta > prev_end:
            print(f"      gap before it: {prev_end:7.2f}-{ta:7.2f} s, WAV {level(prev_end + 0.05, ta - 0.05):6.1f} dBFS")
        print(f"      played {ta:7.2f}-{tb:7.2f} s = {tb - ta:6.2f} s ({tb - ta - length:+.2f} s against the header), "
              f"WAV {level(ta, tb):6.1f} dBFS")
        prev_end = tb
        if not (args.correlate and kind == "VAGp"):
            continue
        pcm = decode_vag(iso, o["sector"], size)
        ratio = rate / 48000.0
        n48 = int(len(pcm) / ratio)

        def ref_at(i):
            x = i * ratio
            k = int(x)
            fr = x - k
            a = pcm[k] if k < len(pcm) else 0
            c = pcm[k + 1] if k + 1 < len(pcm) else 0
            return a + (c - a) * fr

        # coarse alignment on the first second, then 0.2 s windows around it
        W = 9600
        a0, best = int(ta * 48000), (-2.0, 0)
        ref = [ref_at(i) for i in range(0, min(n48, 48000), 4)]
        rr = math.sqrt(sum(v * v for v in ref)) or 1.0
        for lag in range(-int(0.4 * 48000), int(0.4 * 48000), 24):
            b = a0 + lag
            mid = [(s[2 * (b + i)] + s[2 * (b + i) + 1]) * 0.5 for i in range(0, min(n48, 48000), 4)]
            mm = math.sqrt(sum(v * v for v in mid)) or 1.0
            c = sum(x * y for x, y in zip(ref, mid)) / (rr * mm)
            if c > best[0]:
                best = (c, b)
        start = best[1]
        corrs, lags = [], []
        for w in range(0, n48 - W, W):
            ref = [ref_at(w + i) for i in range(0, W, 2)]
            rr = math.sqrt(sum(v * v for v in ref)) or 1.0
            if 20 * math.log10(rr / math.sqrt(len(ref)) / 32768 + 1e-9) < -30:
                continue                                  # a pause in the line
            wb = (-2.0, 0)
            for lag in range(-144, 145, 2):
                b = start + w + lag
                mid = [(s[2 * (b + i)] + s[2 * (b + i) + 1]) * 0.5 for i in range(0, W, 2)]
                mm = math.sqrt(sum(v * v for v in mid)) or 1.0
                c = sum(x * y for x, y in zip(ref, mid)) / (rr * mm)
                if c > wb[0]:
                    wb = (c, lag)
            corrs.append(wb[0])
            lags.append((w, wb[1]))
        if corrs:
            corrs.sort()
            drift = (lags[-1][1] - lags[0][1]) / ((lags[-1][0] - lags[0][0]) / 48000) if len(lags) > 1 else 0.0
            print(f"      decoded from disc vs WAV: starts at {start / 48000:.3f} s, {len(corrs)} windows, "
                  f"median correlation {corrs[len(corrs) // 2]:.3f} (min {corrs[0]:.3f}), drift {drift:+.1f} samples/s")


if __name__ == "__main__":
    main()
