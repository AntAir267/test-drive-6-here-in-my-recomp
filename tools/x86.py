"""Capstone wrapper: decodes 32-bit x86 into small plain-Python records.

Operands:
  ('r', name, size)                       register (eax, ax, al, ah, st0..st7, ...)
  ('i', value, size)                      immediate, masked to its size
  ('m', size, seg, base, index, scale, disp)   memory; seg/base/index are names or None
"""
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM

_md = Cs(CS_ARCH_X86, CS_MODE_32)
_md.detail = True


class Insn:
    __slots__ = ('addr', 'size', 'mn', 'ops', 'raw', 'text', 'prefixes')

    def __init__(self, addr, size, mn, ops, raw, text, prefixes):
        self.addr, self.size, self.mn, self.ops = addr, size, mn, ops
        self.raw, self.text, self.prefixes = raw, text, prefixes

    @property
    def next(self):
        return self.addr + self.size

    def __repr__(self):
        return '%08x: %s' % (self.addr, self.text)


def _reg(r):
    return _md.reg_name(r) if r else None


def decode(code, addr):
    """Decode one instruction from bytes `code` (starting at addr). None if invalid."""
    for ci in _md.disasm(code, addr, 1):
        ops = []
        for op in ci.operands:
            if op.type == X86_OP_REG:
                ops.append(('r', _reg(op.reg), op.size))
            elif op.type == X86_OP_IMM:
                sz = op.size or 4
                ops.append(('i', op.imm & ((1 << (8 * sz)) - 1), sz))
            elif op.type == X86_OP_MEM:
                m = op.mem
                ops.append(('m', op.size, _reg(m.segment), _reg(m.base), _reg(m.index), m.scale,
                            m.disp & 0xffffffff))
            else:
                return None
        return Insn(ci.address, ci.size, ci.mnemonic, ops, bytes(ci.bytes),
                    (ci.mnemonic + ' ' + ci.op_str).strip(), bytes(p for p in ci.prefix if p))
    return None


class Decoder:
    """Cached decoding of a loaded PE image."""

    def __init__(self, pe):
        self.pe = pe
        self.cache = {}
        t = pe.section('.text')
        self.text_lo = pe.base + t['va']
        self.text_hi = self.text_lo + t['vsize']

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def at(self, a):
        i = self.cache.get(a)
        if i is None and a not in self.cache:
            if not self.in_text(a):
                i = None
            else:
                i = decode(self.pe.bytes(a, 16), a)
            self.cache[a] = i
        return i
