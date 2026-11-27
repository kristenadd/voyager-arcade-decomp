"""x87 instructions, decoded from the opcode and ModRM bytes.

Mnemonics are not trusted for the register forms: assemblers and
disassemblers disagree on which of fsub / fsubr (fdiv / fdivr) names the
DC / DE E0..FF encodings, so the operation comes from the Intel encoding
tables directly. ST registers hold doubles (see recomp/cpu.hpp).
"""
import os

from capstone import x86 as X

from semantics import Unsupported

# op(a, b) by ModRM.reg for D8 (st0 = st0 op src) and the memory forms
ARITH = {0: 'x86_nan({a} + {b}, {a}, {b})', 1: 'x86_nan({a} * {b}, {a}, {b})', 4: 'x86_nan({a} - {b}, {a}, {b})',
         5: 'x86_nan({b} - {a}, {a}, {b})', 6: 'x86_nan({a} / {b}, {a}, {b})', 7: 'x86_nan({b} / {a}, {a}, {b})'}
# DC / DE register forms write ST(i) = ST(i) op ST(0) with the sub / div senses swapped
ARITH_REV = {0: ARITH[0], 1: ARITH[1], 4: ARITH[5], 5: ARITH[4], 6: ARITH[7], 7: ARITH[6]}
CONSTANTS = {0xE8: '1.0', 0xE9: '3.321928094887362347870319429489390175864831393', 0xEA: '1.442695040888963407359924681001892137426645954',
             0xEB: '3.141592653589793238462643383279502884197169399', 0xEC: '0.301029995663981195213738894724493026768189881',
             0xED: '0.693147180559945309417232121458176568075500134', 0xEE: '0.0'}
UNARY = {0xE0: '-{x}', 0xE1: 'std::fabs({x})', 0xFA: 'x86_nan(std::sqrt({x}), {x})', 0xFC: 'fpu_round(c.fpu, {x})',
         0xF0: 'std::exp2({x}) - 1.0', 0xFE: 'std::sin({x})', 0xFF: 'std::cos({x})'}
FCMOV = {(0xDA, 0): 'cf', (0xDA, 1): 'zf', (0xDA, 2): '(cf | zf)', (0xDA, 3): 'pf',
         (0xDB, 0): '!cf', (0xDB, 1): '!zf', (0xDB, 2): '!(cf | zf)', (0xDB, 3): '!pf'}


def st(i):
    return 'c.fpu.at(%d)' % i


# Debugging aid: DCUSA_NAN_TRAP=<lo>-<hi> (hex guest addresses) at translation
# time makes every x87 store to memory in that range report, once per
# instruction, the first NaN it writes (nan_trap in recomp/guest.hpp). Unset,
# the output is unchanged.
def _nan_trap_range():
    spec = os.environ.get('DCUSA_NAN_TRAP', '')
    if '-' not in spec:
        return None
    lo, hi = spec.split('-', 1)
    return int(lo, 16), int(hi, 16)


NAN_TRAP = _nan_trap_range()
# The memory stores that write ST(0) as a float: fst / fstp m32, m64, m80.
NAN_TRAP_FORMS = {(0xD9, 2), (0xD9, 3), (0xDD, 2), (0xDD, 3), (0xDB, 7)}


