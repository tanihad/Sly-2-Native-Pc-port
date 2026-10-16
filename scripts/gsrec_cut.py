"""Shorten a GS recording (runtime/gs_record.c, GSREC001) to its first N VBlanks.

    python scripts/gsrec_cut.py <in.gsrec> <out.gsrec> <vblanks>

Keeps the header (VRAM, CLUT, registers at the first VBlank) and every record up to and including
the N-th VBlank record; what the game sent after it (the start of frame N+1) is dropped, so the
result replays to exactly the same pictures for those N VBlanks. Only the end can be cut: the
start state is a snapshot taken when the recording began.
"""
import struct
import sys

HEADER = 8 + (4 << 20) + 1024 + 0x64 * 8
SNAP = 16 * 8


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    src, dst, keep = sys.argv[1], sys.argv[2], int(sys.argv[3])
    data = open(src, 'rb').read()
    if data[:8] != b'GSREC001':
        sys.exit(f'{src}: not a GS recording')
    pos, seen, last = HEADER, 0, None
    while pos + 8 <= len(data) and seen < keep:
        tag, ln = struct.unpack_from('<II', data, pos)
        pos += 8
        if tag == 1:
            pos += 8
        elif tag == 2:
            pos += (ln + 3) & ~3
        elif tag == 3:
            pos += SNAP
            seen += 1
            last = ln
        else:
            sys.exit(f'bad record tag {tag} at {pos - 8}')
    open(dst, 'wb').write(data[:pos])
    print(f'{dst}: {seen} VBlanks (last {last}), {pos / 2**20:.0f} MB of {len(data) / 2**20:.0f} MB')


if __name__ == '__main__':
    main()
