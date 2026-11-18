"""Run the translated build and the reference build (original code in Unicorn) with the same
scripted input in test mode and compare their memory, frame by frame; on a difference, narrow
it to the first runtime call after which they differ and list the differing bytes.

    lockstep.py --game DIR [--keys "900:RETURN*10,..."] [--frames N] [--work DIR] [--no-bisect]
"""
import argparse, os, shutil, subprocess, sys, concurrent.futures as cf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(binary, args, work, name, keys, extra):
    user = os.path.join(work, 'user_' + name)
    shutil.rmtree(user, ignore_errors=True)
    os.makedirs(user)
    env = dict(os.environ, TD6_KEYS=keys or '')
    env.pop('TD6_DUMP_FRAMES', None)
    cmd = [os.path.join(ROOT, 'build', binary), '--game', args.game, '--user', user, '--test',
           '--frames', str(args.frames), '--logfile', os.path.join(work, name + '.log')] + extra
    r = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.returncode


def read_hashes(path):
    out = []
    if os.path.exists(path):
        for line in open(path):
            f = line.split()
            out.append((int(f[0]), int(f[1]), f[2]))
    return out


def both(args, work, extra_nat, extra_ref):
    with cf.ThreadPoolExecutor(2) as ex:
        a = ex.submit(run, 'td6', args, work, 'nat', args.keys, extra_nat)
        b = ex.submit(run, 'td6_ref', args, work, 'ref', args.keys, extra_ref)
        return a.result(), b.result()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True)
    ap.add_argument('--keys', default='')
    ap.add_argument('--frames', type=int, default=1200)
    ap.add_argument('--work', default=os.path.join(ROOT, 'work', 'lockstep'))
    ap.add_argument('--no-bisect', action='store_true')
    args = ap.parse_args()
    work = args.work
    os.makedirs(work, exist_ok=True)

    hn, hr = os.path.join(work, 'nat.hash'), os.path.join(work, 'ref.hash')
    rc = both(args, work, ['--hash', hn], ['--hash', hr])
    A, B = read_hashes(hn), read_hashes(hr)
    print('frames: translated %d, reference %d (exit codes %s)' % (len(A), len(B), rc))
    first = None
    for x, y in zip(A, B):
        if x[1] != y[1] or x[2] != y[2]:
            first = (x, y)
            break
    if first is None:
        if len(A) != len(B):
            print('runs stopped at different frames: see %s/nat.log and ref.log' % work)
            return 1
        print('IDENTICAL: memory matches on all %d frames' % len(A))
        return 0
    (fa, ca, ha), (fb, cb, hb) = first
    print('first difference at frame %d (runtime calls so far: %d vs %d)' % (fa, ca, cb))
    if args.no_bisect:
        return 1
    # previous frame's call count: the difference appeared after it
    prev = 0
    for x in A:
        if x[0] == fa - 1:
            prev = x[1]
    lo, hi = prev, max(ca, cb)
    cn, cr = os.path.join(work, 'nat.calls'), os.path.join(work, 'ref.calls')
    both(args, work, ['--hash-calls', str(lo), str(hi), cn], ['--hash-calls', str(lo), str(hi), cr])
    CA = [l.split() for l in open(cn)] if os.path.exists(cn) else []
    CB = [l.split() for l in open(cr)] if os.path.exists(cr) else []
    k = None
    for x, y in zip(CA, CB):
        if x != y:
            k = (x, y)
            break
    if k is None:
        print('no difference found between calls %d and %d' % (lo, hi))
        return 1
    call = int(k[0][0])
    print('first difference after runtime call %d: translated returned to %s, reference to %s'
          % (call, k[0][1], k[1][1]))
    print('previous call returned to %s' % next((x[1] for x in CA if int(x[0]) == call - 1), '?'))
    dn, dr = os.path.join(work, 'nat.mem'), os.path.join(work, 'ref.mem')
    both(args, work, ['--dump-memory-call', str(call), dn], ['--dump-memory-call', str(call), dr])
    subprocess.run([sys.executable, os.path.join(ROOT, 'third_party', 'antistatic', 'translator', 'memdiff.py'), dn, dr])
    return 1


if __name__ == '__main__':
    sys.exit(main())
