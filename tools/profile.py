"""Turn an INSANE_PROFILE sample file into a per-function report.

    profile.py SAMPLES [--top N] [--lines]

--lines: also list the hottest lines of the generated C (the line of the translated function
each sample falls in, through inlined helpers), with the instruction it comes from.
"""
import collections, subprocess, sys


def image_base(mod):
    """where the program's addresses start: 0 for Linux's position-independent program, the PE header's
    ImageBase for the Windows build (its samples are offsets from where Windows loaded it)"""
    with open(mod, 'rb') as f:
        d = f.read(4096)
    if d[:2] != b'MZ':
        return 0
    pe = int.from_bytes(d[0x3c:0x40], 'little')
    return int.from_bytes(d[pe + 24 + 24:pe + 24 + 32], 'little')     # PE32+ optional header: ImageBase


def symbolize(mod, offsets):
    """offset -> list of (function, file:line), innermost inline level first"""
    base = image_base(mod)
    out = []
    for k in range(0, len(offsets), 1500):          # (in batches: Windows limits a command line to 32K)
        out += subprocess.run(['addr2line', '-a', '-f', '-i', '-e', mod] +
                              ['%x' % (base + o) for o in offsets[k:k + 1500]],
                              capture_output=True, text=True).stdout.split('\n')
    res, cur, k = {}, None, 0
    while k < len(out):
        l = out[k]
        if l.startswith('0x'):
            cur = int(l, 16) - base
            res[cur] = []
            k += 1
            continue
        if cur is not None and k + 1 < len(out):
            res[cur].append((l, out[k + 1]))
            k += 2
        else:
            k += 1
    return res


def source_line(loc):
    path, ln = loc.rsplit(':', 1)
    try:
        src = open(path).read().split('\n')
        i = int(ln.split()[0]) - 1
        j = i
        while j > 0 and '/* ' not in src[j]:
            j -= 1
        return src[j].strip()[:70] + '  |  ' + src[i].strip()[:80]
    except Exception:
        return ''


def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 60
    by_mod = collections.defaultdict(list)
    total = 0
    for line in open(path):
        n, rest = line.split(None, 1)
        mod, off = rest.rsplit(None, 1)            # (Windows paths may have spaces)
        by_mod[mod].append((int(n), int(off, 16)))
        total += int(n)
    funcs = collections.Counter()
    groups = collections.Counter()
    lines = collections.Counter()
    outers = collections.Counter()
    for mod, rows in by_mod.items():
        if mod.replace('\\', '/').split('/')[-1] in ('1nsane', '1nsane_ref', '1nsane.exe', '1nsane_ref.exe'):
            sym = symbolize(mod, [o for _, o in rows])
            for n, off in rows:
                chain = sym.get(off) or [('??', '??:0')]
                fn, loc = chain[0]
                src = loc.rsplit(':', 1)[0].replace('\\', '/').split('/')[-1]
                funcs['%s (%s)' % (fn, src)] += n
                outer = chain[-1]
                osrc = outer[1].rsplit(':', 1)[0].replace('\\', '/').split('/')[-1]
                groups['game code (translated)' if osrc.startswith('code_') else 'runtime ' + src] += n
                if osrc.startswith('code_'):
                    lines[outer[1].split(' ')[0]] += n
                    outers[outer[0]] += n
        else:
            name = mod.replace('\\', '/').split('/')[-1]
            n = sum(k for k, _ in rows)
            funcs[name] += n
            groups['library ' + name] += n
    print('%d samples (1 per ms of CPU time)\n' % total)
    print('By area:')
    for g, n in groups.most_common(30):
        print('  %5.1f%%  %s' % (100.0 * n / total, g))
    print('\nTop functions (innermost, inlined helpers on their own):')
    for f, n in funcs.most_common(top):
        print('  %5.1f%%  %s' % (100.0 * n / total, f))
    print('\nTop translated functions (with their inlined helpers):')
    for f, n in outers.most_common(top // 2):
        print('  %5.1f%%  %s' % (100.0 * n / total, f))
    if '--lines' in sys.argv:
        print('\nHottest generated lines:')
        for loc, n in lines.most_common(top):
            print('  %5.2f%%  %-18s %s' % (100.0 * n / total, loc.split('/')[-1], source_line(loc)))


if __name__ == '__main__':
    main()
