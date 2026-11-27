#!/usr/bin/env python3
"""Reads the STABS debug records and the symbol table of a GCC 2.95 ELF32
(trek, stvserv) and writes an inventory as JSON:

  functions: name, address, size, source file, first line, stabs descriptor,
             parameters (name, stabs type, frame offset)
  globals:   name, address, size, source file, stabs descriptor
  types:     per source file, the raw stabs type definitions (name:t(...)=...)

    python3 -I tools/stabs/stabs.py <elf> <out.json>

The output is derived from the player's binary and stays in build/.
"""
import json, struct, sys

N_GSYM, N_FUN, N_STSYM, N_LCSYM, N_SO, N_SOL, N_PSYM, N_LSYM, N_SLINE, N_RSYM, N_LBRAC, N_RBRAC = (
    0x20, 0x24, 0x26, 0x28, 0x64, 0x84, 0xa0, 0x80, 0x44, 0x40, 0xc0, 0xe0)


class Elf:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[4] != 1:
            raise SystemExit('%s: not an ELF32 file' % path)
        shoff, = struct.unpack_from('<I', d, 0x20)
        shnum, shstrndx = struct.unpack_from('<HH', d, 0x30)
        sh = [struct.unpack_from('<10I', d, shoff + i * 40) for i in range(shnum)]
        names = sh[shstrndx]
        def nm(o):
            s = names[4] + o
            return d[s:d.index(b'\0', s)].decode()
        self.sections = {nm(s[0]): s for s in sh}
        self.sh = sh

    def sect(self, name):
        s = self.sections[name]
        return self.d[s[4]:s[4] + s[5]]

    def symbols(self):
        st = self.sections['.symtab']
        strtab = self.sh[st[6]]
        out = []
        for i in range(st[5] // 16):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.d, st[4] + i * 16)
            s = strtab[4] + name
            out.append((self.d[s:self.d.index(b'\0', s)].decode('latin1'), value, size, info & 0xf, info >> 4, shndx))
        return out


def main():
    elf = Elf(sys.argv[1])
    stab = elf.sect('.stab')
    strs = elf.sect('.stabstr')
    syms = elf.symbols()
    funcs_by_name = {}
    for name, value, size, typ, bind, shndx in syms:
        if typ == 2 and value:  # STT_FUNC
            funcs_by_name.setdefault(name, []).append((value, size))
    objs = {}
    for name, value, size, typ, bind, shndx in syms:
        if typ == 1 and value:  # STT_OBJECT
            objs.setdefault(name, []).append((value, size))

    functions, globals_, types = [], [], {}
    so_dir, so = '', None
    cur = None
    strbase = 0
    for i in range(len(stab) // 12):
        strx, typ, other, desc, value = struct.unpack_from('<IBBHI', stab, i * 12)
        if typ == 0:  # header of one compilation unit's string table
            strbase += 0 if i == 0 else 0
        s = ''
        if strx:
            # Each unit's strings are relative to its own base; GNU ld merges
            # them into one table with N_UNDF entries carrying sizes.
            s = strs[hdr_base + strx: strs.index(b'\0', hdr_base + strx)].decode('latin1') if 'hdr_base' in dir() else ''
        if typ == 0:
            hdr_base = next_base if i else 0
            next_base = hdr_base + value
            continue
        if typ == N_SO:
            if s.endswith('/'):
                so_dir = s
            elif s:
                so = s
            continue
        if so is None or so.startswith('/usr') or so.startswith('..'):
            pass
        if typ == N_FUN and s:
            name, _, desc_s = s.partition(':')
            if not desc_s:
                continue
            cur = {'name': name, 'address': value, 'file': so, 'line': desc, 'stabs': desc_s, 'params': [], 'locals': []}
            cands = funcs_by_name.get(name, [])
            for a, sz in cands:
                if a == value:
                    cur['size'] = sz
            functions.append(cur)
        elif typ == N_FUN and not s and cur is not None:
            cur['end'] = cur['address'] + value
        elif typ == N_PSYM and cur is not None:
            n, _, t = s.partition(':')
            cur['params'].append({'name': n, 'stabs': t, 'offset': value - (1 << 32) if value & 0x80000000 else value})
        elif typ == N_RSYM and cur is not None and s:
            n, _, t = s.partition(':')
            if t.startswith('P'):
                cur['params'].append({'name': n, 'stabs': t, 'register': value})
        elif typ == N_LSYM and s:
            n, _, t = s.partition(':')
            if t[:1] in ('t', 'T'):
                types.setdefault(so, []).append(s)
            elif cur is not None:
                cur['locals'].append({'name': n, 'stabs': t, 'offset': value - (1 << 32) if value & 0x80000000 else value})
        elif typ in (N_GSYM, N_STSYM, N_LCSYM) and s:
            n, _, t = s.partition(':')
            g = {'name': n, 'file': so, 'stabs': t, 'static': typ != N_GSYM}
            if typ == N_GSYM:
                cand = objs.get(n, [])
                if cand:
                    g['address'], g['size'] = cand[0]
            else:
                g['address'] = value
                for a, sz in objs.get(n, []):
                    if a == value:
                        g['size'] = sz
            globals_.append(g)
    # Code without stabs (crt, libc_nonshared, the PLT): from the symbol table.
    have = {f['address'] for f in functions}
    for name, value, size, typ, bind, shndx in syms:
        if typ == 2 and value and value not in have:
            functions.append({'name': name, 'address': value, 'size': size, 'file': None, 'stabs': None, 'params': []})
            have.add(value)
    functions.sort(key=lambda f: f['address'])
    json.dump({'functions': functions, 'globals': globals_, 'types': types}, open(sys.argv[2], 'w'), indent=1)
    files = sorted({f['file'] for f in functions if f['file']})
    print('%d functions (%d with stabs), %d globals, %d source files' % (
        len(functions), sum(1 for f in functions if f['stabs']), len(globals_), len(files)))


if __name__ == '__main__':
    main()
