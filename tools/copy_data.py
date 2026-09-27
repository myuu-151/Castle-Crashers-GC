"""Copies the game's data from the Painters-Playground checkout's assets/ into
the Octave project's Scripts/Data, which the packager puts in the disc image
(PPGC/Scripts/Data).

    python tools/copy_data.py [path to the Painters-Playground checkout]

The audio stays out for now: it is xWMA, which the GameCube can't play.
It also runs make_art.py: the disc banner and the memory card save's icon
and banner, from art/.
"""
import shutil
import sys
from pathlib import Path

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
    make_art.main()


if __name__ == '__main__':
    main()
