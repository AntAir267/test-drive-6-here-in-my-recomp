#!/usr/bin/env python3
"""Install the game's files for TD6 native from your own Test Drive 6 CD.

    tools/install.py SOURCE [--to DIR]

SOURCE is any of: the CD (its mount point), a copy of the CD's files, a disc image (.iso), a .zip
holding one, or a folder where the Windows installer put the game. DIR defaults to
~/.local/share/td6-native/game, the folder the Makefile and the game look in.

TD6 reads its data from the archives the installer copies off the CD as they are
(InstData/GameData/Common: LEGALS.ZIP, frontend.zip, LEVELS/level*.zip, cars/*.zip, ...), so this
copies that folder. Nothing from the CD is run. TD6.exe is checked against the builds the
translator knows (builds/*.py, SHA256); an unknown one is copied but named in the report.
Disc images are read here (ISO 9660 with Joliet names; a .zip is unpacked to a temporary file
first), so nothing else needs to be installed."""
import argparse, glob, hashlib, os, re, shutil, struct, sys, tempfile, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_TO = os.path.expanduser('~/.local/share/td6-native/game')
NEEDED = ['TD6.exe', 'frontend.zip', 'static.zip', 'LEVELS', 'cars', 'sound', 'objects', 'param']


def known_builds():
    out = {}
    for p in glob.glob(os.path.join(ROOT, 'builds', '*.py')):
        m = re.search(r"SHA256\s*=\s*'([0-9a-f]{64})'", open(p).read())
        n = re.search(r"NAME\s*=\s*'([^']+)'", open(p).read())
        if m:
            out[m.group(1)] = n.group(1) if n else os.path.basename(p)
    return out


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def find_ci(folder, name):
    """name in folder, ignoring case (CD file systems and copies differ)"""
    try:
        for e in os.listdir(folder):
            if e.lower() == name.lower():
                return os.path.join(folder, e)
    except OSError:
        pass
    return None


def game_folder(top):
    """the folder holding TD6.exe and the archives: Common on a CD, or an installed game"""
    for parts in (['InstData', 'GameData', 'Common'], []):
        p = top
        for part in parts:
            p = find_ci(p, part) if p else None
        if p and find_ci(p, 'TD6.exe') and find_ci(p, 'frontend.zip'):
            return p
    return None


# ------------------------------------------------------------------ ISO 9660 (Joliet names if present)
class Iso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.root = None
        joliet = None
        for sector in range(16, 64):
            d = self.read(sector * 2048, 2048)
            kind = d[0]
            if d[1:6] != b'CD001' or kind == 255:
                break
            rec = self.record(d, 156)
            if kind == 1 and not self.root:
                self.root, self.enc = rec, 'ascii'
            if kind == 2 and d[88:91] in (b'%/@', b'%/C', b'%/E'):
                joliet = rec
        if joliet:
            self.root, self.enc = joliet, 'utf-16-be'
        if not self.root:
            raise SystemExit(f'{path}: not an ISO 9660 disc image')

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def record(self, d, o):
        ln = d[o]
        extent, size = struct.unpack_from('<I', d, o + 2)[0], struct.unpack_from('<I', d, o + 10)[0]
        flags, nl = d[o + 25], d[o + 32]
        return {'extent': extent, 'size': size, 'dir': bool(flags & 2), 'raw': d[o + 33:o + 33 + nl], 'len': ln}

    def listdir(self, rec):
        data, o, out = self.read(rec['extent'] * 2048, rec['size']), 0, []
        while o < len(data):
            if data[o] == 0:
                o = (o // 2048 + 1) * 2048
                continue
            r = self.record(data, o)
            o += r['len']
            if r['raw'] in (b'\0', b'\1'):
                continue
            name = r['raw'].decode(self.enc, 'replace').split(';')[0]
            if not r['dir'] and name.endswith('.'):
                name = name[:-1]
            out.append((name, r))
        return out

    def find(self, parts):
        rec = self.root
        for part in parts:
            rec = next((r for n, r in self.listdir(rec) if n.lower() == part.lower()), None)
            if rec is None:
                return None
        return rec

    def extract(self, rec, to):
        os.makedirs(to, exist_ok=True)
        for name, r in self.listdir(rec):
            dst = os.path.join(to, name)
            if r['dir']:
                self.extract(r, dst)
                continue
            with open(dst, 'wb') as out:
                self.f.seek(r['extent'] * 2048)
                left = r['size']
                while left:
                    b = self.f.read(min(left, 1 << 20))
                    out.write(b)
                    left -= len(b)


def install_from_iso(path, to, shown=None):
    iso = Iso(path)
    for parts in (['InstData', 'GameData', 'Common'], []):
        rec = iso.find(parts)
        if rec and any(n.lower() == 'td6.exe' for n, _ in iso.listdir(rec)):
            print(f'copying the game data from {shown or os.path.basename(path)} ...')
            iso.extract(rec, to)
            return
    raise SystemExit(f'{path}: no InstData/GameData/Common with TD6.exe on this disc (is it Test Drive 6?)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('source')
    ap.add_argument('--to', default=DEFAULT_TO)
    a = ap.parse_args()
    src, to = os.path.abspath(os.path.expanduser(a.source)), os.path.abspath(os.path.expanduser(a.to))
    if os.path.exists(to) and os.listdir(to):
        raise SystemExit(f'{to} is not empty: remove it first (or pass --to)')
    tmp = None
    try:
        if os.path.isdir(src):
            g = game_folder(src)
            if not g:
                raise SystemExit(f'{src}: no TD6.exe with its archives here or in InstData/GameData/Common')
            print(f'copying the game data from {g} ...')
            shutil.copytree(g, to)
        elif zipfile.is_zipfile(src):
            z = zipfile.ZipFile(src)
            isos = [i for i in z.infolist() if i.filename.lower().endswith('.iso')]
            if not isos:
                raise SystemExit(f'{src}: no .iso inside')
            fd, tmp = tempfile.mkstemp(suffix='.iso', dir=os.path.dirname(to) or None)
            print(f'unpacking {isos[0].filename} ...')
            with os.fdopen(fd, 'wb') as out, z.open(isos[0]) as inp:
                shutil.copyfileobj(inp, out, 1 << 20)
            install_from_iso(tmp, to, isos[0].filename)
        else:
            install_from_iso(src, to)
    finally:
        if tmp and os.path.exists(tmp):
            os.remove(tmp)
    missing = [n for n in NEEDED if not find_ci(to, n)]
    if missing:
        print('warning: not found in the copy:', ', '.join(missing))
    exe = find_ci(to, 'TD6.exe')
    h = sha256(exe)
    build = known_builds().get(h)
    if build:
        print(f'TD6.exe is the {build} release: ready (make GAME={to}, or plain make for the default folder)')
    else:
        print(f'TD6.exe has sha256 {h}, a release the translator has no build for (builds/): '
              'the copy is kept, but it will not run until one is added')
        sys.exit(2)


if __name__ == '__main__':
    main()
