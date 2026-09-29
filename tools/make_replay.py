"""A recorded session, made into the GameCube test build's replay.

    python tools/make_replay.py SESSION [--fast-forward UPDATE]

SESSION is a capture or a session the PC recomp recorded
(build/Release/sessions/session-*.txt): the save it began from, every update,
and each change of input after the update it follows. The GameCube build
with REPLAY=1 (Makefile_GCN) plays it again, as `castle.exe --replay SESSION`
does: CCGC/Scripts/Data/replay.bin, read a little at a time, so it costs the
game next to no memory.

Each input change is tied, as engine/main.cpp's load_replay ties it, to the
last update before it of a movie other than the loading screen and pause
movies: that movie's frame count, frame, and how many times that (count,
frame) had been seen. The game applies it when its own run makes the same
update. Until update UPDATE (--fast-forward) the game runs several ticks a
frame, then in real time (so a level is reached quickly, then timed as
played).

replay.bin, big-endian:
    "CCRP", u32 version (1), u32 records, u32 fast-forward update,
    u32 storage bytes, the bytes, u32 gamer tag bytes, the tag (UTF-8),
    then the records, 68 bytes each:
    s32 total, s32 frame, s32 occurrence, u8 kind, u8 pad, u16 buttons,
    u8 left trigger, u8 right trigger, s16 thumb x, s16 thumb y, u8 connected,
    s8 value, u32 mouse message, f32 mouse x, f32 mouse y, 32 bytes of keys
    (a bit a virtual key).
    Kinds: 1 pad, 2 keys, 3 focus, 4 suspend, 5 suspend for lost focus (old
    captures), 6 mouse.
"""

import argparse
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
OUT = HERE / 'CCGC' / 'Scripts' / 'Data' / 'replay.bin'

PAD, KEYS, FOCUS, SUSPEND, FOCUS_SUSPEND, MOUSE = 1, 2, 3, 4, 5, 6


def record(at, kind, pad=0, buttons=0, lt=0, rt=0, lx=0, ly=0, connected=0, value=0, mouse=(0, 0.0, 0.0), keys=b''):
    total, frame, occurrence = at
    return struct.pack('>iiiBBHBBhhBbIff32s', total, frame, occurrence, kind, pad, buttons, lt, rt, lx, ly,
                       connected, value, mouse[0], mouse[1], mouse[2], keys.ljust(32, b'\0'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('session')
    ap.add_argument('--fast-forward', type=int, default=0, help='the update to run fast until')
    args = ap.parse_args()

    records = []
    seen = {}
    last = (0, 0, 0)          # (total, frame, occurrence) of the last update that counts
    before_last = (0, 0, 0)
    fmt = 1
    has_suspend = False
    storage = b''
    tag = b'Player'
    updates = 0
    with open(args.session, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('== '):
                parts = line.split()
                updates += 1
                if len(parts) >= 6 and parts[2] == 'update' and parts[4] == 'frame':
                    if parts[1] in ('loading', 'pause'):
                        continue
                    frame, total = (int(x) for x in parts[5].split('/'))
                    before_last = last
                    seen[(total, frame)] = seen.get((total, frame), 0) + 1
                    last = (total, frame, seen[(total, frame)])
            elif line.startswith('!! pad '):
                v = line.split()[2:]
                if len(v) == 7 and 0 <= int(v[0]) < 4:
                    records.append(record(last, PAD, pad=int(v[0]), connected=int(v[1]) != 0, buttons=int(v[2]),
                                          lt=int(v[3]), rt=int(v[4]), lx=int(v[5]), ly=int(v[6])))
            elif line.startswith('!! keys'):
                bits = bytearray(32)
                for vk in line[7:].split():
                    if vk.isdigit() and int(vk) < 256:
                        bits[int(vk) // 8] |= 1 << (int(vk) % 8)
                records.append(record(last, KEYS, keys=bytes(bits)))
            elif line.startswith('!! mouse '):
                msg, xb, yb = (int(x, 16) for x in line.split()[2:5])
                x, y = struct.unpack('<ff', struct.pack('<II', xb, yb))
                records.append(record(last, MOUSE, mouse=(msg, x, y)))
            elif line.startswith('!! format '):
                fmt = int(line.split()[2])
            elif line.startswith('!! suspend '):
                records.append(record(last if fmt >= 2 else before_last, SUSPEND, value=int(line.split()[2]) != 0))
                has_suspend = True
            elif line.startswith('!! focus '):
                focus = int(line.split()[2]) != 0
                records.append(record(last if fmt >= 2 else before_last, FOCUS, value=focus))
                records.append(record(last, FOCUS_SUSPEND, value=0 if focus else 1))
            elif line.startswith('!! storage bytes') and not storage:
                storage = bytes(int(b, 16) for b in line[16:].split())
            elif line.startswith('!! gamertag '):
                tag = line[12:].rstrip('\r\n').encode('utf-8')
            elif line.startswith('!! dt '):
                print('note: "!! dt" lines (tick lengths) are not replayed on the GameCube')

    if has_suspend:  # a capture that records its suspension needs no stand-in for it
        records = [r for r in records if r[12] != FOCUS_SUSPEND]

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, 'wb') as out:
        out.write(b'CCRP' + struct.pack('>III', 1, len(records), args.fast_forward))
        out.write(struct.pack('>I', len(storage)) + storage)
        out.write(struct.pack('>I', len(tag)) + tag)
        out.write(b''.join(records))
    print(f'{OUT}: {len(records)} changes over {updates} updates, fast forward to update {args.fast_forward}')


if __name__ == '__main__':
    main()