class X87Mixin:
    def emit_x87(self, insn, m):
        if m in ('wait', 'fwait'):
            return True
        op = insn.opcode[0]
        if not 0xD8 <= op <= 0xDF:
            return False
        modrm = insn.modrm
        reg = (modrm >> 3) & 7
        o = self.out
        if modrm >= 0xC0:
            i = modrm & 7
            return self.x87_register_form(insn, op, reg, i, modrm)
        mem = next(x for x in insn.operands if x.type == X.X86_OP_MEM)
        a = self.addr(insn, mem)
        if op in (0xD8, 0xDC, 0xDA, 0xDE):
            src = {0xD8: '(double)ldf32(M, %s)', 0xDC: 'ldf64(M, %s)', 0xDA: '(double)(int32_t)ld32(M, %s)',
                   0xDE: '(double)(int16_t)ld16(M, %s)'}[op] % a
            if reg in (2, 3):
                o.append('fpu_compare(c.fpu, %s, %s);' % (st(0), src))
                if reg == 3:
                    o.append('c.fpu.pop();')
            else:
                o.append('%s = %s;' % (st(0), ARITH[reg].format(a=st(0), b=src)))
            return True
        forms = {
            (0xD9, 0): 'c.fpu.push((double)ldf32(M, {a}));',
            (0xD9, 2): 'stf32(M, {a}, (float){s0});',
            (0xD9, 3): 'stf32(M, {a}, (float){s0}); c.fpu.pop();',
            (0xD9, 4): 'fpu_load_env(M, {a}, c.fpu);',
            (0xD9, 5): 'c.fpu.control = ld16(M, {a});',
            (0xD9, 6): 'fpu_store_env(M, {a}, c.fpu);',
            (0xD9, 7): 'st16(M, {a}, c.fpu.control);',
            (0xDB, 0): 'c.fpu.push((double)(int32_t)ld32(M, {a}));',
            (0xDB, 1): 'st32(M, {a}, (uint32_t)cvt_trunc_i32({s0})); c.fpu.pop();',
            (0xDB, 2): 'st32(M, {a}, (uint32_t)fpu_to_i32(c.fpu, {s0}));',
            (0xDB, 3): 'st32(M, {a}, (uint32_t)fpu_to_i32(c.fpu, {s0})); c.fpu.pop();',
            (0xDB, 5): 'c.fpu.push(ld80(M, {a}));',
            (0xDB, 7): 'st80(M, {a}, {s0}); c.fpu.pop();',
            (0xDD, 0): 'c.fpu.push(ldf64(M, {a}));',
            (0xDD, 1): 'st64(M, {a}, (uint64_t)cvt_trunc_i64({s0})); c.fpu.pop();',
            (0xDD, 2): 'stf64(M, {a}, {s0});',
            (0xDD, 3): 'stf64(M, {a}, {s0}); c.fpu.pop();',
            (0xDD, 4): 'fpu_restore(M, {a}, c.fpu);',
            (0xDD, 6): 'fpu_save(M, {a}, c.fpu);',
            (0xDD, 7): 'st16(M, {a}, c.fpu.status_word());',
            (0xDF, 0): 'c.fpu.push((double)(int16_t)ld16(M, {a}));',
            (0xDF, 1): 'st16(M, {a}, (uint16_t)fpu_trunc_i16({s0})); c.fpu.pop();',
            (0xDF, 2): 'st16(M, {a}, (uint16_t)fpu_to_i16(c.fpu, {s0}));',
            (0xDF, 3): 'st16(M, {a}, (uint16_t)fpu_to_i16(c.fpu, {s0})); c.fpu.pop();',
            (0xDF, 5): 'c.fpu.push((double)(int64_t)ld64(M, {a}));',
            (0xDF, 7): 'st64(M, {a}, (uint64_t)fpu_to_i64(c.fpu, {s0})); c.fpu.pop();',
        }
        form = forms.get((op, reg))
        if form is None:
            raise self.unsupported('x87 %s (%02X /%d)' % (m, op, reg))
        if NAN_TRAP and (op, reg) in NAN_TRAP_FORMS and NAN_TRAP[0] <= insn.address < NAN_TRAP[1]:
            o.append('nan_trap(c, 0x%08Xu, %s);' % (insn.address, st(0)))
        o.append(form.format(a=a, s0=st(0)))
        return True

    def unsupported(self, what):
        return Unsupported(what)

    def x87_register_form(self, insn, op, reg, i, modrm):
        o = self.out
        if op == 0xD8:
            if reg in (2, 3):
                o.append('fpu_compare(c.fpu, %s, %s);' % (st(0), st(i)))
                if reg == 3:
                    o.append('c.fpu.pop();')
            else:
                o.append('%s = %s;' % (st(0), ARITH[reg].format(a=st(0), b=st(i))))
            return True
        if op in (0xDC, 0xDE):
            if op == 0xDE and modrm == 0xD9:
                o.append('fpu_compare(c.fpu, %s, %s); c.fpu.pop(); c.fpu.pop();' % (st(0), st(1)))
                return True
            if reg in (2, 3):
                raise self.unsupported('x87 alias encoding %02X %02X' % (op, modrm))
            o.append('%s = %s;' % (st(i), ARITH_REV[reg].format(a=st(i), b=st(0))))
            if op == 0xDE:
                o.append('c.fpu.pop();')
            return True
        if op in (0xDA, 0xDB) and reg < 4:
            o.append('if (%s) %s = %s;' % (FCMOV[(op, reg)], st(0), st(i)))
            return True
        if op == 0xDA and modrm == 0xE9:
            o.append('fpu_compare(c.fpu, %s, %s); c.fpu.pop(); c.fpu.pop();' % (st(0), st(1)))
            return True
        if op in (0xDB, 0xDF) and reg in (5, 6):
            o.append('fp_compare(%s, %s, zf, pf, cf); of = sf = af = 0;' % (st(0), st(i)))
            if op == 0xDF:
                o.append('c.fpu.pop();')
            return True
        if op == 0xDB and modrm == 0xE2:
            o.append('c.fpu.status &= 0x7F00u;')
            return True
        if op == 0xDB and modrm == 0xE3:
            o.append('c.fpu = Fpu{};')
            return True
        if op == 0xDF and modrm == 0xE0:
            o.append('eax = (eax & 0xFFFF0000u) | c.fpu.status_word();')
            return True
        if op == 0xDD:
            if reg == 0:
                o.append('c.fpu.tag |= (uint16_t)(3u << (2 * ((c.fpu.top + %d) & 7)));' % i)
            elif reg in (2, 3):
                o.append('%s = %s;' % (st(i), st(0)))
                if reg == 3:
                    o.append('c.fpu.pop();')
            elif reg in (4, 5):
                o.append('fpu_compare(c.fpu, %s, %s);' % (st(0), st(i)))
                if reg == 5:
                    o.append('c.fpu.pop();')
            else:
                raise self.unsupported('x87 DD %02X' % modrm)
            return True
        if op == 0xD9:
            if reg == 0:
                o.append('{ double v_ = %s; c.fpu.push(v_); }' % st(i))
            elif reg == 1:
                o.append('{ double v_ = %s; %s = %s; %s = v_; }' % (st(0), st(0), st(i), st(i)))
            elif modrm == 0xD0:
                pass
            elif modrm in CONSTANTS:
                o.append('c.fpu.push(%s);' % CONSTANTS[modrm])
            elif modrm in UNARY:
                o.append('%s = %s;' % (st(0), UNARY[modrm].format(x=st(0))))
                if modrm in (0xFE, 0xFF):
                    o.append('c.fpu.status &= (uint16_t)~0x0400u;')
            elif modrm == 0xE4:
                o.append('fpu_compare(c.fpu, %s, 0.0);' % st(0))
            elif modrm == 0xE5:
                o.append('fpu_examine(c.fpu);')
            elif modrm == 0xF1:
                o.append('%s = %s * std::log2(%s); c.fpu.pop();' % (st(1), st(1), st(0)))
            elif modrm == 0xF9:
                o.append('%s = %s * std::log2(%s + 1.0); c.fpu.pop();' % (st(1), st(1), st(0)))
            elif modrm == 0xF2:
                o.append('%s = std::tan(%s); c.fpu.push(1.0); c.fpu.status &= (uint16_t)~0x0400u;' % (st(0), st(0)))
            elif modrm == 0xF3:
                o.append('%s = std::atan2(%s, %s); c.fpu.pop();' % (st(1), st(1), st(0)))
            elif modrm == 0xFB:
                o.append('{ double x_ = %s; %s = std::sin(x_); c.fpu.push(std::cos(x_)); c.fpu.status &= (uint16_t)~0x0400u; }' % (
                    st(0), st(0)))
            elif modrm == 0xFD:
                o.append('%s = fpu_scale(%s, %s);' % (st(0), st(0), st(1)))
            elif modrm == 0xF8:
                o.append('fpu_prem(c.fpu, false);')
            elif modrm == 0xF5:
                o.append('fpu_prem(c.fpu, true);')
            elif modrm == 0xF6:
                o.append('c.fpu.top = (c.fpu.top - 1) & 7;')
            elif modrm == 0xF7:
                o.append('c.fpu.top = (c.fpu.top + 1) & 7;')
            else:
                raise self.unsupported('x87 D9 %02X' % modrm)
            return True
        raise self.unsupported('x87 %02X %02X' % (op, modrm))
