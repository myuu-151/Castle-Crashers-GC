"""Copies the game's data from the Castle-Crashers checkout's assets/ into the
Octave project's Scripts/Data, which the packager puts in the disc image
(CastleCrashers/Scripts/Data).

    python tools/copy_data.py [path to the Castle-Crashers checkout]

The audio stays out for now: it is xWMA, which the GameCube can't play.
Also writes save_icon.bin, the memory card's icon for the save: the game's
icon (engine/resources/icon.png) at 32 x 32 in RGB5A3, GX's 4 x 4 tiles.
"""
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
CC_REPO = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / 'CastleCrashers-GC'
SOURCE = CC_REPO / 'assets'
TARGET = HERE / 'CastleCrashers' / 'Scripts' / 'Data'
FOLDERS = ['swf', 'fonts', 'text', 'bsp']


def save_icon(png, out):
    from PIL import Image
    im = Image.open(png).convert('RGBA').resize((32, 32), Image.LANCZOS)
    px = im.load()
    data = bytearray()
    for ty in range(0, 32, 4):
        for tx in range(0, 32, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    r, g, b, a = px[x, y]
                    if a >= 224:
                        v = 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
                    else:
                        v = (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)
                    data += v.to_bytes(2, 'big')
    out.write_bytes(bytes(data))


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
    save_icon(CC_REPO / 'engine' / 'resources' / 'icon.png', TARGET / 'save_icon.bin')
    print(f'{total / 1e6:.1f} MB in {TARGET}, and save_icon.bin')


if __name__ == '__main__':
    main()
