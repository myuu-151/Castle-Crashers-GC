"""Converts the game's sound (assets/audio, xWMA, which the GameCube can't
decode) into what PPGC plays: Microsoft ADPCM WAVs at 32 kHz, 4 bits a
sample, which Source/audio_gc.cpp decodes as it mixes.

    python tools/convert_audio.py [path to the Painters-Playground checkout]

Effects (sounds/) keep their channels, in blocks of 256 bytes; music keeps
its two channels, in blocks of 1024. ffmpeg's trellis search is left off:
on loud effects it wraps around (measured -9 dB against the original, where
the plain encoder gives 34 dB). A file is converted again only when its
source is newer. Uses the ffmpeg in the Octave engine checkout, one file at
a time, at low priority.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
CC_REPO = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / 'PaintersPlayground'
SOURCE = CC_REPO / 'assets' / 'audio'
TARGET = HERE / 'PPGC' / 'Scripts' / 'Data' / 'audio'
FFMPEG = Path(os.environ.get('OCTAVE', HERE.parent / 'octave-libogc')) / 'External' / 'ffmpeg' / 'bin' / 'ffmpeg.exe'
RATE = 32000
BLOCK = {'sounds': 256, 'music': 1024}
BELOW_NORMAL = 0x00004000


def main():
    converted = skipped = 0
    for folder in ('sounds', 'music'):
        (TARGET / folder).mkdir(parents=True, exist_ok=True)
        for src in sorted((SOURCE / folder).glob('*.xma')):
            dst = TARGET / folder / (src.stem + '.wav')
            if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
                skipped += 1
                continue
            subprocess.run([str(FFMPEG), '-v', 'error', '-y', '-i', str(src), '-ar', str(RATE),
                            '-c:a', 'adpcm_ms', '-block_size', str(BLOCK[folder]), str(dst)],
                           check=True, creationflags=BELOW_NORMAL if os.name == 'nt' else 0)
            converted += 1
    size = sum(f.stat().st_size for f in TARGET.rglob('*.wav'))
    print(f'audio: {converted} converted, {skipped} up to date, {size / 1e6:.1f} MB in {TARGET}')


if __name__ == '__main__':
    main()
