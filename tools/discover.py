"""Code discovery for a 32-bit MSVC Windows program.

Finds every function (entry point, call targets, code pointers held in data or pushed as
immediates, per-build extras), the instructions of each one, its switch jump tables and the
places where control passes to another function without a call (tail calls, fall-through into
the next function).
"""
import struct
from collections import defaultdict

TERMINATORS = {'ret', 'retf', 'iret', 'iretd', 'hlt', 'int3', 'ud2'}
COND_JUMPS = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp',
              'jl', 'jge', 'jle', 'jg', 'jcxz', 'jecxz', 'loop', 'loope', 'loopne'}
# Never seen in real code of this kind of program: their presence means we decoded data.
WEIRD = {'in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'hlt', 'cli', 'sti',
         'iret', 'iretd', 'into', 'bound', 'arpl', 'les', 'lds', 'lss', 'lfs', 'lgs', 'lgdt',
         'lidt', 'sgdt', 'sidt', 'sldt', 'lldt', 'str', 'ltr', 'lar', 'lsl', 'clts', 'invd',
         'wbinvd', 'rdmsr', 'wrmsr', 'sysenter', 'sysexit', 'syscall', 'sysret', 'aaa', 'aas',
         'daa', 'das', 'aam', 'aad', 'salc', 'femms', 'retf', 'lcall', 'ljmp', 'int1', 'icebp',
         'ud2', 'verr', 'verw', 'smsw', 'lmsw', 'invlpg', 'rsm', 'cmpxchg8b', 'pi2fd', 'pi2fw',
         'enter', 'xlatb', 'ud0', 'ud1', 'fsetpm', 'fneni', 'fndisi'}


def is_weird(i):
    m = i.mn
    if m in WEIRD:
        return True
    if m == 'int':
        return True
    if m.startswith('p') and m not in ('push', 'pop', 'pushal', 'popal', 'pushaw', 'popaw', 'pushfd', 'popfd',
                                       'pause'):
        return True                      # MMX / 3DNow!: none in this program's real code
    for op in i.ops:
        if op[0] == 'r' and op[1] in ('cs', 'ds', 'es', 'ss', 'fs', 'gs', 'cr0', 'dr0'):
            return True
        if op[0] == 'm' and op[2] not in (None, 'fs', 'ds', 'ss', 'es'):
            return True
    return False


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}           # addr -> Insn
        self.jtables = {}         # jmp addr -> [targets]
        self.jt_info = {}         # jmp addr -> (table, count, how)
        self.tails = {}           # insn addr -> function entry reached without a call
        self.calls = set()        # direct call targets
        self.code_imms = set()    # immediates pointing into .text
        self.errors = []          # (addr, message)
        self.has_ret = False
        self.indirect_jumps = set()   # jmp reg/mem that is not a jump table: a tail call
        self.import_tails = {}    # insn addr -> iat address (jmp [__imp_X])
        self.fall_tails = {}      # insn addr -> function entry its fall-through runs into


