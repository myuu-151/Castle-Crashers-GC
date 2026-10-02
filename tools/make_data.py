"""Makes the game's data from your Steam copy of Castle Crashers, into
CCGC/Scripts/Data (which git ignores), for the disc.

    python tools/make_data.py [--game <the game's folder>]

1. Finds the game: the folder given (--game, or CC_GAME in the environment:
   a depot download, say, which Steam doesn't list), else through Steam (app
   204360); and checks some of its files are the Steam copy's.
2. Extracts from it, into build/game/assets/ (laid out as the
   Castle-Crashers-Recomp checkout's assets/):
   - the SWFs: the .pak archives decrypted, unwrapped from their COK6
     wrappers and their scripts normalized (tools/extract/);
   - the collision (bsp), the fonts, and the sound and music as they are;
   - the text, from the game's castle.exe.
3. Copies that into the project with tools/copy_data.py: the SWFs, fonts,
   text and collision, the sound converted (tools/convert_audio.py, with the
   ffmpeg in Octave-libogc), files.txt, and the disc and memory card art.

Nothing from the game is stored in this repository.
"""
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
TOOLS = HERE / 'tools'
EXTRACT = TOOLS / 'extract'
WORK = HERE / 'build' / 'game'
ASSETS = WORK / 'assets'

STEAM_APP = 204360  # Castle Crashers
# The Steam copy's, checked before anything is made from it.
KNOWN_FILES = {
    'castle.exe': 'f39c3e6ff4600ca0452a908abe552c7d33cd3c9f27305ea766f02373da93490d',
    'data/game/main.pak': 'ac39e11817e35ff7fa7fd76927b2f468f94c13da7e12e16078bc03a28820405d',
    'data/game/player.pak': '522ae30f86ec49e4f9c35174c1b832ccca69fe8b913d6757110470f9ef1e65a7',
    'data/levels/level20.pak': '64f6dda6a977e98a703da4fc7419ad3bbead9389a2c105ea3df3073524ae41ff',
}


def check_game(game):
    """The folder, if it holds the game: its castle.exe and data/, and some
    files the Steam copy's. Exits, saying why, if not."""
    game = Path(game)
    if not (game / 'castle.exe').exists() or not (game / 'data').is_dir():
        sys.exit(f"{game} isn't Castle Crashers' folder (no castle.exe and data/ in it).")
    for name, digest in KNOWN_FILES.items():
        file = game / name
        if not file.exists() or hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            sys.exit(f"{file} is missing or not the Steam copy's: a different build of the game, "
                     "or not all of it (verify the files in Steam, or download the depot again).")
    return game


def find_game(chosen=None):
    """The game's folder: the one chosen (an argument, or CC_GAME), else Steam's."""
    import os
    chosen = chosen or os.environ.get('CC_GAME')
    return check_game(chosen) if chosen else steam_game()


def steam_game():
    """The game's folder, from Steam: Steam's path (the registry), its
    libraries (steamapps/libraryfolders.vdf), the library holding the app's
    manifest (appmanifest_204360.acf) and the folder that names; then its
    files checked. Exits, saying why, if any of it isn't there."""
    steam = None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
            steam = Path(winreg.QueryValueEx(key, 'SteamPath')[0])
    except OSError:
        pass
    if steam is None or not steam.exists():
        steam = Path(r'C:\Program Files (x86)\Steam')
    libraries = [steam]
    vdf = steam / 'steamapps' / 'libraryfolders.vdf'
    if vdf.exists():
        for path in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(encoding='utf-8', errors='replace')):
            libraries.append(Path(path.replace('\\\\', '\\')))  # the file doubles its backslashes
    for library in libraries:
        manifest = library / 'steamapps' / f'appmanifest_{STEAM_APP}.acf'
        if not manifest.exists():
            continue
        found = re.search(r'"installdir"\s+"([^"]+)"', manifest.read_text(encoding='utf-8', errors='replace'))
        game = library / 'steamapps' / 'common' / found.group(1) if found else None
        if game is None or not game.is_dir():
            sys.exit(f'Steam lists Castle Crashers ({manifest}) but its folder is missing: install it in Steam.')
        return check_game(game)
    sys.exit('Castle Crashers (Steam app 204360) is not installed through Steam on this PC: '
             'install it, or choose the folder your copy is in (a depot download, say).')


def step(title, args):
    print(f'-- {title}', flush=True)
    subprocess.run([sys.executable, '-u'] + [str(a) for a in args], check=True)


def copy_tree(src, dst, pattern='*'):
    dst.mkdir(parents=True, exist_ok=True)
    n = 0
    for f in sorted(src.glob(pattern)):
        if f.is_file():
            shutil.copy2(f, dst / f.name)
            n += 1
    return n


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Makes the data from your copy of Castle Crashers.')
    parser.add_argument('--game', help="the game's folder (else CC_GAME, else Steam's)")
    game = find_game(parser.parse_args().game)
    print(f'The game: {game}', flush=True)
    if WORK.exists():
        shutil.rmtree(WORK)
    pak, swf = WORK / 'pak', WORK / 'swf'
    step('decrypting the .pak archives', [EXTRACT / 'decrypt_pak.py', '--game', game, '--out', pak])
    step('unwrapping them', [EXTRACT / 'unwrap_cok6.py', '--pak', pak, '--out', swf])
    step('normalizing the scripts', [EXTRACT / 'normalize_swf.py', '--swf', swf, '--out', ASSETS / 'swf'])
    step('the text', [EXTRACT / 'extract_strings.py', '--exe', game / 'castle.exe', '--out', ASSETS / 'text'])
    print('-- the collision, fonts and sound', flush=True)
    print(f"   bsp {copy_tree(pak / 'bsps', ASSETS / 'bsp', '*.pdag')}, "
          f"fonts {copy_tree(game / 'data' / 'fonts', ASSETS / 'fonts')}, "
          f"music {copy_tree(game / 'data' / 'music', ASSETS / 'audio' / 'music')}, "
          f"sounds {copy_tree(game / 'data' / 'sounds', ASSETS / 'audio' / 'sounds')}", flush=True)
    # Into the project: copy_data (and convert_audio, which it runs) take the
    # folder holding assets/ as their first argument.
    print('-- into the project', flush=True)
    sys.argv[1:] = [str(WORK)]
    sys.path.insert(0, str(TOOLS))
    import copy_data
    copy_data.main()
    print('The data is made.')


if __name__ == '__main__':
    main()
