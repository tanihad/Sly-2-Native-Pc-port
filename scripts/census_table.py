"""Cross-recording tables from gs_census reports (scripts/gs_census.ps1 output).

    python scripts/census_table.py build/census/*.txt
    python scripts/census_table.py build/census/*.txt --new "gpu-C-*" [--novel-only]

Prints Markdown tables: per-recording totals (per game frame = VBlank with work), primitive mix,
GPU hazards (TRXDIR=1 readbacks, TRXDIR=2 copies, CLUT loads from drawn memory: the `hazards:` line),
targets other than the main double buffer, every distinct ZBUF/TEX1/CLAMP/TEXA/ALPHA/TEST/CLUT/
transfer/display line that is not part of the common pattern, and the non-sprite feedback draws.

--new <glob,...>: the reports whose names match are the new recordings, all others the baseline;
prints, per new recording, every table line (pass signature, primitive combo, target/texture format,
ALPHA, TEST, TEX1, CLAMP, TEXA, CLUT, display, transfers, feedback, large sprites) whose content
(counts, frame ranges and drawn rectangles removed) appears in no baseline report. Reports made with
gs_census -a (gs_census.ps1 -Full) list every entry, so nothing is "new" just because a top-N list
cut it. --novel-only prints only that part."""
import fnmatch
import glob
import re
import sys

NOVEL_SECTIONS = ('pass signatures', 'primitive attribute combos', 'FRAME targets', 'ZBUF with FRAME', 'TEX0',
                  'texture sizes', 'TEX1', 'CLAMP', 'TEXA', 'CLUT loads', 'ALPHA', 'TEST', 'DIMX',
                  'feedback: textures reading drawn memory', 'large sprites', 'host->local uploads',
                  'local->local copies', 'local->host readbacks', 'display registers')


def norm(line):
    """a table line without its count, frame range and drawn rectangle, and with addresses, buffer
    widths, sizes and positions wildcarded: what is left is the kind of operation (formats, masks,
    blend/test forms, attributes)"""
    s = re.sub(r'^\s*\d+\s+f\d+-\d+\s+', '', line.strip())
    s = re.sub(r'\s+drawn=\S+ \(\d+x\d+\)$', '', s)
    s = re.sub(r'\b(tbp|base|fbp|dbp|sbp|zbp|cbp|tbw|dbw|sbw|fbw)=-?[0-9A-F]+(\(\d+px\))?', r'\1=*', s)
    s = re.sub(r'\b\d+x\d+\b', 'WxH', s)
    s = re.sub(r'\bat -?\d+,-?\d+', 'at *', s)
    s = re.sub(r'\b(scissor|xyoff)=\S+', r'\1=*', s)
    s = re.sub(r'\b-?\d+,-?\d+ ', '*,* ', s)
    s = re.sub(r'\(\d+ bytes\)', '', s)
    return s


def hazards(lines):
    h = next((l for l in lines if l.startswith('hazards:')), '')
    return h[len('hazards: '):] if h else '(no hazards line: report from an older gs_census)'


def sections(lines):
    out = {}
    cur = None
    for ln in lines:
        if ln.startswith('== '):
            cur = ln[3:].split(' (')[0]
            out[cur] = []
        elif cur and ln.strip():
            out[cur].append(ln)
    return out


def num(pat, s, default=0):
    m = re.search(pat, s)
    return int(m.group(1)) if m else default


