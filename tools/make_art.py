"""The disc's banner and the memory card save's pictures, from art/.

    python tools/make_art.py        (needs Pillow; copy_data.py runs it too)

art/banner.png (96 x 32), art/card_banner.png (96 x 32) and art/icon.png
(32 x 32) become:

- PPGC/opening.bnr: the disc banner Dolphin, Swiss and the console's
  own menu show. BNR1: "BNR1", padding to 0x20; the picture at 0x20, 96 x 32
  RGB5A3 in 4 x 4 tiles, big-endian (0x1800 bytes); then the short name
  (0x20), short maker (0x20), long name (0x40), long maker (0x40) and
  description (0x80). 0x1960 bytes. The packager puts a project's own
  opening.bnr on the disc in place of the engine's default.
- Scripts/Data/save_icon.bin: the save's icon on the card, 32 x 32 RGB5A3 in
  4 x 4 tiles (2048 bytes).
- Scripts/Data/save_banner.bin: the save's banner on the card (card_banner.png),
  96 x 32 CI8 in
  8 x 4 tiles, then its 256-colour RGB5A3 palette (3584 bytes).

And art/menu_background.png, the title screen's, becomes the menu mod's
picture (mod/menu/mod.txt), in Scripts/Data/mods/ppgc/menu/:

- background.png: the picture replaces the sky (shape 497, which covered
  x -2.3..661 and y 0..498.5 of the 848 x 480 stage), drawn 1.5639 times as
  large about the sky's centre: 1037.3 x 498.5 stage pixels from x -189.3.
  The picture, cut to the stage's shape, fills the stage's part of that; the
  rest (off the stage) repeats its edges.
The background is 1024 wide, as wide as GX allows, so it stays sharp when
Dolphin draws above the console's resolution.
"""

import shutil
import struct
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parents[1]
ART = HERE / 'art'
PROJECT = HERE / 'PPGC'
DATA = PROJECT / 'Scripts' / 'Data'

NAME = "Painter's Playground"
MAKER = 'Octave Engine'  # as the engine's default banner
DESCRIPTION = "Painter's Playground for the GameCube"


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


def menu_art():
    out = DATA / 'mods' / 'ppgc' / 'menu'
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(HERE / 'mod' / 'menu' / 'mod.txt', out / 'mod.txt')

    # The stage's part of the canvas.
    left, width, height = -189.3, 1037.3, 498.5
    cw = 1024  # the widest a GX texture can be
    ch = round(cw * height / width)
    x0 = round(-left / width * cw)
    y1 = round(480 / height * ch)
    src = Image.open(ART / 'menu_background.png').convert('RGB')
    sw, sh = src.size
    crop_h = round(sw * 480 / 848)
    top = min(max(0, sh - crop_h - 55), sh - crop_h)  # keep the painter, low in the picture
    stage = src.crop((0, top, sw, top + crop_h)).resize((cw - x0, y1), Image.LANCZOS)
    canvas = Image.new('RGB', (cw, ch))
    canvas.paste(stage, (x0, 0))
    canvas.paste(stage.crop((0, 0, 1, y1)).resize((x0, y1)), (0, 0))
    canvas.paste(canvas.crop((0, y1 - 1, cw, y1)).resize((cw, ch - y1)), (0, y1))
    canvas.save(out / 'background.png')


def main():
    banner = picture(ART / 'banner.png', (96, 32))
    card_banner = picture(ART / 'card_banner.png', (96, 32))
    icon = picture(ART / 'icon.png', (32, 32))

    bnr = bytearray(b'BNR1' + b'\0' * 0x1C) + rgb5a3_tiles(banner)
    bnr += text(NAME, 0x20) + text(MAKER, 0x20) + text(NAME, 0x40) + text(MAKER, 0x40) + text(DESCRIPTION, 0x80)
    assert len(bnr) == 0x1960, hex(len(bnr))
    (PROJECT / 'opening.bnr').write_bytes(bytes(bnr))

    DATA.mkdir(parents=True, exist_ok=True)
    save_icon = rgb5a3_tiles(icon)
    save_banner = ci8_tiles(card_banner)
    assert len(save_icon) == 2048 and len(save_banner) == 3584
    (DATA / 'save_icon.bin').write_bytes(save_icon)
    (DATA / 'save_banner.bin').write_bytes(save_banner)
    menu_art()
    print(f'wrote {PROJECT / "opening.bnr"}, save_icon.bin, save_banner.bin and the menu mod')


if __name__ == '__main__':
    main()
