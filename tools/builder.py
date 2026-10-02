"""CCGC Builder: the whole build in a window.

    Double-click "Build CCGC.bat" (or: python tools/builder.py)

It checks what the build needs (your copy of Castle Crashers: Steam's, or a
folder you choose, such as a depot download; Pillow,
devkitPro, Octave-libogc, Castle-Crashers-Recomp) and says how to fix what's
missing; then one button makes the data from your copy (tools/make_data.py),
fetches the two small libraries the engine uses if they aren't there, and
packages the disc with Octave, and shows where the ISO is. The folders it
remembers in tools/.builder.json (not committed).

Every step runs in the background, without a console window of its own; the
window shows each step's progress, and of the steps' output only what matters
(errors, and the steps themselves; "Show every line" for the rest). All of it
is in build/builder.log.
"""
import io
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import tkinter as tk
import urllib.request
import zipfile
from pathlib import Path
from tkinter import filedialog, ttk

HERE = Path(__file__).resolve().parents[1]
PROJECT = HERE / 'CCGC'
DATA = PROJECT / 'Scripts' / 'Data'
ISO = PROJECT / 'Packaged' / 'GameCube' / 'CCGC.iso'
SETTINGS = Path(__file__).with_name('.builder.json')
LOW_PRIORITY = 0x4000 | 0x08000000  # below normal, no console window (Windows)
LOG_FILE = HERE / 'build' / 'builder.log'
# "@@ DONE TOTAL": how far a step is (the data tools print it when CC_PROGRESS is set).
PROGRESS = re.compile(r'^@@ (\d+) (\d+)$')
# A source file make is compiling (Makefile_GCN prints each one's name).
SOURCE_LINE = re.compile(r'^[\w.+-]+\.(?:cpp|c)$')
# Octave's packaging chatter: never an error, even where it says so.
NOISE = re.compile(r'^(?:Asset (?:loaded|saved)|Unloading|Loading script|Cannot unload|Auto-parenting|\[Exec\]|'
                   r'Attempting to watch|Headless mode|Running EngineStartup|\(Octave\)|Begin packaging|'
                   r'DevkitPro is installed|Shutdown Complete|Failed to open file|Stream failed|_mkdir error|'
                   r'make: (?:Entering|Leaving)|\s*>>|\s*\d+ [Ff]ile\(s\) copied|The file cannot be copied|'
                   r'The system cannot find the file|[A-Za-z]:[\\/])')
IMPORTANT = re.compile(r'\berror\b|undefined reference|No rule to make|ld returned|\bfailed\b', re.IGNORECASE)
# Makefile_GCN's sources (its SOURCES and EXCLUDE), to count what make has left.
ENGINE_DIRS = ('as', 'audio', 'common', 'input', 'menu', 'player', 'save', 'swf', 'text')
EXCLUDE = {'renderer.cpp', 'gl.cpp', 'offscreen.cpp', 'files.cpp', 'crash.cpp', 'png.cpp', 'dump.cpp'}
# The libraries the engine compiles with (Castle-Crashers-Recomp fetches them
# into its build/_deps/ when it's configured for the PC): fetched here, at the
# commits CCGC is built with, when that isn't there.
DEPS = HERE / 'build' / 'deps'
LIBRARIES = {
    'libtess2-src': 'https://github.com/memononen/libtess2/archive/8dbd6483e920311a58c9af10a10beb278efebc36.zip',
    'stb-src': 'https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.zip',
}
# Of each, only what the build reads (and nothing nested deep enough to pass
# Windows' 260-character path limit): libtess2's sources and headers, stb's
# headers.
KEEP = {'libtess2-src': ('Source/', 'Include/', 'LICENSE'), 'stb-src': ('stb_', 'LICENSE')}


def msys(path):
    """C:\\devkitPro -> /c/devkitPro, as devkitPro's makefiles want it."""
    p = Path(path).as_posix()
    return f'/{p[0].lower()}{p[2:]}' if len(p) > 1 and p[1] == ':' else p


def find_devkitpro():
    for candidate in (os.environ.get('DEVKITPRO_WIN'), os.environ.get('DEVKITPRO'), r'C:\devkitPro'):
        if not candidate:
            continue
        if candidate.startswith('/') and len(candidate) > 2 and candidate[2] == '/':
            candidate = f'{candidate[1].upper()}:{candidate[2:]}'  # /c/devkitPro
        elif candidate.startswith('/opt/devkitpro'):
            candidate = r'C:\devkitPro'
        if (Path(candidate) / 'devkitPPC' / 'bin' / 'powerpc-eabi-gcc.exe').exists():
            return Path(candidate)
    return None


