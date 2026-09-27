"""The disc's banner and the memory card save's pictures, from art/.

    python tools/make_art.py        (needs Pillow; copy_data.py runs it too)

art/banner.png (96 x 32) and art/icon.png (32 x 32) become:

- CastleCrashers/opening.bnr: the disc banner Dolphin, Swiss and the console's
  own menu show. BNR1: "BNR1", padding to 0x20; the picture at 0x20, 96 x 32
  RGB5A3 in 4 x 4 tiles, big-endian (0x1800 bytes); then the short name
  (0x20), short maker (0x20), long name (0x40), long maker (0x40) and
  description (0x80). 0x1960 bytes. The packager puts a project's own
  opening.bnr on the disc in place of the engine's default.
- Scripts/Data/save_icon.bin: the save's icon on the card, 32 x 32 RGB5A3 in
  4 x 4 tiles (2048 bytes).
- Scripts/Data/save_banner.bin: the save's banner on the card, 96 x 32 CI8 in
  8 x 4 tiles, then its 256-colour RGB5A3 palette (3584 bytes).
"""

import struct
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parents[1]
ART = HERE / 'art'
PROJECT = HERE / 'CastleCrashers'
DATA = PROJECT / 'Scripts' / 'Data'

NAME = 'Castle Crashers'
MAKER = 'Octave Engine'  # as the engine's default banner
DESCRIPTION = 'Castle Crashers for the GameCube'


def rgb5a3(r, g, b, a=255):
    if a >= 0xE0:  # opaque: 1 RRRRR GGGGG BBBBB
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    return ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)  # 0 AAA RRRR GGGG BBBB


def text(value, size):
    data = value.encode('ascii')[:size - 1]
    return data + b'\0' * (size - len(data))


def picture(path, size):
    img = Image.open(path).convert('RGBA')
    if img.size != size:
        img = img.resize(size, Image.LANCZOS)
    return img


def rgb5a3_tiles(img):
    """4 x 4 tiles of big-endian RGB5A3."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for ty in range(0, h, 4):
        for tx in range(0, w, 4):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 4):
                    out += struct.pack('>H', rgb5a3(*px[x, y]))
    return bytes(out)


def ci8_tiles(img):
    """8 x 4 tiles of palette indices, then the 256-entry RGB5A3 palette."""
    w, h = img.size
    quant = img.quantize(256, method=Image.Quantize.FASTOCTREE)
    idx = quant.load()
    rgba_palette = quant.getpalette('RGBA') or []
    out = bytearray()
    for ty in range(0, h, 4):
        for tx in range(0, w, 8):
            for y in range(ty, ty + 4):
                for x in range(tx, tx + 8):
                    out.append(idx[x, y])
    for i in range(256):
        c = rgba_palette[i * 4:i * 4 + 4] if i * 4 + 4 <= len(rgba_palette) else [0, 0, 0, 255]
        out += struct.pack('>H', rgb5a3(*c))
    return bytes(out)


def main():
    banner = picture(ART / 'banner.png', (96, 32))
    icon = picture(ART / 'icon.png', (32, 32))

    bnr = bytearray(b'BNR1' + b'\0' * 0x1C) + rgb5a3_tiles(banner)
    bnr += text(NAME, 0x20) + text(MAKER, 0x20) + text(NAME, 0x40) + text(MAKER, 0x40) + text(DESCRIPTION, 0x80)
    assert len(bnr) == 0x1960, hex(len(bnr))
    (PROJECT / 'opening.bnr').write_bytes(bytes(bnr))

    DATA.mkdir(parents=True, exist_ok=True)
    save_icon = rgb5a3_tiles(icon)
    save_banner = ci8_tiles(banner)
    assert len(save_icon) == 2048 and len(save_banner) == 3584
    (DATA / 'save_icon.bin').write_bytes(save_icon)
    (DATA / 'save_banner.bin').write_bytes(save_banner)
    print(f'wrote {PROJECT / "opening.bnr"}, save_icon.bin and save_banner.bin')


if __name__ == '__main__':
    main()
