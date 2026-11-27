"""Function bodies by recursive descent over the guest image.

Shared by discovery and the translator so both agree on what a function is:
which instructions belong to it, where its jump tables point, which jumps
leave it (tail calls) and which calls never return.
"""
import re

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

HEX = re.compile(r'0x[0-9a-f]+')
# jmp dword ptr [reg*4 + table]
JUMP_TABLE = re.compile(r'^dword ptr \[(e[a-z]{2})\*4 \+ (0x[0-9a-f]+)\]$')
# movzx reg, byte ptr [reg2 + table]
BYTE_INDEX = re.compile(r'^(e[a-z]{2}), byte ptr \[(e[a-z]{2}) \+ (0x[0-9a-f]+)\]$')
CMP_IMM = re.compile(r'^(e[a-z]{2}), (0x[0-9a-f]+|\d+)$')

CONDITIONAL = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp', 'jl', 'jge', 'jle', 'jg',
               'jecxz', 'jcxz', 'loop', 'loope', 'loopne'}
STOPS = {'ret', 'retf', 'int3', 'hlt', 'ud2', 'iretd'}
MAX_TABLE = 1024


THREAD_NAME_EXCEPTION = '0x406d1388'


class Insn:
    __slots__ = ('va', 'size', 'mnem', 'ops')

    def __init__(self, va, size, mnem, ops):
        self.va, self.size, self.mnem, self.ops = va, size, mnem, ops

    @property
    def next(self):
        return self.va + self.size

    def direct_target(self):
        """Target of a direct jmp / jcc / call, else None."""
        if self.ops.startswith('0x') and ' ' not in self.ops:
            return int(self.ops, 16)
        return None


class Decoder:
    def __init__(self, image):
        self.image = image
        self.cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self.cache = {}

    def at(self, va):
        insn = self.cache.get(va)
        if insn is None and va not in self.cache:
            insn = None
            if self.image.in_text(va):
                for a, size, mnem, ops in self.cs.disasm_lite(self.image.bytes(va, 16), va, 1):
                    # capstone prints prefixes into the mnemonic ("rep stosd", "lock xadd")
                    insn = Insn(a, size, mnem, ops)
            self.cache[va] = insn
        return insn


class JumpTable:
    __slots__ = ('va', 'index_table', 'targets', 'bounded')

    def __init__(self, va, targets, bounded, index_table=None):
        self.va, self.targets, self.bounded, self.index_table = va, targets, bounded, index_table


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}          # va -> Insn
        self.callees = set()     # direct call targets
        self.tail_calls = set()  # direct jumps to other functions
        self.jump_tables = {}    # va of the jmp -> JumpTable
        self.indirect_jumps = []  # vas of jumps the walker could not resolve
        self.undecodable = []    # vas where decoding failed inside the body
        self.noreturn_calls = set()

    @property
    def size(self):
        if not self.insns:
            return 0
        return max(i.next for i in self.insns.values()) - self.entry


def _imm(text):
    return int(text, 16) if text.startswith('0x') else int(text)


