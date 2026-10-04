"""The game's font page, packed for the GameCube: only the glyphs it can show.

    python tools/pack_font.py

synjUnicode_20_0.dds is a 1024 x 1024 page of 2,026 glyphs, most of them
Chinese, Japanese and Korean for the other languages' text; the GameCube game
is English only (CastleGame loads en.txt), which uses 98. Its texture took
868 KB of main memory for the whole game. This keeps the characters en.txt
uses and printable ASCII (numbers and names put together in play), each
copied with the 2 pixels around it in the page as they are, so
the texture's filtering at a glyph's edge reads what it read before: the
text looks the same. The .fnt's glyphs are moved to match; the page is as
wide as before and only as tall as they need (a multiple of 4).

Reads the Castle-Crashers-Recomp checkout's assets/fonts (as copy_data.py),
writes CCGC/Scripts/Data/fonts. The PC game keeps the whole page.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
CC_REPO = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / 'CastleCrashersRecomp'
SOURCE = CC_REPO / 'assets' / 'fonts'
TARGET = HERE / 'CCGC' / 'Scripts' / 'Data' / 'fonts'
TEXT = HERE / 'CCGC' / 'Scripts' / 'Data' / 'text' / 'en.txt'
FONT = 'synjUnicode_20'
BORDER = 2  # pixels kept around each glyph, as they are in the page
ASCII = set(range(32, 127))


def blocks(fnt):
    """The .fnt's blocks: (type, body start, length)."""
    assert fnt[:4] == b'BMF\x03', 'not a BMFont v3 binary'
    pos = 4
    while pos + 5 <= len(fnt):
        kind, length = fnt[pos], struct.unpack_from('<I', fnt, pos + 1)[0]
        yield kind, pos + 5, length
        pos += 5 + length


def main():
    fnt = bytearray((SOURCE / f'{FONT}.fnt').read_bytes())
    dds = (SOURCE / f'{FONT}_0.dds').read_bytes()
    assert dds[:4] == b'DDS ' and struct.unpack_from('<I', dds, 88)[0] == 32, 'not a 32-bit DDS'
    height, width = struct.unpack_from('<II', dds, 12)
    pixels = dds[128:128 + width * height * 4]

    used = {ord(c) for c in TEXT.read_text(encoding='utf-8')}
    chars = next((b, n) for k, b, n in blocks(fnt) if k == 4)
    glyphs = []  # (id, record offset, x, y, w, h)
    for c in range(chars[0], chars[0] + chars[1], 20):
        gid, x, y, w, h = struct.unpack_from('<IHHHH', fnt, c)
        glyphs.append((gid, c, x, y, w, h))
    keep = [g for g in glyphs if g[0] in used | ASCII]
    missing = sorted(c for c in used if c >= 32 and c not in {g[0] for g in glyphs})

    # Shelves, tallest first: each glyph with its border.
    order = sorted(keep, key=lambda g: (-(g[5] + 2 * BORDER), g[0]))
    places = {}
    x = y = shelf = 0
    for gid, _, gx, gy, w, h in order:
        bw, bh = w + 2 * BORDER, h + 2 * BORDER
        if x + bw > width:
            x, y, shelf = 0, y + shelf, 0
        places[gid] = (x, y)
        x += bw
        shelf = max(shelf, bh)
    new_height = (y + shelf + 3) // 4 * 4

    page = bytearray(width * new_height * 4)
    for gid, rec, gx, gy, w, h in keep:
        px, py = places[gid]
        for row in range(-BORDER, h + BORDER):
            sy = gy + row
            for col in range(-BORDER, w + BORDER):
                sx = gx + col
                if 0 <= sx < width and 0 <= sy < height:
                    src = (sy * width + sx) * 4
                    dst = ((py + BORDER + row) * width + (px + BORDER + col)) * 4
                    page[dst:dst + 4] = pixels[src:src + 4]

    # The kept glyphs' records, moved; the others left out.
    records = bytearray()
    for gid, rec, gx, gy, w, h in keep:
        r = bytearray(fnt[rec:rec + 20])
        px, py = places[gid]
        struct.pack_into('<HH', r, 4, px + BORDER, py + BORDER)
        records += r
    start = chars[0] - 5
    fnt[start:chars[0] + chars[1]] = bytes([4]) + struct.pack('<I', len(records)) + records
    common = next(b for k, b, n in blocks(fnt) if k == 2)
    struct.pack_into('<H', fnt, common + 6, new_height)  # scaleH: the page's height

    header = bytearray(dds[:128])
    struct.pack_into('<I', header, 12, new_height)
    if struct.unpack_from('<I', header, 20)[0]:  # pitch or linear size
        struct.pack_into('<I', header, 20, width * 4)
    TARGET.mkdir(parents=True, exist_ok=True)
    (TARGET / f'{FONT}.fnt').write_bytes(bytes(fnt))
    (TARGET / f'{FONT}_0.dds').write_bytes(bytes(header) + bytes(page))
    print(f'font: {len(keep)} of {len(glyphs)} glyphs, page {width} x {height} -> {width} x {new_height}'
          + (f'; en.txt characters with no glyph: {missing}' if missing else ''))


if __name__ == '__main__':
    main()
