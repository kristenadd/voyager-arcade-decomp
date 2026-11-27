#!/usr/bin/env python3
"""Every function of a GCC 2.95 ELF (trek, stvsnd, stvserv).

The binaries are not stripped, so the function starts are known exactly: the
symbol table (STT_FUNC, global and local) plus one function per PLT entry.
Bodies come from the shared recursive-descent walker (cfg.py). Only the PLT
entries of libc functions that never return are marked no-return.

    PYTHONPATH=build/pylib python3 tools/x86tr/discover.py <elf> <out folder>
"""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from image import Image  # noqa: E402
from cfg import Walker  # noqa: E402

NORETURN_IMPORTS = {'exit', '_exit', 'abort', '__assert_fail'}


def symbol_functions(img):
    out = {}
    st = img.sh.get('.symtab')
    if not st:
        return out
    strtab = img.sh_list[st[6]]
    for i in range(st[5] // 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', img.raw, st[4] + i * 16)
        if info & 0xF not in (0, 2) or not value or not img.in_text(value):
            continue
        s = strtab[4] + name
        n = img.raw[s:img.raw.index(b'\0', s)].decode('latin1')
        # Untyped symbols are labels of hand-written assembly (ps_perf's
        # get_curr_cpu_time, the context switch helpers), except the
        # compiler's own markers.
        if info & 0xF == 0 and (n.startswith('.') or n in ('Letext', 'gcc2_compiled.', '__gnu_compiled_c') or not n):
            continue
        out.setdefault(value, n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('out')
    args = ap.parse_args()
    t0 = time.time()
    img = Image(args.elf)
    names = symbol_functions(img)
    names.setdefault(img.entry, '_start')  # a NOTYPE symbol in crt1.o
    imports = img.imports()
    plt = img.plt_entries()
    for va, slot in plt.items():
        names.setdefault(va, imports.get(slot, ('so', '?'))[1] + '@plt')
    walker = Walker(img)
    walker.function_starts.update(names)
    for va, slot in plt.items():
        if imports.get(slot, ('', ''))[1] in NORETURN_IMPORTS:
            walker.noreturn.add(va)
    # Only the libc functions GCC itself knows never return. The game's own
    # bailing() / bail() never return either, but GCC 2.95 did not know that,
    # so the code after their calls is real code and is translated too.
    functions = {va: walker.walk(va) for va in sorted(names)}
    funcs = []
    for va in sorted(functions):
        fn = functions[va]
        funcs.append({
            'entry': va, 'name': names[va], 'size': fn.size, 'insns': len(fn.insns),
            'noreturn': va in walker.noreturn, 'reachable': True,
            'plt_slot': plt.get(va),
            'callees': sorted(fn.callees), 'tail_calls': sorted(fn.tail_calls),
            'jump_tables': {str(k): {'table': j.va, 'targets': j.targets, 'bounded': j.bounded,
                                     'index_table': j.index_table} for k, j in fn.jump_tables.items()},
            'indirect_jumps': fn.indirect_jumps, 'undecodable': fn.undecodable,
        })
    stats = {
        'functions': len(funcs), 'plt_entries': len(plt),
        'instructions': sum(f['insns'] for f in funcs),
        'jump_tables': sum(len(f['jump_tables']) for f in funcs),
        'unresolved_indirect_jumps': sum(len(f['indirect_jumps']) for f in funcs if not f['plt_slot']),
        'undecodable_in_bodies': sum(len(f['undecodable']) for f in funcs),
        'noreturn_functions': sum(1 for f in funcs if f['noreturn']),
        'seconds': round(time.time() - t0, 1),
    }
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, 'functions.json'), 'w') as f:
        json.dump({'edition': img.edition, 'stats': stats, 'noreturn_imports': [], 'functions': funcs}, f)
    print(json.dumps(stats))
    for f in funcs:
        if f['indirect_jumps'] and not f['plt_slot']:
            print('  indirect jump in %s (%08x): %s' % (f['name'], f['entry'], ' '.join('%x' % v for v in f['indirect_jumps'])))
        if f['undecodable']:
            print('  undecodable in %s: %s' % (f['name'], f['undecodable']))


if __name__ == '__main__':
    main()
