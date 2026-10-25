"""C code generation: one C function per discovered function.

Registers live in C locals inside each function and are written back to the CPU record
(runtime/cpu.h) around every call and return. Flags are computed only where a later
instruction reads them: either fused into the condition of the next jump/set when the
flag-setting instruction sits just before it in the same block, or kept in per-flag locals.
The x87 stack lives in the CPU record and is computed in the host's 80-bit long double with
the program's own control word.
"""
import re
import fpustack
from collections import defaultdict

CF, PF, AF, ZF, SF, OF = 1, 2, 4, 8, 16, 32
ALL = 63
FNAME = {CF: 'cf', PF: 'pf', AF: 'af', ZF: 'zf', SF: 'sf', OF: 'of'}

REG32 = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp',
         'si': 'esi', 'di': 'edi'}
REG8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
REG8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}

CC_USES = {'o': OF, 'no': OF, 'b': CF, 'ae': CF, 'e': ZF, 'ne': ZF, 'be': CF | ZF, 'a': CF | ZF,
           's': SF, 'ns': SF, 'p': PF, 'np': PF, 'l': SF | OF, 'ge': SF | OF, 'le': ZF | SF | OF,
           'g': ZF | SF | OF}
JCC = {'j' + k: k for k in CC_USES}
SETCC = {'set' + k: k for k in CC_USES}

ARITH = {'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'cmp', 'test'}
SHIFTS = {'shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'}
STRING = {'movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
          'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'}
FUSABLE = {'add', 'sub', 'adc', 'sbb', 'logic', 'inc', 'dec', 'neg', 'sahf', 'shl', 'shr', 'sar'}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SIGN = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}
STYPE = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}
BITS = {1: 8, 2: 16, 4: 32}


def split_prefix(mn):
    for p in ('rep ', 'repe ', 'repne ', 'repz ', 'repnz ', 'lock '):
        if mn.startswith(p):
            return p.strip(), mn[len(p):]
    return None, mn


def hx(v):
    return '0x%xu' % (v & 0xffffffff)


class Unsupported(Exception):
    pass


# ---------------------------------------------------------------------- flag effects
def shift_count(i):
    if len(i.ops) < 2:
        return 1
    op = i.ops[-1]
    if op[0] == 'i':
        return op[1] & 31
    return None


