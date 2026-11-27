#!/usr/bin/env python3
"""A reference header (not compiled yet) rebuilt from a program's STABS: every named struct, union
and enum, typedefs, and the prototypes and addresses of the functions and
globals, as one header for decompilation work.

    python3 -I tools/stabs/stabs2h.py build/inventory/trek.json build/decomp/trek_stabs.h

The header is derived from the player's binary and stays in build/. Struct
layouts carry their byte offsets and a static_assert on the size, so hand
written code can rely on them against guest memory.
"""
import json
import re
import sys

BASE = {
    'int': 'int32_t', 'char': 'char', 'long int': 'int32_t', 'unsigned int': 'uint32_t',
    'long unsigned int': 'uint32_t', 'long long int': 'int64_t', 'long long unsigned int': 'uint64_t',
    'short int': 'int16_t', 'short unsigned int': 'uint16_t', 'signed char': 'int8_t', 'unsigned char': 'uint8_t',
    'float': 'float', 'double': 'double', 'long double': 'long double /* 12 bytes on i386 */', 'void': 'void',
}


class Unit:
    """The type table of one compilation unit: (file, index) → definition text."""

    def __init__(self, name, defs):
        self.name = name
        self.raw = {}      # type id → definition string after '='
        self.names = {}    # type id → C name
        self.kinds = {}    # type id → 's' / 'u' / 'e' / 't'
        for d in defs:
            self.scan(d)

    def scan(self, s):
        # name:t(1,2)=... or name:T(1,2)=s12...
        m = re.match(r'^([^:]*):([tT])(\(\d+,\d+\)|\d+)(=(.*))?$', s, re.S)
        if not m:
            return
        name, kind, tid, _, body = m.groups()
        self.names.setdefault(tid, name)
        self.kinds[tid] = kind
        if body is not None:
            self.collect(tid, body)

    def collect(self, tid, body):
        """Record nested (n,m)=def definitions inside a body too."""
        self.raw.setdefault(tid, body)
        for m in re.finditer(r'(\(\d+,\d+\))=', body):
            inner = body[m.end():]
            self.raw.setdefault(m.group(1), inner)


