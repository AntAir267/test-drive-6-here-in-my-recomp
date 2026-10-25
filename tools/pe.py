"""Minimal PE32 reader: headers, sections, imports, raw image."""
import struct


class PE:
    def __init__(self, path):
        self.path = path
        d = self.data = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('not a PE file')
        self.machine, nsec, self.timestamp, _, _, optsz, self.characteristics = \
            struct.unpack_from('<HHIIIHH', d, pe + 4)
        o = pe + 24
        if struct.unpack_from('<H', d, o)[0] != 0x10b:
            raise ValueError('not PE32')
        self.entry_rva = struct.unpack_from('<I', d, o + 16)[0]
        self.base = struct.unpack_from('<I', d, o + 28)[0]
        self.size_of_image = struct.unpack_from('<I', d, o + 56)[0]
        self.stack_reserve, self.stack_commit, self.heap_reserve, self.heap_commit = \
            struct.unpack_from('<IIII', d, o + 72)
        ndd = struct.unpack_from('<I', d, o + 92)[0]
        self.dirs = [struct.unpack_from('<II', d, o + 96 + 8 * i) for i in range(ndd)]
        self.sections = []
        so = o + optsz
        for i in range(nsec):
            nm, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, so + 40 * i)
            ch = struct.unpack_from('<I', d, so + 40 * i + 36)[0]
            self.sections.append(dict(name=nm.rstrip(b'\0').decode(), va=va, vsize=vs,
                                      raw=ra, rsize=rs, flags=ch))
        self.entry = self.base + self.entry_rva
        self.image = self._build_image()
        self.imports = self._imports()

    def _build_image(self):
        img = bytearray(self.size_of_image)
        hdr = self.sections[0]['raw'] if self.sections else 0x400
        img[:hdr] = self.data[:hdr]
        for s in self.sections:
            n = min(s['rsize'], s['vsize'] if s['vsize'] else s['rsize'])
            img[s['va']:s['va'] + n] = self.data[s['raw']:s['raw'] + n]
        return img

    def section(self, name):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None

    def section_at(self, va):
        rva = va - self.base
        for s in self.sections:
            if s['va'] <= rva < s['va'] + max(s['vsize'], s['rsize']):
                return s
        return None

    def u8(self, va):
        return self.image[va - self.base]

    def u32(self, va):
        return struct.unpack_from('<I', self.image, va - self.base)[0]

    def bytes(self, va, n):
        o = va - self.base
        return bytes(self.image[o:o + n])

    def cstr(self, va):
        o = va - self.base
        e = self.image.index(0, o)
        return self.image[o:e].decode('latin-1')

    def _imports(self):
        """[(dll, name_or_ordinal, iat_va)]"""
        out = []
        rva, size = self.dirs[1]
        if not rva:
            return out
        i = 0
        while True:
            olt, ts, fc, nm, iat = struct.unpack_from('<IIIII', self.image, rva + 20 * i)
            if not nm:
                break
            dll = self.cstr(self.base + nm)
            k = 0
            src = olt or iat
            while True:
                v = struct.unpack_from('<I', self.image, src + 4 * k)[0]
                if not v:
                    break
                name = ('#%d' % (v & 0xffff)) if v & 0x80000000 else self.cstr(self.base + v + 2)
                out.append((dll, name, self.base + iat + 4 * k))
                k += 1
            i += 1
        return out
