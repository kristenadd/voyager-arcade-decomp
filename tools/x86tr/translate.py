#!/usr/bin/env python3
"""x86-32 → C++ translator for the Voyager arcade programs (trek, stvsnd, stvserv).

Reads the program's ELF file and the discovery output
(discover.py), and writes C++ for the selected functions into shards under
the output folder, plus the entry table and a coverage report. The output is
derived from the player's executable: it is generated at build time and never
committed.

One C++ function per guest function, named f_<address>. Registers are
locals (written back to the Cpu around calls and at returns), flags are
computed only where a later instruction reads them, and a cmp / test feeding
a single branch / setcc / cmovcc becomes a direct comparison. The guest
stack is kept exactly as on x86: calls push the return address and ret n
pops it.

    PYTHONPATH=build/pylib python3 tools/x86tr/translate.py \\
        <game>/trek build/x86tr/trek/functions.json build/x86tr/trek/gen
"""
import re
import argparse
import bisect
import collections
import json
import os
import sys
import time


sys.path.insert(0, os.path.dirname(__file__))
from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402
from capstone import x86 as X  # noqa: E402

from image import Image  # noqa: E402
from cfg import Walker  # noqa: E402
import semantics as F  # noqa: E402
from x87 import X87Mixin  # noqa: E402
from sse import SseMixin  # noqa: E402

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
REG8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
REG8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
SEGMENTS = {'cs', 'ds', 'es', 'fs', 'gs', 'ss'}
WINDOWS_SELECTORS = {'cs': 0x23, 'ds': 0x2B, 'es': 0x2B, 'ss': 0x2B, 'fs': 0x53, 'gs': 0x2B}
UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF, 8: 0xFFFFFFFFFFFFFFFF}
STRING_OPS = {'movs', 'stos', 'lods', 'cmps', 'scas'}
FUSABLE = {'e', 'ne', 'b', 'ae', 'be', 'a', 'l', 'ge', 'le', 'g', 's', 'ns'}
FUSED = {
    'e': '{a} == {b}', 'ne': '{a} != {b}', 'b': '{a} < {b}', 'ae': '{a} >= {b}', 'be': '{a} <= {b}', 'a': '{a} > {b}',
    'l': '({s}){a} < ({s}){b}', 'ge': '({s}){a} >= ({s}){b}', 'le': '({s}){a} <= ({s}){b}', 'g': '({s}){a} > ({s}){b}',
    's': '({s})({u})({a} - {b}) < 0', 'ns': '({s})({u})({a} - {b}) >= 0',
}


Unsupported = F.Unsupported


def h(v):
    return '0x%Xu' % (v & 0xFFFFFFFF)


# Where an inlined body continues after its ret ('@' is the call site).
RET_LABEL = 'R_@'


