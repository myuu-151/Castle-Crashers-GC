"""Copies the game's data from the Painters-Playground checkout's assets/ into
the Octave project's Scripts/Data, which the packager puts in the disc image
(PPGC/Scripts/Data).

    python tools/copy_data.py [path to the Painters-Playground checkout]

It also writes files.txt (every file's size), and runs convert_audio.py (the sound, made Microsoft ADPCM: the xWMA
the PC plays can't be decoded on the GameCube) and make_art.py (the disc
banner and the memory card save's icon and banner, from art/).
"""
import shutil
import struct
import sys
from pathlib import Path

import convert_audio
import make_art

HERE = Path(__file__).resolve().parents[1]
CC_REPO = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / 'PaintersPlayground'
SOURCE = CC_REPO / 'assets'
TARGET = HERE / 'PPGC' / 'Scripts' / 'Data'
FOLDERS = ['swf', 'fonts', 'text', 'bsp']


def main():
    total = 0
    for folder in FOLDERS:
        src = SOURCE / folder
        dst = TARGET / folder
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(src, dst)
        size = sum(f.stat().st_size for f in dst.rglob('*') if f.is_file())
        total += size
        print(f'{folder}: {size / 1e6:.1f} MB')
    print(f'{total / 1e6:.1f} MB in {TARGET}')
    convert_audio.main()
    write_sizes()
    make_art.main()


HOLE_MIN = 64 * 1024  # bitmaps' pixels this big or bigger are read apart


def pixel_holes(path):
    """A SWF's large bitmaps' pixels (tag 148 at the top level, its "PIXL"
    header's words 9 and 12 their start and end): (offset, size) in the
    file. The game reads the file without them and each bitmap's pixels
    alone, straight into its texture (files::read_holed): read whole, a SWF
    with two 1 MB skies needed 2.35 MB in one piece, more than was left."""
    d = path.read_bytes()
    if len(d) < 21 or d[:3] != b'FWS':
        return []
    holes = []
    pos = 8 + (5 + 4 * (d[8] >> 3) + 7) // 8 + 4
    while pos + 2 <= len(d):
        code_len = struct.unpack_from('<H', d, pos)[0]
        code, length, header = code_len >> 6, code_len & 0x3F, 2
        if length == 0x3F:
            length, header = struct.unpack_from('<I', d, pos + 2)[0], 6
        body = pos + header
        if body + length > len(d):
            break
        if code == 148:
            pixl = d.find(b'LXIP', body + 20, body + length - 60)
            if pixl >= 0:
                start, end = struct.unpack_from('<I', d, pixl + 36)[0], struct.unpack_from('<I', d, pixl + 48)[0]
                if end - start >= HOLE_MIN and pixl + end <= body + length:
                    holes.append((pixl + start, end - start))
        if code == 0:
            break
        pos = body + length
    return holes


def write_sizes():
    """files.txt: every file's size, so the game can read a file straight
    into the memory it keeps (Octave's whole-file read gives a copy of its
    own, and the second copy the game makes needs as much again, in one
    piece: in the keep it didn't have it); after a SWF's size, its large
    bitmaps' pixels as offset:size (pixel_holes)."""
    lines = []
    holed = 0
    for f in sorted(TARGET.rglob('*')):
        if f.is_file() and f.name != 'files.txt':
            line = f'{f.relative_to(TARGET).as_posix()} {f.stat().st_size}'
            if f.suffix == '.swf':
                holes = pixel_holes(f)
                holed += bool(holes)
                line += ''.join(f' {o}:{s}' for o, s in holes)
            lines.append(line)
    (TARGET / 'files.txt').write_text('\n'.join(lines) + '\n', newline='\n')
    print(f'files.txt: {len(lines)} files, {holed} with bitmaps read apart')


if __name__ == '__main__':
    main()