class Discovery:
    def __init__(self, pe, dec, imports_by_iat, noreturn_imports=(), extra_entries=(),
                 jt_override=None, not_entries=()):
        self.pe, self.dec = pe, dec
        self.iat = imports_by_iat
        self.noreturn_imports = set(noreturn_imports)
        self.extra = set(extra_entries)
        self.jt_override = jt_override or {}
        self.not_entries = set(not_entries)
        self.noreturn = set()
        self.log = []

    # ------------------------------------------------------------------ jump tables
    def _prev(self, body_ends, a):
        return body_ends.get(a)

    def jump_table(self, f, i, body_ends):
        """jmp dword ptr [idx*4 + table]: returns (table, count, targets, how) or None."""
        if i.addr in self.jt_override:
            table, count = self.jt_override[i.addr]
            return table, count, self._read_table(table, count), 'config'
        op = i.ops[0]
        if op[0] != 'm' or op[3] is not None or op[4] is None or op[5] != 4:
            return None
        idx, table = op[4], op[6]
        if not self.dec.in_text(table) and self.pe.section_at(table) is None:
            return None
        # walk back through straight-line code for the bound check
        a, steps, byte_tab = i.addr, 0, None
        want = idx
        while steps < 12:
            p = self._prev(body_ends, a)
            if p is None or p.mn in ('jmp', 'ret', 'int3'):
                break
            steps += 1
            a = p.addr
            m = p.mn
            if byte_tab is None and m in ('mov', 'movzx') and len(p.ops) == 2 and p.ops[1][0] == 'm' \
                    and p.ops[1][1] == 1 and p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want):
                bop = p.ops[1]
                if bop[3] is not None and bop[4] is None:
                    byte_tab = bop[6]
                    want = bop[3]
                    continue
                if bop[3] is None and bop[4] is not None and bop[5] == 1:
                    byte_tab = bop[6]
                    want = bop[4]
                    continue
            if m == 'cmp' and p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want) and p.ops[1][0] == 'i':
                n = p.ops[1][1] + 1
                if byte_tab is not None:
                    idxs = self.pe.bytes(byte_tab, n)
                    count = max(idxs) + 1
                    return table, count, self._read_table(table, count), 'byte table %x[%d]' % (byte_tab, n)
                return table, n, self._read_table(table, n), 'cmp'
            # anything else writing the register we track ends the search
            if p.ops and p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want) and m not in ('cmp', 'test'):
                if m in ('mov', 'movzx', 'movsx') and p.ops[1][0] == 'r':
                    want = p.ops[1][1]
                    continue
                if m in ('add', 'sub', 'dec', 'inc', 'and', 'xor'):
                    if m in ('and',) and p.ops[1][0] == 'i':
                        n = p.ops[1][1] + 1
                        return table, n, self._read_table(table, n), 'and mask'
                    if m == 'xor' and byte_tab is not None:
                        continue
                    continue
                break
        # no bound found: read plausible entries
        targets, n = [], 0
        while n < 1024:
            v = self.pe.u32(table + 4 * n)
            if not self.dec.in_text(v):
                break
            n += 1
        if n == 0:
            return None
        self.log.append('jump table at %08x: no bound found, %d entries by scan' % (i.addr, n))
        return table, n, self._read_table(table, n), 'scan'

    def _read_table(self, table, n):
        return [self.pe.u32(table + 4 * k) for k in range(n)]

    # ------------------------------------------------------------------ one function
    def traverse(self, entry, known):
        dec = self.dec
        f = Func(entry)
        body_ends = {}
        todo = [entry]
        while todo:
            a = todo.pop()
            if a in f.insns:
                continue
            i = dec.at(a)
            if i is None:
                f.errors.append((a, 'cannot decode'))
                continue
            f.insns[a] = i
            body_ends[i.next] = i
            m = i.mn
            nxt = i.next
            succ = []
            if is_weird(i):
                f.errors.append((a, 'unexpected instruction ' + i.text))
            if m in ('ret', 'retn'):
                f.has_ret = True
            elif m in TERMINATORS:
                pass
            elif m == 'jmp':
                op = i.ops[0]
                if op[0] == 'i':
                    t = op[1]
                    if t != entry and t in known:
                        f.tails[a] = t
                    elif not dec.in_text(t):
                        f.errors.append((a, 'jump outside code %08x' % t))
                    else:
                        succ.append(t)
                elif op[0] == 'm' and op[3] is None and op[4] is None and op[6] in self.iat:
                    f.import_tails[a] = op[6]
                else:
                    jt = self.jump_table(f, i, body_ends) if op[0] == 'm' else None
                    if jt and all(t in known and t != entry for t in jt[2]):
                        # a table of functions (also called through elsewhere): a tail call
                        f.indirect_jumps.add(a)
                        jt = None
                    elif jt:
                        table, count, targets, how = jt
                        f.jtables[a] = targets
                        f.jt_info[a] = (table, count, how)
                        if how == 'scan' or how == 'and mask':
                            # entries an index can never select may be neighbouring data
                            targets = [t for t in targets if dec.in_text(t) and dec.at(t)]
                            f.jtables[a] = targets
                        for t in targets:
                            if not dec.in_text(t):
                                f.errors.append((a, 'jump table target outside code %08x' % t))
                            elif t in known and t != entry:
                                f.errors.append((a, 'jump table target is a function %08x' % t))
                            else:
                                succ.append(t)
                    else:
                        f.indirect_jumps.add(a)
            elif m in COND_JUMPS:
                t = i.ops[0][1]
                if t != entry and t in known:
                    f.tails[a] = t
                elif not dec.in_text(t):
                    f.errors.append((a, 'jump outside code %08x' % t))
                else:
                    succ.append(t)
                succ.append(nxt)
            elif m == 'call':
                op = i.ops[0]
                returns = True
                if op[0] == 'i':
                    t = op[1]
                    if dec.in_text(t):
                        f.calls.add(t)
                        if t in self.noreturn:
                            returns = False
                    else:
                        f.errors.append((a, 'call outside code %08x' % t))
                elif op[0] == 'm' and op[3] is None and op[4] is None and op[6] in self.iat:
                    if self.iat[op[6]] in self.noreturn_imports:
                        returns = False
                if returns:
                    succ.append(nxt)
            else:
                succ.append(nxt)
            # code addresses used as immediates (callbacks, handlers; also MSVC's branch-free
            # selects like `neg; sbb; not; and reg, offset func`)
            if m not in ('call', 'jmp') and m not in COND_JUMPS:
                for op in i.ops:
                    if op[0] == 'i' and op[2] == 4 and dec.in_text(op[1]):
                        f.code_imms.add(op[1])
            for s in succ:
                if s != entry and s in known:
                    if s == nxt:
                        f.fall_tails[a] = s     # runs on into the next function
                    else:
                        f.tails[a] = s
                else:
                    todo.append(s)
        return f

    def acceptable_candidate(self, f):
        if f.errors or not f.insns:
            return False
        return self.looks_like_start(f.entry)

    def looks_like_start(self, a):
        """A code pointer found in data must sit where a function can begin: 16-byte aligned
        (MSVC's function alignment) or right after padding or an instruction that does not
        fall through. ASCII text in data often forms .text addresses; this rejects those."""
        if a % 16 == 0:
            return True
        b = self.pe.u8(a - 1)
        if b in (0xcc, 0x90):
            return True
        for back in range(1, 8):
            i = self.dec.at(a - back)
            if i is not None and i.next == a and i.mn in ('ret', 'jmp'):
                return True
        return False

    # ------------------------------------------------------------------ whole program
    def run(self, seeds, ptr_candidates):
        """seeds: trusted entries. ptr_candidates: code addresses found in data (validated)."""
        known = set(seeds) | self.extra
        known -= self.not_entries
        rejected = {}
        for rnd in range(20):
            funcs = {}
            work = sorted(known)
            pending_cands = set(ptr_candidates)
            while work:
                new = []
                for e in work:
                    if e in funcs:
                        continue
                    f = self.traverse(e, known)
                    funcs[e] = f
                    for t in f.calls:
                        if t not in known and t not in self.not_entries:
                            known.add(t)
                            new.append(t)
                    pending_cands |= f.code_imms
                # validate pointer candidates once the trusted set settles
                if not new:
                    interior = self._interior(funcs)
                    occupied = set(a for f in funcs.values() for a in f.insns)
                    for cand in sorted(pending_cands - known - set(rejected) - self.not_entries):
                        g = self.traverse(cand, known)
                        if cand in occupied and not self.after_terminator(cand):
                            # code pointers in data point at function starts, never into the
                            # middle of another function's code
                            rejected[cand] = ['inside the code of another function']
                        elif self.acceptable_candidate(g) and cand not in interior:
                            known.add(cand)
                            new.append(cand)
                            occupied.update(g.insns)
                        else:
                            rejected[cand] = g.errors[:1] or ['inside an instruction']
                    pending_cands = set()
                work = new
            # bodies depend on the full entry set: redo them all
            funcs = {e: self.traverse(e, known) for e in sorted(known)}
            nr = self._noreturn(funcs)
            if nr == self.noreturn:
                break
            self.noreturn = nr
        self.rejected = rejected
        return funcs

    def after_terminator(self, a):
        """True if the code before a cannot fall into it (a function another one jumps to)."""
        if self.pe.u8(a - 1) == 0xcc:
            return True
        for back in range(1, 8):
            i = self.dec.at(a - back)
            if i is not None and i.next == a and i.mn in ('ret', 'jmp', 'int3'):
                return True
        return False

    def _interior(self, funcs):
        """Addresses inside (not at the start of) instructions of the known functions."""
        starts, inside = set(), set()
        for f in funcs.values():
            for a, i in f.insns.items():
                starts.add(a)
                inside.update(range(a + 1, a + i.size))
        return inside - starts

    def _noreturn(self, funcs):
        returns = set()
        changed = True
        while changed:
            changed = False
            for e, f in funcs.items():
                if e in returns:
                    continue
                if f.has_ret or f.indirect_jumps or f.import_tails or f.errors or \
                        any(t in returns for t in f.tails.values()) or \
                        any(t in returns for t in f.fall_tails.values()):
                    returns.add(e)
                    changed = True
        return set(funcs) - returns


def _same_reg(a, b):
    fam = {'eax': 'a', 'ax': 'a', 'al': 'a', 'ah': 'a', 'ebx': 'b', 'bx': 'b', 'bl': 'b', 'bh': 'b',
           'ecx': 'c', 'cx': 'c', 'cl': 'c', 'ch': 'c', 'edx': 'd', 'dx': 'd', 'dl': 'd', 'dh': 'd',
           'esi': 'si', 'si': 'si', 'edi': 'di', 'di': 'di', 'ebp': 'bp', 'bp': 'bp', 'esp': 'sp'}
    return fam.get(a, a) == fam.get(b, b)


def data_code_pointers(pe, dec):
    """Aligned dwords in the initialised data sections that point into .text."""
    out = set()
    for s in pe.sections:
        if s['name'] in ('.rdata', '.data'):
            lo = pe.base + s['va']
            n = min(s['vsize'], s['rsize'])
            for off in range(0, n - 3, 4):
                v = struct.unpack_from('<I', pe.image, s['va'] + off)[0]
                if dec.in_text(v):
                    out.add(v)
    return out