class FunctionTranslator(X87Mixin, SseMixin):
    def __init__(self, tr, fn):
        self.tr = tr
        self.fn = fn
        self.entry = fn.entry
        self.out = []
        self.tmp = 0
        self.refs = set()
        self.fused = {}
        self.locked = False
        self.site_overrides = SITE_OVERRIDES.get(getattr(tr.img, 'edition', None), {})
        # Set when this function's body is being inlined into a caller
        # (Translator.inline_body): ret and leaving jumps continue at RET_LABEL.
        self.inlined = False

    # ---------------- operands ----------------
    def rn(self, insn, r):
        return insn.reg_name(r)

    def addr(self, insn, op):
        m = op.mem
        parts = []
        if m.base:
            b = self.rn(insn, m.base)
            if b not in REG32:
                raise Unsupported('address base %s' % b)
            parts.append(b)
        if m.index:
            i = self.rn(insn, m.index)
            if i not in REG32:
                raise Unsupported('address index %s' % i)
            parts.append('%s * %du' % (i, m.scale) if m.scale != 1 else i)
        if m.disp:
            parts.append(h(m.disp))
        seg = self.rn(insn, m.segment) if m.segment else None
        if seg == 'fs':
            parts.append('c.fs_base')
        elif seg not in (None, 'ds', 'ss', 'es', 'cs'):
            raise Unsupported('segment %s' % seg)
        if not parts:
            return '0u'
        return '(uint32_t)(%s)' % ' + '.join(parts)

    def read(self, insn, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_IMM:
            if size == 8:
                return '0x%Xull' % (op.imm & MASK[8])
            return h(op.imm & MASK[size])
        if op.type == X.X86_OP_REG:
            r = self.rn(insn, op.reg)
            if r in REG32:
                return r if size == 4 else '(%s)%s' % (UT[size], r)
            if r in REG16:
                return '(uint16_t)%s' % REG16[r]
            if r in REG8L:
                return '(uint8_t)%s' % REG8L[r]
            if r in REG8H:
                return '(uint8_t)(%s >> 8)' % REG8H[r]
            raise Unsupported('register %s' % r)
        if op.type == X.X86_OP_MEM:
            fn = {1: 'ld8', 2: 'ld16', 4: 'ld32', 8: 'ld64'}.get(size)
            if not fn:
                raise Unsupported('memory read of %d bytes' % size)
            return '%s(M, %s)' % (fn, self.addr(insn, op))
        raise Unsupported('operand type %d' % op.type)

    def write(self, insn, op, value, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            r = self.rn(insn, op.reg)
            if r in REG32:
                return '%s = (uint32_t)(%s);' % (r, value)
            if r in REG16:
                g = REG16[r]
                return '%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);' % (g, g, value)
            if r in REG8L:
                g = REG8L[r]
                return '%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);' % (g, g, value)
            if r in REG8H:
                g = REG8H[r]
                return '%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);' % (g, g, value)
            raise Unsupported('register %s' % r)
        if op.type == X.X86_OP_MEM:
            fn = {1: 'st8', 2: 'st16', 4: 'st32', 8: 'st64'}.get(size)
            if not fn:
                raise Unsupported('memory write of %d bytes' % size)
            return '%s(M, %s, (%s)(%s));' % (fn, self.addr(insn, op), UT[size], value)
        raise Unsupported('write to operand type %d' % op.type)

    def tmpvar(self, ctype, expr):
        self.tmp += 1
        name = 't%d' % self.tmp
        self.out.append('%s %s = %s;' % (ctype, name, expr))
        return name

    # ---------------- flags ----------------
    def flag_code(self, need, kind, a, b, r, size, cin=None):
        """Statements setting the flags in `need` after r = a op b (kind: add, sub, logic)."""
        bits = size * 8
        s = []
        if need & F.Z:
            s.append('zf = (%s)%s == 0;' % (UT[size], r))
        if need & F.S:
            s.append('sf = (%s >> %d) & 1;' % (r, bits - 1))
        if need & F.P:
            s.append('pf = parity(%s);' % r)
        if kind == 'logic':
            for bit in (F.C, F.O, F.A):
                if need & bit:
                    s.append('%s = 0;' % F.NAMES[bit])
            return s
        if need & F.A:
            s.append('af = ((%s ^ %s ^ %s) >> 4) & 1;' % (a, b, r))
        c = ' + %s' % cin if cin else ''
        if kind == 'add':
            if need & F.C:
                s.append('cf = ((uint64_t)%s + %s%s) >> %d;' % (a, b, c, bits))
            if need & F.O:
                s.append('of = (((%s ^ %s) & (%s ^ %s)) >> %d) & 1;' % (a, r, b, r, bits - 1))
        else:
            if need & F.C:
                s.append('cf = (uint64_t)%s < (uint64_t)%s%s;' % (a, b, c))
            if need & F.O:
                s.append('of = (((%s ^ %s) & (%s ^ %s)) >> %d) & 1;' % (a, b, a, r, bits - 1))
        return s

    def undefined_flags(self, need):
        for bit in (F.C, F.P, F.A, F.Z, F.S, F.O):
            if need & bit:
                self.out.append('%s = 0;' % F.NAMES[bit])

    def need(self, insn):
        d, _ = self.effects[insn.address]
        return self.live_out[insn.address] & d

    def cond(self, insn, cc):
        fused = self.fused.get(insn.address)
        if fused:
            size = fused
            return '(' + FUSED[cc].format(a='(%s)ta' % UT[size], b='(%s)tb' % UT[size], s=ST[size], u=UT[size]) + ')'
        return F.CONDITIONS[cc][1]

    def fusable(self, insn):
        """The next instruction reads this cmp / test's flags and nothing else does."""
        i = self.cur_idx + 1
        if i >= len(self.order) or self.order[i] != insn.address + insn.size:
            return False
        nxt = self.insns[self.order[i]]
        if nxt.address in self.branch_in:
            return False
        cc = F.cc_of(nxt.mnemonic.split()[-1])
        if cc not in FUSABLE:
            return False
        d, _ = self.effects[insn.address]
        if self.live_out[nxt.address] & d:
            return False
        return True

    # ---------------- control flow ----------------
    def sync_to(self, target):
        """The write-back before a direct call to `target`. SYNC_TO is resolved
        when the shards are written (resolve_sync_to): without the flags unless
        the target reads flags on entry. Code compiled for x86 passes no flags
        into a call, so this drops six stores per call."""
        return 'SYNC_TO(%s)' % h(target) if target in self.tr.entries else 'SYNC()'

    def call_expr(self, target):
        if target in self.tr.entries:
            self.refs.add(target)
            return 'f_%08X(c)' % target
        return 'call_indirect(c, %s)' % h(target)

    def inside(self, target):
        return target in self.insns and not (target != self.entry and target in self.tr.walker.function_starts)

    def goto_or_leave(self, target):
        if self.inside(target):
            self.targets.add(target)
            # A backward jump closes a loop: another guest thread may be owed
            # the CPU there (threads.cpp preempts only at these points).
            if target <= self.order[self.cur_idx]:
                return 'GUEST_PREEMPT(); goto L_%08X;' % target
            return 'goto L_%08X;' % target
        if self.inlined:  # a tail jump: the target's ret is ours
            return 'SYNC(); %s; RELOAD(); goto %s;' % (self.call_expr(target), RET_LABEL)
        return 'SYNC(); %s; return;' % self.call_expr(target)

    # ---------------- driver ----------------
    def decode(self):
        cs = self.tr.cs
        img = self.tr.img
        self.insns = {}
        for va, lite in self.fn.insns.items():
            self.insns[va] = next(cs.disasm(img.bytes(va, lite.size), va, 1))
        self.order = sorted(self.insns)

    def translate(self):
        self.decode()
        self.targets = {self.entry}
        self.liveness()
        blocks = []
        self.trapped = []
        for idx, va in enumerate(self.order):
            insn = self.insns[va]
            self.out = []
            self.cur_idx = idx
            site = self.site_overrides.get(va)
            try:
                if site:
                    self.out = [site[1]]
                    # Translated functions the override calls are declared like any call's.
                    for t in re.findall(r'\bf_([0-9A-F]{8})\(c\)', site[1]):
                        self.refs.add(int(t, 16))
                else:
                    self.emit(insn)
            except Unsupported as e:
                # Fails closed only if reached: most are behind CPU feature checks
                # (SSE4 / AVX paths in the C runtime) that the reported cpuid rules out.
                self.trapped.append((va, str(e)))
                self.out = ['SYNC(); trap(c, %s, "unsupported instruction: %s");' % (
                    h(va), ('%s %s' % (insn.mnemonic, insn.op_str)).replace('"', "'"))]
            if self.falls_through(insn):
                nxt = va + insn.size
                if idx + 1 >= len(self.order) or self.order[idx + 1] != nxt:
                    self.out.append(self.goto_or_leave(nxt))
            blocks.append((va, insn, self.out))
        if self.inlined:
            # The body alone, labels made unique per call site ('@').
            lines = [] if self.order[0] == self.entry else ['goto L_%08X_@;' % self.entry]
            for va, insn, stmts in blocks:
                if va in self.targets:
                    lines.append('L_%08X_@:;' % va)
                text = ' '.join(stmts)
                if text:
                    lines.append('{ %s }' % text)
            return [re.sub(r'\bL_([0-9A-F]{8})\b(?!_@)', r'L_\1_@', l) for l in lines]
        cuts = self.split_points()
        name = 'f_%08X' % self.entry
        lines = ['static void %s_p%d(Cpu& c);' % (name, p) for p in range(1, len(cuts) + 1)]
        bounds = [0] + cuts + [len(blocks)]
        for p in range(len(bounds) - 1):
            lines.append(('void %s(Cpu& c) {' % name) if p == 0 else ('static void %s_p%d(Cpu& c) {' % (name, p)))
            start = len(lines)
            # A function that reads no flags before writing them need not load them.
            no_flags = p == 0 and not (self.live_in.get(self.entry, F.ALL) & F.ALL)
            lines.append('    PROLOGUE_NF();' if no_flags else '    PROLOGUE();')
            if p == 0:
                lines.extend('    ' + site[0] for site_va, site in sorted(self.site_overrides.items())
                             if site[0] and site_va in self.insns)
            for va, insn, stmts in blocks[bounds[p]:bounds[p + 1]]:
                if va in self.targets:
                    lines.append('L_%08X:;' % va)
                text = ' '.join(stmts)
                lines.append('    { %s }  // %08X %s %s' % (text, va, insn.mnemonic, insn.op_str) if text else
                             '    // %08X %s %s' % (va, insn.mnemonic, insn.op_str))
            if p + 1 < len(bounds) - 1 and self.falls_through(blocks[bounds[p + 1] - 1][1]):
                lines.append('    SYNC(); %s_p%d(c); return;' % (name, p + 1))
            lines[start:] = self.keep_xmm_local(lines[start:])
            lines.append('}')
        return '\n'.join(lines)

    XMM_REF = re.compile(r'\bxm([0-7])_')
    SYNC_CALL = re.compile(r'\b(SYNC|SYNC_R|RELOAD)\(\)|\bSYNC_TO\(0x[0-9A-F]+u\)'
                           r'|\b(RELOAD)_(?:AFTER\(0x[0-9A-F]+u, [01]\)|M\(\d+\))')

    def keep_xmm_local(self, body):
        """The xmm registers a function uses live in locals, like the GPRs.

        In c.xmm they could not stay in host registers: every guest store goes
        through M (a byte pointer) and may alias the Cpu, so each access was a
        reload. The used registers are loaded after PROLOGUE and written back
        wherever the GPRs are (SYNC) and read again after calls (RELOAD);
        registers the function never names stay in c.xmm untouched."""
        used = sorted({int(n) for line in body for n in self.XMM_REF.findall(line)})
        if not used:
            return body
        out = [body[0]]
        out.append('    Xmm %s;' % ', '.join('xm%d_ = c.xmm[%d]' % (n, n) for n in used))
        out.append('#define XS_() (%s)' % ', '.join('c.xmm[%d] = xm%d_' % (n, n) for n in used))
        out.append('#define XR_() (%s)' % ', '.join('xm%d_ = c.xmm[%d]' % (n, n) for n in used))
        for line in body[1:]:
            out.append(self.SYNC_CALL.sub(lambda m: '(%s, %s())' % (
                m.group(0), 'XR_' if 'RELOAD' in (m.group(1) or '', m.group(2) or '') else 'XS_'), line))
        out.append('#undef XS_')
        out.append('#undef XR_')
        return out

    SPLIT_INSNS = 2000

    def split_points(self):
        """Indices where an oversized function is cut into chained parts: no
        branch spans the cut and no fused compare is separated from its reader
        (registers and flags are synced at the cut, the fused operands are not)."""
        n = len(self.order)
        if n <= self.SPLIT_INSNS * 3 // 2:
            return []
        index = {va: i for i, va in enumerate(self.order)}
        spans = [0] * (n + 2)
        for i, va in enumerate(self.order):
            for s in self.succ[va]:
                j = index[s]
                if j != i + 1:
                    lo, hi = min(i, j), max(i, j)
                    spans[lo + 1] += 1
                    spans[hi + 1] -= 1
        cuts = []
        depth = 0
        start = 0
        for k in range(n):
            depth += spans[k]
            if k - start >= self.SPLIT_INSNS and depth == 0 and self.order[k] not in self.fused:
                cuts.append(k)
                start = k
        return cuts

    def falls_through(self, insn):
        m = insn.mnemonic.split()[-1]
        if m == 'jmp' or m in ('ret', 'retf', 'int3', 'ud2', 'hlt'):
            return False
        if m == 'call' and insn.address in self.fn.noreturn_calls:
            return False
        return True

    def successors(self, va):
        insn = self.insns[va]
        m = insn.mnemonic.split()[-1]
        nxt = va + insn.size
        if m == 'jmp':
            if va in self.fn.jump_tables:
                return [t for t in self.fn.jump_tables[va].targets if self.inside(t)]
            op = insn.operands[0]
            if op.type == X.X86_OP_IMM and self.inside(op.imm & 0xFFFFFFFF):
                return [op.imm & 0xFFFFFFFF]
            return []
        if m in ('ret', 'retf', 'int3', 'ud2', 'hlt'):
            return []
        out = []
        if m.startswith('j') or m.startswith('loop'):
            t = insn.operands[0].imm & 0xFFFFFFFF
            if self.inside(t):
                out.append(t)
        if m == 'call' and va in self.fn.noreturn_calls:
            return out
        if self.inside(nxt):
            out.append(nxt)
        return out

    PRESERVABLE = (('ebx', 1), ('ebp', 2), ('esi', 4), ('edi', 8))

    def writes_reg(self, insn, name):
        """`insn` writes the 32-bit register `name` (or a part of it)."""
        try:
            _, written = insn.regs_access()
        except Exception:
            return True
        parts = {'ebx': ('ebx', 'bx', 'bl', 'bh'), 'ebp': ('ebp', 'bp'), 'esi': ('esi', 'si'), 'edi': ('edi', 'di')}[name]
        return any(insn.reg_name(r) in parts for r in written)

    def preserved_regs(self):
        """A mask of ebx (1), ebp (2), esi (4), edi (8) that this function returns
        with their entry values, by the MSVC pattern: pushed in the straight-line
        prologue before anything writes them, and popped in the straight-line run
        of instructions before every ret, with nothing writing them between that
        pop and the ret. Anything else (a tail jmp or other exit, an indirect jmp,
        no ret, a prologue helper such as __SEH_prolog) preserves nothing. Callers
        then reload only what the callee may change (RELOAD_AFTER)."""
        if self.inlined or self.fn.indirect_jumps or not self.order:
            return 0
        rets = [va for va in self.order if self.insns[va].mnemonic.split()[-1] == 'ret']
        if not rets or any(self.leaves(va) for va in self.order):
            return 0
        # The prologue: from the entry, straight line, until a branch target, a
        # jump, call or ret.
        pushed = 0
        written = 0
        va = self.entry
        while va in self.insns:
            insn = self.insns[va]
            m = insn.mnemonic.split()[-1]
            if va != self.entry and va in self.branch_in:
                break
            if m in ('call', 'ret', 'jmp') or F.cc_of(m) is not None or m.startswith('j'):
                break
            for name, bit in self.PRESERVABLE:
                if m == 'push' and insn.op_str == name and not (written & bit):
                    pushed |= bit
                elif self.writes_reg(insn, name):
                    written |= bit
            va += insn.size
        if not pushed:
            return 0
        cookies = set(COOKIE_CHECKS.get(getattr(self.tr.img, 'edition', None), {}))
        index = {a: i for i, a in enumerate(self.order)}
        keep = pushed
        for r in rets:
            popped = 0
            dirty = 0  # written after its pop (walking back: seen before the pop)
            i = index[r]
            while i > 0:
                prev = self.order[i - 1]
                cur = self.order[i]
                if self.insns[prev].address + self.insns[prev].size != cur or cur in self.branch_in:
                    break  # another path joins here: earlier pops are not on it
                insn = self.insns[prev]
                m = insn.mnemonic.split()[-1]
                if m == 'call':
                    op = insn.operands[0]
                    if not (op.type == X.X86_OP_IMM and (op.imm & 0xFFFFFFFF) in cookies):
                        break
                elif m == 'leave':
                    if not (dirty & 2):
                        popped |= 2
                elif m == 'pop' and insn.op_str in ('ebx', 'ebp', 'esi', 'edi'):
                    bit = dict(self.PRESERVABLE)[insn.op_str]
                    if not (dirty & bit):
                        popped |= bit
                else:
                    if F.cc_of(m) is not None or m.startswith('j'):
                        break
                    for name, bit in self.PRESERVABLE:
                        if not (popped & bit) and self.writes_reg(insn, name):
                            dirty |= bit
                i -= 1
            keep &= popped & ~dirty
            if not keep:
                return 0
        return keep

    def leaves(self, va):
        """True when control can go from `va` to code outside this function
        other than by ret or a call: a tail jmp, a branch out, a fall-through
        into the next function, or an indirect jmp that is not a jump table."""
        insn = self.insns[va]
        m = insn.mnemonic.split()[-1]
        nxt = va + insn.size
        if m in ('ret', 'retf', 'int3', 'ud2', 'hlt'):
            return False
        if m == 'jmp':
            if va in self.fn.jump_tables:
                return any(not self.inside(t) for t in self.fn.jump_tables[va].targets)
            op = insn.operands[0]
            return not (op.type == X.X86_OP_IMM and self.inside(op.imm & 0xFFFFFFFF))
        if m.startswith('j') or m.startswith('loop'):
            if not self.inside(insn.operands[0].imm & 0xFFFFFFFF):
                return True
        if m == 'call' and va in self.fn.noreturn_calls:
            return False
        return not self.inside(nxt)

    def flag_effect(self, insn):
        parts = insn.mnemonic.split()
        m = parts[-1]
        if m == 'ret' and (self.tr.flags_live_at_exit or self.fn.entry in self.tr.flags_returned):
            return 0, F.ALL
        if parts[0] in ('rep', 'repe', 'repne') and m[:4] in ('cmps', 'scas'):
            return F.ALL, F.ALL
        if m in ('cmpsd', 'movsd') and any(op.type == X.X86_OP_REG and self.rn(insn, op.reg).startswith('xmm')
                                           for op in insn.operands):
            return 0, 0
        imm_nonzero = False
        if m in F.SHIFTS or m in F.ROTATES or m in F.ROTATES_C:
            ops = insn.operands
            cnt = ops[2] if m in ('shld', 'shrd') else (ops[1] if len(ops) > 1 else None)
            if cnt is None:
                imm_nonzero = True
            elif cnt.type == X.X86_OP_IMM:
                imm_nonzero = (cnt.imm & 31) != 0
        return F.flags_of(m, imm_nonzero)

    def liveness(self):
        self.effects = {va: self.flag_effect(self.insns[va]) for va in self.order}
        succ = self.succ = {va: self.successors(va) for va in self.order}
        self.branch_in = set()
        for va in self.order:
            nxt = va + self.insns[va].size
            for s in succ[va]:
                if s != nxt:
                    self.branch_in.add(s)
        for t in self.fn.jump_tables.values():
            self.branch_in.update(t.targets)
        self.branch_in.add(self.entry)
        # A jump or fall-through into another translated function passes the
        # flags in the Cpu. The code there may read them (discovery can start a
        # function at a label between a cmp and its jcc), so every flag is live
        # at such an exit. A ret, or a call that does not return, ends them.
        self.exit_live = {va: (F.ALL if self.leaves(va) else 0) for va in self.order}
        live_in = dict.fromkeys(self.order, 0)
        changed = True
        while changed:
            changed = False
            for va in reversed(self.order):
                out = self.exit_live[va]
                for s in succ[va]:
                    out |= live_in[s]
                d, u = self.effects[va]
                li = (out & ~d) | u
                if li != live_in[va]:
                    live_in[va] = li
                    changed = True
        self.live_in = live_in
        self.live_out = {}
        for va in self.order:
            out = self.exit_live[va]
            for s in succ[va]:
                out |= live_in[s]
            self.live_out[va] = out

    # ---------------- emission ----------------
    def emit(self, insn):
        parts = insn.mnemonic.split()
        prefix = parts[0] if len(parts) > 1 else None
        m = parts[-1]
        xmm_form = any(op.type == X.X86_OP_REG and self.rn(insn, op.reg).startswith('xmm') for op in insn.operands)
        if m[:-1] in STRING_OPS and m[-1] in 'bwd' and not xmm_form:
            return self.emit_string(insn, m, prefix)
        if prefix in ('rep', 'repz', 'repe') and m == 'ret':
            prefix = None  # "rep ret": an AMD branch-predictor idiom, a plain ret
        if prefix == 'notrack' and m in ('jmp', 'call'):
            prefix = None  # CET hint (a ds override byte) with no effect here
        if prefix not in (None, 'lock'):
            raise Unsupported('prefix %s on %s' % (prefix, m))
        self.locked = prefix == 'lock'
        handler = getattr(self, 'i_' + m, None)
        if handler is not None:
            return handler(insn)
        cc = F.cc_of(m)
        if cc:
            if m.startswith('j'):
                return self.emit_jcc(insn, cc)
            if m.startswith('set'):
                return self.out.append(self.write(insn, insn.operands[0], self.cond(insn, cc)))
            if m.startswith('cmov'):
                d, s = insn.operands
                return self.out.append('if (%s) { %s }' % (self.cond(insn, cc), self.write(insn, d, self.read(insn, s))))
        if self.emit_x87(insn, m):
            return
        if self.emit_sse(insn, m):
            return
        raise Unsupported(m)

    # ---------------- atomics ----------------
    def atomic_mem(self, insn, op):
        """A memory operand the lock prefix (or xchg, which locks implicitly) applies to."""
        if op.type != X.X86_OP_MEM:
            if self.locked:
                raise Unsupported('lock on a register')
            return False
        return self.locked or insn.mnemonic.split()[-1] == 'xchg'

    def atomic_rmw(self, insn, op, size, new_value):
        """Updates the memory operand atomically. `new_value` is a C expression in `o`, the old
        contents. Leaves `o_` holding that old value."""
        a = self.addr(insn, op)
        if size > 1:
            self.out.append('if ((%s & %du) != 0) { SYNC(); trap(c, %s, "unaligned locked access"); }' % (
                a, size - 1, h(insn.address)))
        ut = UT[size]
        self.out.append('%s o_ = atomic_rmw<%s>(M, %s, [&](%s o) { return (%s)(%s); });' % (
            ut, ut, a, ut, ut, new_value))

    # integer arithmetic -------------------------------------------------
    def arith(self, insn, kind, carry=False, store=True):
        d, s = insn.operands
        size = d.size
        need = self.need(insn)
        b = self.tmpvar(UT[size], self.read(insn, s, size))
        cin = '(uint32_t)cf' if carry else None
        extra = (' %s %s' % (('+' if kind == 'add' else '-'), cin)) if cin else ''
        op = '+' if kind == 'add' else '-'
        if store and self.atomic_mem(insn, d):
            self.atomic_rmw(insn, d, size, 'o %s %s%s' % (op, b, extra))
            a = 'o_'
        else:
            a = self.tmpvar(UT[size], self.read(insn, d))
        r = self.tmpvar(UT[size], '(%s)(%s %s %s%s)' % (UT[size], a, op, b, extra))
        self.out.extend(self.flag_code(need, kind, a, b, r, size, cin))
        if store and a != 'o_':
            self.out.append(self.write(insn, d, r))

    def same_reg(self, insn):
        d, s = insn.operands[0], insn.operands[1]
        return d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg

    def zero_idiom(self, insn):
        need = self.need(insn)
        self.out.append(self.write(insn, insn.operands[0], '0'))
        for bit, v in ((F.Z, 1), (F.S, 0), (F.P, 1), (F.C, 0), (F.O, 0), (F.A, 0)):
            if need & bit:
                self.out.append('%s = %d;' % (F.NAMES[bit], v))

    def i_add(self, insn):
        self.arith(insn, 'add')

    def i_adc(self, insn):
        self.arith(insn, 'add', carry=True)

    def i_sub(self, insn):
        if self.same_reg(insn):
            return self.zero_idiom(insn)
        self.arith(insn, 'sub')

    def i_sbb(self, insn):
        if self.same_reg(insn):
            # sbb r, r: 0 or all ones from the carry; CF and the parity are unchanged / even either way
            need = self.need(insn)
            self.out.append(self.write(insn, insn.operands[0], '0u - (uint32_t)cf'))
            for bit, v in ((F.Z, '!cf'), (F.S, 'cf'), (F.A, 'cf'), (F.P, '1'), (F.O, '0')):
                if need & bit:
                    self.out.append('%s = %s;' % (F.NAMES[bit], v))
            return
        self.arith(insn, 'sub', carry=True)

    def i_cmp(self, insn):
        if self.fusable(insn):
            d, s = insn.operands
            self.out.append('ta = %s; tb = %s;' % (self.read(insn, d), self.read(insn, s, d.size)))
            self.fused[self.order[self.cur_idx + 1]] = d.size
            return
        self.arith(insn, 'sub', store=False)

    def logic(self, insn, op, store=True):
        d, s = insn.operands
        size = d.size
        b = self.tmpvar(UT[size], self.read(insn, s, size))
        if store and self.atomic_mem(insn, d):
            self.atomic_rmw(insn, d, size, 'o %s %s' % (op, b))
            src = 'o_'
        else:
            src = self.read(insn, d)
        r = self.tmpvar(UT[size], '(%s)(%s %s %s)' % (UT[size], src, op, b))
        self.out.extend(self.flag_code(self.need(insn), 'logic', None, None, r, size))
        if store and src != 'o_':
            self.out.append(self.write(insn, d, r))

    def i_and(self, insn):
        self.logic(insn, '&')

    def i_or(self, insn):
        self.logic(insn, '|')

    def i_xor(self, insn):
        if self.same_reg(insn):
            return self.zero_idiom(insn)
        self.logic(insn, '^')

    def i_test(self, insn):
        if self.fusable(insn):
            d, s = insn.operands
            self.out.append('ta = (%s)(%s & %s); tb = 0;' % (UT[d.size], self.read(insn, d), self.read(insn, s, d.size)))
            self.fused[self.order[self.cur_idx + 1]] = d.size
            return
        self.logic(insn, '&', store=False)

    def incdec(self, insn, op):
        d = insn.operands[0]
        size = d.size
        if self.atomic_mem(insn, d):
            self.atomic_rmw(insn, d, size, 'o %s 1u' % op)
            a = 'o_'
        else:
            a = self.tmpvar(UT[size], self.read(insn, d))
        r = self.tmpvar(UT[size], '(%s)(%s %s 1u)' % (UT[size], a, op))
        self.out.extend(self.flag_code(self.need(insn) & ~F.C, 'add' if op == '+' else 'sub', a, '1u', r, size))
        if a != 'o_':
            self.out.append(self.write(insn, d, r))

    def i_inc(self, insn):
        self.incdec(insn, '+')

    def i_dec(self, insn):
        self.incdec(insn, '-')

    def i_neg(self, insn):
        d = insn.operands[0]
        size = d.size
        need = self.need(insn)
        if self.atomic_mem(insn, d):
            self.atomic_rmw(insn, d, size, '0u - o')
            b = 'o_'
        else:
            b = self.tmpvar(UT[size], self.read(insn, d))
        r = self.tmpvar(UT[size], '(%s)(0u - %s)' % (UT[size], b))
        self.out.extend(self.flag_code(need & ~F.C, 'sub', '0u', b, r, size))
        if need & F.C:
            self.out.append('cf = %s != 0;' % b)
        if b != 'o_':
            self.out.append(self.write(insn, d, r))

    def i_not(self, insn):
        d = insn.operands[0]
        if self.atomic_mem(insn, d):
            return self.atomic_rmw(insn, d, d.size, '~o')
        self.out.append(self.write(insn, d, '~%s' % self.read(insn, d)))

    def i_mov(self, insn):
        d, s = insn.operands
        if d.type == X.X86_OP_REG and self.rn(insn, d.reg) in SEGMENTS:
            raise Unsupported('mov to a segment register')
        if s.type == X.X86_OP_REG and self.rn(insn, s.reg) in SEGMENTS:
            # the fixed selectors of a 32-bit Windows process (crash reports and context records read them)
            sel = WINDOWS_SELECTORS[self.rn(insn, s.reg)]
            return self.out.append(self.write(insn, d, h(sel), d.size if d.type == X.X86_OP_MEM else None))
        self.out.append(self.write(insn, d, self.read(insn, s, d.size)))

    def i_xlatb(self, insn):
        self.out.append('eax = (eax & 0xFFFFFF00u) | ld8(M, ebx + (uint8_t)eax);')

    def i_movzx(self, insn):
        d, s = insn.operands
        self.out.append(self.write(insn, d, '(%s)%s' % (UT[d.size], self.read(insn, s))))

    def i_movsx(self, insn):
        d, s = insn.operands
        self.out.append(self.write(insn, d, '(%s)(%s)(%s)%s' % (UT[d.size], ST[d.size], ST[s.size], self.read(insn, s))))

    def i_lea(self, insn):
        d, s = insn.operands
        if s.mem.segment:
            raise Unsupported('lea with a segment')
        self.out.append(self.write(insn, d, self.addr(insn, s)))

    def i_xchg(self, insn):
        if self.same_reg(insn):
            return
        d, s = insn.operands
        mem = d if d.type == X.X86_OP_MEM else s if s.type == X.X86_OP_MEM else None
        if mem is not None:
            other = s if mem is d else d
            size = mem.size
            v = self.tmpvar(UT[size], self.read(insn, other, size))
            a = self.addr(insn, mem)
            if size > 1:
                self.out.append('if ((%s & %du) != 0) { SYNC(); trap(c, %s, "unaligned locked access"); }' % (
                    a, size - 1, h(insn.address)))
            got = self.tmpvar(UT[size], 'atomic_xchg<%s>(M, %s, %s)' % (UT[size], a, v))
            self.out.append(self.write(insn, other, got))
            return
        t = self.tmpvar(UT[d.size], self.read(insn, d))
        self.out.append(self.write(insn, d, self.read(insn, s)))
        self.out.append(self.write(insn, s, t))

    def i_xadd(self, insn):
        d, s = insn.operands
        size = d.size
        b = self.tmpvar(UT[size], self.read(insn, s))
        if self.atomic_mem(insn, d):
            self.atomic_rmw(insn, d, size, 'o + %s' % b)
            a = 'o_'
        else:
            a = self.tmpvar(UT[size], self.read(insn, d))
        r = self.tmpvar(UT[size], '(%s)(%s + %s)' % (UT[size], a, b))
        self.out.extend(self.flag_code(self.need(insn), 'add', a, b, r, size))
        self.out.append(self.write(insn, s, a))
        if a != 'o_':
            self.out.append(self.write(insn, d, r))

    def i_cmpxchg(self, insn):
        d, s = insn.operands
        size = d.size
        acc = {1: '(uint8_t)eax', 2: '(uint16_t)eax', 4: 'eax'}[size]
        load = {1: 'eax = (eax & 0xFFFFFF00u) | %s;', 2: 'eax = (eax & 0xFFFF0000u) | %s;', 4: 'eax = %s;'}[size]
        if self.atomic_mem(insn, d):
            a = self.addr(insn, d)
            if size > 1:
                self.out.append('if ((%s & %du) != 0) { SYNC(); trap(c, %s, "unaligned locked access"); }' % (
                    a, size - 1, h(insn.address)))
            self.out.append('%s exp_ = %s;' % (UT[size], acc))
            self.out.append('atomic_cmpxchg<%s>(M, %s, exp_, %s);' % (UT[size], a, self.read(insn, s, size)))
            b = 'exp_'
        else:
            b = self.tmpvar(UT[size], self.read(insn, d))
        a = self.tmpvar(UT[size], acc)
        r = self.tmpvar(UT[size], '(%s)(%s - %s)' % (UT[size], a, b))
        self.out.extend(self.flag_code(self.need(insn) | F.Z, 'sub', a, b, r, size))
        if b == 'exp_':
            self.out.append('if (!zf) { %s }' % (load % 'exp_'))
        else:
            self.out.append('if (zf) { %s } else { %s }' % (self.write(insn, d, self.read(insn, s)), load % b))

    def i_cmpxchg8b(self, insn):
        a = self.addr(insn, insn.operands[0])
        if self.locked:
            self.out.append('if ((%s & 7u) != 0) { SYNC(); trap(c, %s, "unaligned locked access"); }' % (a, h(insn.address)))
            self.out.append('{ uint64_t exp_ = ((uint64_t)edx << 32) | eax; zf = atomic_cmpxchg<uint64_t>(M, %s, exp_, ((uint64_t)ecx << 32) | ebx); '
                            'if (!zf) { eax = (uint32_t)exp_; edx = (uint32_t)(exp_ >> 32); } }' % a)
            return
        self.out.append('{ uint64_t m_ = ld64(M, %s); if (m_ == (((uint64_t)edx << 32) | eax)) { st64(M, %s, ((uint64_t)ecx << 32) | ebx); zf = 1; } '
                        'else { eax = (uint32_t)m_; edx = (uint32_t)(m_ >> 32); zf = 0; } }' % (a, a))

    def i_bswap(self, insn):
        d = insn.operands[0]
        self.out.append(self.write(insn, d, '__builtin_bswap32(%s)' % self.read(insn, d)))

    def i_cdq(self, insn):
        self.out.append('edx = (uint32_t)((int32_t)eax >> 31);')

    def i_cwde(self, insn):
        self.out.append('eax = (uint32_t)(int32_t)(int16_t)eax;')

    def i_cbw(self, insn):
        self.out.append('eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;')

    def i_cwd(self, insn):
        self.out.append('edx = (edx & 0xFFFF0000u) | (uint16_t)((int16_t)eax >> 15);')

    def i_nop(self, insn):
        pass

    def i_pause(self, insn):
        # A spin-wait hint: the thread that owns what this loop waits for runs now.
        self.out.append('guest_pause(c);')

    def i_mfence(self, insn):
        self.out.append('fence_full();')

    def i_sfence(self, insn):
        self.out.append('fence_store();')

    def i_lfence(self, insn):
        self.out.append('fence_load();')
    i_prefetchnta = i_nop
    i_prefetcht0 = i_nop
    i_prefetcht1 = i_nop
    i_prefetcht2 = i_nop
    i_prefetchw = i_nop
    i_prefetch = i_nop

    def i_int3(self, insn):
        self.out.append('SYNC(); trap(c, %s, "int3");' % h(insn.address))

    def i_ud2(self, insn):
        self.out.append('SYNC(); trap(c, %s, "ud2");' % h(insn.address))

    def i_hlt(self, insn):
        self.out.append('SYNC(); trap(c, %s, "hlt");' % h(insn.address))

    def i_stc(self, insn):
        self.out.append('cf = 1;')

    def i_clc(self, insn):
        self.out.append('cf = 0;')

    def i_cmc(self, insn):
        self.out.append('cf ^= 1;')

    def i_std(self, insn):
        self.out.append('c.df = 1;')

    def i_cld(self, insn):
        self.out.append('c.df = 0;')

    def i_lahf(self, insn):
        self.out.append('eax = (eax & 0xFFFF00FFu) | ((uint32_t)(sf << 7 | zf << 6 | af << 4 | pf << 2 | 2 | cf) << 8);')

    def i_sahf(self, insn):
        self.out.append('{ uint32_t ah_ = eax >> 8; sf = (ah_ >> 7) & 1; zf = (ah_ >> 6) & 1; af = (ah_ >> 4) & 1; '
                        'pf = (ah_ >> 2) & 1; cf = ah_ & 1; }')

    def i_cpuid(self, insn):
        self.out.append('SYNC(); guest_cpuid(c); RELOAD();')

    def i_rdtsc(self, insn):
        self.out.append('{ uint64_t t_ = guest_rdtsc(); eax = (uint32_t)t_; edx = (uint32_t)(t_ >> 32); }')

    # multiply / divide --------------------------------------------------
    def i_mul(self, insn):
        s = insn.operands[0]
        size = s.size
        need = self.need(insn)
        v = self.read(insn, s)
        if size == 4:
            self.out.append('{ uint64_t p_ = (uint64_t)eax * %s; eax = (uint32_t)p_; edx = (uint32_t)(p_ >> 32); }' % v)
            hi = 'edx'
        elif size == 2:
            self.out.append('{ uint32_t p_ = (uint32_t)(uint16_t)eax * %s; eax = (eax & 0xFFFF0000u) | (uint16_t)p_; '
                            'edx = (edx & 0xFFFF0000u) | (uint16_t)(p_ >> 16); }' % v)
            hi = '(uint16_t)edx'
        else:
            self.out.append('{ uint32_t p_ = (uint32_t)(uint8_t)eax * %s; eax = (eax & 0xFFFF0000u) | (uint16_t)p_; }' % v)
            hi = '(uint8_t)(eax >> 8)'
        if need & (F.C | F.O):
            self.out.append('cf = of = %s != 0;' % hi)
        self.undefined_flags(need & ~(F.C | F.O))

    def i_imul(self, insn):
        ops = insn.operands
        need = self.need(insn)
        ov = 'cf = of = ' if need & (F.C | F.O) else '(void)'
        if len(ops) == 1:
            s = ops[0]
            size = s.size
            v = '(%s)%s' % (ST[size], self.read(insn, s))
            if size == 4:
                self.out.append('{ int64_t p_ = (int64_t)(int32_t)eax * %s; eax = (uint32_t)p_; edx = (uint32_t)((uint64_t)p_ >> 32); '
                                '%s(p_ != (int64_t)(int32_t)p_); }' % (v, ov))
            elif size == 2:
                self.out.append('{ int32_t p_ = (int32_t)(int16_t)eax * %s; eax = (eax & 0xFFFF0000u) | (uint16_t)p_; '
                                'edx = (edx & 0xFFFF0000u) | (uint16_t)((uint32_t)p_ >> 16); %s(p_ != (int16_t)p_); }' % (v, ov))
            else:
                self.out.append('{ int32_t p_ = (int32_t)(int8_t)eax * %s; eax = (eax & 0xFFFF0000u) | (uint16_t)p_; '
                                '%s(p_ != (int8_t)p_); }' % (v, ov))
        else:
            d = ops[0]
            size = d.size
            a = self.read(insn, ops[1] if len(ops) == 3 else d)
            b = self.read(insn, ops[2] if len(ops) == 3 else ops[1], size)
            self.out.append('{ int64_t p_ = (int64_t)(%s)%s * (int64_t)(%s)%s; %s %s(p_ != (int64_t)(%s)p_); }' % (
                ST[size], a, ST[size], b, self.write(insn, d, '(%s)p_' % UT[size]), ov, ST[size]))
        self.undefined_flags(need & ~(F.C | F.O))

    def divide(self, insn, signed):
        s = insn.operands[0]
        size = s.size
        at = h(insn.address)
        v = self.tmpvar(UT[size], self.read(insn, s))
        self.out.append('if (%s == 0) { SYNC(); trap(c, %s, "divide by zero"); }' % (v, at))
        if size == 4:
            if signed:
                self.out.append('{ int64_t n_ = (int64_t)(((uint64_t)edx << 32) | eax); int64_t d_ = (int32_t)%s; '
                                'if (n_ == INT64_MIN && d_ == -1) { SYNC(); trap(c, %s, "divide overflow"); } '
                                'int64_t q_ = n_ / d_; if (q_ != (int32_t)q_) { SYNC(); trap(c, %s, "divide overflow"); } '
                                'edx = (uint32_t)(n_ %% d_); eax = (uint32_t)q_; }' % (v, at, at))
            else:
                self.out.append('{ uint64_t n_ = ((uint64_t)edx << 32) | eax; uint64_t q_ = n_ / %s; '
                                'if (q_ > 0xFFFFFFFFull) { SYNC(); trap(c, %s, "divide overflow"); } '
                                'edx = (uint32_t)(n_ %% %s); eax = (uint32_t)q_; }' % (v, at, v))
        elif size == 2:
            if signed:
                self.out.append('{ int32_t n_ = (int32_t)(((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax); int32_t q_ = n_ / (int16_t)%s; '
                                'if (q_ != (int16_t)q_) { SYNC(); trap(c, %s, "divide overflow"); } '
                                'edx = (edx & 0xFFFF0000u) | (uint16_t)(n_ %% (int16_t)%s); eax = (eax & 0xFFFF0000u) | (uint16_t)q_; }' % (v, at, v))
            else:
                self.out.append('{ uint32_t n_ = ((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax; uint32_t q_ = n_ / %s; '
                                'if (q_ > 0xFFFFu) { SYNC(); trap(c, %s, "divide overflow"); } '
                                'edx = (edx & 0xFFFF0000u) | (uint16_t)(n_ %% %s); eax = (eax & 0xFFFF0000u) | (uint16_t)q_; }' % (v, at, v))
        else:
            if signed:
                self.out.append('{ int32_t n_ = (int16_t)(uint16_t)eax; int32_t q_ = n_ / (int8_t)%s; '
                                'if (q_ != (int8_t)q_) { SYNC(); trap(c, %s, "divide overflow"); } '
                                'eax = (eax & 0xFFFF0000u) | (uint16_t)(((uint32_t)(uint8_t)(n_ %% (int8_t)%s) << 8) | (uint8_t)q_); }' % (v, at, v))
            else:
                self.out.append('{ uint32_t n_ = (uint16_t)eax; uint32_t q_ = n_ / %s; '
                                'if (q_ > 0xFFu) { SYNC(); trap(c, %s, "divide overflow"); } '
                                'eax = (eax & 0xFFFF0000u) | (uint16_t)(((n_ %% %s) << 8) | q_); }' % (v, at, v))
        self.undefined_flags(self.need(insn))

    def i_div(self, insn):
        self.divide(insn, False)

    def i_idiv(self, insn):
        self.divide(insn, True)

    # shifts / rotates ---------------------------------------------------
    def count(self, insn, op):
        if op is None:
            return 1, None
        if op.type == X.X86_OP_IMM:
            return op.imm & 31, None
        if self.rn(insn, op.reg) != 'cl':
            raise Unsupported('shift count in %s' % self.rn(insn, op.reg))
        return None, '(ecx & 31u)'

    def shift(self, insn, kind):
        ops = insn.operands
        d = ops[0]
        size = d.size
        bits = size * 8
        n_imm, n_expr = self.count(insn, ops[1] if len(ops) > 1 else None)
        if n_imm == 0:
            return
        need = self.need(insn)
        n = str(n_imm) + 'u' if n_expr is None else 'n_'
        s = []
        s.append('%s a_ = %s;' % (UT[size], self.read(insn, d)))
        if kind == 'shl':
            s.append('%s r_ = (%s)((uint64_t)a_ << %s);' % (UT[size], UT[size], n))
            if need & (F.C | F.O):
                s.append('cf = (uint8_t)((((uint64_t)a_ << %s) >> %d) & 1);' % (n, bits))
            if need & F.O:
                s.append('of = (uint8_t)(((r_ >> %d) & 1) ^ cf);' % (bits - 1))
        elif kind == 'shr':
            s.append('%s r_ = (%s)((uint64_t)a_ >> %s);' % (UT[size], UT[size], n))
            if need & F.C:
                s.append('cf = (uint8_t)(((uint64_t)a_ >> (%s - 1)) & 1);' % n)
            if need & F.O:
                s.append('of = (uint8_t)((a_ >> %d) & 1);' % (bits - 1))
        else:
            s.append('%s r_ = (%s)((int64_t)(%s)a_ >> %s);' % (UT[size], UT[size], ST[size], n))
            if need & F.C:
                s.append('cf = (uint8_t)(((int64_t)(%s)a_ >> (%s - 1)) & 1);' % (ST[size], n))
            if need & F.O:
                s.append('of = 0;')
        s.extend(self.flag_code(need & (F.Z | F.S | F.P | F.A), 'logic', None, None, 'r_', size))
        s.append(self.write(insn, d, 'r_'))
        if n_expr is None:
            self.out.append('{ %s }' % ' '.join(s))
        else:
            self.out.append('{ uint32_t n_ = %s; if (n_) { %s } }' % (n_expr, ' '.join(s)))

    def i_shl(self, insn):
        self.shift(insn, 'shl')

    i_sal = i_shl

    def i_shr(self, insn):
        self.shift(insn, 'shr')

    def i_sar(self, insn):
        self.shift(insn, 'sar')

    def rotate(self, insn, left):
        ops = insn.operands
        d = ops[0]
        size = d.size
        bits = size * 8
        n_imm, n_expr = self.count(insn, ops[1] if len(ops) > 1 else None)
        if n_imm == 0:
            return
        need = self.need(insn)
        n = str(n_imm) + 'u' if n_expr is None else 'n_'
        s = ['%s a_ = %s;' % (UT[size], self.read(insn, d)), 'uint32_t m_ = %s %% %du;' % (n, bits)]
        if left:
            s.append('%s r_ = (%s)((a_ << m_) | (a_ >> ((%du - m_) %% %du)));' % (UT[size], UT[size], bits, bits))
            if need & (F.C | F.O):
                s.append('cf = (uint8_t)(r_ & 1);')
            if need & F.O:
                s.append('of = (uint8_t)(((r_ >> %d) & 1) ^ cf);' % (bits - 1))
        else:
            s.append('%s r_ = (%s)((a_ >> m_) | (a_ << ((%du - m_) %% %du)));' % (UT[size], UT[size], bits, bits))
            if need & F.C:
                s.append('cf = (uint8_t)((r_ >> %d) & 1);' % (bits - 1))
            if need & F.O:
                s.append('of = (uint8_t)(((r_ >> %d) ^ (r_ >> %d)) & 1);' % (bits - 1, bits - 2))
        s.append(self.write(insn, d, 'r_'))
        if n_expr is None:
            self.out.append('{ %s }' % ' '.join(s))
        else:
            self.out.append('{ uint32_t n_ = %s; if (n_) { %s } }' % (n_expr, ' '.join(s)))

    def i_rol(self, insn):
        self.rotate(insn, True)

    def i_ror(self, insn):
        self.rotate(insn, False)

    def rotate_carry(self, insn, left):
        ops = insn.operands
        d = ops[0]
        size = d.size
        bits = size * 8
        n_imm, n_expr = self.count(insn, ops[1] if len(ops) > 1 else None)
        if n_imm == 0:
            return
        n = str(n_imm) + 'u' if n_expr is None else n_expr
        if left:
            step = 'uint32_t o_ = (a_ >> %d) & 1; a_ = (%s)((a_ << 1) | y_); y_ = o_;' % (bits - 1, UT[size])
            of = 'of = (uint8_t)(((a_ >> %d) & 1) ^ y_);' % (bits - 1)
        else:
            step = 'uint32_t o_ = a_ & 1; a_ = (%s)((a_ >> 1) | (y_ << %d)); y_ = o_;' % (UT[size], bits - 1)
            of = 'of = (uint8_t)(((a_ >> %d) ^ (a_ >> %d)) & 1);' % (bits - 1, bits - 2)
        self.out.append('{ uint32_t n_ = %s; if (n_) { %s a_ = %s; uint32_t y_ = cf; for (uint32_t k_ = n_ %% %du; k_; --k_) { %s } '
                        'cf = (uint8_t)y_; %s %s } }' % (n, UT[size], self.read(insn, d), bits + 1, step, of,
                                                          self.write(insn, d, 'a_')))

    def i_rcl(self, insn):
        self.rotate_carry(insn, True)

    def i_rcr(self, insn):
        self.rotate_carry(insn, False)

    def double_shift(self, insn, left):
        d, s, cnt = insn.operands
        if d.size != 4:
            raise Unsupported('16-bit shld / shrd')
        n_imm, n_expr = self.count(insn, cnt)
        if n_imm == 0:
            return
        need = self.need(insn)
        n = str(n_imm) + 'u' if n_expr is None else 'n_'
        body = ['uint32_t a_ = %s; uint32_t b_ = %s;' % (self.read(insn, d), self.read(insn, s))]
        if left:
            body.append('uint32_t r_ = (uint32_t)((((uint64_t)a_ << 32 | b_) << %s) >> 32);' % n)
            if need & F.C:
                body.append('cf = (uint8_t)((a_ >> (32u - %s)) & 1);' % n)
        else:
            body.append('uint32_t r_ = (uint32_t)((((uint64_t)b_ << 32) | a_) >> %s);' % n)
            if need & F.C:
                body.append('cf = (uint8_t)((a_ >> (%s - 1)) & 1);' % n)
        if need & F.O:
            body.append('of = (uint8_t)(((r_ ^ a_) >> 31) & 1);')
        body.extend(self.flag_code(need & (F.Z | F.S | F.P | F.A), 'logic', None, None, 'r_', 4))
        body.append(self.write(insn, d, 'r_'))
        if n_expr is None:
            self.out.append('{ %s }' % ' '.join(body))
        else:
            self.out.append('{ uint32_t n_ = %s; if (n_) { %s } }' % (n_expr, ' '.join(body)))

    def i_shld(self, insn):
        self.double_shift(insn, True)

    def i_shrd(self, insn):
        self.double_shift(insn, False)

    # bit operations -----------------------------------------------------
    def bit_test(self, insn, op):
        d, s = insn.operands
        size = d.size
        bits = size * 8
        need = self.need(insn)
        if s.type == X.X86_OP_IMM:
            bit = '%du' % (s.imm & (bits - 1))
            where = d
            loc = None
        else:
            off = self.read(insn, s)
            bit = '(%s & %du)' % (off, bits - 1)
            if d.type == X.X86_OP_MEM:
                if size != 4:
                    raise Unsupported('bit string with a 16-bit operand')
                loc = '(uint32_t)(%s + (uint32_t)(((int32_t)%s >> 5) * 4))' % (self.addr(insn, d), off)
            else:
                loc = None
            where = d
        if self.locked:
            if d.type != X.X86_OP_MEM:
                raise Unsupported('lock on a register')
            addr = loc if loc else self.addr(insn, d)
            width = 4 if loc else size
            ut = UT[width]
            new = {'bts': 'o | (1u << b_)', 'btr': 'o & ~(1u << b_)', 'btc': 'o ^ (1u << b_)'}.get(op)
            s_ = ['uint32_t b_ = %s;' % bit]
            if width > 1:
                s_.append('if ((%s & %du) != 0) { SYNC(); trap(c, %s, "unaligned locked access"); }' % (
                    addr, width - 1, h(insn.address)))
            if new:
                s_.append('%s o_ = atomic_rmw<%s>(M, %s, [&](%s o) { return (%s)(%s); });' % (ut, ut, addr, ut, ut, new))
                s_.append('cf = (uint8_t)((o_ >> b_) & 1);')
            else:
                s_.append('cf = (uint8_t)((atomic_load<%s>(M, %s) >> b_) & 1);' % (ut, addr))
            self.out.append('{ %s }' % ' '.join(s_))
            self.undefined_flags(need & (F.P | F.A | F.S | F.O))
            return
        if loc:
            v = 'ld32(M, l_)'
            s_ = ['uint32_t l_ = %s;' % loc, 'uint32_t b_ = %s; uint32_t v_ = %s;' % (bit, v)]
            store = 'st32(M, l_, %s);'
        else:
            s_ = ['uint32_t b_ = %s; uint32_t v_ = %s;' % (bit, self.read(insn, where))]
            store = None
        s_.append('cf = (uint8_t)((v_ >> b_) & 1);')
        newv = {'bts': 'v_ | (1u << b_)', 'btr': 'v_ & ~(1u << b_)', 'btc': 'v_ ^ (1u << b_)'}.get(op)
        if newv:
            s_.append(store % newv if store else self.write(insn, where, newv))
        self.out.append('{ %s }' % ' '.join(s_))
        self.undefined_flags(need & (F.P | F.A | F.S | F.O))

    def i_bt(self, insn):
        self.bit_test(insn, 'bt')

    def i_bts(self, insn):
        self.bit_test(insn, 'bts')

    def i_btr(self, insn):
        self.bit_test(insn, 'btr')

    def i_btc(self, insn):
        self.bit_test(insn, 'btc')

    def bit_scan(self, insn, forward):
        d, s = insn.operands
        if d.size != 4:
            raise Unsupported('16-bit bsf / bsr')
        need = self.need(insn)
        r = '(uint32_t)__builtin_ctz(v_)' if forward else '(31u - (uint32_t)__builtin_clz(v_))'
        self.out.append('{ uint32_t v_ = %s; zf = v_ == 0; if (v_) { %s } }' % (self.read(insn, s), self.write(insn, d, r)))
        self.undefined_flags(need & ~F.Z)

    def i_bsf(self, insn):
        self.bit_scan(insn, True)

    def i_bsr(self, insn):
        self.bit_scan(insn, False)

    # stack --------------------------------------------------------------
    def i_push(self, insn):
        s = insn.operands[0]
        if s.type == X.X86_OP_REG and self.rn(insn, s.reg) in SEGMENTS:
            raise Unsupported('push of a segment register')
        size = s.size if s.type != X.X86_OP_IMM else 4
        if size == 2:
            self.out.append('{ uint16_t v_ = %s; esp -= 2; st16(M, esp, v_); }' % self.read(insn, s))
        else:
            self.out.append('{ uint32_t v_ = %s; esp -= 4; st32(M, esp, v_); }' % self.read(insn, s, 4))

    def i_pop(self, insn):
        d = insn.operands[0]
        if d.type == X.X86_OP_REG and self.rn(insn, d.reg) in SEGMENTS:
            raise Unsupported('pop of a segment register')
        if d.size == 2:
            self.out.append('{ uint16_t v_ = ld16(M, esp); esp += 2; %s }' % self.write(insn, d, 'v_'))
        else:
            self.out.append('{ uint32_t v_ = ld32(M, esp); esp += 4; %s }' % self.write(insn, d, 'v_'))

    def i_pushfd(self, insn):
        self.out.append('esp -= 4; st32(M, esp, 0x202u | cf | pf << 2 | af << 4 | zf << 6 | sf << 7 | (uint32_t)c.df << 10 | '
                        '(uint32_t)of << 11 | c.eflags_extra);')

    def i_popfd(self, insn):
        self.out.append('{ uint32_t v_ = ld32(M, esp); esp += 4; cf = v_ & 1; pf = (v_ >> 2) & 1; af = (v_ >> 4) & 1; '
                        'zf = (v_ >> 6) & 1; sf = (v_ >> 7) & 1; c.df = (v_ >> 10) & 1; of = (v_ >> 11) & 1; '
                        'c.eflags_extra = v_ & 0x240000u; }')

    def i_pushal(self, insn):
        self.out.append('{ uint32_t s_ = esp; uint32_t r_[8] = {eax, ecx, edx, ebx, s_, ebp, esi, edi}; '
                        'for (int i_ = 0; i_ < 8; ++i_) { esp -= 4; st32(M, esp, r_[i_]); } }')

    def i_popal(self, insn):
        self.out.append('edi = ld32(M, esp); esi = ld32(M, esp + 4); ebp = ld32(M, esp + 8); ebx = ld32(M, esp + 16); '
                        'edx = ld32(M, esp + 20); ecx = ld32(M, esp + 24); eax = ld32(M, esp + 28); esp += 32;')

    def i_leave(self, insn):
        self.out.append('esp = ebp; ebp = ld32(M, esp); esp += 4;')

    def i_enter(self, insn):
        size, level = insn.operands
        if level.imm & 31:
            raise Unsupported('enter with a nesting level')
        self.out.append('esp -= 4; st32(M, esp, ebp); ebp = esp; esp -= %s;' % h(size.imm & 0xFFFF))

    # control flow -------------------------------------------------------
    def i_call(self, insn):
        op = insn.operands[0]
        ret = insn.address + insn.size
        s = []
        if op.type == X.X86_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t == ret and t not in self.tr.entries:
                self.out.append('esp -= 4; st32(M, esp, %s);' % h(ret))  # call $+5: pushes its own address
                return
            s.append('esp -= 4; st32(M, esp, %s);' % h(ret))
            cookie = COOKIE_CHECKS.get(getattr(self.tr.img, 'edition', None), {}).get(t)
            if cookie and self.tr.img.bytes(t, 6) != b'\x3b\x0d' + cookie[0].to_bytes(4, 'little'):
                raise ValueError('COOKIE_CHECKS: %08X is not cmp ecx, [%08X]' % (t, cookie[0]))
            limit = INLINE_LIMITS.get(getattr(self.tr.img, 'edition', None), {}).get(self.entry, Translator.INLINE_MAX)
            body = None if self.inlined or cookie or t == self.entry else self.tr.inline_body(t, limit)
            if body is not None:
                lines, refs = body
                site = '%08X' % insn.address
                self.refs |= refs
                s.append(' '.join(l.replace('@', site) for l in lines))
                s.append('%s:;' % RET_LABEL.replace('@', site))
            elif cookie and not self.need(insn):
                # __security_check_cookie inline: the common case touches no
                # register, so the call's SYNC / RELOAD (and the callee's own)
                # are skipped. The flags it sets are dead here; the return
                # address stays below esp as the real call leaves it.
                s.append('if (ecx == ld32(M, %s)) { esp += 4; } else { %s; %s; %s; }' % (
                    h(cookie[0]), self.sync_to(t), self.call_expr(t), self.reload_after(insn, t)))
            else:
                s.append('%s; %s; %s;' % (self.sync_to(t), self.call_expr(t), self.reload_after(insn, t)))
        elif self.inline_import(insn, op, ret, s):
            pass
        else:
            s.append('{ uint32_t t_ = %s; esp -= 4; st32(M, esp, %s); SYNC(); call_indirect(c, t_); %s; }' % (
                self.read(insn, op, 4), h(ret), self.reload_after(insn, None)))
        if insn.address in self.fn.noreturn_calls:
            s.append('SYNC(); trap(c, %s, "returned from a call that never returns");' % h(insn.address))
        self.out.extend(s)

    def flags_live_after(self, insn):
        if self.tr.flags_live_at_exit or self.entry in self.tr.flags_returned:
            return True  # a ret may hand them on
        return bool(self.live_out[insn.address] & F.ALL)

    def reload_after(self, insn, target):
        """The registers read back after a call: RELOAD_AFTER(target, flags) is
        resolved once every function is translated (resolve_sync_to), to the
        registers `target` may change; an indirect call reloads them all, the
        flags only when something reads them before writing them."""
        if not PRESERVE_ANALYSIS:
            return 'RELOAD()'
        flags = 1 if self.flags_live_after(insn) else 0
        if target is None:
            return 'RELOAD()' if flags else 'RELOAD_M(15)'
        return 'RELOAD_AFTER(%s, %d)' % (h(target), flags)

    # kernel32 imports called through the IAT that are done in place: the same
    # work as the host handler (win32.cpp), without leaving translated code.
    # Each pops its stdcall argument; only eax changes.
    INLINE_IMPORTS = {
        'InterlockedIncrement': 'eax = atomic_rmw<uint32_t>(M, p_, [](uint32_t v) { return v + 1u; }) + 1u;',
        'InterlockedDecrement': 'eax = atomic_rmw<uint32_t>(M, p_, [](uint32_t v) { return v - 1u; }) - 1u;',
    }

    def inline_import(self, insn, op, ret, s):
        if op.type != X.X86_OP_MEM or op.mem.base or op.mem.index or op.mem.segment:
            return False
        imp = self.tr.imports.get(op.mem.disp & 0xFFFFFFFF)
        if imp is None or imp[0].lower() != 'kernel32.dll' or imp[1] not in self.INLINE_IMPORTS:
            return False
        # The return address goes where the call would put it, below the argument.
        s.append('{ uint32_t p_ = ld32(M, esp); st32(M, esp - 4u, %s); esp += 4u; %s }' % (
            h(ret), self.INLINE_IMPORTS[imp[1]]))
        return True

    def i_ret(self, insn):
        n = insn.operands[0].imm & 0xFFFF if insn.operands else 0
        if self.inlined:
            self.out.append('esp += %du; goto %s;' % (4 + n, RET_LABEL))
            return
        keep_flags = self.tr.flags_live_at_exit or self.entry in self.tr.flags_returned
        self.out.append('esp += %du; %s; return;' % (4 + n, 'SYNC()' if keep_flags else 'SYNC_R()'))

    def i_jmp(self, insn):
        op = insn.operands[0]
        if op.type == X.X86_OP_IMM:
            return self.out.append(self.goto_or_leave(op.imm & 0xFFFFFFFF))
        target = self.read(insn, op, 4)
        table = self.fn.jump_tables.get(insn.address)
        if table:
            cases = []
            for t in sorted(set(table.targets)):
                cases.append('case %s: %s' % (h(t), self.goto_or_leave(t)))
            self.out.append('{ uint32_t t_ = %s; switch (t_) { %s default: SYNC(); call_indirect(c, t_); return; } }' % (
                target, ' '.join(cases)))
            return
        self.out.append('{ uint32_t t_ = %s; SYNC(); call_indirect(c, t_); return; }' % target)

    def emit_jcc(self, insn, cc):
        t = insn.operands[0].imm & 0xFFFFFFFF
        self.out.append('if (%s) { %s }' % (self.cond(insn, cc), self.goto_or_leave(t)))

    def i_jecxz(self, insn):
        self.out.append('if (ecx == 0) { %s }' % self.goto_or_leave(insn.operands[0].imm & 0xFFFFFFFF))

    def i_loop(self, insn):
        self.out.append('ecx -= 1; if (ecx) { %s }' % self.goto_or_leave(insn.operands[0].imm & 0xFFFFFFFF))

    def i_loope(self, insn):
        self.out.append('ecx -= 1; if (ecx && zf) { %s }' % self.goto_or_leave(insn.operands[0].imm & 0xFFFFFFFF))

    def i_loopne(self, insn):
        self.out.append('ecx -= 1; if (ecx && !zf) { %s }' % self.goto_or_leave(insn.operands[0].imm & 0xFFFFFFFF))

    # string operations --------------------------------------------------
    def emit_string(self, insn, m, prefix):
        op = m[:-1]
        size = {'b': 1, 'w': 2, 'd': 4}[m[-1]]
        ut = UT[size]
        ld = {1: 'ld8', 2: 'ld16', 4: 'ld32'}[size]
        st = {1: 'st8', 2: 'st16', 4: 'st32'}[size]
        acc = {1: '(uint8_t)eax', 2: '(uint16_t)eax', 4: 'eax'}[size]
        for o in insn.operands:
            if o.type == X.X86_OP_MEM and o.mem.segment and self.rn(insn, o.mem.segment) not in ('es', 'ds'):
                raise Unsupported('string operation with a segment override')
        step = 'uint32_t d_ = c.df ? 0x%Xu : %du;' % ((-size) & 0xFFFFFFFF, size)
        need = self.need(insn)
        if op == 'movs':
            body = '%s(M, edi, %s(M, esi)); esi += d_; edi += d_;' % (st, ld)
        elif op == 'stos':
            body = '%s(M, edi, (%s)%s); edi += d_;' % (st, ut, acc)
        elif op == 'lods':
            body = {1: 'eax = (eax & 0xFFFFFF00u) | ld8(M, esi);', 2: 'eax = (eax & 0xFFFF0000u) | ld16(M, esi);',
                    4: 'eax = ld32(M, esi);'}[size] + ' esi += d_;'
        elif op == 'cmps':
            body = 'a_ = %s(M, esi); b_ = %s(M, edi); esi += d_; edi += d_;' % (ld, ld)
        else:
            body = 'a_ = %s; b_ = %s(M, edi); edi += d_;' % (acc, ld)
        compare = op in ('cmps', 'scas')
        if prefix in (None, 'lock'):
            if compare:
                flags = self.flag_code(need, 'sub', 'a_', 'b_', 'r_', size)
                self.out.append('{ %s %s a_, b_; %s %s r_ = (%s)(a_ - b_); %s }' % (step, ut, body, ut, ut, ' '.join(flags)))
            else:
                self.out.append('{ %s %s }' % (step, body))
            return
        if prefix not in ('rep', 'repe', 'repne'):
            raise Unsupported('prefix %s on %s' % (prefix, m))
        if not compare:
            fast = ''
            if op == 'movs':
                fast = ('if (!c.df && (uint64_t)(uint32_t)(edi - esi) >= (uint64_t)ecx * %d) { uint32_t n_ = ecx * %du; '
                        'std::memmove(M + edi, M + esi, n_); esi += n_; edi += n_; ecx = 0; } ' % (size, size))
            elif op == 'stos' and size == 1:
                fast = 'if (!c.df) { std::memset(M + edi, (uint8_t)eax, ecx); edi += ecx; ecx = 0; } '
            self.out.append('{ %s %swhile (ecx) { %s ecx -= 1; } }' % (step, fast, body))
            return
        want = '0' if prefix == 'repne' else '1'
        flags = self.flag_code(need, 'sub', 'a_', 'b_', 'r_', size)
        self.out.append('{ %s %s a_ = 0, b_ = 0; bool ran_ = ecx != 0; while (ecx) { %s ecx -= 1; if ((a_ == b_) != %s) break; } '
                        'if (ran_) { %s r_ = (%s)(a_ - b_); %s } }' % (step, ut, body, want, ut, ut, ' '.join(flags) or '(void)r_;'))


# cdecl CRT routines replaced wholesale. The bodies are SSSE3 / SSE4.2
# dispatch (and, for the copy routines, jump tables laid over the instruction
# stream). Callers still enter f_<address>; that function forwards here.
# One instruction replaced by native code: (entry statement, site statement).
# The entry statement runs after PROLOGUE in the function that holds the site.
SITE_OVERRIDES = {}

# TRANSLATE_PRESERVE=1: callers reload only the registers a callee may change
# (preserved_regs, RELOAD_AFTER). Off by default.
import os as _os
PRESERVE_ANALYSIS = _os.environ.get('TRANSLATE_PRESERVE', '0') == '1'

INLINE_LIMITS = {}
COOKIE_CHECKS = {}

# Guest functions replaced by native host code, by program and function name
# (src/recomp/native.hpp). Each native function is a cdecl callee like the
# guest function it replaces.
OVERRIDES_BY_NAME = {
    'trek': {
        # The Dallas iButton key check (ibck.c) reads the keys through port
        # I/O; the port answers as an authenticated cabinet.
        'verify_ib': 'native_verify_ib',
        # The vblank counter came from the NVIDIA line of /proc/interrupts
        # and wait_for_vblank busy-waited on it; the host clock replaces both.
        'get_frame_number': 'native_get_frame_number',
        'wait_for_vblank': 'native_wait_for_vblank',
        'read_cpu_MHz': 'native_read_cpu_mhz',
        'get_cpu_mhz': 'native_get_cpu_mhz',
        # The MegaJamma board's raw gun samples, calibrated: the port's
        # device hands over game coordinates directly (host/natives.cpp).
        'get_gun_calibrated_x_y': 'native_get_gun_calibrated_x_y',
    },
    'stvserv': {
        'get_frame_number': 'native_get_frame_number',
        'wait_for_vblank': 'native_wait_for_vblank',
        'get_cpu_mhz': 'native_get_cpu_mhz',
        'get_gun_calibrated_x_y': 'native_get_gun_calibrated_x_y',
    },
    'stvsnd': {},
}
OVERRIDES = {}


class Translator:
    def __init__(self, image, data, flags_live_at_exit=False):
        """`data` is discovery's functions.json content. flags_live_at_exit (tests
        only) keeps every flag live at ret so all of them reach the Cpu."""
        self.img = image
        self.cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self.cs.detail = True
        self.data = data
        self.flags_live_at_exit = flags_live_at_exit
        self.flags_returned = set()  # see find_flag_returning
        self.inline_cache = {}
        self.entry_reads_flags = set()
        self.preserves = {}  # entry -> registers it returns unchanged (preserved_regs)
        self.imports = image.imports()
        self.functions = {fn['entry']: fn for fn in data['functions']}
        self.entries = set(self.functions)
        self.walker = Walker(image)
        self.walker.function_starts.update(self.entries)
        self.walker.noreturn.update(fn['entry'] for fn in data['functions'] if fn.get('noreturn'))
        self.walker.noreturn.update(data.get('noreturn_imports', []))

    INLINE_MAX = 30
    INLINE_BIGGEST = 300  # the largest INLINE_LIMITS allows

    def inline_body(self, entry, limit=INLINE_MAX):
        """The body of `entry` to inline at a direct call, or None.

        A call writes every register back to the Cpu and reads them again, and
        so does the callee; for the engine's small helpers (reference-counted
        pointers, accessors) that was most of their cost. Inlined: small
        functions that read no flags on entry, return none, have no indirect
        jump and no native override. The return address is still pushed and
        popped, so the guest stack is the same."""
        if entry in self.inline_cache:
            got = self.inline_cache[entry]
            return got[:2] if got is not None and got[2] <= limit else None
        self.inline_cache[entry] = None
        edition = getattr(self.img, 'edition', None)
        if (entry not in self.functions or entry in self.flags_returned or self.flags_live_at_exit
                or entry in OVERRIDES.get(edition, {}) or entry in COOKIE_CHECKS.get(edition, {})
                or self.functions[entry].get('noreturn')):
            return None
        fn = self.walker.walk(entry)
        if (not fn.insns or len(fn.insns) > self.INLINE_BIGGEST or fn.jump_tables or fn.indirect_jumps or fn.undecodable
                or any(va in SITE_OVERRIDES.get(edition, {}) for va in fn.insns)):
            return None
        mnems = [i.mnem for i in fn.insns.values()]
        if 'ret' not in mnems:
            return None
        ft = FunctionTranslator(self, fn)
        ft.inlined = True
        try:
            ft.decode()
            if any(i.mnemonic.split()[-1] == 'jmp' and i.operands and i.operands[0].type != X.X86_OP_IMM
                   for i in ft.insns.values()):
                return None
            lines = ft.translate()
        except Unsupported:
            return None
        if ft.trapped or ft.live_in.get(entry, 0) & F.ALL:
            return None
        self.inline_cache[entry] = (lines, ft.refs, len(fn.insns))
        return (lines, ft.refs) if len(fn.insns) <= limit else None

    def translate(self, entry):
        fn = self.walker.walk(entry)
        ft = FunctionTranslator(self, fn)
        text = ft.translate()
        if ft.live_in.get(entry, F.ALL) & F.ALL:
            self.entry_reads_flags.add(entry)
        if PRESERVE_ANALYSIS:
            self.preserves[entry] = ft.preserved_regs()
        return text, ft.refs, len(fn.insns), ft.trapped

    SYNC_TO = re.compile(r'\bSYNC_TO\(0x([0-9A-F]+)u\)')

    def resolve_sync_to(self, text, translated):
        """SYNC_TO(target) becomes SYNC_R() when `target` is a function this run
        translated and its entry reads no flags; SYNC() otherwise."""
        def one(m):
            t = int(m.group(1), 16)
            return 'SYNC_R()' if t in translated and t not in self.entry_reads_flags else 'SYNC()'

        def after(m):
            t, flags = int(m.group(1), 16), int(m.group(2))
            kept = self.preserves.get(t, 0) if t in translated else 0
            mask = (15 & ~kept) | (16 if flags else 0)
            return 'RELOAD()' if mask == 31 else 'RELOAD_M(%d)' % mask
        return self.RELOAD_AFTER.sub(after, self.SYNC_TO.sub(one, text))

    RELOAD_AFTER = re.compile(r'\bRELOAD_AFTER\(0x([0-9A-F]+)u, ([01])\)')


def find_flag_returning(tr, entries):
    """Functions whose flags at ret a caller reads.

    Flags normally die at ret. Some compiler and CRT helpers return a result in
    them: 0xC630C8 ends in `cmp eax, 0x7ff00000; ret`, and the CRT sqrt wrapper
    then calls 0xC398FD, whose first branch (`je`) reads that compare. With the
    flags dropped at ret the branch read stale flags and sqrt returned its input.

    For every direct call, the instructions after it are scanned in a straight
    line: a flag read before anything writes it, or a call to a function that
    reads flags on entry, makes the callee flag-returning. So are the functions
    it tail-jumps to, whose ret is its ret."""
    cs = tr.cs
    entry_reads = {}

    def decode(va):
        if not tr.img.contains(va, 16):
            return None
        return next(cs.disasm(tr.img.bytes(va, 16), va, 1), None)

    def reads_before_writing(va, depth):
        defined = 0
        for _ in range(16):
            insn = decode(va)
            if insn is None:
                return False
            m = insn.mnemonic.split()[-1]
            if m == 'call':
                op = insn.operands[0] if insn.operands else None
                if op is not None and op.type == X.X86_OP_IMM and depth < 2:
                    t = op.imm & 0xFFFFFFFF
                    if t not in entry_reads:
                        entry_reads[t] = reads_before_writing(t, depth + 1)
                    return entry_reads[t]
                return False
            d, u = F.flags_of(m, True)
            if u & ~defined:
                return True
            defined |= d
            if defined == F.ALL or m in ('ret', 'retf', 'jmp', 'int3', 'ud2', 'hlt'):
                return False
            va += insn.size
        return False

    marked = set()
    for entry in entries:
        fn = tr.walker.walk(entry)
        for va in sorted(fn.insns):
            insn = fn.insns[va]  # cfg.Insn: va, size, mnem, ops
            if insn.mnem != 'call':
                continue
            target = insn.direct_target()
            if target is None or target in marked or target not in tr.functions:
                continue
            if reads_before_writing(insn.next, 0):
                marked.add(target)
    # A function's ret may be in a piece it tail-jumps to.
    todo = list(marked)
    while todo:
        f = tr.functions.get(todo.pop())
        for t in (f or {}).get('tail_calls', []):
            if t not in marked and t in tr.functions:
                marked.add(t)
                todo.append(t)
    return marked


STUB = 'void f_%08X(Cpu& c) { guest_unported(c, 0x%08Xu, "%s"); }'
HEADER = '// Generated by tools/x86tr/translate.py from the player\'s executable. Do not edit.\n#include "recomp/guest.hpp"\n'

# Functions are emitted into shards of about SHARD_FUNCTIONS each, in address order.
SHARD_FUNCTIONS = 120
SHARD_STARTS = ()


def shard_start(entry):
    i = bisect.bisect_right(SHARD_STARTS, entry) - 1
    return SHARD_STARTS[i if i >= 0 else 0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cache')
    ap.add_argument('functions')
    ap.add_argument('out')
    ap.add_argument('--all', action='store_true', help='translate unreachable functions too')
    ap.add_argument('--limit', type=int, default=0, help='translate only the first N selected functions (testing)')
    args = ap.parse_args()
    t0 = time.time()
    global SHARD_STARTS
    with open(args.functions) as f:
        data = json.load(f)
    img = Image(args.cache, data.get('edition'))
    names = {fn['name']: fn['entry'] for fn in data['functions']}
    OVERRIDES[img.edition] = {names[n]: native for n, native in OVERRIDES_BY_NAME.get(img.edition, {}).items()}
    tr = Translator(img, data)
    starts = sorted(tr.functions)
    SHARD_STARTS = tuple(starts[i] for i in range(0, len(starts), SHARD_FUNCTIONS))
    os.makedirs(args.out, exist_ok=True)
    selected = [e for e in sorted(tr.functions) if args.all or tr.functions[e].get('reachable')]
    if args.limit:
        selected = selected[:args.limit]
    chosen = set(selected)
    tr.flags_returned = find_flag_returning(tr, selected)
    print('%d functions return flags to their callers (%.0fs)' % (len(tr.flags_returned), time.time() - t0), flush=True)

    reasons = collections.Counter()
    samples = collections.defaultdict(list)
    bodies = {}
    refs_of = {}
    translated = failed = insns_done = 0

    def bucket(entry):
        start = shard_start(entry)
        if start not in bodies:
            bodies[start] = []
            refs_of[start] = set()
        return start

    stubs = []
    translated_set = set()
    trap_reasons = collections.Counter()
    trap_samples = collections.defaultdict(list)
    trap_sites = []
    trapped_total = 0
    overridden = []
    edition_overrides = OVERRIDES.get(tr.img.edition, {})
    for k, entry in enumerate(selected):
        start = bucket(entry)
        native = edition_overrides.get(entry)
        if native:
            bodies[start].append('void f_%08X(Cpu& c) { %s(c); }' % (entry, native))
            translated += 1
            overridden.append((entry, native))
            if k % 5000 == 0:
                print('%d / %d functions, %.0fs' % (k, len(selected), time.time() - t0), flush=True)
            continue
        try:
            text, refs, n, trapped = tr.translate(entry)
            for va, reason in trapped:
                trap_reasons[reason] += 1
                trapped_total += 1
                trap_sites.append((va, reason))
                if len(trap_samples[reason]) < 5:
                    trap_samples[reason].append('%08X' % va)
        except Unsupported as e:
            reason = str(e)
            reasons[reason] += 1
            if len(samples[reason]) < 5:
                samples[reason].append('%08X' % entry)
            stubs.append(STUB % (entry, entry, ('unsupported: ' + reason).replace('"', "'")))
            failed += 1
            continue
        bodies[start].append(text)
        translated_set.add(entry)
        refs_of[start] |= refs
        translated += 1
        insns_done += n
        if k % 5000 == 0:
            print('%d / %d functions, %.0fs' % (k, len(selected), time.time() - t0), flush=True)
    # Each edition's functions live in their own namespace and its table is
    # translated_entries_<edition>() (recomp/generated.hpp), so one app can
    # link both editions and pick one at start.
    edition = tr.img.edition
    ns = 'voy_%s' % edition
    ns_open, ns_close = 'namespace %s {\n\n' % ns, '\n}  // namespace %s\n' % ns
    shards = []
    for start in sorted(bodies):
        if not bodies[start]:
            continue
        name = 'tr_%08X.cpp' % start
        decls = ['void f_%08X(Cpu&);' % r for r in sorted(refs_of[start])]
        body = tr.resolve_sync_to('\n\n'.join(bodies[start]), translated_set)
        write_if_changed(os.path.join(args.out, name), HEADER + ns_open + '\n'.join(decls) + '\n\n' + body + '\n' + ns_close)
        shards.append(name)
    keep = set(shards)
    for name in os.listdir(args.out):
        if name.startswith('tr_') and name.endswith('.cpp') and name not in keep:
            os.remove(os.path.join(args.out, name))
    for entry in sorted(tr.entries - chosen):
        stubs.append(STUB % (entry, entry, 'not reachable from any root'))
    write_if_changed(os.path.join(args.out, 'stubs.cpp'), HEADER + ns_open + '\n'.join(stubs) + '\n' + ns_close)
    entries = sorted(tr.entries)
    table = [HEADER, '#include <span>\n', ns_open]
    table += ['void f_%08X(Cpu&);' % e for e in entries]
    table.append(ns_close + '\nnamespace voy::recomp {\n\nstatic const GuestEntry k_entries[] = {')
    table += ['    {0x%08Xu, %s::f_%08X},' % (e, ns, e) for e in entries]
    table.append('};\n\nstd::span<const GuestEntry> translated_entries_%s() { return k_entries; }\n\n}' % edition)
    write_if_changed(os.path.join(args.out, 'entries.cpp'), '\n'.join(table) + '\n')
    report = {
        'edition': tr.data.get('edition'),
        'selected': len(selected), 'translated': translated, 'unsupported': failed,
        'translated_instructions': insns_done, 'stubs': len(stubs), 'shards': shards,
        'reasons': [{'reason': r, 'functions': n, 'samples': samples[r]} for r, n in reasons.most_common()],
        'overrides': [{'address': '%08X' % e, 'native': name} for e, name in overridden],
        'trapped_instructions': trapped_total,
        'trap_reasons': [{'reason': r, 'instructions': n, 'samples': trap_samples[r]} for r, n in trap_reasons.most_common()],
        'seconds': round(time.time() - t0, 1),
    }
    allow_errors = []
    with open(os.path.join(args.out, 'coverage.json'), 'w') as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(args.out, 'sources.txt'), 'w') as f:
        f.write('\n'.join(shards + ['stubs.cpp', 'entries.cpp']) + '\n')
    print('translated %d / %d functions (%d instructions), %d native overrides, %d unsupported, %d shards, %.0fs' % (
        translated, len(selected), insns_done, len(overridden), failed, len(shards), time.time() - t0))
    for r, n in reasons.most_common(40):
        print('  %6d  %s  e.g. %s' % (n, r, ' '.join(samples[r][:3])))
    print('%d instructions fail closed if reached:' % trapped_total)
    for r, n in trap_reasons.most_common(40):
        print('  %6d  %s  e.g. %s' % (n, r, ' '.join(trap_samples[r][:3])))
    if allow_errors:
        print('trap allow-list mismatch:', file=sys.stderr)
        for line in allow_errors:
            print('  ' + line, file=sys.stderr)
        sys.exit(1)


def write_if_changed(path, text):
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    tmp = path + '.partial'
    with open(tmp, 'w') as f:
        f.write(text)
    os.replace(tmp, path)


if __name__ == '__main__':
    main()