def flag_info(i):
    """(uses, defs, mdefs, kind): defs are flags the instruction always writes, mdefs flags it
    writes only sometimes (shift by cl = 0, rep with ecx = 0 leave them alone). kind names
    the producer for fused conditions (None = cannot be fused)."""
    pre, m = split_prefix(i.mn)
    if m in ('add',):
        return 0, ALL, 0, 'add'
    if m in ('sub', 'cmp'):
        return 0, ALL, 0, 'sub'
    if m == 'adc':
        return CF, ALL, 0, 'adc'
    if m == 'sbb':
        return CF, ALL, 0, 'sbb'
    if m in ('and', 'or', 'xor', 'test'):
        return 0, ALL, 0, 'logic'
    if m == 'inc':
        return 0, ALL & ~CF, 0, 'inc'
    if m == 'dec':
        return 0, ALL & ~CF, 0, 'dec'
    if m == 'neg':
        return 0, ALL, 0, 'neg'
    if m in ('shl', 'sal', 'shr', 'sar'):
        n = shift_count(i)
        if n is None:
            return 0, 0, ALL, None
        if n == 0:
            return 0, 0, 0, None
        return 0, ALL, 0, {'sal': 'shl'}.get(m, m)
    if m in ('rol', 'ror'):
        n = shift_count(i)
        if n is None:
            return 0, 0, CF | OF, None
        return (0, CF | OF, 0, None) if n else (0, 0, 0, None)
    if m in ('rcl', 'rcr'):
        n = shift_count(i)
        if n is None:
            return CF, 0, CF | OF, None
        return (CF, CF | OF, 0, None) if n else (0, 0, 0, None)
    if m in ('shld', 'shrd'):
        n = shift_count(i)
        if n is None:
            return 0, 0, ALL, None
        return (0, ALL, 0, None) if n else (0, 0, 0, None)
    if m in ('mul', 'imul'):
        return 0, ALL, 0, None
    if m in JCC:
        return CC_USES[JCC[m]], 0, 0, None
    if m in SETCC:
        return CC_USES[SETCC[m]], 0, 0, None
    if m in ('loope', 'loopne'):
        return ZF, 0, 0, None
    if m in ('clc', 'stc'):
        return 0, CF, 0, None
    if m == 'cmc':
        return CF, CF, 0, None
    if m == 'sahf':
        return 0, CF | PF | AF | ZF | SF, 0, 'sahf'
    if m == 'lahf':
        return CF | PF | AF | ZF | SF, 0, 0, None
    if m in ('pushfd', 'pushf'):
        return ALL, 0, 0, None
    if m in ('popfd', 'popf'):
        return 0, ALL, 0, None
    if m in ('scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
        if pre in ('rep', 'repe', 'repne', 'repz', 'repnz'):
            return 0, 0, ALL, None
        return 0, ALL, 0, None
    if m in ('call',):
        return 0, ALL, 0, None             # set by the callee (see Program.ret_flags)
    return 0, 0, 0, None


def flag_expr(kind, f, n, d, s, r, ci=None):
    """C expression for flag f after producer `kind` of size n with inputs d, s, result r."""
    M, S, T = MASK[n], SIGN[n], STYPE[n]
    if f == ZF:
        if kind == 'sahf':
            return '((%s >> 6) & 1)' % r
        return '(%s == 0)' % r
    if f == SF:
        if kind == 'sahf':
            return '((%s >> 7) & 1)' % r
        return '((%s & %s) != 0)' % (r, S)
    if f == PF:
        if kind == 'sahf':
            return '((%s >> 2) & 1)' % r
        return 'PARITY(%s)' % r
    if f == AF:
        if kind == 'sahf':
            return '((%s >> 4) & 1)' % r
        if kind in ('add', 'sub', 'adc', 'sbb', 'inc', 'dec', 'neg'):
            return '(((%s ^ %s ^ %s) >> 4) & 1)' % (d, s, r)
        return '0'
    if f == CF:
        if kind == 'add':
            return '(%s < %s)' % (r, d)
        if kind == 'adc':
            return '(%s ? %s <= %s : %s < %s)' % (ci, r, d, r, d)
        if kind == 'sub':
            return '(%s < %s)' % (d, s)
        if kind == 'sbb':
            return '(%s ? %s <= %s : %s < %s)' % (ci, d, s, d, s)
        if kind == 'neg':
            return '(%s != 0)' % s
        if kind == 'logic':
            return '0'
        if kind == 'sahf':
            return '(%s & 1)' % r
        if kind == 'shl':
            if n == 4:
                return '((%s >> (32 - %s)) & 1)' % (d, s)
            return '((%s <= %d) ? ((%s >> (%d - %s)) & 1) : 0)' % (s, BITS[n], d, BITS[n], s)
        if kind in ('shr',):
            return '((%s >> (%s - 1)) & 1)' % (d, s)
        if kind == 'sar':
            return '((uint32_t)((int32_t)(%s)%s >> (%s - 1)) & 1)' % (T, d, s)
    if f == OF:
        if kind in ('add', 'adc'):
            return '((~(%s ^ %s) & (%s ^ %s) & %s) != 0)' % (d, s, d, r, S)
        if kind in ('sub', 'sbb'):
            return '(((%s ^ %s) & (%s ^ %s) & %s) != 0)' % (d, s, d, r, S)
        if kind == 'neg':
            return '(%s == %s)' % (r, S)
        if kind == 'inc':
            return '(%s == %s)' % (r, S)
        if kind == 'dec':
            return '(%s == %s - 1)' % (r, S)
        if kind in ('logic', 'sar'):
            return '0'
        if kind == 'shl':
            return '(((%s & %s) != 0) ^ %s)' % (r, S, flag_expr('shl', CF, n, d, s, r))
        if kind == 'shr':
            return '((%s & %s) != 0)' % (d, S)
    raise Unsupported('flag %s of %s' % (FNAME[f], kind))


def cond_expr(cc, flag):
    """Condition from a flag accessor flag(F) -> C expr."""
    if cc == 'o': return flag(OF)
    if cc == 'no': return '!' + flag(OF)
    if cc == 'b': return flag(CF)
    if cc == 'ae': return '!' + flag(CF)
    if cc == 'e': return flag(ZF)
    if cc == 'ne': return '!' + flag(ZF)
    if cc == 'be': return '(%s | %s)' % (flag(CF), flag(ZF))
    if cc == 'a': return '!(%s | %s)' % (flag(CF), flag(ZF))
    if cc == 's': return flag(SF)
    if cc == 'ns': return '!' + flag(SF)
    if cc == 'p': return flag(PF)
    if cc == 'np': return '!' + flag(PF)
    if cc == 'l': return '(%s != %s)' % (flag(SF), flag(OF))
    if cc == 'ge': return '(%s == %s)' % (flag(SF), flag(OF))
    if cc == 'le': return '(%s | (%s != %s))' % (flag(ZF), flag(SF), flag(OF))
    if cc == 'g': return '(!%s & (%s == %s))' % (flag(ZF), flag(SF), flag(OF))
    raise Unsupported(cc)


def fused_cond(kind, n, cc):
    """Condition straight from the saved inputs fa/fb/fr of producer `kind`."""
    T = STYPE[n]
    if kind == 'sub':
        rel = {'b': 'fa < fb', 'ae': 'fa >= fb', 'e': 'fa == fb', 'ne': 'fa != fb',
               'be': 'fa <= fb', 'a': 'fa > fb',
               'l': '(%s)fa < (%s)fb' % (T, T), 'ge': '(%s)fa >= (%s)fb' % (T, T),
               'le': '(%s)fa <= (%s)fb' % (T, T), 'g': '(%s)fa > (%s)fb' % (T, T)}
        if cc in rel:
            return '(%s)' % rel[cc]
    if kind == 'logic':
        rel = {'e': 'fr == 0', 'ne': 'fr != 0', 's': '(%s)fr < 0' % T, 'ns': '(%s)fr >= 0' % T,
               'l': '(%s)fr < 0' % T, 'ge': '(%s)fr >= 0' % T,
               'le': '(%s)fr <= 0' % T, 'g': '(%s)fr > 0' % T,
               'b': '0', 'ae': '1', 'be': 'fr == 0', 'a': 'fr != 0', 'o': '0', 'no': '1'}
        if cc in rel:
            return '(%s)' % rel[cc]
    return cond_expr(cc, lambda f: flag_expr(kind, f, n, 'fa', 'fb', 'fr', 'fc'))


# ---------------------------------------------------------------------- function generator
class FuncGen:
    def __init__(self, prog, f):
        self.prog = prog
        self.f = f
        self.cur = None
        self.out = []
        self.labels = set()
        self.warn = []

    # -------------------------------------------------- operand helpers
    def const(self, v):
        """A 32-bit constant: an address inside a movable table (in the functions allowed to
        move it) becomes the table's run-time base plus the offset."""
        t = self.prog.table_for(self.f.entry, v)
        if t:
            off = v - t['base']
            return '(reloc_%s %s 0x%xu)' % (t['name'], '-' if off < 0 else '+', abs(off))
        return None

    def ea(self, op):
        _, size, seg, base, index, scale, disp = op
        parts = []
        rc = self.const(disp) if disp else None
        if base:
            parts.append(self.r32name(base))
        if index:
            sc = self.prog.const_at(self.cur.addr, scale) if scale != 1 and self.cur else None
            parts.append(self.r32name(index) + (' * rc_%s' % sc if sc else '' if scale == 1 else ' * %d' % scale))
        if rc:
            parts.append(rc)
        elif disp or not parts:
            if parts and disp >= 0x80000000:
                parts.append('- 0x%xu' % (0x100000000 - disp))
            else:
                parts.append(hx(disp))
        e = ' + '.join(parts).replace('+ -', '-')
        if seg == 'fs':
            e = 'c->fs_base + ' + e
        elif seg not in (None, 'ds', 'ss', 'es'):
            raise Unsupported('segment ' + seg)
        return '(uint32_t)(%s)' % e if len(parts) > 1 or seg == 'fs' else e

    def imm(self, v):
        lim = self.prog.limit_at(self.cur.addr, v)
        if lim:
            return 'limit_%s' % lim
        rc = self.prog.const_at(self.cur.addr, v)
        if rc:
            return 'rc_%s' % rc
        return self.const(v) or hx(v)

    def r32name(self, r):
        if r in REG32:
            return r
        raise Unsupported('address register ' + r)

    def rd(self, op, ea=None):
        t = op[0]
        if t == 'r':
            r = op[1]
            if r in REG32: return r
            if r in REG16: return '(%s & 0xffffu)' % REG16[r]
            if r in REG8L: return '(%s & 0xffu)' % REG8L[r]
            if r in REG8H: return '((%s >> 8) & 0xffu)' % REG8H[r]
            raise Unsupported('register ' + r)
        if t == 'i':
            return self.imm(op[1])
        n = op[1]
        a = ea or self.ea(op)
        if n == 1: return 'R8(%s)' % a
        if n == 2: return 'R16(%s)' % a
        if n == 4: return 'R32(%s)' % a
        raise Unsupported('memory read size %d' % n)

    def wr(self, op, v, ea=None):
        t = op[0]
        if t == 'r':
            r = op[1]
            if r in REG32: return '%s = %s;' % (r, v)
            if r in REG16: return '%s = (%s & 0xffff0000u) | ((%s) & 0xffffu);' % (REG16[r], REG16[r], v)
            if r in REG8L: return '%s = (%s & 0xffffff00u) | ((%s) & 0xffu);' % (REG8L[r], REG8L[r], v)
            if r in REG8H: return '%s = (%s & 0xffff00ffu) | (((%s) & 0xffu) << 8);' % (REG8H[r], REG8H[r], v)
            raise Unsupported('register ' + r)
        if t == 'm':
            n = op[1]
            a = ea or self.ea(op)
            if n == 1: return 'W8(%s, %s);' % (a, v)
            if n == 2: return 'W16(%s, %s);' % (a, v)
            if n == 4: return 'W32(%s, %s);' % (a, v)
            raise Unsupported('memory write size %d' % n)
        raise Unsupported('write to ' + t)

    def opsize(self, op):
        if op[0] == 'r':
            r = op[1]
            if r in REG32: return 4
            if r in REG16: return 2
            if r in REG8L or r in REG8H: return 1
            raise Unsupported('register ' + r)
        if op[0] == 'm':
            return op[1]
        return op[2]

    def emit(self, s):
        self.out.append('    ' + s)

    # -------------------------------------------------- flags
    def set_flags(self, i, kind, n, d, s, r, ci=None):
        """Emit materialised flags that are live, and fused inputs if needed."""
        live = self.live_out_defs.get(i.addr, 0)
        for f in (CF, PF, AF, ZF, SF, OF):
            if live & f:
                self.emit('    %s = %s;' % (FNAME[f], flag_expr(kind, f, n, d, s, r, ci)))
        if i.addr in self.fused_producers:
            self.emit('    fa = %s; fb = %s; fr = %s;%s' % (d, s, r, (' fc = %s;' % ci) if ci else ''))

    def cond(self, i, cc):
        p = self.fused.get(i.addr)
        if p is not None:
            kind, n = p
            return fused_cond(kind, n, cc)
        return cond_expr(cc, lambda f: FNAME[f])

    # -------------------------------------------------- control transfer helpers
    def goto(self, t):
        self.labels.add(t)
        if t <= self.cur_addr:
            # a loop: program memory is not volatile in the generated C, so make every pass
            # read it again (another thread may be what the loop waits for)
            return '{ MEMBARRIER(); goto L_%x; }' % t
        return 'goto L_%x;' % t

    def tail(self, t):
        st = 'STOREFLAGS(); ' if self.prog.entry_flags.get(t) else ''
        return '%sSAVE(); %s(c); return;' % (st, self.prog.fname(t))

    def fallthrough(self, i, nxt_emitted):
        """Code to continue after instruction i when it falls through."""
        t = i.next
        if i.addr in self.f.fall_tails:
            return self.tail(self.f.fall_tails[i.addr])
        if t == nxt_emitted:
            return None
        if t in self.f.insns:
            return self.goto(t)
        return 'SAVE(); fell_off(c, %s); return;' % hx(i.addr)

    # -------------------------------------------------- analysis
    def analyse(self):
        f = self.f
        insns = f.insns
        addrs = sorted(insns)
        self.addrs = addrs
        # successors inside the function
        succ = {}
        targets = set()
        for a in addrs:
            i = insns[a]
            pre, m = split_prefix(i.mn)
            s = []
            nxt = i.next
            falls = True
            if m in ('ret', 'int3', 'hlt', 'ud2'):
                falls = False
            elif m == 'jmp':
                falls = False
                if a in f.jtables:
                    s.extend(f.jtables[a])
                elif i.ops[0][0] == 'i' and a not in f.tails:
                    s.append(i.ops[0][1])
            elif m in JCC or m in ('jecxz', 'jcxz', 'loop', 'loope', 'loopne'):
                t = i.ops[0][1]
                if a not in f.tails:
                    s.append(t)
            elif m == 'call':
                op = i.ops[0]
                if op[0] == 'i' and op[1] in self.prog.noreturn:
                    falls = False
                if op[0] == 'm' and op[3] is None and op[4] is None and \
                        self.prog.iat_name(op[6]) in self.prog.noreturn_imports:
                    falls = False
            if falls and a not in f.fall_tails and nxt in insns:
                s.append(nxt)
            for t in s:
                if t != nxt or not falls:
                    targets.add(t)
            succ[a] = [t for t in s if t in insns]
            for t in s:
                if t not in insns:
                    raise Unsupported('successor %x of %x not in function' % (t, a))
        self.succ = succ
        targets.add(f.entry)
        self.block_starts = targets
        # flag info
        info = {a: flag_info(insns[a]) for a in addrs}
        # fusion: consumer -> producer in the same block, nothing in between touching flags
        prev = {}
        for a in addrs:
            i = insns[a]
            prev[i.next] = a
        fused = {}
        fused_prod = set()
        for a in addrs:
            uses = info[a][0]
            if not uses:
                continue
            i = insns[a]
            pre, m = split_prefix(i.mn)
            if not (m in JCC or m in SETCC or m in ('adc', 'sbb')):
                continue
            cur = a
            while cur not in targets:
                p = prev.get(cur)
                if p is None or p not in insns:
                    break
                pi = insns[p]
                if not (self.falls_into(pi, cur)):
                    break
                pu, pd, pmd, pk = info[p]
                if pd or pmd:
                    if pk in FUSABLE and (pd & uses) == uses:
                        n = self.opsize(pi.ops[0]) if pk != 'sahf' else 1
                        fused[a] = (pk, n)
                        fused_prod.add(p)
                    break
                cur = p
        self.fused = fused
        self.fused_producers = fused_prod
        # liveness (fused consumers read the saved inputs, not the flags); calls, returns and
        # tail calls pass flags to and from other functions as Program.entry/ret_flags say
        prog = self.prog
        uses = {a: (0 if a in fused else info[a][0]) for a in addrs}
        defs = {a: info[a][1] for a in addrs}
        mdefs = {a: info[a][2] for a in addrs}
        self.call_target = {}
        for a in addrs:
            i = insns[a]
            m = split_prefix(i.mn)[1]
            if m == 'call' and i.ops[0][0] == 'i':
                self.call_target[a] = i.ops[0][1]
                uses[a] |= prog.entry_flags.get(i.ops[0][1], 0)
            elif m == 'ret':
                uses[a] |= prog.ret_flags.get(f.entry, 0)
            for t in (f.tails.get(a), f.fall_tails.get(a)):
                if t is not None:
                    uses[a] |= prog.entry_flags.get(t, 0)
        live_in = {a: 0 for a in addrs}
        live_out = {a: 0 for a in addrs}
        preds = defaultdict(list)
        for a in addrs:
            for t in succ[a]:
                preds[t].append(a)
        work = list(addrs)
        inwork = set(work)
        while work:
            a = work.pop()
            inwork.discard(a)
            lo = 0
            for t in succ[a]:
                lo |= live_in[t]
            li = uses[a] | (lo & ~defs[a])
            live_out[a] = lo
            if li != live_in[a]:
                live_in[a] = li
                for p in preds[a]:
                    if p not in inwork:
                        work.append(p)
                        inwork.add(p)
        self.live_in = live_in
        self.live_out_defs = {a: live_out[a] & (defs[a] | mdefs[a]) for a in addrs}
        self.entry_live = live_in[f.entry]
        # flags read after calls: the callee must hand them back
        self.after_call = {}
        for a in addrs:
            i = insns[a]
            if split_prefix(i.mn)[1] == 'call':
                need = 0
                for t in succ[a]:
                    need |= live_in[t]
                if need:
                    if a in self.call_target:
                        self.after_call[a] = need
                    else:
                        self.warn.append('flags %x read after indirect/import call at %x' % (need, a))

    def falls_into(self, pi, cur):
        pre, m = split_prefix(pi.mn)
        if pi.next != cur:
            return False
        if m in ('jmp', 'ret', 'int3', 'call'):
            return False
        return True

    # -------------------------------------------------- generation
    def gen(self):
        self.cur_addr = 0
        self.ilines = {}
        self.analyse()
        f = self.f
        body = []
        self.out = body
        addrs = self.addrs
        for k, a in enumerate(addrs):
            i = f.insns[a]
            nxt = addrs[k + 1] if k + 1 < len(addrs) else None
            body.append('L_%x: /* %s */' % (a, i.text.replace('*/', '* /')))
            self.cur_addr = a
            start = len(body)
            try:
                ends = self.insn(i)
            except Unsupported as e:
                self.warn.append('%08x %s: %s' % (a, i.text, e))
                self.emit('SAVE(); unsupported(c, %s, "%s"); return;' % (hx(a), i.text))
                ends = True
            if not ends:
                ft = self.fallthrough(i, nxt)
                if ft:
                    self.emit(ft)
            self.ilines[a] = (start, len(body))
        self.fpu_body = list(body)
        fpu_head = self.fpu_locals(body)
        # assemble
        name = self.prog.fname(f.entry)
        hook = self.prog.hooks.get(f.entry)
        out = ['/* %08x */' % f.entry]
        if hook:
            # hooked: the runtime's hook runs first and may adjust the arguments, run the
            # original (F_x_orig) itself and work on its results, or replace it (returns 1)
            out += ['void %s(CPU *c)' % name, '{',
                    '    if (hook_%s && hook_%s(c)) return;' % (hook, hook),
                    '    %s_orig(c);' % name, '}', '', 'void %s_orig(CPU *restrict c)' % name]
        else:
            # restrict: the CPU record is never program memory, so the compiler may keep registers
            # and the x87 stack top in host registers across the program's memory accesses
            out += ['void %s(CPU *restrict c)' % name]
        out += ['{'] + \
              ['    REGS_IN;',
               '    uint32_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0;',
               '    uint32_t fa = 0, fb = 0, fr = 0, fc = 0;',
               '    (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of;',
               '    (void)fa; (void)fb; (void)fr; (void)fc;']
        if self.entry_live:
            out.append('    LOADFLAGS();')
        out.extend(fpu_head)
        if addrs[0] != f.entry:
            out.append('    goto L_%x;' % f.entry)
            self.labels.add(f.entry)
        for line in body:
            if line.startswith('L_'):
                lab = int(line[2:line.index(':')], 16)
                if lab in self.labels:
                    out.append(line)
                else:
                    out.append('/* %s' % line[line.index('/*') + 3:])
            else:
                out.append(line)
        out.append('}')
        return '\n'.join(out) + '\n'

    # -------------------------------------------------- x87 stack in locals (tools/fpustack.py)
    def fpu_plan(self, body):
        """Per instruction: text events, stack effect, call effect and exits; then the depths.
        Sets self.fpu_delta (the function's stack effect, None if unknown) and returns the
        depth at each instruction if the function can keep its stack in locals, else None."""
        f = self.f
        P = self.prog
        delta = P.fpu_delta
        evs, eff, callfx, exits, check = {}, {}, {}, {}, set()
        uses, ok = False, True
        for a in self.addrs:
            s, e = self.ilines[a]
            text = '\n'.join(body[s:e])
            ev = fpustack.events(text)
            evs[a] = ev
            if ev:
                uses = True
            eff[a] = fpustack.net(ev)
            if 'unsupported(c' in text:
                ok = False
            i = f.insns[a]
            pre, m = split_prefix(i.mn)
            ex = []                         # (where: 'in' or 'out', callee or None)
            if m == 'call' and 'done in place' not in text:
                op = i.ops[0]
                if op[0] == 'i':
                    callfx[a] = 0 if op[1] in P.noreturn else delta.get(op[1])
                elif op[0] == 'm' and op[3] is None and op[4] is None and P.iat_name(op[6]):
                    callfx[a] = 0
                else:
                    callfx[a] = 0           # through a pointer: assumed, checked at run time
                    check.add(a)
            elif m in ('ret', 'retn'):
                ex.append(('in', None))
            elif m == 'jmp':
                if a in f.tails:
                    ex.append(('in', f.tails[a]))
                elif a in f.import_tails or a in f.indirect_jumps:
                    ex.append(('in', None))
                elif a in P.nonlocal_jumps:
                    ok = False
            elif a in f.tails:              # conditional jump to another function
                ex.append(('in', f.tails[a]))
            if a in f.fall_tails:
                ex.append(('out', f.fall_tails[a]))
            exits[a] = ex
        self.fpu_uses = uses
        # depths from the entry
        depth = {f.entry: 0}
        work = [f.entry]
        known = True
        exit_depths = set()
        self.fpu_why = ''
        while work:
            a = work.pop()
            d = depth[a]
            n = eff[a]
            if n is None:
                known = False
                self.fpu_why = 'unknown stack use at %x' % a
                break
            dout = d + n
            if a in callfx:
                if callfx[a] is None:
                    known = False
                    self.fpu_why = 'call at %x to a function with unknown stack effect' % a
                    break
                dout += callfx[a]
            for where, callee in exits[a]:
                x = d if where == 'in' else dout
                if callee is not None:
                    if delta.get(callee) is None:
                        known = False
                        self.fpu_why = 'tail at %x to %x with unknown stack effect' % (a, callee)
                        break
                    x += delta[callee]
                exit_depths.add(x)
            if not known:
                break
            for t in self.succ[a]:
                if t in depth:
                    if depth[t] != dout:
                        known = False
                        self.fpu_why = 'depth %d and %d meet at %x' % (depth[t], dout, t)
                        break
                else:
                    depth[t] = dout
                    work.append(t)
            if not known:
                break
        if not known or len(exit_depths) > 1:
            if known:
                self.fpu_why = 'exits at depths %s' % sorted(exit_depths)
            self.fpu_delta = None
            return None
        self.fpu_delta = exit_depths.pop() if exit_depths else 0
        if not uses or not ok:
            self.fpu_why = 'no x87 use' if not uses else 'unsupported or nonlocal code'
            return None
        # locals only where every position read was pushed here and the stack fits
        for a, ev in evs.items():
            if a not in depth:
                continue
            d = depth[a]
            for kind, arg, rel, span in ev:
                r = d + rel
                if r < 0 or r > 8 or (kind == 'st' and r - arg < 1) or kind == 'bad':
                    self.fpu_why = 'reads the caller\'s stack or %s at %x (depth %d)' % (kind, a, r)
                    return None
                if kind == 'push' and r + 1 > 8:
                    self.fpu_why = 'deeper than 8 at %x' % a
                    return None
        self.fpu_check = check
        self.fpu_callfx = callfx
        return depth

    def fpu_locals(self, body):
        """Rewrite the body with the x87 stack in locals where possible; returns the lines
        declaring them."""
        depth = self.fpu_plan(body)
        if not self.prog.fpu_final or depth is None:
            return []
        f = self.f
        maxd = 0
        for a in self.addrs:
            s, e = self.ilines[a]
            if a not in depth or s == e:
                continue                    # unreachable (never runs) or no code (nop)
            d = depth[a]
            maxd = max(maxd, d)
            text = '\n'.join(body[s:e])
            lines = body[s:e]
            for kind, arg, rel, span in fpustack.events(text):
                maxd = max(maxd, d + rel + (1 if kind == 'push' else 0))
            new = self.fpu_split_depths(lines, d)
            pre = []
            post = []
            i = f.insns[a]
            pre_m = split_prefix(i.mn)[1]
            leaves = any('return;' in l for l in lines)
            if a in self.fpu_callfx:
                after = d + (self.fpu_callfx[a] or 0)
                pre = ['    %s;' % x for x in fpustack.spill(d)]
                post = ['    %s;' % x for x in fpustack.reload(after)]
                if a in self.fpu_check:
                    post.append('    if (c->ftop != ((ft0 - %d) & 7)) fpu_depth_fail(c, %s);' % (after, hx(a)))
            elif leaves:
                pre = ['    %s;' % x for x in fpustack.spill(d)]
            if a in self.fpu_callfx:
                # the reload goes right after the call, before a fall-through goto
                k = len(new)
                while k > 0 and new[k - 1].lstrip().startswith('goto L_'):
                    k -= 1
                new = new[:k] + post + new[k:]
                post = []
            body[s:e] = [''] * (e - s)
            body[s] = '\n'.join(pre + new + post)
        return ['    const uint32_t ft0 = c->ftop;',
                '    f80 %s;' % ', '.join('s%d = 0' % p for p in range(1, maxd + 1))] if maxd else \
               ['    const uint32_t ft0 = c->ftop;']

    def fpu_split_depths(self, lines, d):
        """the instruction's lines, each rewritten at the depth reached by the lines before it"""
        out = []
        for l in lines:
            out.append((l, d))
            n = fpustack.net(fpustack.events(l))
            d += n or 0
        res = []
        for l, dl in out:
            res.append(fpustack.rewrite(l, dl) if any(t in l for t in ('ST(', 'FPUSH(', 'FPOP()', 'fpu_')) else l)
        return res

    # -------------------------------------------------- instructions
    def insn(self, i):
        """Emit code for one instruction. Returns True if control never falls through."""
        pre, m = split_prefix(i.mn)
        ops = i.ops
        a = i.addr
        self.cur = i
        E = self.emit
        if m in ('nop', 'wait', 'fwait'):
            return False
        if m == 'mov':
            E(self.wr(ops[0], self.rd(ops[1])))
            return False
        if m == 'movzx':
            E(self.wr(ops[0], self.rd(ops[1])))
            return False
        if m == 'movsx':
            n = self.opsize(ops[1])
            E(self.wr(ops[0], '(uint32_t)(int32_t)(%s)%s' % (STYPE[n], self.rd(ops[1]))))
            return False
        if m == 'lea':
            o = ops[1]
            E(self.wr(ops[0], self.ea(('m', 4, None) + o[3:])))
            return False
        if m == 'push':
            n = self.opsize(ops[0])
            if n != 4:
                raise Unsupported('push size %d' % n)
            E('{ uint32_t v = %s; esp -= 4; W32(esp, v); }' % self.rd(ops[0]))
            return False
        if m == 'pop':
            E('{ uint32_t v = R32(esp); esp += 4; %s }' % self.wr(ops[0], 'v'))
            return False
        if m == 'xchg':
            x, y = ops
            if x[0] == 'm' or y[0] == 'm':
                mo, ro = (x, y) if x[0] == 'm' else (y, x)
                E('{ uint32_t ea = %s; uint32_t t = %s; %s %s }' % (
                    self.ea(mo), self.rd(mo, 'ea'), self.wr(mo, self.rd(ro), 'ea'), self.wr(ro, 't')))
            else:
                E('{ uint32_t t = %s; %s %s }' % (self.rd(x), self.wr(x, self.rd(y)), self.wr(y, 't')))
            return False
        if m == 'leave':
            E('esp = ebp; ebp = R32(esp); esp += 4;')
            return False
        if m == 'cdq':
            E('edx = ((int32_t)eax < 0) ? 0xffffffffu : 0;')
            return False
        if m == 'cwde':
            E('eax = (uint32_t)(int32_t)(int16_t)eax;')
            return False
        if m == 'cbw':
            E('eax = (eax & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xffffu);')
            return False
        if m == 'cwd':
            E('edx = (edx & 0xffff0000u) | (((int16_t)eax < 0) ? 0xffffu : 0);')
            return False
        if m == 'cld':
            E('c->df = 0;')
            return False
        if m == 'std':
            E('c->df = 1;')
            return False
        if m == 'clc':
            if self.live_out_defs.get(a, 0) & CF: E('cf = 0;')
            return False
        if m == 'stc':
            if self.live_out_defs.get(a, 0) & CF: E('cf = 1;')
            return False
        if m == 'cmc':
            if self.live_out_defs.get(a, 0) & CF: E('cf ^= 1;')
            return False
        if m == 'sahf':
            E('{ uint32_t r = (eax >> 8) & 0xffu;')
            self.set_flags(i, 'sahf', 1, 'r', 'r', 'r')
            E('}')
            return False
        if m == 'lahf':
            E('eax = (eax & 0xffff00ffu) | ((cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | 2u) << 8);')
            return False
        if m in ('pushfd', 'pushf'):
            # the flags as the program sees them: the arithmetic ones, the direction flag, IF and
            # the always-one bit (pushf/popf pairs save the direction flag round an inline memset)
            E('{ uint32_t v = cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (of << 11) | '
              '(c->df << 10) | 0x202u | c->eflags_id; esp -= 4; W32(esp, v); }')
            return False
        if m in ('popfd', 'popf'):
            E('{ uint32_t v = R32(esp); esp += 4; cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; '
              'zf = (v >> 6) & 1; sf = (v >> 7) & 1; of = (v >> 11) & 1; c->df = (v >> 10) & 1; '
              'c->eflags_id = v & 0x200000u; }')
            return False
        if m in ('pushaw', 'pushal'):
            n = 2 if m == 'pushaw' else 4
            E('{ uint32_t s0 = esp; uint32_t r[8] = {eax, ecx, edx, ebx, s0, ebp, esi, edi}; '
              'for (int k = 0; k < 8; k++) { esp -= %d; W%d(esp, r[k]); } }' % (n, 8 * n))
            return False
        if m in ('popaw', 'popal'):
            n = 2 if m == 'popaw' else 4
            keep = '0xffff0000u' if n == 2 else '0'
            E('{ uint32_t r[8]; for (int k = 7; k >= 0; k--) { r[k] = R%d(esp); esp += %d; } '
              'edi = (edi & %s) | r[7]; esi = (esi & %s) | r[6]; ebp = (ebp & %s) | r[5]; '
              'ebx = (ebx & %s) | r[3]; edx = (edx & %s) | r[2]; ecx = (ecx & %s) | r[1]; '
              'eax = (eax & %s) | r[0]; }' % ((8 * n, n) + (keep,) * 7))
            return False
        if m == 'cpuid':
            E('SAVE(); x86_cpuid(c); LOAD();')
            return False
        if m == 'rdtsc':
            E('{ uint64_t t = x86_rdtsc(c); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }')
            return False
        if m in ARITH:
            return self.arith(i, m)
        if m in ('inc', 'dec', 'neg', 'not'):
            return self.unary(i, m)
        if m in SHIFTS:
            return self.shift(i, m)
        if m in ('shld', 'shrd'):
            return self.dshift(i, m)
        if m in ('mul', 'imul', 'div', 'idiv'):
            return self.muldiv(i, m)
        if m in SETCC:
            E(self.wr(ops[0], '(%s) ? 1u : 0u' % self.cond(i, SETCC[m])))
            return False
        if m in JCC:
            t = ops[0][1]
            if a in self.f.tails:
                E('if (%s) { %s }' % (self.cond(i, JCC[m]), self.tail(self.f.tails[a])))
            else:
                E('if (%s) %s' % (self.cond(i, JCC[m]), self.goto(t)))
            return False
        if m in ('jecxz', 'loop', 'loope', 'loopne'):
            t = ops[0][1]
            go = self.tail(self.f.tails[a]) if a in self.f.tails else self.goto(t)
            if m == 'jecxz':
                E('if (ecx == 0) { %s }' % go)
            elif m == 'loop':
                E('if (--ecx != 0) { %s }' % go)
            elif m == 'loope':
                E('if (--ecx != 0 && %s) { %s }' % (self.cond(i, 'e'), go))
            else:
                E('if (--ecx != 0 && !%s) { %s }' % (self.cond(i, 'e'), go))
            return False
        if m == 'jmp':
            return self.jmp(i)
        if m == 'call':
            return self.call(i)
        if m == 'ret':
            n = ops[0][1] if ops else 0
            st = 'STOREFLAGS(); ' if self.prog.ret_flags.get(self.f.entry) else ''
            E('%sc->ret = R32(esp); esp += %d; SAVE(); return;' % (st, 4 + n))
            return True
        if m == 'int3':
            E('SAVE(); trap_int3(c, %s); return;' % hx(a))
            return True
        if m in STRING:
            return self.string(i, pre, m)
        if i.raw and self.x87_opcode(i) is not None:
            return self.x87(i)
        raise Unsupported('instruction')

    def arith(self, i, m):
        dst, src = i.ops
        n = self.opsize(dst)
        E = self.emit
        M = MASK[n]
        writes = m not in ('cmp', 'test')
        if dst[0] == 'm':
            E('{ uint32_t ea = %s;' % self.ea(dst))
            dea = 'ea'
        else:
            E('{')
            dea = None
        sv = self.rd(src)
        if src[0] == 'i':
            sv = self.imm(src[1]) if n == 4 else hx(src[1] & ((1 << (8 * n)) - 1))
        E('    uint32_t d = %s, s = %s;' % (self.rd(dst, dea), sv))
        ci = None
        if m in ('adc', 'sbb'):
            p = self.fused.get(i.addr)
            civ = fused_cond(p[0], p[1], 'b') if p else 'cf'
            E('    uint32_t ci = %s;' % civ)
            ci = 'ci'
        op = {'add': 'd + s', 'sub': 'd - s', 'cmp': 'd - s', 'and': 'd & s', 'or': 'd | s',
              'xor': 'd ^ s', 'test': 'd & s', 'adc': 'd + s + ci', 'sbb': 'd - s - ci'}[m]
        if n == 4:
            E('    uint32_t r = %s;' % op)
        else:
            E('    uint32_t r = (%s) & %s;' % (op, M))
        if writes:
            E('    ' + self.wr(dst, 'r', dea))
        kind = {'add': 'add', 'sub': 'sub', 'cmp': 'sub', 'adc': 'adc', 'sbb': 'sbb'}.get(m, 'logic')
        self.set_flags(i, kind, n, 'd', 's', 'r', ci)
        E('}')
        return False

    def unary(self, i, m):
        dst = i.ops[0]
        n = self.opsize(dst)
        E = self.emit
        M = MASK[n]
        if dst[0] == 'm':
            E('{ uint32_t ea = %s;' % self.ea(dst))
            dea = 'ea'
        else:
            E('{')
            dea = None
        if m == 'not':
            E('    %s' % self.wr(dst, '~%s & %s' % (self.rd(dst, dea), M), dea))
            E('}')
            return False
        if m == 'neg':
            E('    uint32_t d = 0, s = %s, r = (0u - s) & %s;' % (self.rd(dst, dea), M))
        elif m == 'inc':
            E('    uint32_t d = %s, s = 1, r = (d + 1) & %s;' % (self.rd(dst, dea), M))
        else:
            E('    uint32_t d = %s, s = 1, r = (d - 1) & %s;' % (self.rd(dst, dea), M))
        E('    ' + self.wr(dst, 'r', dea))
        self.set_flags(i, m, n, 'd', 's', 'r')
        E('}')
        return False

    def shift(self, i, m):
        dst = i.ops[0]
        n = self.opsize(dst)
        E = self.emit
        M = MASK[n]
        bits = BITS[n]
        cnt = shift_count(i)
        if dst[0] == 'm':
            E('{ uint32_t ea = %s;' % self.ea(dst))
            dea = 'ea'
        else:
            E('{')
            dea = None
        if cnt is None:
            E('    uint32_t s = ecx & 31;')
        else:
            if cnt == 0:
                E('}')
                return False
            rc = self.prog.const_at(i.addr, cnt)
            E('    uint32_t s = %s;' % ('rc_' + rc if rc else cnt))
        E('    uint32_t d = %s, r;' % self.rd(dst, dea))
        live = self.live_out_defs.get(i.addr, 0)
        if m in ('shl', 'sal'):
            E('    r = d << s;' if n == 4 else '    r = (d << s) & %s;' % M)
            kind = 'shl'
        elif m == 'shr':
            E('    r = d >> s;')
            kind = 'shr'
        elif m == 'sar':
            E('    r = (uint32_t)((int32_t)(%s)d >> s) & %s;' % (STYPE[n], M))
            kind = 'sar'
        elif m in ('rol', 'ror'):
            E('    uint32_t k = s %% %d;' % bits)
            if m == 'rol':
                E('    r = k ? (((d << k) | (d >> (%d - k))) & %s) : d;' % (bits, M))
                cfe, ofe = '(r & 1)', '((((r >> %d) & 1) ^ (r & 1)) & 1)' % (bits - 1)
            else:
                E('    r = k ? (((d >> k) | (d << (%d - k))) & %s) : d;' % (bits, M))
                cfe, ofe = '((r >> %d) & 1)' % (bits - 1), '((((r >> %d) ^ (r >> %d)) & 1))' % (bits - 1, bits - 2)
            E('    ' + self.wr(dst, 'r', dea))
            if live & (CF | OF):
                E('    if (s) { %s%s }' % ('cf = %s; ' % cfe if live & CF else '',
                                         'of = %s;' % ofe if live & OF else ''))
            E('}')
            return False
        elif m in ('rcl', 'rcr'):
            if n != 4:
                raise Unsupported('rcl/rcr size')
            E('    uint32_t k = s % 33, cin = cf;')
            if m == 'rcr':
                E('    r = d; for (uint32_t q = 0; q < k; q++) { uint32_t o = r & 1; r = (r >> 1) | (cin << 31); cin = o; }')
                ofe = '(((r >> 31) ^ (r >> 30)) & 1)'
            else:
                E('    r = d; for (uint32_t q = 0; q < k; q++) { uint32_t o = r >> 31; r = (r << 1) | cin; cin = o; }')
                ofe = '(((r >> 31) ^ cin) & 1)'
            E('    ' + self.wr(dst, 'r', dea))
            E('    if (k) { cf = cin; of = %s; }' % ofe)
            E('}')
            return False
        E('    ' + self.wr(dst, 'r', dea))
        if cnt is None and live:
            E('    if (s) {')
            self.set_flags(i, kind, n, 'd', 's', 'r')
            E('    }')
        else:
            self.set_flags(i, kind, n, 'd', 's', 'r')
        E('}')
        return False

    def dshift(self, i, m):
        dst, src, c = i.ops
        if self.opsize(dst) != 4:
            raise Unsupported('16-bit double shift')
        E = self.emit
        if dst[0] == 'm':
            E('{ uint32_t ea = %s;' % self.ea(dst))
            dea = 'ea'
        else:
            E('{')
            dea = None
        E('    uint32_t s = %s;' % ('ecx & 31' if c[0] == 'r' else str(c[1] & 31)))
        E('    uint32_t d = %s, x = %s, r = d;' % (self.rd(dst, dea), self.rd(src)))
        if m == 'shld':
            E('    if (s) r = (d << s) | (x >> (32 - s));')
            cfe = '((d >> (32 - s)) & 1)'
        else:
            E('    if (s) r = (d >> s) | (x << (32 - s));')
            cfe = '((d >> (s - 1)) & 1)'
        E('    ' + self.wr(dst, 'r', dea))
        live = self.live_out_defs.get(i.addr, 0)
        if live:
            E('    if (s) {')
            for f in (CF, PF, ZF, SF, OF, AF):
                if live & f:
                    e = {CF: cfe, PF: 'PARITY(r)', ZF: '(r == 0)', SF: '(r >> 31)',
                         OF: '(((r ^ d) >> 31) & 1)', AF: '0'}[f]
                    E('        %s = %s;' % (FNAME[f], e))
            E('    }')
        E('}')
        return False

    def muldiv(self, i, m):
        ops = i.ops
        E = self.emit
        live = self.live_out_defs.get(i.addr, 0)
        if m == 'imul' and len(ops) >= 2:
            dst = ops[0]
            n = self.opsize(dst)
            a = self.rd(ops[1]) if len(ops) == 3 else self.rd(dst)
            b = self.rd(ops[2]) if len(ops) == 3 else self.rd(ops[1])
            T = STYPE[n]
            E('{ int64_t p = (int64_t)(%s)%s * (int64_t)(%s)%s; uint32_t r = (uint32_t)p & %s;' % (T, a, T, b, MASK[n]))
            E('    ' + self.wr(dst, 'r'))
            self.mulflags(live, '(p != (int64_t)(%s)r)' % T, 'r', n)
            E('}')
            return False
        src = ops[0]
        n = self.opsize(src)
        sv = self.rd(src)
        if m == 'mul':
            if n == 4:
                E('{ uint64_t p = (uint64_t)eax * %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32);' % sv)
                self.mulflags(live, '(edx != 0)', 'eax', 4)
            elif n == 1:
                E('{ uint32_t p = (eax & 0xffu) * %s; eax = (eax & 0xffff0000u) | (p & 0xffffu);' % sv)
                self.mulflags(live, '((p >> 8) != 0)', '(p & 0xffu)', 1)
            else:
                E('{ uint32_t p = (eax & 0xffffu) * %s; eax = (eax & 0xffff0000u) | (p & 0xffffu); edx = (edx & 0xffff0000u) | (p >> 16);' % sv)
                self.mulflags(live, '((p >> 16) != 0)', '(p & 0xffffu)', 2)
            E('}')
            return False
        if m == 'imul':
            if n == 4:
                E('{ int64_t p = (int64_t)(int32_t)eax * (int64_t)(int32_t)%s; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32);' % sv)
                self.mulflags(live, '(p != (int64_t)(int32_t)eax)', 'eax', 4)
            elif n == 1:
                E('{ int32_t p = (int32_t)(int8_t)eax * (int32_t)(int8_t)%s; eax = (eax & 0xffff0000u) | ((uint32_t)p & 0xffffu);' % sv)
                self.mulflags(live, '(p != (int32_t)(int8_t)p)', '((uint32_t)p & 0xffu)', 1)
            else:
                E('{ int32_t p = (int32_t)(int16_t)eax * (int32_t)(int16_t)%s; eax = (eax & 0xffff0000u) | ((uint32_t)p & 0xffffu); edx = (edx & 0xffff0000u) | (((uint32_t)p >> 16) & 0xffffu);' % sv)
                self.mulflags(live, '(p != (int32_t)(int16_t)p)', '((uint32_t)p & 0xffffu)', 2)
            E('}')
            return False
        # div / idiv
        if n != 4:
            if n == 1 and m == 'div':
                E('{ uint32_t v = %s, x = eax & 0xffffu; if (v == 0 || x / v > 0xff) { SAVE(); div_fault(c, %s); return; }' % (sv, hx(i.addr)))
                E('    eax = (eax & 0xffff0000u) | ((x %% v) << 8) | (x / v); }')
                return False
            raise Unsupported('div size %d' % n)
        if m == 'div':
            E('{ uint32_t v = %s; uint64_t x = ((uint64_t)edx << 32) | eax;' % sv)
            E('    if (v == 0 || x / v > 0xffffffffu) { SAVE(); div_fault(c, %s); return; }' % hx(i.addr))
            E('    eax = (uint32_t)(x / v); edx = (uint32_t)(x % v); }')
        else:
            E('{ int32_t v = (int32_t)%s; int64_t x = (int64_t)(((uint64_t)edx << 32) | eax);' % sv)
            E('    if (v == 0 || (v == -1 && x == INT64_MIN) || x / v > INT32_MAX || x / v < INT32_MIN) { SAVE(); div_fault(c, %s); return; }' % hx(i.addr))
            E('    eax = (uint32_t)(int32_t)(x / v); edx = (uint32_t)(int32_t)(x % v); }')
        return False

    def mulflags(self, live, ovf, r, n):
        E = self.emit
        if live & (CF | OF):
            E('    cf = of = %s;' % ovf)
        if live & ZF: E('    zf = (%s == 0);' % r)
        if live & SF: E('    sf = ((%s & %s) != 0);' % (r, SIGN[n]))
        if live & PF: E('    pf = PARITY(%s);' % r)
        if live & AF: E('    af = 0;')

    # -------------------------------------------------- control flow
    def jmp(self, i):
        f = self.f
        a = i.addr
        E = self.emit
        op = i.ops[0]
        if a in f.tails:
            E(self.tail(f.tails[a]))
            return True
        if op[0] == 'i':
            E(self.goto(op[1]))
            return True
        if a in f.import_tails:
            E('SAVE(); %s(c); return;' % self.prog.import_fn(f.import_tails[a]))
            return True
        if a in f.jtables:
            E('switch (%s) {' % self.rd(op))
            for t in sorted(set(f.jtables[a])):
                E('case %s: %s' % (hx(t), self.goto(t)))
            E('default: SAVE(); bad_jump(c, %s, %s); return;' % (hx(a), self.rd(op)))
            E('}')
            return True
        if a in self.prog.nonlocal_jumps:
            E('SAVE(); nonlocal_jump(c, %s, %s); return;' % (hx(a), self.rd(op)))
            return True
        # indirect tail call
        E('{ uint32_t t = %s; SAVE(); icall(c, t); return; }' % self.rd(op))
        return True

    def call(self, i):
        a = i.addr
        op = i.ops[0]
        E = self.emit
        ret = hx(i.next)
        noret = False
        if op[0] == 'i' and self.prog.inline.get(op[1]) == 'ftol' and not self.prog.ret_flags.get(op[1]) \
                and a not in self.after_call:
            # MSVC's _ftol: truncate ST(0) to a 64-bit integer in edx:eax and pop. The original
            # switches the rounding mode twice (fldcw, slow on current CPUs); fisttp truncates
            # by itself with the same result and exception flags. Its stack bytes are written
            # as it writes them: return address, saved ebp, the control word twice, the result.
            E('{ uint32_t b = esp - 8; uint16_t cw = (uint16_t)c->fcw; int64_t r;   /* call _ftol, done in place */')
            E('  W32(esp - 4, %s); W32(b, ebp); W16(b - 2, cw); W16(b - 4, cw | 0x0c00u);' % ret)
            E('  r = fpu_ftol(ST(0)); FPOP(); W64(b - 12, (uint64_t)r);')
            E('  eax = (uint32_t)r; edx = (uint32_t)((uint64_t)r >> 32); }')
            return False
        if op[0] == 'i':
            t = op[1]
            st = 'STOREFLAGS(); ' if self.prog.entry_flags.get(t) else ''
            ld = ' LOADFLAGS();' if a in self.after_call else ''
            E('%sesp -= 4; W32(esp, %s); SAVE(); %s(c); LOAD(); CHKRET(%s, %s);%s' % (
                st, ret, self.prog.fname(t), hx(a), ret, ld))
            noret = t in self.prog.noreturn
        elif op[0] == 'm' and op[3] is None and op[4] is None and self.prog.iat_name(op[6]):
            E('esp -= 4; W32(esp, %s); SAVE(); %s(c); LOAD(); CHKRET(%s, %s);' % (
                ret, self.prog.import_fn(op[6]), hx(a), ret))
            noret = self.prog.iat_name(op[6]) in self.prog.noreturn_imports
        else:
            E('{ uint32_t t = %s; esp -= 4; W32(esp, %s); SAVE(); icall(c, t); LOAD(); CHKRET(%s, %s); }' % (
                self.rd(op), ret, hx(a), ret))
        if noret:
            E('SAVE(); returned_from_noreturn(c, %s); return;' % hx(a))
            return True
        return False

    # -------------------------------------------------- string instructions
    def string(self, i, pre, m):
        E = self.emit
        n = {'b': 1, 'w': 2, 'd': 4}[m[-1]]
        base = m[:-1]
        R = {1: 'R8', 2: 'R16', 4: 'R32'}[n]
        W = {1: 'W8', 2: 'W16', 4: 'W32'}[n]
        acc = {1: '(eax & 0xffu)', 2: '(eax & 0xffffu)', 4: 'eax'}[n]
        step = '(c->df ? (uint32_t)-%d : %du)' % (n, n)
        live = self.live_out_defs.get(i.addr, 0)
        if base == 'movs':
            if pre == 'rep':
                E('REP_MOVS(%d);' % n)
            else:
                E('{ %s(edi, %s(esi)); esi += %s; edi += %s; }' % (W, R, step, step))
            return False
        if base == 'stos':
            if pre == 'rep':
                E('REP_STOS(%d);' % n)
            else:
                E('{ %s(edi, %s); edi += %s; }' % (W, acc, step))
            return False
        if base == 'lods':
            if pre:
                raise Unsupported('rep lods')
            E('{ uint32_t v = %s(esi); esi += %s; %s }' % (R, step, self.wr(('r', {1: 'al', 2: 'ax', 4: 'eax'}[n], n), 'v')))
            return False
        # scas / cmps
        if base == 'scas':
            dv, sv, adv = acc, '%s(edi)' % R, 'edi += %s;' % step
        else:
            dv, sv, adv = '%s(esi)' % R, '%s(edi)' % R, 'esi += %s; edi += %s;' % (step, step)
        if pre is None:
            E('{ uint32_t d = %s, s = %s, r = (d - s) & %s; %s' % (dv, sv, MASK[n], adv))
            self.set_flags(i, 'sub', n, 'd', 's', 'r')
            E('}')
            return False
        stop = '!=' if pre in ('repe', 'repz') else '=='
        E('{ uint32_t d = 0, s = 0, r = 0, ran = 0;')
        E('    while (ecx) { ecx--; d = %s; s = %s; r = (d - s) & %s; %s ran = 1; if (r %s 0) break; }' % (dv, sv, MASK[n], adv, stop))
        if live:
            E('    if (ran) {')
            for f in (CF, PF, AF, ZF, SF, OF):
                if live & f:
                    E('        %s = %s;' % (FNAME[f], flag_expr('sub', f, n, 'd', 's', 'r')))
            E('    }')
        E('}')
        return False

    # -------------------------------------------------- x87
    def x87_opcode(self, i):
        b = i.raw
        k = 0
        while k < len(b) and b[k] in (0x9b, 0x66, 0x67, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65):
            k += 1
        if k + 1 < len(b) and 0xd8 <= b[k] <= 0xdf:
            return b[k], b[k + 1]
        return None

    def x87(self, i):
        E = self.emit
        op, modrm = self.x87_opcode(i)
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        mem = [o for o in i.ops if o[0] == 'm']
        ea = self.ea(mem[0]) if mem else None
        ARITH_OPS = ['add', 'mul', 'com', 'comp', 'sub', 'subr', 'div', 'divr']

        def arith_st0(kind, v):
            if kind == 'add': E('ST(0) = ST(0) + %s;' % v)
            elif kind == 'mul': E('ST(0) = ST(0) * %s;' % v)
            elif kind == 'sub': E('ST(0) = ST(0) - %s;' % v)
            elif kind == 'subr': E('ST(0) = %s - ST(0);' % v)
            elif kind == 'div': E('ST(0) = ST(0) / %s;' % v)
            elif kind == 'divr': E('ST(0) = %s / ST(0);' % v)
            elif kind == 'com': E('fpu_com(c, ST(0), %s);' % v)
            elif kind == 'comp': E('fpu_com(c, ST(0), %s); FPOP();' % v)

        if mod != 3:
            if op in (0xd8, 0xdc, 0xda, 0xde):
                v = {0xd8: '(f80)RF32(%s)', 0xdc: '(f80)RF64(%s)', 0xda: '(f80)(int32_t)R32(%s)',
                     0xde: '(f80)(int16_t)R16(%s)'}[op] % ea
                E('{ f80 v = %s;' % v)
                arith_st0(ARITH_OPS[reg], 'v')
                E('}')
                return False
            if op == 0xd9:
                if reg == 0: E('FPUSH((f80)RF32(%s));' % ea); return False
                if reg == 2: E('WF32(%s, (float)ST(0));' % ea); return False
                if reg == 3: E('WF32(%s, (float)ST(0)); FPOP();' % ea); return False
                if reg == 4: E('fpu_ldenv(c, %s);' % ea); return False
                if reg == 5: E('fpu_setcw(c, R16(%s));' % ea); return False
                if reg == 6: E('fpu_stenv(c, %s);' % ea); return False
                if reg == 7: E('W16(%s, c->fcw);' % ea); return False
            if op == 0xdb:
                if reg == 0: E('FPUSH((f80)(int32_t)R32(%s));' % ea); return False
                if reg == 2: E('W32(%s, (uint32_t)fpu_ist32(ST(0)));' % ea); return False
                if reg == 3: E('W32(%s, (uint32_t)fpu_ist32(ST(0))); FPOP();' % ea); return False
                if reg == 5: E('FPUSH(RF80(%s));' % ea); return False
                if reg == 7: E('WF80(%s, ST(0)); FPOP();' % ea); return False
            if op == 0xdd:
                if reg == 0: E('FPUSH((f80)RF64(%s));' % ea); return False
                if reg == 2: E('WF64(%s, (double)ST(0));' % ea); return False
                if reg == 3: E('WF64(%s, (double)ST(0)); FPOP();' % ea); return False
                if reg == 4: E('fpu_rstor(c, %s);' % ea); return False
                if reg == 6: E('fpu_save(c, %s);' % ea); return False
                if reg == 7: E('W16(%s, fpu_sw(c));' % ea); return False
            if op == 0xdf:
                if reg == 0: E('FPUSH((f80)(int16_t)R16(%s));' % ea); return False
                if reg == 2: E('W16(%s, (uint16_t)fpu_ist16(ST(0)));' % ea); return False
                if reg == 3: E('W16(%s, (uint16_t)fpu_ist16(ST(0))); FPOP();' % ea); return False
                if reg == 5: E('FPUSH((f80)(int64_t)R64(%s));' % ea); return False
                if reg == 7: E('W64(%s, (uint64_t)fpu_ist64(ST(0))); FPOP();' % ea); return False
            raise Unsupported('x87 memory form %02x /%d' % (op, reg))
        # register forms
        si = 'ST(%d)' % rm
        if op == 0xd8:
            arith_st0(ARITH_OPS[reg], si)
            return False
        if op in (0xdc, 0xde):
            pop = ' FPOP();' if op == 0xde else ''
            if op == 0xde and reg == 3 and rm == 1:
                E('fpu_com(c, ST(0), ST(1)); FPOP(); FPOP();')
                return False
            k = ARITH_OPS[reg]
            if k == 'add': E('%s = %s + ST(0);%s' % (si, si, pop))
            elif k == 'mul': E('%s = %s * ST(0);%s' % (si, si, pop))
            elif k == 'sub': E('%s = ST(0) - %s;%s' % (si, si, pop))      # E0+i: fsubr(p) st(i),st0
            elif k == 'subr': E('%s = %s - ST(0);%s' % (si, si, pop))     # E8+i: fsub(p) st(i),st0
            elif k == 'div': E('%s = ST(0) / %s;%s' % (si, si, pop))      # F0+i: fdivr(p)
            elif k == 'divr': E('%s = %s / ST(0);%s' % (si, si, pop))     # F8+i: fdiv(p)
            elif k == 'com': E('fpu_com(c, ST(0), %s);%s' % (si, pop))
            elif k == 'comp': E('fpu_com(c, ST(0), %s); FPOP();%s' % (si, pop))
            return False
        if op == 0xd9:
            if reg == 0: E('{ f80 t = %s; FPUSH(t); }' % si); return False
            if reg == 1: E('{ f80 t = ST(0); ST(0) = %s; %s = t; }' % (si, si)); return False
            if modrm == 0xd0: return False
            simple = {0xe0: 'ST(0) = -ST(0);', 0xe1: 'ST(0) = fpu_fabs(ST(0));',
                      0xe4: 'fpu_com(c, ST(0), 0.0L);', 0xe5: 'fpu_xam(c);',
                      0xe8: 'fpu_ldconst(c, 0);', 0xe9: 'fpu_ldconst(c, 1);', 0xea: 'fpu_ldconst(c, 2);',
                      0xeb: 'fpu_ldconst(c, 3);', 0xec: 'fpu_ldconst(c, 4);', 0xed: 'fpu_ldconst(c, 5);',
                      0xee: 'FPUSH(0.0L);', 0xf0: 'fpu_f2xm1(c);', 0xf1: 'fpu_fyl2x(c);',
                      0xf2: 'fpu_fptan(c);', 0xf3: 'fpu_fpatan(c);', 0xf4: 'fpu_fxtract(c);',
                      0xf5: 'fpu_fprem1(c);', 0xf6: 'c->ftop = (c->ftop - 1) & 7;',
                      0xf7: 'c->ftop = (c->ftop + 1) & 7;', 0xf8: 'fpu_fprem(c);',
                      0xf9: 'fpu_fyl2xp1(c);', 0xfa: 'ST(0) = fpu_fsqrt(ST(0));', 0xfb: 'fpu_fsincos(c);',
                      0xfc: 'ST(0) = fpu_frndint(ST(0));', 0xfd: 'fpu_fscale(c);', 0xfe: 'fpu_fsin(c);',
                      0xff: 'fpu_fcos(c);'}
            if modrm in simple:
                E(simple[modrm])
                return False
        if op == 0xda and modrm == 0xe9:
            E('fpu_com(c, ST(0), ST(1)); FPOP(); FPOP();')
            return False
        if op == 0xdb:
            if modrm == 0xe2: E('fpu_clex(c);'); return False
            if modrm == 0xe3: E('fpu_init(c);'); return False
        if op == 0xdd:
            if reg == 0: E('/* ffree */'); return False
            if reg == 2: E('%s = ST(0);' % si); return False
            if reg == 3: E('%s = ST(0); FPOP();' % si); return False
            if reg == 4: E('fpu_com(c, ST(0), %s);' % si); return False
            if reg == 5: E('fpu_com(c, ST(0), %s); FPOP();' % si); return False
        if op == 0xdf and modrm == 0xe0:
            E('eax = (eax & 0xffff0000u) | fpu_sw(c);')
            return False
        raise Unsupported('x87 register form %02x %02x' % (op, modrm))


# ---------------------------------------------------------------------- whole program
class Program:
    def __init__(self, pe, funcs, noreturn, iat, noreturn_imports, nonlocal_jumps):
        self.pe = pe
        self.funcs = funcs
        self.noreturn = noreturn
        self.iat = iat                      # iat va -> (dll, name)
        self.noreturn_imports = set(noreturn_imports)
        self.nonlocal_jumps = set(nonlocal_jumps)
        self.tables = []                    # movable fixed-size tables (builds/*.py TABLES)
        self.hooks = {}                     # function -> name of a runtime hook run at its entry
        self.inline = {}                    # function -> kind, for runtime helpers done in place
        self.limits = {}                    # insn address -> table whose capacity it checks
        self.consts = {}                    # insn address -> {literal: name} (builds/*.py CONSTS)
        self.entry_flags = {}               # function -> flags it reads from its caller
        self.fpu_delta = {}                 # function -> x87 stack effect (tools/fpustack.py)
        self.fpu_final = False              # generation (True) or the effects fixpoint
        self.ret_flags = {}                 # function -> flags its callers read after it returns

    def flag_interface(self, log=print):
        """Fixpoint over the whole program for flags passed between functions (the CRT's
        maths helpers return ZF to their callers, and some entries read the caller's flags)."""
        for rnd in range(20):
            changed = False
            for e, f in self.funcs.items():
                g = FuncGen(self, f)
                try:
                    g.analyse()
                except Unsupported:
                    continue
                if g.entry_live != self.entry_flags.get(e, 0):
                    self.entry_flags[e] = g.entry_live
                    changed = True
                for a, need in g.after_call.items():
                    t = g.call_target[a]
                    if need & ~self.ret_flags.get(t, 0):
                        self.ret_flags[t] = self.ret_flags.get(t, 0) | need
                        changed = True
                mine = self.ret_flags.get(e, 0)
                if mine:
                    for t in list(f.tails.values()) + list(f.fall_tails.values()):
                        if mine & ~self.ret_flags.get(t, 0):
                            self.ret_flags[t] = self.ret_flags.get(t, 0) | mine
                            changed = True
            if not changed:
                break
        for e, v in sorted(self.entry_flags.items()):
            if v:
                log('  function %08x reads flags %02x from its caller' % (e, v))
        for e, v in sorted(self.ret_flags.items()):
            if v:
                log('  function %08x returns flags %02x' % (e, v))

    def fpu_interface(self, log=print):
        """Fixpoint over the whole program for each function's x87 stack effect."""
        gens = {}
        for e, f in self.funcs.items():
            g = FuncGen(self, f)
            try:
                g.gen()
            except Unsupported:
                continue
            gens[e] = g
        # optimistic start (every function leaves the depth as it was), so recursion settles;
        # a function whose effect cannot be found, or does not settle, gets None, which spreads
        # to its callers
        self.fpu_delta = {e: 0 for e in gens}
        last = set()
        for rnd in range(40):
            changed = set()
            for e, g in gens.items():
                if self.fpu_delta.get(e, 0) is None:
                    continue
                g.fpu_plan(g.fpu_body)
                if self.fpu_delta.get(e) != g.fpu_delta:
                    self.fpu_delta[e] = g.fpu_delta
                    changed.add(e)
            if not changed:
                break
            last = changed
        else:
            for e in last:
                self.fpu_delta[e] = None
            while True:                     # let None reach every caller
                spread = False
                for e, g in gens.items():
                    if self.fpu_delta.get(e) is None:
                        continue
                    g.fpu_plan(g.fpu_body)
                    if g.fpu_delta != self.fpu_delta[e]:
                        self.fpu_delta[e] = g.fpu_delta
                        spread = True
                if not spread:
                    break
        self.fpu_delta = {e: v for e, v in self.fpu_delta.items() if v is not None}
        self.fpu_final = True
        n = sum(1 for g in gens.values() if g.fpu_uses)
        k = sum(1 for e, g in gens.items() if g.fpu_uses and g.fpu_plan(g.fpu_body) is not None)
        log('  x87 stack: %d of %d functions using it keep it in locals; %d with a known stack effect'
            % (k, n, len(self.fpu_delta)))
        import os
        for w in os.environ.get('FPU_WHY', '').split(','):
            if w and int(w, 16) in gens:
                g = gens[int(w, 16)]
                g.fpu_plan(g.fpu_body)
                log('  x87 stack of %s: %s' % (w, g.fpu_why or 'in locals'))

    def set_tables(self, tables):
        for t in tables:
            t = dict(t)
            t['size'] = t['entry'] * t['count']
            t['funcs'] = set(t['funcs']) if t.get('funcs') else None     # None: every function
            self.tables.append(t)
            for a in t.get('limits', []):
                self.limits[a] = t

    def table_for(self, func, v):
        for t in self.tables:
            if (t['funcs'] is None or func in t['funcs']) and t['base'] <= v < t['base'] + t['size']:
                return t
        return None

    def set_consts(self, consts):
        for name, d in consts.items():
            for a in d['insns']:
                self.consts.setdefault(a, {})[d['value']] = name

    def const_at(self, addr, v):
        return self.consts.get(addr, {}).get(v)

    def limit_at(self, addr, v):
        t = self.limits.get(addr)
        if t and v == t['count']:
            return t['name']
        return None

    def fname(self, a):
        return 'F_%08x' % a

    def iat_name(self, va):
        x = self.iat.get(va)
        return x[1] if x else None

    def import_fn(self, va):
        dll, name = self.iat[va]
        return import_symbol(dll, name)


def import_symbol(dll, name):
    d = dll.split('.')[0].upper()
    n = re.sub(r'[^A-Za-z0-9]', '_', name.lstrip('#'))
    return 'w_%s_%s' % (d, n)