def main():
    files = []
    new_pats = []
    novel_only = False
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--new':
            new_pats += [p for p in argv[i + 1].split(',') if p]
            i += 2
            continue
        if a == '--novel-only':
            novel_only = True
        else:
            files.extend(sorted(glob.glob(a)) or [a])
        i += 1
    rows = []
    odd = {}
    haz = []
    keys = {}                      # name -> {section: {normalised line: original line}}
    for f in files:
        lines = open(f, encoding='utf-8-sig').read().splitlines()
        name = re.sub(r'\.txt$', '', f.replace('\\', '/').split('/')[-1])
        haz.append((name, hazards(lines)))
        sec_all = sections(lines)
        keys[name] = {s: {norm(l): l.strip() for l in sec_all.get(s, []) if not l.strip().startswith('...')}
                      for s in NOVEL_SECTIONS}
        tot = next((l for l in lines if l.startswith('# totals')), '')
        prims = next((l for l in lines if l.startswith('prims=')), '')
        types = next((l for l in lines if l.startswith('types:')), '')
        attrs = next((l for l in lines if l.startswith('attrs:')), '')
        fb = next((l for l in lines if l.startswith('feedback prims')), '')
        tr = next((l for l in lines if l.startswith('transfers:')), '')
        vb = num(r'over (\d+) VBlanks', tot)
        work = num(r'\((\d+) with work', tot)
        # game frames: VBlanks with >= 1000 prims (the small ones are the motion-blur VBlank)
        big = [l for l in lines if l.startswith('f') and ' prims=' in l and num(r'prims=(\d+)', l) >= 1000]
        nbig = len(big) or 1
        pr = [num(r'prims=(\d+)', l) for l in big]
        st = [num(r'states=(\d+)', l) for l in big]
        un = [num(r'uniq=(\d+)', l) for l in big]
        tx = [num(r'tex=\d+/(\d+)', l) for l in big]
        cl = [num(r'clut=\d+/(\d+)', l) for l in big]
        ve = [num(r'verts=(\d+)', l) for l in big]
        ar = [num(r'area=(\d+)K', l) for l in big]
        up = [num(r'up=\d+/(\d+)K', l) for l in big]
        P = sum(pr) / nbig
        rows.append((name, vb, work, len(big), P, max(pr or [0]), sum(ve) / nbig, sum(st) / nbig, max(st or [0]),
                     sum(un) / nbig, sum(tx) / nbig, sum(cl) / nbig, sum(ar) / nbig / 1024, sum(up) / nbig,
                     num(r'line=(\d+)', types) + num(r'linestrip=(\d+)', types), num(r'tristrip=(\d+)', types),
                     num(r'trifan=(\d+)', types), num(r'sprite=(\d+)', types), num(r'point=(\d+)', types),
                     num(r'aa1=(\d+)', attrs), num(r'date=(\d+)', attrs), num(r'fba=(\d+)', attrs),
                     num(r'colclamp0=(\d+)', attrs), num(r'same-frame=(\d+)', fb), num(r'prev-frame=(\d+)', fb),
                     num(r'copies=(\d+)', tr), num(r'readbacks=(\d+)', tr), num(r'uploads=(\d+)', tr), types, attrs))
        sec = sections(lines)
        o = odd.setdefault(name, [])
        for l in sec.get('FRAME targets', []):
            if not re.search(r'fbp=(000|038) ', l):
                o.append(('FRAME', l.strip()))
        for l in sec.get('ZBUF with FRAME', []):
            if 'zbp=070 Z32' not in l:
                o.append(('ZBUF', l.strip()))
        for l in sec.get('TEX1', []):
            if 'mxl=0' not in l or 'lcm=1' not in l:
                o.append(('TEX1', l.strip()))
        for l in sec.get('CLAMP', []):
            if 'RCLAMP' in l or 'RREPEAT' in l:
                o.append(('CLAMP', l.strip()))
        for l in sec.get('TEXA', []):
            if 'ta0=0 aem=0 ta1=128' not in l:
                o.append(('TEXA', l.strip()))
        for l in sec.get('CLUT loads', []):
            if 'csm=1' in l or 'csa=' in l and 'csa=0' not in l or 'cpsm=CT32' not in l or 'DRAWN' in l:
                o.append(('CLUT', l.strip()))
        for l in sec.get('texture sizes', []):
            if re.search(r'(CT32|CT16|CT24|T8H|T4H|Z\d)', l):
                o.append(('TEXSZ', l.strip()))
        for k in ('local->local copies', 'local->host readbacks', 'host->local uploads'):
            for l in sec.get(k, []):
                o.append((k.split('-')[0].upper(), l.strip()))
        for l in sec.get('feedback: textures reading drawn memory', []):
            if not l.strip().split('  ')[-1].startswith('sprite'):
                o.append(('FEEDBACK', l.strip()))
        for l in sec.get('display registers', []):
            if 'DISPLAY' in l or 'PMODE' in l:
                o.append(('DISPLAY', l.strip()))
        for l in sec.get('ALPHA', []):
            o.append(('ALPHA', l.strip()))
        for l in sec.get('TEST', [])[:40]:
            o.append(('TEST', l.strip()))
        for l in sec.get('primitive attribute combos', []):
            o.append(('PRIM', l.strip()))
        for l in sec.get('distinct FRAME FBPs per frame', []):
            o.append(('NTARGETS', l.strip()))

    is_new = {n: any(fnmatch.fnmatch(n, p) for p in new_pats) for n in keys}
    if new_pats:
        base = {}
        for n, secs in keys.items():
            if not is_new[n]:
                for s, d in secs.items():
                    base.setdefault(s, set()).update(d)
        print('## Novel versus the baseline (%s)' % ', '.join(n for n in keys if not is_new[n]))
        print()
        seen_new = {}
        for n, secs in keys.items():
            if not is_new[n]:
                continue
            out = []
            for s in NOVEL_SECTIONS:
                for k, l in secs[s].items():
                    if k not in base.get(s, set()):
                        first = (s, k) not in seen_new
                        seen_new.setdefault((s, k), n)
                        out.append((s, l, '' if first else ' (also in %s)' % seen_new[(s, k)]))
            print('### %s: %d novel lines' % (n, len(out)))
            last = None
            for s, l, also in out:
                if s != last:
                    print('- **%s**' % s)
                    last = s
                print('  - `%s`%s' % (l, also))
            print()
    print('| recording | GPU hazards (gs_census `hazards:`) |')
    print('|---|---|')
    for n, h in haz:
        print('| %s | %s |' % (n, h))
    print()
    if novel_only:
        return
    print('| recording | VBlanks | game frames | tris/frame (max) | verts/frame | state changes/frame (max) | distinct states | distinct textures | distinct CLUTs | ~Mpx/frame | upload KB/frame |')
    print('|---|---|---|---|---|---|---|---|---|---|---|')
    for r in rows:
        print('| %s | %d | %d | %.0f (%d) | %.0f | %.0f (%d) | %.0f | %.0f | %.0f | %.1f | %.0f |' % (r[0], r[1], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13]))
    print()
    print('| recording | lines(+strips) | tristrip | trifan | sprite | point | AA1 | DATE | FBA | COLCLAMP0 | feedback prims same/prev frame | copies | readbacks | uploads |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for r in rows:
        print('| %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d / %d | %d | %d | %d |' % (r[0], r[14], r[15], r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23], r[24], r[25], r[26], r[27]))
    print()
    for name, o in odd.items():
        print('### %s' % name)
        last = None
        for k, l in o:
            if k != last:
                print('- **%s**' % k)
                last = k
            print('  - `%s`' % l)
        print()


if __name__ == '__main__':
    main()
