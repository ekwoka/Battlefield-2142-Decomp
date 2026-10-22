"""Minimal PE32 reader: section mapping and virtual-address reads."""
import struct


class PE:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        count = struct.unpack_from("<H", d, pe + 6)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        self.image_base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
        self.sections = []
        for i in range(count):
            o = pe + 24 + opt_size + 40 * i
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, o)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append((name.rstrip(b"\0").decode(), va, max(vsize, rsize), raw, rsize, chars))

    def offset(self, va):
        rva = va - self.image_base
        for _, sva, size, raw, rsize, _ in self.sections:
            if sva <= rva < sva + size:
                return raw + rva - sva if rva - sva < rsize else None
        return None

    def is_code(self, va):
        rva = va - self.image_base
        return any(sva <= rva < sva + size and chars & 0x20000000
                   for _, sva, size, _, _, chars in self.sections)

    def u32(self, va):
        o = self.offset(va)
        return None if o is None else struct.unpack_from("<I", self.data, o)[0]

    def cstr(self, va, limit=256):
        o = self.offset(va)
        return None if o is None else self.data[o:o + limit].split(b"\0")[0].decode("latin-1")