def console_python():
    """Python's console executable (python.exe beside pythonw.exe): the steps
    run under it with a hidden console, which everything they start (ffmpeg)
    shares instead of opening a window each."""
    exe = Path(sys.executable)
    if exe.name.lower() == 'pythonw.exe' and exe.with_name('python.exe').exists():
        return str(exe.with_name('python.exe'))
    return str(exe)


def sources_to_compile(recomp, deps):
    """(the sources make will compile, all of them): a source's object missing
    or older than it. A changed header makes more; the count grows then."""
    dirs = [PROJECT / 'Source', PROJECT / 'Generated', *(Path(recomp) / 'engine' / d for d in ENGINE_DIRS),
            Path(deps) / 'libtess2-src' / 'Source']
    sources = [f for d in dirs for pattern in ('*.cpp', '*.c') for f in d.glob(pattern) if f.name not in EXCLUDE]
    objects = PROJECT / 'Intermediate' / 'GCN'
    stale = [f for f in sources if not (objects / (f.stem + '.o')).exists()
             or (objects / (f.stem + '.o')).stat().st_mtime < f.stat().st_mtime]
    return len(stale), len(sources)


def deps_folder(recomp):
    """The libraries' folder: the recomp's build/_deps if it has them, else ours."""
    theirs = Path(recomp) / 'build' / '_deps'
    if all((theirs / name).is_dir() for name in LIBRARIES):
        return theirs
    return DEPS


