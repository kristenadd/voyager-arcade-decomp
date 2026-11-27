"""A dynamically linked i386 ELF executable (trek, stvsnd, stvserv) as guest-addressed bytes.

`data` covers every PT_LOAD segment from the lowest one's address to the
highest one's end, .bss zero-filled. Imports are the GOT slots named by the
.rel.plt (R_386_JUMP_SLOT) and .rel.got (R_386_GLOB_DAT) relocations; each
PLT entry (`jmp [slot]`) is an ordinary function the translator translates,
and its slot holds an import handler's address at run time.
"""
import os
import struct

R_386_GLOB_DAT, R_386_JUMP_SLOT, R_386_COPY = 6, 7, 5


class Section:
    def __init__(self, name, va, size, file_backed, executable):
        self.name, self.va, self.size, self.file_backed, self._x = name, va, size, file_backed, executable

    @property
    def end(self):
        return self.va + self.size

    @property
    def executable(self):
        return self._x

    def __repr__(self):
        return '%s %08x..%08x' % (self.name, self.va, self.end)


class Image:
    def __init__(self, path, program=None):
        with open(path, 'rb') as f:
            raw = f.read()
        if raw[:4] != b'\x7fELF' or raw[4] != 1 or struct.unpack_from('<H', raw, 18)[0] != 3:
            raise ValueError('%s is not an i386 ELF32 executable' % path)
        self.raw = raw
        self.edition = program or os.path.basename(path).split('.')[0]
        self.entry, phoff, shoff = struct.unpack_from('<III', raw, 24)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', raw, 42)
        loads = []
        for i in range(phnum):
            p_type, off, va, pa, filesz, memsz, flags, align = struct.unpack_from('<8I', raw, phoff + i * phentsize)
            if p_type == 1:
                loads.append((va, off, filesz, memsz, flags))
        self.base = min(l[0] for l in loads) & ~0xFFF
        end = max(l[0] + l[3] for l in loads)
        buf = bytearray(end - self.base)
        for va, off, filesz, memsz, flags in loads:
            buf[va - self.base:va - self.base + filesz] = raw[off:off + filesz]
        self.data = bytes(buf)
        self.loads = loads
        sh = [struct.unpack_from('<10I', raw, shoff + i * shentsize) for i in range(shnum)]
        names = sh[shstrndx]

        def nm(o):
            s = names[4] + o
            return raw[s:raw.index(b'\0', s)].decode()
        self.sh = {nm(s[0]): s for s in sh}
        self.sh_list = sh
        self.sections = []
        for s in sh:
            name, typ, flags, addr, off, size = nm(s[0]), s[1], s[2], s[3], s[4], s[5]
            if addr and flags & 2:  # SHF_ALLOC
                self.sections.append(Section(name, addr, size, 0 if typ == 8 else size, bool(flags & 4)))
        self.code_sections = [s for s in self.sections if s.executable]
        first = min(s.va for s in self.code_sections)
        last = max(s.va + s.file_backed for s in self.code_sections)
        self.text = Section('.text', first, last - first, last - first, True)
        self._imports = None

    def section_of(self, va):
        for s in self.sections:
            if s.va <= va < s.end:
                return s
        return None

    def contains(self, va, n=1):
        return self.base <= va and va + n <= self.base + len(self.data)

    def bytes(self, va, n):
        o = va - self.base
        return self.data[o:o + n]

    def u8(self, va):
        return self.data[va - self.base]

    def u16(self, va):
        return struct.unpack_from('<H', self.data, va - self.base)[0]

    def u32(self, va):
        return struct.unpack_from('<I', self.data, va - self.base)[0]

    def i32(self, va):
        return struct.unpack_from('<i', self.data, va - self.base)[0]

    def cstr(self, va, limit=512):
        o = va - self.base
        end = self.data.find(b'\0', o, o + limit)
        return self.data[o:end if end >= 0 else o + limit]

    def in_text(self, va):
        for s in self.code_sections:
            if s.va <= va < s.va + s.file_backed:
                return True
        return False

    def _dynsym(self):
        ds, dstr = self.sh['.dynsym'], self.sh['.dynstr']
        out = []
        for i in range(ds[5] // 16):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.raw, ds[4] + i * 16)
            s = dstr[4] + name
            out.append((self.raw[s:self.raw.index(b'\0', s)].decode(), value, size, info, shndx))
        return out

    def relocations(self):
        """[(type, slot va, symbol name, symbol value, symbol size)] from every REL section."""
        syms = self._dynsym()
        out = []
        for name, s in self.sh.items():
            if s[1] != 9:  # SHT_REL
                continue
            for i in range(s[5] // 8):
                off, info = struct.unpack_from('<II', self.raw, s[4] + i * 8)
                sym = syms[info >> 8]
                out.append((info & 0xFF, off, sym[0], sym[1], sym[2]))
        return out

    def imports(self):
        """{GOT slot va: (library, name)} for JUMP_SLOT and GLOB_DAT relocations."""
        if self._imports is None:
            self._imports = {}
            for typ, slot, name, value, size in self.relocations():
                if typ in (R_386_JUMP_SLOT, R_386_GLOB_DAT):
                    self._imports[slot] = ('so', name)
        return self._imports

    def plt_entries(self):
        """{PLT entry va: GOT slot} for each `jmp dword ptr [slot]` entry after PLT0."""
        plt = self.sh.get('.plt')
        out = {}
        if not plt:
            return out
        va = plt[3] + 16
        while va + 16 <= plt[3] + plt[5]:
            if self.bytes(va, 2) == b'\xff\x25':
                out[va] = self.u32(va + 2)
            va += 16
        return out

    def load_config(self):
        return None
