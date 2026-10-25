"""x87 stack in C locals.

The generated C keeps the program's x87 stack in the CPU record (c->st[8], c->ftop) and every
ST(i) is a load or store of an 80-bit value through a computed index. Compiled code uses the
x87 stack in a disciplined way: within a function its depth at each instruction is fixed
whatever path reaches it. Where that holds, the stack positions a function pushes become C
locals s1..s8 (position p = depth after its push, counted from the function's entry), which
the compiler keeps in registers. The record is brought up to date (spilled) before every call,
return and helper that works on the record, and the positions are read back after calls.

The analysis reads each instruction's generated text (the same text that is rewritten), so the
two cannot disagree:
- ST(i) reads position depth - i; FPUSH(x) evaluates x and then pushes; FPOP() pops;
- fpu_<helper>(c, ...) works on the record (spill before, reload after, known stack effect);
- fpu_sw(c) reads c->ftop (brought up to date first);
- c->ftop, fpu_ldenv/stenv/save/rstor/init: the function keeps the record-based stack.

A function's stack effect (depth at its exits) is found for the whole program by a fixpoint;
a call takes its callee's effect. Calls through pointers are assumed to leave the stack as it
was, which the generated code checks (fpu_depth_fail stops with the address).
"""
import re

TOKEN = re.compile(r'ST\((\d+)\)|FPUSH\(|FPOP\(\)|\b(fpu_[a-z0-9_]+)\(c\b|c->ftop')
HELPERS = {                                 # helper -> stack effect
    'fpu_xam': 0, 'fpu_ldconst': 1, 'fpu_f2xm1': 0, 'fpu_fyl2x': -1, 'fpu_fptan': 1,
    'fpu_fpatan': -1, 'fpu_fxtract': 1, 'fpu_fprem1': 0, 'fpu_fprem': 0, 'fpu_fyl2xp1': -1,
    'fpu_fsincos': 1, 'fpu_fscale': 0, 'fpu_fsin': 0, 'fpu_fcos': 0}
UNKNOWN = ('fpu_ldenv', 'fpu_stenv', 'fpu_save', 'fpu_rstor', 'fpu_init')
VALUE_ONLY = ('fpu_com', 'fpu_setcw', 'fpu_clex')  # take c but leave the stack alone


def close_paren(text, k):
    """index of the ')' closing the '(' just before k"""
    depth = 1
    while k < len(text):
        ch = text[k]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                return k
        k += 1
    raise ValueError('unbalanced: ' + text)


def events(text):
    """x87 stack events of one instruction's text in evaluation order:
    (kind, arg, rel, span) with rel the depth change before it; kind is 'st' (arg = i), 'push',
    'pop', 'helper' (arg = name, span = the whole call), 'sw' (fpu_sw call span) or 'bad'."""
    out = []
    pending = []                            # close-paren index of FPUSHes whose value is pending
    rel = 0
    skip_to = -1
    for m in TOKEN.finditer(text):
        k = m.start()
        while pending and pending[0][0] < k:
            end, start = pending.pop(0)
            out.append(('push', None, rel, (start, end)))
            rel += 1
        if k < skip_to:
            continue
        tok = m.group(0)
        if m.group(1) is not None:
            out.append(('st', int(m.group(1)), rel, m.span()))
        elif tok == 'FPUSH(':
            end = close_paren(text, m.end())
            pending.append((end, k))
            pending.sort()
        elif tok == 'FPOP()':
            out.append(('pop', None, rel, m.span()))
            rel -= 1
        elif tok == 'c->ftop':
            out.append(('bad', tok, rel, m.span()))
        else:
            name = m.group(2)
            if name in HELPERS:
                end = close_paren(text, m.end(2) + 1)
                out.append(('helper', name, rel, (k, end + 1)))
                rel += HELPERS[name]
                skip_to = end + 1
            elif name == 'fpu_sw':
                end = close_paren(text, m.end(2) + 1)
                out.append(('sw', name, rel, (k, end + 1)))
            elif name in UNKNOWN:
                out.append(('bad', name, rel, m.span()))
    while pending:
        end, start = pending.pop(0)
        out.append(('push', None, rel, (start, end)))
        rel += 1
    return out


def net(evs):
    """stack effect of one instruction's text, None if unknown"""
    n = 0
    for kind, arg, rel, span in evs:
        if kind == 'bad':
            return None
        if kind == 'push':
            n += 1
        elif kind == 'pop':
            n -= 1
        elif kind == 'helper':
            n += HELPERS[arg]
    return n


def spill(d, top='ft0'):
    """bring the record up to date at depth d"""
    parts = ['c->st[(%s - %d) & 7] = s%d' % (top, p, p) for p in range(1, d + 1)]
    parts.append('c->ftop = (%s - %d) & 7' % (top, d))
    return parts


def reload(d, top='ft0'):
    return ['s%d = c->st[(%s - %d) & 7]' % (p, top, p) for p in range(1, d + 1)]


def rewrite(text, d):
    """one instruction's text at depth d with the stack in locals"""
    evs = events(text)
    repl = []                               # (start, end, new text)
    for kind, arg, rel, span in evs:
        r = d + rel
        if kind == 'st':
            repl.append((span[0], span[1], 's%d' % (r - arg)))
        elif kind == 'push':
            start, end = span
            repl.append((start, start + len('FPUSH('), 's%d = (' % (r + 1)))
        elif kind == 'pop':
            repl.append((span[0], span[1], '(void)0'))
        elif kind == 'helper':
            call = text[span[0]:span[1]]
            after = r + HELPERS[arg]
            repl.append((span[0], span[1], '(' + ', '.join(spill(r) + [call] + reload(after)) + ')'))
        elif kind == 'sw':
            call = text[span[0]:span[1]]
            repl.append((span[0], span[1], '(c->ftop = (ft0 - %d) & 7, %s)' % (r, call)))
    repl.sort()
    out, k = [], 0
    for start, end, new in repl:
        if start < k:
            continue                        # inside a helper call already copied whole
        out.append(text[k:start])
        out.append(new)
        k = end
    out.append(text[k:])
    return ''.join(out)
