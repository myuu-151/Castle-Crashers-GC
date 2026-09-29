"""Converts the game's sound (assets/audio, xWMA, which the GameCube can't
decode) into what PPGC plays (Source/audio_gc.cpp), Microsoft ADPCM:

- music: one WAV a track, 32 kHz stereo, blocks of 1024 bytes, streamed
  from the disc as it plays (audio/music/NAME.wav).
- effects: 24 kHz, blocks of 256 bytes, all in one bank (audio/sounds.bank,
  each effect's data 32-byte aligned) with an index (audio/sounds.idx, a
  line an effect: name offset size rate channels block_align block_frames
  frames). The bank is read into ARAM once, at boot, in one pass: loaded a
  file at a time when attached, as the PC does, the game read hundreds of
  files off the disc at ~100 ms each, and showed a black screen for 40 s.
  At 24 kHz all of them fit in ARAM (10.4 MB, where 32 kHz took 13.9).

    python tools/convert_audio.py [path to the Castle-Crashers-Recomp checkout]

ffmpeg's trellis search is left off: on loud effects it wraps around
(measured -9 dB against the original, where the plain encoder gives 34 dB).
A file is converted again only when its source is newer; converted effects
are kept in build/audio/ (not on the disc). Uses the ffmpeg in the Octave
engine checkout, one file at a time, at low priority.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
CC_REPO = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / 'CastleCrashersRecomp'
SOURCE = CC_REPO / 'assets' / 'audio'
TARGET = HERE / 'CCGC' / 'Scripts' / 'Data' / 'audio'
CACHE = HERE / 'build' / 'audio'
FFMPEG = Path(os.environ.get('OCTAVE', HERE.parent / 'octave-libogc')) / 'External' / 'ffmpeg' / 'bin' / 'ffmpeg.exe'
BELOW_NORMAL = 0x00004000
# The coefficient pairs every Microsoft ADPCM file uses (audio_gc.cpp has them).
STANDARD_COEFS = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]


def convert(src, dst, rate, block):
    if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
        return False
    subprocess.run([str(FFMPEG), '-v', 'error', '-y', '-i', str(src), '-ar', str(rate),
                    '-c:a', 'adpcm_ms', '-block_size', str(block), str(dst)],
                   check=True, creationflags=BELOW_NORMAL if os.name == 'nt' else 0)
    return True


def s16(v):
    return v - 65536 if v >= 32768 else v


def parse(d):
    """The format and data of a Microsoft ADPCM WAV."""
    f = {}
    pos = 12
    while pos + 8 <= len(d):
        tag, n = d[pos:pos + 4], int.from_bytes(d[pos + 4:pos + 8], 'little')
        b = pos + 8
        if tag == b'fmt ':
            assert int.from_bytes(d[b:b + 2], 'little') == 2
            f['channels'] = int.from_bytes(d[b + 2:b + 4], 'little')
            f['rate'] = int.from_bytes(d[b + 4:b + 8], 'little')
            f['align'] = int.from_bytes(d[b + 12:b + 14], 'little')
            f['block_frames'] = int.from_bytes(d[b + 18:b + 20], 'little')
            ncoef = int.from_bytes(d[b + 20:b + 22], 'little')
            coefs = [(s16(int.from_bytes(d[b + 22 + i * 4:b + 24 + i * 4], 'little')),
                      s16(int.from_bytes(d[b + 24 + i * 4:b + 26 + i * 4], 'little'))) for i in range(ncoef)]
            assert coefs == STANDARD_COEFS, coefs
        elif tag == b'fact':
            f['frames'] = int.from_bytes(d[b:b + 4], 'little')
        elif tag == b'data':
            f['data'] = d[b:b + n]
            break
        pos += 8 + n + (n & 1)
    return f


def main():
    converted = 0
    (TARGET / 'music').mkdir(parents=True, exist_ok=True)
    for src in sorted((SOURCE / 'music').glob('*.xma')):
        converted += convert(src, TARGET / 'music' / (src.stem + '.wav'), 32000, 1024)

    CACHE.mkdir(parents=True, exist_ok=True)
    bank = bytearray()
    index = []
    for src in sorted((SOURCE / 'sounds').glob('*.xma')):
        wav = CACHE / (src.stem + '.wav')
        converted += convert(src, wav, 24000, 256)
        f = parse(wav.read_bytes())
        offset = len(bank)
        bank += f['data']
        bank += bytes(-len(bank) % 32)
        index.append(f"{src.stem} {offset} {len(f['data'])} {f['rate']} {f['channels']} {f['align']} "
                     f"{f['block_frames']} {f['frames']}")
    (TARGET / 'sounds.bank').write_bytes(bank)
    (TARGET / 'sounds.idx').write_text('\n'.join(index) + '\n', newline='\n')
    # (the effects' own WAVs, from before the bank)
    old = TARGET / 'sounds'
    if old.exists():
        for f in old.glob('*'):
            f.unlink()
        old.rmdir()
    music = sum(f.stat().st_size for f in (TARGET / 'music').glob('*.wav'))
    print(f'audio: {converted} converted; music {music / 1e6:.1f} MB, effects bank {len(bank) / 1e6:.1f} MB '
          f'({len(index)} effects)')


if __name__ == '__main__':
    main()
