"""Copies the game's data from the Painters-Playground checkout's assets/ into
the Octave project's Scripts/Data, which the packager puts in the disc image
(PPGC/Scripts/Data).

    python tools/copy_data.py [path to the Painters-Playground checkout]

It also writes files.txt (every file's size), and runs convert_audio.py (the sound, made Microsoft ADPCM: the xWMA
the PC plays can't be decoded on the GameCube) and make_art.py (the disc
banner and the memory card save's icon and banner, from art/).
"""
import shutil
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


def write_sizes():
    """files.txt: every file's size, so the game can read a file straight
    into the memory it keeps (Octave's whole-file read gives a copy of its
    own, and the second copy the game makes needs as much again, in one
    piece: in the keep it didn't have it)."""
    lines = []
    for f in sorted(TARGET.rglob('*')):
        if f.is_file() and f.name != 'files.txt':
            lines.append(f'{f.relative_to(TARGET).as_posix()} {f.stat().st_size}')
    (TARGET / 'files.txt').write_text('\n'.join(lines) + '\n', newline='\n')
    print(f'files.txt: {len(lines)} files')


if __name__ == '__main__':
    main()