class Builder:
    def __init__(self, root):
        self.root = root
        self.lines = queue.Queue()
        self.busy = False
        root.title('CCGC Builder')
        root.minsize(660, 540)
        settings = {}
        try:
            settings = json.loads(SETTINGS.read_text())
        except (OSError, ValueError):
            pass
        self.octave = tk.StringVar(value=settings.get('octave', str(HERE.parent / 'octave-libogc')))
        self.recomp = tk.StringVar(value=settings.get('recomp', str(HERE.parent / 'CastleCrashersRecomp')))
        # the game's folder, if chosen (a depot download, say, which Steam doesn't list); else Steam's
        self.game_folder = tk.StringVar(value=settings.get('game', ''))
        self.diag = tk.BooleanVar(value=False)
        self.remake = tk.BooleanVar(value=False)
        self.verbose = tk.BooleanVar(value=False)
        self.entries = []  # every line of output: (text, shown without "Show every line")
        self.phase = ''
        self.step = ''

        pad = {'padx': 10, 'pady': 4}
        ttk.Label(root, text='Castle Crashers for the GameCube', font=('Segoe UI', 14, 'bold')).pack(anchor='w', **pad)

        checks = ttk.LabelFrame(root, text='What the build needs')
        checks.pack(fill='x', **pad)
        self.rows = {}
        for key, title in (('game', 'Castle Crashers (Steam)'), ('pillow', 'Python: Pillow'),
                           ('devkitpro', 'devkitPro (devkitPPC)'), ('octave', 'Octave-libogc'),
                           ('recomp', 'Castle-Crashers-Recomp')):
            row = ttk.Frame(checks)
            row.pack(fill='x', padx=6, pady=2)
            mark = ttk.Label(row, width=3, font=('Segoe UI', 11, 'bold'))
            mark.pack(side='left')
            ttk.Label(row, text=title, width=26).pack(side='left')
            note = ttk.Label(row, foreground='#555')
            note.pack(side='left', fill='x', expand=True)
            if key in ('game', 'octave', 'recomp'):
                ttk.Button(row, text='Choose...', command=lambda k=key: self.choose(k)).pack(side='right')
            self.rows[key] = (mark, note)

        options = ttk.Frame(root)
        options.pack(fill='x', **pad)
        ttk.Checkbutton(options, text='Make the data again', variable=self.remake).pack(side='left')
        ttk.Checkbutton(options, text='Diagnostic build (memory census, flicker detector; slower)',
                        variable=self.diag).pack(side='left', padx=12)

        buttons = ttk.Frame(root)
        buttons.pack(fill='x', **pad)
        self.build_button = ttk.Button(buttons, text='Build CCGC', command=self.build)
        self.build_button.pack(side='left')
        self.open_button = ttk.Button(buttons, text='Open the ISO folder', command=self.open_folder)
        self.open_button.pack(side='left', padx=8)
        self.progress = ttk.Progressbar(buttons, mode='indeterminate', length=240, maximum=100)  # while building

        self.status = ttk.Label(root, text='')
        self.status.pack(anchor='w', **pad)

        ttk.Checkbutton(root, text='Show every line', variable=self.verbose,
                        command=self.show_log).pack(anchor='w', padx=10)
        frame = ttk.Frame(root)
        frame.pack(fill='both', expand=True, padx=10, pady=(0, 10))
        self.log = tk.Text(frame, height=14, wrap='none', font=('Consolas', 9), state='disabled')
        scroll = ttk.Scrollbar(frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)

        self.check()
        self.update_open()
        root.after(100, self.pump)

    # --- what the build needs -------------------------------------------------

    def set_row(self, key, ok, note):
        mark, label = self.rows[key]
        mark.configure(text='OK' if ok else 'X', foreground='#1a7f37' if ok else '#c62828')
        label.configure(text=note)

    def check(self):
        ok = True
        sys.path.insert(0, str(HERE / 'tools'))
        try:
            import PIL  # noqa: F401
            self.set_row('pillow', True, 'installed')
        except ImportError:
            self.set_row('pillow', False, 'run: py -m pip install pillow')
            ok = False
        try:
            import make_data
            self.game = make_data.find_game(self.game_folder.get() or None)
            self.set_row('game', True, str(self.game) + ('' if self.game_folder.get() else '  (Steam)'))
        except SystemExit as e:
            self.set_row('game', False, str(e))
            ok = False
        self.devkitpro = find_devkitpro()
        if self.devkitpro:
            self.set_row('devkitpro', True, str(self.devkitpro))
        else:
            self.set_row('devkitpro', False, 'install devkitPro with devkitPPC (devkitpro.org)')
            ok = False
        octave = Path(self.octave.get())
        if not (octave / 'Octave.exe').exists():
            self.set_row('octave', False, f'no Octave.exe in {octave}')
            ok = False
        elif not (octave / 'External' / 'ffmpeg' / 'bin' / 'ffmpeg.exe').exists():
            self.set_row('octave', False, 'no External/ffmpeg in it (Octave-libogc comes with one)')
            ok = False
        elif not (octave / 'Engine' / 'Build' / 'GCN' / 'libEngine.a').exists():
            self.set_row('octave', False, 'its GameCube engine library is not built (Engine/Build/GCN/libEngine.a)')
            ok = False
        else:
            self.set_row('octave', True, str(octave))
        recomp = Path(self.recomp.get())
        if not (recomp / 'engine' / 'player' / 'game.cpp').exists():
            self.set_row('recomp', False, f'no Castle-Crashers-Recomp engine/ in {recomp}')
            ok = False
        else:
            deps = deps_folder(recomp)
            self.set_row('recomp', True, str(recomp) + ('' if deps != DEPS or all(
                (DEPS / n).is_dir() for n in LIBRARIES) else '  (libtess2 and stb fetched when building)'))
        self.ready = ok
        self.build_button.configure(state='normal' if ok and not self.busy else 'disabled')
        self.status.configure(text='Ready to build.' if ok else 'Fix the X items above, then build.')
        return ok

    def choose(self, key):
        var, title = {'game': (self.game_folder, "Castle Crashers' folder (the one with castle.exe and data)"),
                      'octave': (self.octave, 'The Octave-libogc folder'),
                      'recomp': (self.recomp, 'The Castle-Crashers-Recomp folder')}[key]
        folder = filedialog.askdirectory(title=title, initialdir=var.get() or str(HERE.parent))
        if folder:
            var.set(folder)
            try:
                SETTINGS.write_text(json.dumps({'game': self.game_folder.get(), 'octave': self.octave.get(),
                                                'recomp': self.recomp.get()}))
            except OSError:
                pass
            self.check()

    # --- building ---------------------------------------------------------

    def write(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        self.log.see('end')
        self.log.configure(state='disabled')

    def show_log(self):
        """The log again, every line or only those that matter."""
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', ''.join(text + '\n' for text, shown in self.entries if shown or self.verbose.get()))
        self.log.see('end')
        self.log.configure(state='disabled')

    def pump(self):
        try:
            while True:
                kind, *rest = self.lines.get_nowait()
                if kind == 'line':
                    text, shown = rest
                    self.entries.append((text, shown))
                    if shown or self.verbose.get():
                        self.write(text + '\n')
                elif kind == 'phase':
                    self.phase = rest[0]
                    self.show_step('starting')
                elif kind == 'step':
                    self.show_step(rest[0])
                elif kind == 'progress':
                    self.show_progress(*rest)
                elif kind == 'done':
                    self.finished(*rest)
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def show_step(self, step):
        """A step without a count yet: the bar moves to show it's working."""
        self.step = step
        self.progress.stop()
        self.progress.configure(mode='indeterminate')
        self.progress.start(12)
        self.status.configure(text=f'{self.phase}: {step}...')

    def show_progress(self, done, total):
        if str(self.progress.cget('mode')) != 'determinate':
            self.progress.stop()
            self.progress.configure(mode='determinate')
        percent = 100 * done // max(total, 1)
        self.progress.configure(value=percent)
        self.status.configure(text=f'{self.phase}: {self.step}, {done} of {total} ({percent}%)')

    def say(self, text):
        """A line of the builder's own, always shown."""
        self.lines.put(('line', text, True))
        with open(LOG_FILE, 'a', encoding='utf-8') as log:
            log.write(text + '\n')

    def build(self):
        if self.busy or not self.check():
            return
        self.busy = True
        self.build_button.configure(state='disabled')
        self.progress.pack(side='right')
        self.status.configure(foreground='')
        self.entries = []
        self.show_log()
        LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
        LOG_FILE.write_text('', encoding='utf-8')
        threading.Thread(target=self.run_build, daemon=True).start()

    def run(self, args, cwd, env, watch):
        """Runs a step, in the background at low priority; each line of its
        output to build/builder.log, and to watch, which says whether the
        window shows it (and may note a step or progress). True if it
        succeeded."""
        proc = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, creationflags=LOW_PRIORITY if os.name == 'nt' else 0)
        with open(LOG_FILE, 'a', encoding='utf-8') as log:
            for raw in proc.stdout:
                # (a Windows program's lines end \r\n; a \r alone rewrites the line)
                text = raw.decode('utf-8', 'replace').rstrip('\r\n').split('\r')[-1].rstrip()
                if not text:
                    continue
                found = PROGRESS.match(text)
                if found:
                    self.lines.put(('progress', int(found[1]), int(found[2])))
                    continue
                log.write(text + '\n')
                self.lines.put(('line', text, watch(text)))
        return proc.wait() == 0

    def watch_data(self, text):
        """make_data's output: its steps ("-- decrypting...") and its counts
        are shown."""
        if text.startswith('-- '):
            self.lines.put(('step', text[3:]))
        return not NOISE.match(text)

    def watch_disc(self, recomp, deps):
        """For Octave's output: the steps of the packaging, the compiling
        counted; shown, only errors."""
        stale, everything = sources_to_compile(recomp, deps)
        count = {'done': 0, 'total': stale or everything}

        # (from make's output, which comes as it happens: Octave's own comes
        # in blocks, its "Compiling game executable" after make is done)
        def watch(text):
            if SOURCE_LINE.match(text):
                if count['done'] == 0:
                    self.lines.put(('step', 'compiling the game'))
                count['done'] += 1
                if count['done'] > count['total']:
                    count['total'] = everything  # a header changed: more than the sources' dates said
                self.lines.put(('progress', count['done'], max(count['total'], count['done'])))
            elif text.startswith('linking'):
                self.lines.put(('step', 'linking the game'))
            elif text.startswith('output ...'):
                self.lines.put(('step', 'writing the disc image'))
            return bool(IMPORTANT.search(text)) and not NOISE.match(text)
        return watch

    def fetch_libraries(self):
        """libtess2 and stb into build/deps/, at the pinned commits."""
        for name, url in LIBRARIES.items():
            target = DEPS / name
            if target.is_dir():
                continue
            self.say(f'fetching {name} ({url})')
            data = urllib.request.urlopen(url, timeout=60).read()
            tmp = DEPS / (name + '.tmp')
            if tmp.exists():
                shutil.rmtree(tmp)
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                for info in archive.infolist():
                    inner = info.filename.split('/', 1)[1] if '/' in info.filename else ''
                    if info.is_dir() or not inner.startswith(KEEP[name]) or '/' in inner and inner.startswith('stb_'):
                        continue
                    out = tmp / inner
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(archive.read(info))
            tmp.rename(target)

    def run_build(self):
        ok = True
        if self.remake.get() or not (DATA / 'files.txt').exists():
            self.say('== Making the data from your copy of the game')
            self.lines.put(('phase', 'Making the data'))
            env = dict(os.environ, OCTAVE=self.octave.get(), CC_PROGRESS='1', PYTHONUNBUFFERED='1')
            ok = self.run([console_python(), '-u', str(HERE / 'tools' / 'make_data.py'), '--game', str(self.game)],
                          HERE, env, self.watch_data)
        recomp = Path(self.recomp.get())
        deps = deps_folder(recomp)
        if ok and deps == DEPS:
            self.lines.put(('phase', 'Fetching libtess2 and stb'))
            try:
                self.fetch_libraries()
            except Exception as e:  # noqa: BLE001 (any failure: say so)
                self.say(f'could not fetch the libraries: {e}')
                ok = False
        if ok:
            self.say('== Building the disc with Octave')
            self.lines.put(('phase', 'Building the disc'))
            self.lines.put(('step', "packaging Octave's assets"))
            octave = Path(self.octave.get())
            dkp = self.devkitpro
            env = dict(os.environ)
            env['PATH'] = os.pathsep.join([str(dkp / 'devkitPPC' / 'bin'), str(dkp / 'tools' / 'bin'),
                                           str(dkp / 'msys2' / 'usr' / 'bin'), env.get('PATH', '')])
            env['DEVKITPRO'] = msys(dkp)
            env['DEVKITPPC'] = msys(dkp / 'devkitPPC')
            env['OCTAVE'] = octave.as_posix()
            env['CC_REPO'] = recomp.as_posix()
            env['DEPS'] = deps.as_posix()
            env['DIAG'] = '1' if self.diag.get() else ''
            env['AUTOPRESS'] = env['CARDTEST'] = env['REPLAY'] = ''
            # DIAG is a compile flag: the files that read it, and the program,
            # made again (make can't tell it changed).
            for stale in (PROJECT / 'Intermediate' / 'GCN' / 'renderer_gx.o',
                          PROJECT / 'Intermediate' / 'GCN' / 'new_gc.o',
                          PROJECT / 'Intermediate' / 'GCN' / 'CastleGame.o',
                          PROJECT / 'Build' / 'GCN' / 'CCGC.dol', ISO):
                try:
                    stale.unlink()
                except OSError:
                    pass
            watch = self.watch_disc(recomp, deps)
            self.run([str(octave / 'Octave.exe'), '-headless', '-project', (PROJECT / 'CCGC.octp').as_posix(),
                      '-build', 'GameCube'], octave, env, watch)
            ok = ISO.exists()
        self.lines.put(('done', ok))

    def finished(self, ok):
        self.busy = False
        self.progress.stop()
        self.progress.configure(mode='determinate', value=0)
        self.progress.pack_forget()
        self.build_button.configure(state='normal' if self.ready else 'disabled')
        if ok:
            size = ISO.stat().st_size / (1024 * 1024)
            self.status.configure(text=f'Done: {ISO} ({size:.0f} MB)', foreground='#1a7f37')
            self.entries.append((f'== Done: {ISO}', True))
            self.write(f'== Done: {ISO}\n')
        else:
            self.status.configure(text=f'The build failed: the log says why (all of it: {LOG_FILE}).',
                                  foreground='#c62828')
            if not self.verbose.get():
                # what led up to it, which the short log may not have shown
                hidden = [text for text, shown in self.entries if not shown][-25:]
                if hidden:
                    self.write('\n-- the last lines of output:\n' + ''.join(text + '\n' for text in hidden))
        self.update_open()

    def update_open(self):
        self.open_button.configure(state='normal' if ISO.exists() else 'disabled')

    def open_folder(self):
        if ISO.exists():
            subprocess.Popen(['explorer', '/select,', str(ISO)])


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use('vista')
    except tk.TclError:
        pass
    Builder(root)
    root.mainloop()


if __name__ == '__main__':
    main()
