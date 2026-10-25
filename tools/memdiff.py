"""Compare two memory dumps written by --dump-memory and list the differing byte ranges.

    memdiff.py A.mem B.mem [--context N]
"""
import sys, struct


def load(path):
    regions = {}
    with open(path, 'rb') as f:
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            base, size = struct.unpack('<II', hdr)
            regions[base] = f.read(size)
    return regions


def ranges(a, b, base):
    """Differing ranges between equal-length byte strings (merged when < 16 bytes apart)."""
    out = []
    n = len(a)
    step = 4096
    for off in range(0, n, step):
        if a[off:off + step] == b[off:off + step]:
            continue
        for i in range(off, min(off + step, n)):
            if a[i] != b[i]:
                if out and base + i - out[-1][1] < 16:
                    out[-1][1] = base + i + 1
                else:
                    out.append([base + i, base + i + 1])
    return out


def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    total = 0
    for base in sorted(set(A) | set(B)):
        if base not in A or base not in B:
            print('region %08x only in %s' % (base, 'A' if base in A else 'B'))
            continue
        a, b = A[base], B[base]
        if len(a) != len(b):
            print('region %08x size differs %x vs %x' % (base, len(a), len(b)))
            continue
        for lo, hi in ranges(a, b, base):
            total += 1
            if total <= 60:
                sa = a[lo - base:min(hi, lo + 32) - base].hex()
                sb = b[lo - base:min(hi, lo + 32) - base].hex()
                print('%08x-%08x (%d bytes) in region %08x\n    A %s\n    B %s' % (lo, hi, hi - lo, base, sa, sb))
    print('%d differing ranges' % total)


if __name__ == '__main__':
    main()
