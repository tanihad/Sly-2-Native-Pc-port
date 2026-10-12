"""Cross-recording tables from gs_census reports (scripts/gs_census.ps1 output).

    python scripts/census_table.py build/census/*.txt

Prints Markdown tables: per-recording totals (per game frame = VBlank with work), primitive mix,
targets other than the main double buffer, every distinct ZBUF/TEX1/CLAMP/TEXA/ALPHA/TEST/CLUT/
transfer/display line that is not part of the common pattern, and the non-sprite feedback draws."""
import glob
import re
import sys


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
    for a in sys.argv[1:]:
        files.extend(sorted(glob.glob(a)) or [a])
    rows = []
    odd = {}
    for f in files:
        lines = open(f, encoding='utf-8-sig').read().splitlines()
        name = re.sub(r'\.txt$', '', f.replace('\\', '/').split('/')[-1])
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