class Walker:
    def __init__(self, image, decoder=None):
        self.image = image
        self.dec = decoder or Decoder(image)
        self.function_starts = set()   # known entries: jumps there are tail calls
        self.noreturn = set()          # functions (or IAT slots) known never to return
        self.data_in_text = set()      # bytes of jump / index tables inside .text
        self._sorted_starts = None
        self._sorted_count = 0

    def _bound(self, trace, reg):
        """Entries of a table indexed by `reg`.

        `cmp reg, N` followed by an unsigned above-branch allows 0..N, so N + 1 entries.
        A `jb` (below) only reaches the table for 0..N-1; the extra slot is dropped later,
        when it is not a code address. `and reg, M` with M a small 2^n-1 mask leaves 0..M.
        """
        for insn in reversed(trace[-12:]):
            if insn.mnem == 'cmp':
                m = CMP_IMM.match(insn.ops)
                if m and m.group(1) == reg:
                    return _imm(m.group(2)) + 1
            if insn.mnem == 'and':
                m = CMP_IMM.match(insn.ops)
                if m and m.group(1) == reg:
                    mask = _imm(m.group(2))
                    if 0 < mask <= 31 and mask & (mask + 1) == 0:
                        return mask + 1
            if insn.mnem in ('mov', 'movzx', 'lea', 'add', 'sub', 'dec', 'inc') and insn.ops.startswith(reg + ','):
                # the index was rewritten; a bound on the old value no longer applies
                m = BYTE_INDEX.match(insn.ops) if insn.mnem == 'movzx' else None
                if not m:
                    if insn.mnem in ('dec', 'inc', 'add', 'sub'):
                        continue
                    return None
        return None

    def _jump_table(self, insn, trace):
        m = JUMP_TABLE.match(insn.ops)
        if not m:
            return None
        reg, table = m.group(1), int(m.group(2), 16)
        img = self.image
        count = self._bound(trace, reg)
        index_table = None
        # two-level form: movzx reg, byte ptr [idx + bytes]; jmp [reg*4 + table]
        for prev in reversed(trace[-6:]):
            if prev.mnem == 'movzx':
                bm = BYTE_INDEX.match(prev.ops)
                if bm and bm.group(1) == reg:
                    n = self._bound(trace[:trace.index(prev)], bm.group(2))
                    bytes_va = int(bm.group(3), 16)
                    if n and img.contains(bytes_va, n):
                        index_table = (bytes_va, n)
                        count = max(img.bytes(bytes_va, n)) + 1
                    break
        bounded = count is not None
        if not bounded:
            count = MAX_TABLE
        targets = []
        for k in range(count):
            va = table + 4 * k
            if not img.contains(va, 4):
                break
            t = img.u32(va)
            if not img.in_text(t):
                # The cmp bound is one high when the branch into the table is `jb` (ecx < N)
                # rather than `ja` (ecx > N). Only the final slot is spare; an earlier
                # non-code word means this is not a table.
                if bounded and targets and k == count - 1:
                    break
                if bounded:
                    return JumpTable(table, None, True)
                break
            targets.append(t)
        if not targets:
            return None
        if img.in_text(table):
            self.data_in_text.update(range(table, table + 4 * len(targets)))
            if index_table:
                self.data_in_text.update(range(index_table[0], index_table[0] + index_table[1]))
        return JumpTable(table, targets, bounded, index_table)

    def _call_returns(self, insn, trace=()):
        t = insn.direct_target()
        if t is not None and t in self.noreturn:
            return False
        m = re.match(r'^dword ptr \[(0x[0-9a-f]+)\]$', insn.ops)
        if m and int(m.group(1), 16) in self.noreturn:
            # RaiseException(0x406D1388) names a thread for a debugger. The caller's
            # own __except swallows it, so the call returns.
            if any(i.mnem == 'push' and i.ops == THREAD_NAME_EXCEPTION for i in trace[-8:]):
                return True
            return False
        # MSVC pads after calls that never return; a call right before int3 padding is one.
        nxt = insn.next
        if self.image.in_text(nxt) and self.image.u8(nxt) == 0xCC and nxt not in self.function_starts:
            return False
        return True

    def _epilog_past_thunk(self, limit, target):
        """True when every function start in [limit, target) is a tiny function
        that returns before target. MSVC plants an SEH cleanup between a body
        and its epilog, and that cleanup is a call target, so it becomes the
        walk limit and the epilog would otherwise be left out of the function."""
        import bisect
        starts = self._sorted_starts
        i = bisect.bisect_left(starts, limit)
        if i >= len(starts) or starts[i] >= target:
            return False
        while i < len(starts) and starts[i] < target:
            thunk = starts[i]
            va = thunk
            saw_ret = False
            for _ in range(10):
                insn = self.dec.at(va)
                if insn is None or va >= target:
                    return False
                if insn.mnem.startswith('ret'):
                    saw_ret = insn.va - thunk <= 32
                    break
                va = insn.next
            if not saw_ret:
                return False
            i += 1
        return True

    def _leaves(self, entry, target, limit):
        """A direct jump that leaves the function: to a known start, before the entry,
        at or past the next known start, or onto int3 alignment padding (a function start).
        A jump past a tiny SEH thunk to this function's own epilog does not leave."""
        if target == entry:
            return False
        if target in self.function_starts or target < entry:
            return True
        if limit is not None and target >= limit:
            return not self._epilog_past_thunk(limit, target)
        return self.image.in_text(target - 1) and self.image.u8(target - 1) == 0xCC

    def walk(self, entry):
        import bisect
        if self._sorted_starts is None or self._sorted_count != len(self.function_starts):
            self._sorted_starts = sorted(self.function_starts)
            self._sorted_count = len(self.function_starts)
        i = bisect.bisect_right(self._sorted_starts, entry)
        limit = self._sorted_starts[i] if i < len(self._sorted_starts) else None
        return self._walk(entry, limit)

    def _walk(self, entry, limit):
        fn = Function(entry)
        work = [(entry, [])]
        dec = self.dec
        while work:
            va, trace = work.pop()
            trace = list(trace)
            while True:
                if va in fn.insns:
                    break
                if va != entry and va in self.function_starts:
                    # fell through into another function: treat as a tail call
                    fn.tail_calls.add(va)
                    break
                insn = dec.at(va)
                if insn is None:
                    fn.undecodable.append(va)
                    break
                fn.insns[va] = insn
                trace.append(insn)
                m = insn.mnem
                if m in STOPS or m.startswith('ret') or m.startswith('rep ret') or m == 'bnd ret':
                    break
                if m == 'jmp':
                    t = insn.direct_target()
                    if t is not None:
                        if self._leaves(entry, t, limit):
                            fn.tail_calls.add(t)
                        elif self.image.in_text(t):
                            work.append((t, trace[-12:]))
                        break
                    table = self._jump_table(insn, trace)
                    if table and table.targets:
                        fn.jump_tables[va] = table
                        for t in table.targets:
                            work.append((t, []))
                    else:
                        fn.indirect_jumps.append(va)
                    break
                if m in CONDITIONAL:
                    t = insn.direct_target()
                    if t is not None and self.image.in_text(t):
                        if self._leaves(entry, t, limit):
                            fn.tail_calls.add(t)
                        else:
                            work.append((t, trace[-12:]))
                    va = insn.next
                    continue
                if m == 'call':
                    t = insn.direct_target()
                    if t is not None:
                        fn.callees.add(t)
                    if not self._call_returns(insn, trace):
                        fn.noreturn_calls.add(va)
                        break
                va = insn.next
        return fn