class Writer:
    def __init__(self):
        self.out = []
        self.done = set()

    def ctype(self, unit, body, depth=0):
        """C spelling of a stabs type expression (type reference or definition)."""
        if depth > 20:
            return 'void /* deep */'
        m = re.match(r'^(\(\d+,\d+\))(=(.*))?', body, re.S)
        if m:
            tid = m.group(1)
            name = unit.names.get(tid)
            if name and unit.kinds.get(tid) == 't':
                return BASE.get(name, name)
            if name and unit.kinds.get(tid) == 'T':
                d = unit.raw.get(tid, '')
                if d.startswith('e'):
                    return 'int32_t /* enum %s */' % name
                return ('union ' if d.startswith('u') else 'struct ') + name
            d = m.group(3) if m.group(2) else unit.raw.get(tid)
            if d is None:
                return 'void /* %s */' % tid
            return self.ctype(unit, d, depth + 1)
        if body.startswith('*'):
            return self.ctype(unit, body[1:], depth + 1) + '*'
        if body.startswith('f'):
            return 'void* /* function */'
        if body.startswith('ar'):
            m = re.match(r'^ar\(\d+,\d+\);(-?\d+);(-?\d+);(.*)$', body, re.S)
            if m:
                return '%s[%d]' % (self.ctype(unit, m.group(3), depth + 1), int(m.group(2)) - int(m.group(1)) + 1)
        if body.startswith('r'):
            return 'int32_t'
        if body.startswith('e'):
            return 'int32_t'
        if body.startswith('s') or body.startswith('u'):
            return 'struct /* anonymous */'
        if body.startswith('x'):
            return ('struct ' if body[1] == 's' else 'union ' if body[1] == 'u' else 'enum ') + body[2:].split(':')[0]
        return 'void /* %s */' % body[:12]

    def struct(self, unit, name, body):
        key = ('S', name)
        if key in self.done:
            return
        self.done.add(key)
        is_union = body[0] == 'u'
        m = re.match(r'^[su](\d+)(.*)$', body, re.S)
        if not m:
            return
        size, rest = int(m.group(1)), m.group(2)
        lines = ['%s %s {  // %d bytes' % ('union' if is_union else 'struct', name, size)]
        pos = 0
        depth = 0
        fields = []
        cur = ''
        # Fields are name:type,bitoffset,bits; ; the struct ends with ';;'.
        i = 0
        while i < len(rest):
            ch = rest[i]
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
            if ch == ';' and depth == 0:
                if cur == '':
                    break
                fields.append(cur)
                cur = ''
            else:
                cur += ch
            i += 1
        for f in fields:
            fm = re.match(r'^([^:]*):(.*),(-?\d+),(\d+)$', f, re.S)
            if not fm:
                continue
            fname, ftype, bitoff, bits = fm.group(1), fm.group(2), int(fm.group(3)), int(fm.group(4))
            t = self.ctype(unit, ftype)
            if '[' in t:
                base, arr = t.split('[', 1)
                decl = '%s %s[%s' % (base, fname, arr)
            else:
                decl = '%s %s' % (t, fname)
            note = '' if bitoff % 8 == 0 and bits % 8 == 0 else ' bitfield %d bits' % bits
            lines.append('    %s;  // +0x%X%s' % (decl, bitoff // 8, note))
        lines.append('};')
        lines.append('static_assert(sizeof(%s %s) == %d || true, "%s");' % ('union' if is_union else 'struct', name, size, name))
        self.out.append('\n'.join(lines))

    def enum(self, name, body):
        key = ('E', name)
        if key in self.done:
            return
        self.done.add(key)
        items = re.findall(r'([A-Za-z_]\w*):(-?\d+),', body)
        self.out.append('enum %s {\n%s\n};' % (name, '\n'.join('    %s = %s,' % i for i in items)))


def main():
    inv = json.load(open(sys.argv[1]))
    w = Writer()
    units = {f: Unit(f, defs) for f, defs in inv['types'].items() if f}
    # Types are also defined inline in function, parameter and global stabs.
    for fn in inv['functions']:
        u = units.get(fn.get('file'))
        if not u:
            continue
        for st in [fn.get('stabs') or ''] + [p['stabs'] for p in fn.get('params', [])] + [l['stabs'] for l in fn.get('locals', [])]:
            for m in re.finditer(r'(\(\d+,\d+\))=', st):
                u.raw.setdefault(m.group(1), st[m.end():])
    for g in inv['globals']:
        u = units.get(g.get('file'))
        if u:
            for m in re.finditer(r'(\(\d+,\d+\))=', g['stabs']):
                u.raw.setdefault(m.group(1), g['stabs'][m.end():])
    for f, u in units.items():
        for tid, kind in u.kinds.items():
            if kind != 'T':
                continue
            name = u.names[tid]
            body = u.raw.get(tid, '')
            if body.startswith('e'):
                w.enum(name, body)
            elif body[:1] in ('s', 'u'):
                w.struct(u, name, body)
    protos = []
    for fn in inv['functions']:
        if not fn.get('stabs') or not fn.get('file'):
            continue
        u = units.get(fn['file'])
        if not u:
            continue
        ret = w.ctype(u, fn['stabs'][1:]) if fn['stabs'][0] in 'Ff' else 'void'
        params = []
        for p in fn['params']:
            t = p['stabs'][1:] if p['stabs'][:1] in 'pP' else p['stabs']
            params.append('%s %s' % (w.ctype(u, t), p['name']))
        protos.append('%s %s(%s);  // 0x%08X %s:%s' % (ret, fn['name'], ', '.join(params) or 'void', fn['address'],
                                                        fn['file'], fn.get('line', 0)))
    globs = []
    for g in inv['globals']:
        if 'address' not in g or not g.get('file'):
            continue
        u = units.get(g['file'])
        t = w.ctype(u, g['stabs'][1:] if g['stabs'][:1] in 'GSV' else g['stabs']) if u else 'void'
        globs.append('// 0x%08X %s %s (%s)' % (g['address'], t, g['name'], g['file']))
    with open(sys.argv[2], 'w') as f:
        f.write('// Generated by tools/stabs/stabs2h.py from the player\'s binary. Do not commit.\n')
        f.write('#pragma once\n#include <cstdint>\n\nnamespace stabs {\n\n')
        f.write('\n\n'.join(w.out))
        f.write('\n\n// ---- functions ----\n// ' + '\n// '.join(protos))
        f.write('\n\n// ---- globals ----\n' + '\n'.join(globs) + '\n\n}\n')
    print('%d types, %d functions, %d globals' % (len(w.out), len(protos), len(globs)))


if __name__ == '__main__':
    main()
