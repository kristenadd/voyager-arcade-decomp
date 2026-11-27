"""Flag definitions and uses per x86 mnemonic, and condition codes.

Flags: C(arry) P(arity) A(uxiliary) Z(ero) S(ign) O(verflow). "Defines"
includes flags the manual calls undefined after the instruction: nothing may
read them, so they are dead before it.
"""


class Unsupported(Exception):
    """An instruction or operand form the translator does not handle."""


C, P, A, Z, S, O = 1, 2, 4, 8, 16, 32
ALL = C | P | A | Z | S | O
NAMES = {C: 'cf', P: 'pf', A: 'af', Z: 'zf', S: 'sf', O: 'of'}

CONDITIONS = {
    'o': (O, 'of'), 'no': (O, '!of'),
    'b': (C, 'cf'), 'ae': (C, '!cf'),
    'e': (Z, 'zf'), 'ne': (Z, '!zf'),
    'be': (C | Z, '(cf | zf)'), 'a': (C | Z, '!(cf | zf)'),
    's': (S, 'sf'), 'ns': (S, '!sf'),
    'p': (P, 'pf'), 'np': (P, '!pf'),
    'l': (S | O, '(sf != of)'), 'ge': (S | O, '(sf == of)'),
    'le': (Z | S | O, '(zf | (sf != of))'), 'g': (Z | S | O, '(!zf & (sf == of))'),
}

DEF_ALL = {'add', 'sub', 'cmp', 'neg', 'and', 'or', 'xor', 'test', 'mul', 'imul', 'div', 'idiv', 'bsf', 'bsr',
           'comiss', 'ucomiss', 'comisd', 'ucomisd', 'fcomi', 'fcomip', 'fucomi', 'fucomip', 'popfd', 'popal',
           'xadd', 'cmpxchg', 'cmpxchg8b', 'tzcnt', 'lzcnt', 'popcnt', 'ptest'}
USE_C_DEF_ALL = {'adc', 'sbb'}
DEF_NOT_C = {'inc', 'dec'}
BIT_TEST = {'bt', 'bts', 'btr', 'btc'}           # CF, others undefined, ZF kept
SHIFTS = {'shl', 'sal', 'shr', 'sar', 'shld', 'shrd'}
ROTATES = {'rol', 'ror'}
ROTATES_C = {'rcl', 'rcr'}


def cc_of(mnem):
    """Condition suffix of jcc / setcc / cmovcc / fcmovcc, normalised, else None."""
    alias = {'c': 'b', 'nae': 'b', 'nb': 'ae', 'nc': 'ae', 'z': 'e', 'nz': 'ne', 'na': 'be', 'nbe': 'a',
             'pe': 'p', 'po': 'np', 'nge': 'l', 'nl': 'ge', 'ng': 'le', 'nle': 'g'}
    for prefix in ('cmov', 'set', 'j'):
        if mnem.startswith(prefix):
            cc = mnem[len(prefix):]
            cc = alias.get(cc, cc)
            if cc in CONDITIONS:
                return cc
    return None


def flags_of(insn, count_is_imm_nonzero):
    """(defined, used) flag masks of one instruction (capstone mnemonic, prefixes stripped)."""
    m = insn
    if m in DEF_ALL:
        return ALL, 0
    if m in USE_C_DEF_ALL:
        return ALL, C
    if m in DEF_NOT_C:
        return P | A | Z | S | O, 0
    if m in BIT_TEST:
        return C | P | A | S | O, 0
    if m in SHIFTS:
        # a zero count leaves the flags alone: a count in CL may be zero
        return (ALL, 0) if count_is_imm_nonzero else (ALL, ALL)
    if m in ROTATES:
        return (C | O, 0) if count_is_imm_nonzero else (C | O, C | O)
    if m in ROTATES_C:
        return (C | O, C) if count_is_imm_nonzero else (C | O, C | O)
    if m in ('stc', 'clc'):
        return C, 0
    if m == 'cmc':
        return C, C
    if m == 'sahf':
        return C | P | A | Z | S, 0
    if m == 'lahf':
        return 0, C | P | A | Z | S
    if m in ('pushfd', 'pushal'):
        return 0, ALL
    if m in ('call',):
        return ALL, 0
    if m.startswith('rep') and any(m.endswith(s) for s in ('cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd')):
        return ALL, ALL  # nothing happens when ecx is 0
    if m in ('cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd'):
        return ALL, 0
    if m.startswith('fcmov'):
        cc = {'fcmovb': 'b', 'fcmovnb': 'ae', 'fcmove': 'e', 'fcmovne': 'ne', 'fcmovbe': 'be', 'fcmovnbe': 'a',
              'fcmovu': 'p', 'fcmovnu': 'np'}.get(m)
        return 0, CONDITIONS[cc][0] if cc else ALL
    cc = cc_of(m)
    if cc:
        return 0, CONDITIONS[cc][0]
    return 0, 0
