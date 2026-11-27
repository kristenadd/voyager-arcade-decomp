"""SSE / SSE2 / MMX instructions.

Element-wise operations go through lanes<T, N> (recomp/guest.hpp); scalar
forms (ss / sd) are the same operation over lane 0 only, leaving the other
lanes alone as x86 does. Memory sources are read at the instruction's
access width, never a full 16 bytes for a scalar.
"""
from capstone import x86 as X

from semantics import Unsupported

FLOAT_FORMS = {'ps': ('float', 4, 16), 'ss': ('float', 1, 4), 'pd': ('double', 2, 16), 'sd': ('double', 1, 8)}
FLOAT_BINARY = {'add': 'a + b', 'sub': 'a - b', 'mul': 'a * b', 'div': 'a / b', 'min': 'sse_min(a, b)', 'max': 'sse_max(a, b)'}
FLOAT_UNARY = {'sqrt': 'std::sqrt(b)', 'rcp': '1.0f / b', 'rsqrt': '1.0f / std::sqrt(b)'}
PREDICATES = {'eq': 0, 'lt': 1, 'le': 2, 'unord': 3, 'neq': 4, 'nlt': 5, 'nle': 6, 'ord': 7}
BITWISE = {'and': 'a & b', 'andn': '~a & b', 'or': 'a | b', 'xor': 'a ^ b'}

INT_LANES = {}
for sfx, t in (('b', 8), ('w', 16), ('d', 32), ('q', 64)):
    u = 'uint%d_t' % t
    INT_LANES['padd' + sfx] = (u, '(%s)(a + b)' % u)
    INT_LANES['psub' + sfx] = (u, '(%s)(a - b)' % u)
for sfx, t in (('b', 8), ('w', 16), ('d', 32)):
    u, s = 'uint%d_t' % t, 'int%d_t' % t
    INT_LANES['pcmpeq' + sfx] = (u, '(%s)(a == b ? ~(%s)0 : 0)' % (u, u))
    INT_LANES['pcmpgt' + sfx] = (s, '(%s)(a > b ? -1 : 0)' % s)
INT_LANES.update({
    'paddsb': ('int8_t', 'sat_i8(a + b)'), 'paddsw': ('int16_t', 'sat_i16(a + b)'),
    'paddusb': ('uint8_t', 'sat_u8(a + b)'), 'paddusw': ('uint16_t', 'sat_u16(a + b)'),
    'psubsb': ('int8_t', 'sat_i8(a - b)'), 'psubsw': ('int16_t', 'sat_i16(a - b)'),
    'psubusb': ('uint8_t', 'sat_u8(a - b)'), 'psubusw': ('uint16_t', 'sat_u16(a - b)'),
    'pmullw': ('int16_t', '(int16_t)(a * b)'), 'pmulhw': ('int16_t', '(int16_t)((a * b) >> 16)'),
    'pmulhuw': ('uint16_t', '(uint16_t)(((uint32_t)a * b) >> 16)'),
    'pmuludq': ('uint64_t', '(uint64_t)(uint32_t)a * (uint32_t)b'),
    'pavgb': ('uint8_t', '(uint8_t)((a + b + 1) >> 1)'), 'pavgw': ('uint16_t', '(uint16_t)((a + b + 1) >> 1)'),
    'pminub': ('uint8_t', 'a < b ? a : b'), 'pmaxub': ('uint8_t', 'a > b ? a : b'),
    'pminsw': ('int16_t', 'a < b ? a : b'), 'pmaxsw': ('int16_t', 'a > b ? a : b'),
    'pand': ('uint64_t', 'a & b'), 'pandn': ('uint64_t', '~a & b'), 'por': ('uint64_t', 'a | b'), 'pxor': ('uint64_t', 'a ^ b'),
})
SIZES = {'uint8_t': 1, 'int8_t': 1, 'uint16_t': 2, 'int16_t': 2, 'uint32_t': 4, 'int32_t': 4, 'uint64_t': 8, 'int64_t': 8,
         'float': 4, 'double': 8}
SHIFTS = {'psllw': ('uint16_t', 'l'), 'pslld': ('uint32_t', 'l'), 'psllq': ('uint64_t', 'l'),
          'psrlw': ('uint16_t', 'r'), 'psrld': ('uint32_t', 'r'), 'psrlq': ('uint64_t', 'r'),
          'psraw': ('int16_t', 'a'), 'psrad': ('int32_t', 'a')}
UNPACK = {'punpcklbw': ('u8', 0), 'punpckhbw': ('u8', 1), 'punpcklwd': ('u16', 0), 'punpckhwd': ('u16', 1),
          'punpckldq': ('u32', 0), 'punpckhdq': ('u32', 1), 'punpcklqdq': ('u64', 0), 'punpckhqdq': ('u64', 1),
          'unpcklps': ('u32', 0), 'unpckhps': ('u32', 1), 'unpcklpd': ('u64', 0), 'unpckhpd': ('u64', 1)}
FIELD_BYTES = {'u8': 1, 'u16': 2, 'u32': 4, 'u64': 8}
# SSE3 horizontal / alternating forms: (type, field, [(lane expression, NaN-order operands)])
SSE3_PAIRS = {
    'haddps': ('float', 'f', [('a_.f[0] + a_.f[1]', 'a_.f[0]', 'a_.f[1]'), ('a_.f[2] + a_.f[3]', 'a_.f[2]', 'a_.f[3]'),
                              ('b_.f[0] + b_.f[1]', 'b_.f[0]', 'b_.f[1]'), ('b_.f[2] + b_.f[3]', 'b_.f[2]', 'b_.f[3]')]),
    'hsubps': ('float', 'f', [('a_.f[0] - a_.f[1]', 'a_.f[0]', 'a_.f[1]'), ('a_.f[2] - a_.f[3]', 'a_.f[2]', 'a_.f[3]'),
                              ('b_.f[0] - b_.f[1]', 'b_.f[0]', 'b_.f[1]'), ('b_.f[2] - b_.f[3]', 'b_.f[2]', 'b_.f[3]')]),
    'haddpd': ('double', 'd', [('a_.d[0] + a_.d[1]', 'a_.d[0]', 'a_.d[1]'), ('b_.d[0] + b_.d[1]', 'b_.d[0]', 'b_.d[1]')]),
    'hsubpd': ('double', 'd', [('a_.d[0] - a_.d[1]', 'a_.d[0]', 'a_.d[1]'), ('b_.d[0] - b_.d[1]', 'b_.d[0]', 'b_.d[1]')]),
    'addsubps': ('float', 'f', [('a_.f[%d] %s b_.f[%d]' % (k, '-' if k % 2 == 0 else '+', k), 'a_.f[%d]' % k, 'b_.f[%d]' % k)
                                for k in range(4)]),
    'addsubpd': ('double', 'd', [('a_.d[%d] %s b_.d[%d]' % (k, '-' if k % 2 == 0 else '+', k), 'a_.d[%d]' % k, 'b_.d[%d]' % k)
                                 for k in range(2)]),
}
MOVES128 = {'movaps', 'movups', 'movapd', 'movupd', 'movdqa', 'movdqu', 'lddqu', 'movntps', 'movntpd', 'movntdq'}


class SseMixin:
    def vreg(self, insn, op):
        if op.type != X.X86_OP_REG:
            return None
        r = self.rn(insn, op.reg)
        if r.startswith('xmm'):
            return 'xm%s_' % r[3:]  # a local, see FunctionTranslator.keep_xmm_local
        if r.startswith('mm'):
            return 'c.mm[%s]' % r[2:]
        return None

    def vsrc(self, insn, op, nbytes, mmx=False):
        r = self.vreg(insn, op)
        if r:
            return r
        if op.type == X.X86_OP_MEM:
            a = self.addr(insn, op)
            if mmx:
                return 'mmx_load(M, %s, %d)' % (a, nbytes)
            return 'ld128(M, %s)' % a if nbytes == 16 else 'xmm_load(M, %s, %d)' % (a, nbytes)
        raise Unsupported('vector source operand')

    def emit_sse(self, insn, m):
        ops = insn.operands
        o = self.out
        d = ops[0] if ops else None
        D = self.vreg(insn, d) if d is not None else None
        mmx = D is not None and D.startswith('c.mm')
        V = 'Mmx' if mmx else 'Xmm'
        width = 8 if mmx else 16

        if m == 'emms':
            o.append('c.fpu.tag = 0xFFFF;')
            return True
        if m == 'ldmxcsr':
            o.append('c.mxcsr = ld32(M, %s);' % self.addr(insn, d))
            return True
        if m == 'stmxcsr':
            o.append('st32(M, %s, c.mxcsr);' % self.addr(insn, d))
            return True

        # element-wise float arithmetic
        base, form = m[:-2], m[-2:]
        if form in FLOAT_FORMS and (base in FLOAT_BINARY or base in FLOAT_UNARY or base in BITWISE
                                    or base.startswith('cmp')) and D and not mmx:
            t, n, nbytes = FLOAT_FORMS[form]
            s = ops[1]
            if base in BITWISE:
                if form in ('ss', 'sd'):
                    return False
                expr = BITWISE[base]
                o.append('{ Xmm s_ = %s; lanes<uint64_t, 2>(%s, s_, [](uint64_t a, uint64_t b) -> uint64_t { return %s; }); }' % (
                    self.vsrc(insn, s, 16), D, expr))
                return True
            if base.startswith('cmp'):
                pred = base[3:]
                if pred == '' and len(ops) == 3:
                    p = ops[2].imm & 7
                elif pred in PREDICATES:
                    p = PREDICATES[pred]
                else:
                    raise Unsupported('compare predicate %s' % m)
                ut = 'uint32_t' if t == 'float' else 'uint64_t'
                conv = 'f32_bits' if t == 'float' else 'f64_bits'
                o.append('{ Xmm s_ = %s; lanes<%s, %d>(%s, s_, [](%s a, %s b) -> %s { return sse_cmp(%s(a), %s(b), %d) ? ~(%s)0 : 0; }); }' % (
                    self.vsrc(insn, s, nbytes), ut, n, D, ut, ut, ut, conv, conv, p, ut))
                return True
            if base in FLOAT_BINARY:
                expr = FLOAT_BINARY[base]
                if base not in ('min', 'max'):
                    expr = 'x86_nan(%s, a, b)' % expr
            else:
                expr = 'x86_nan(%s, b)' % FLOAT_UNARY[base]
            if t == 'double':
                expr = expr.replace('1.0f', '1.0')
            o.append('{ Xmm s_ = %s; lanes<%s, %d>(%s, s_, [](%s a, %s b) -> %s { (void)a; return %s; }); }' % (
                self.vsrc(insn, s, nbytes), t, n, D, t, t, t, expr))
            return True

        if m in INT_LANES and D:
            t, expr = INT_LANES[m]
            n = width // SIZES[t]
            o.append('{ %s s_ = %s; lanes<%s, %d>(%s, s_, [](%s a, %s b) -> %s { return (%s)(%s); }); }' % (
                V, self.vsrc(insn, ops[1], width, mmx), t, n, D, t, t, t, t, expr))
            return True

        if m in SHIFTS and D:
            t, kind = SHIFTS[m]
            bits = SIZES[t] * 8
            n = width // SIZES[t]
            c_ = ops[1]
            if c_.type == X.X86_OP_IMM:
                count = '%du' % (c_.imm & 0xFF)
            elif self.vreg(insn, c_):
                r = self.vreg(insn, c_)
                count = '%s.u64' % r if r.startswith('c.mm') else '%s.u64[0]' % r
            else:
                count = 'ld64(M, %s)' % self.addr(insn, c_)
            if kind == 'l':
                body = 'return n_ >= %d ? (%s)0 : (%s)(a << n_);' % (bits, t, t)
            elif kind == 'r':
                body = 'return n_ >= %d ? (%s)0 : (%s)(a >> n_);' % (bits, t, t)
            else:
                body = 'return (%s)(a >> (n_ >= %d ? %d : n_));' % (t, bits, bits - 1)
            o.append('{ uint64_t n_ = %s; lanes<%s, %d>(%s, %s, [n_](%s a, %s) -> %s { %s }); }' % (
                count, t, n, D, D, t, t, t, body))
            return True

        if m in ('pslldq', 'psrldq') and D and not mmx:
            k = ops[1].imm & 0xFF
            if m == 'pslldq':
                o.append('{ Xmm a_ = %s; Xmm r_{}; for (unsigned i_ = %d; i_ < 16; ++i_) r_.u8[i_] = a_.u8[i_ - %d]; %s = r_; }' % (
                    D, min(k, 16), min(k, 16), D))
            else:
                o.append('{ Xmm a_ = %s; Xmm r_{}; for (unsigned i_ = 0; i_ + %d < 16; ++i_) r_.u8[i_] = a_.u8[i_ + %d]; %s = r_; }' % (
                    D, min(k, 16), min(k, 16), D))
            return True

        if m in UNPACK and D:
            field, high = UNPACK[m]
            n = width // FIELD_BYTES[field]
            if mmx and field == 'u64':
                raise Unsupported(m + ' on mmx')
            s = self.vsrc(insn, ops[1], 4 if mmx and not high else width, mmx)
            half = n // 2
            moves = []
            for k in range(half):
                src = k + (half if high else 0)
                moves.append('r_.%s[%d] = a_.%s[%d]; r_.%s[%d] = b_.%s[%d];' % (field, 2 * k, field, src, field, 2 * k + 1, field, src))
            o.append('{ %s a_ = %s; %s b_ = %s; %s r_; %s %s = r_; }' % (V, D, V, s, V, ' '.join(moves), D))
            return True

        if m in ('packsswb', 'packuswb', 'packssdw') and D:
            src_f, dst_f, sat = {'packsswb': ('i16', 'i8', 'sat_i8'), 'packuswb': ('i16', 'u8', 'sat_u8'),
                                 'packssdw': ('i32', 'i16', 'sat_i16')}[m]
            n = width // (2 if src_f == 'i16' else 4)
            moves = ['r_.%s[%d] = %s(a_.%s[%d]);' % (dst_f, k, sat, src_f, k) for k in range(n)]
            moves += ['r_.%s[%d] = %s(b_.%s[%d]);' % (dst_f, n + k, sat, src_f, k) for k in range(n)]
            o.append('{ %s a_ = %s; %s b_ = %s; %s r_; %s %s = r_; }' % (
                V, D, V, self.vsrc(insn, ops[1], width, mmx), V, ' '.join(moves), D))
            return True

        if m == 'pmaddwd' and D:
            n = width // 4
            moves = ['r_.i32[%d] = (int32_t)((uint32_t)(a_.i16[%d] * b_.i16[%d]) + (uint32_t)(a_.i16[%d] * b_.i16[%d]));' % (
                k, 2 * k, 2 * k, 2 * k + 1, 2 * k + 1) for k in range(n)]
            o.append('{ %s a_ = %s; %s b_ = %s; %s r_; %s %s = r_; }' % (
                V, D, V, self.vsrc(insn, ops[1], width, mmx), V, ' '.join(moves), D))
            return True

        if m == 'psadbw' and D:
            groups = width // 8
            moves = []
            for g in range(groups):
                terms = ' + '.join('(uint32_t)std::abs(a_.u8[%d] - b_.u8[%d])' % (8 * g + k, 8 * g + k) for k in range(8))
                moves.append('s_[%d] = %s;' % (g, terms))
            store = ' '.join('r_.u64%s = s_[%d];' % ('' if mmx else '[%d]' % g, g) for g in range(groups))
            o.append('{ %s a_ = %s; %s b_ = %s; %s r_{}; uint64_t s_[%d]; %s %s %s = r_; }' % (
                V, D, V, self.vsrc(insn, ops[1], width, mmx), V, groups, ' '.join(moves), store, D))
            return True

        if m in ('shufps', 'shufpd', 'pshufd', 'pshuflw', 'pshufhw', 'pshufw'):
            imm = ops[2].imm & 0xFF
            s = self.vsrc(insn, ops[1], width, mmx)
            if m == 'shufps':
                sel = ['r_.u32[0] = a_.u32[%d];' % (imm & 3), 'r_.u32[1] = a_.u32[%d];' % ((imm >> 2) & 3),
                       'r_.u32[2] = b_.u32[%d];' % ((imm >> 4) & 3), 'r_.u32[3] = b_.u32[%d];' % ((imm >> 6) & 3)]
            elif m == 'shufpd':
                sel = ['r_.u64[0] = a_.u64[%d];' % (imm & 1), 'r_.u64[1] = b_.u64[%d];' % ((imm >> 1) & 1)]
            elif m == 'pshufd':
                sel = ['r_.u32[%d] = b_.u32[%d];' % (k, (imm >> (2 * k)) & 3) for k in range(4)]
            elif m == 'pshufw':
                sel = ['r_.u16[%d] = b_.u16[%d];' % (k, (imm >> (2 * k)) & 3) for k in range(4)]
            elif m == 'pshuflw':
                sel = ['r_.u16[%d] = b_.u16[%d];' % (k, (imm >> (2 * k)) & 3) for k in range(4)] + ['r_.u64[1] = b_.u64[1];']
            else:
                sel = ['r_.u64[0] = b_.u64[0];'] + ['r_.u16[%d] = b_.u16[%d];' % (4 + k, 4 + ((imm >> (2 * k)) & 3)) for k in range(4)]
            o.append('{ %s a_ = %s; %s b_ = %s; %s r_; %s %s = r_; (void)a_; }' % (V, D, V, s, V, ' '.join(sel), D))
            return True

        if m in MOVES128:
            if D and not mmx:
                o.append('%s = %s;' % (D, self.vsrc(insn, ops[1], 16)))
            elif d.type == X.X86_OP_MEM:
                o.append('st128(M, %s, %s);' % (self.addr(insn, d), self.vreg(insn, ops[1])))
            else:
                raise Unsupported(m + ' operands')
            return True

        if m in ('movss', 'movsd'):
            nb, f = (4, 'u32') if m == 'movss' else (8, 'u64')
            s = ops[1]
            if D and self.vreg(insn, s):
                o.append('%s.%s[0] = %s.%s[0];' % (D, f, self.vreg(insn, s), f))
            elif D:
                o.append('%s = xmm_load(M, %s, %d);' % (D, self.addr(insn, s), nb))
            else:
                o.append('%s(M, %s, %s.%s[0]);' % ('st32' if nb == 4 else 'st64', self.addr(insn, d), self.vreg(insn, s), f))
            return True

        if m == 'movd':
            s = ops[1]
            if D:
                o.append('{ uint32_t v_ = %s; %s = %s{}; %s.u32[0] = v_; }' % (self.read(insn, s, 4), D, V, D))
            else:
                o.append(self.write(insn, d, '%s.u32[0]' % self.vreg(insn, s), 4))
            return True

        if m == 'movq':
            s = ops[1]
            S = self.vreg(insn, s)
            if D and mmx:
                o.append('%s.u64 = %s;' % (D, ('%s.u64' % S) if S and S.startswith('c.mm') else
                                           ('%s.u64[0]' % S) if S else 'ld64(M, %s)' % self.addr(insn, s)))
            elif D:
                src = ('%s.u64' % S) if S and S.startswith('c.mm') else ('%s.u64[0]' % S) if S else 'ld64(M, %s)' % self.addr(insn, s)
                o.append('{ uint64_t v_ = %s; %s = Xmm{}; %s.u64[0] = v_; }' % (src, D, D))
            else:
                src = ('%s.u64' % S) if S.startswith('c.mm') else '%s.u64[0]' % S
                o.append('st64(M, %s, %s);' % (self.addr(insn, d), src))
            return True

        if m in ('movntq', 'movnti'):
            S = self.vreg(insn, ops[1])
            if m == 'movntq':
                o.append('st64(M, %s, %s.u64);' % (self.addr(insn, d), S))
            else:
                o.append('st32(M, %s, %s);' % (self.addr(insn, d), self.read(insn, ops[1], 4)))
            return True

        if m in ('movlps', 'movlpd', 'movhps', 'movhpd'):
            k = 0 if m[3] == 'l' else 1
            if D:
                o.append('%s.u64[%d] = ld64(M, %s);' % (D, k, self.addr(insn, ops[1])))
            else:
                o.append('st64(M, %s, %s.u64[%d]);' % (self.addr(insn, d), self.vreg(insn, ops[1]), k))
            return True
        if m == 'movhlps':
            o.append('%s.u64[0] = %s.u64[1];' % (D, self.vreg(insn, ops[1])))
            return True
        if m == 'movlhps':
            o.append('%s.u64[1] = %s.u64[0];' % (D, self.vreg(insn, ops[1])))
            return True
        if m == 'movq2dq':
            o.append('{ uint64_t v_ = %s.u64; %s = Xmm{}; %s.u64[0] = v_; }' % (self.vreg(insn, ops[1]), D, D))
            return True
        if m == 'movdq2q':
            o.append('%s.u64 = %s.u64[0];' % (D, self.vreg(insn, ops[1])))
            return True

        if m in ('movmskps', 'movmskpd', 'pmovmskb'):
            S = self.vreg(insn, ops[1])
            if m == 'movmskps':
                v = ' | '.join('((%s.u32[%d] >> 31) << %d)' % (S, k, k) for k in range(4))
            elif m == 'movmskpd':
                v = ' | '.join('(uint32_t)((%s.u64[%d] >> 63) << %d)' % (S, k, k) for k in range(2))
            else:
                v = ' | '.join('((uint32_t)(%s.u8[%d] >> 7) << %d)' % (S, k, k) for k in range(8 if S.startswith('c.mm') else 16))
            o.append(self.write(insn, d, '(uint32_t)(%s)' % v, 4))
            return True

        if m == 'pextrw':
            S = self.vreg(insn, ops[1])
            k = ops[2].imm & (3 if S.startswith('c.mm') else 7)
            o.append(self.write(insn, d, '(uint32_t)%s.u16[%d]' % (S, k), 4))
            return True
        if m == 'pinsrw':
            k = ops[2].imm & (3 if mmx else 7)
            o.append('%s.u16[%d] = (uint16_t)%s;' % (D, k, self.read(insn, ops[1], 2 if ops[1].type == X.X86_OP_MEM else 4)))
            return True

        if m in SSE3_PAIRS:
            t, f, lanes_ = SSE3_PAIRS[m]
            s = self.vsrc(insn, ops[1], 16)
            o.append('{ Xmm a_ = %s; Xmm b_ = %s; Xmm r_; %s %s = r_; }' % (D, s, ' '.join(
                'r_.%s[%d] = x86_nan(%s, %s, %s);' % (f, k, expr, x, y) for k, (expr, x, y) in enumerate(lanes_)), D))
            return True
        if m in ('movsldup', 'movshdup'):
            k = 0 if m == 'movsldup' else 1
            o.append('{ Xmm b_ = %s; Xmm r_; r_.u32[0] = r_.u32[1] = b_.u32[%d]; r_.u32[2] = r_.u32[3] = b_.u32[%d]; %s = r_; }' % (
                self.vsrc(insn, ops[1], 16), k, k + 2, D))
            return True
        if m == 'movddup':
            o.append('{ Xmm b_ = %s; %s.u64[0] = %s.u64[1] = b_.u64[0]; }' % (self.vsrc(insn, ops[1], 8), D, D))
            return True
        if m in ('cvtps2pi', 'cvttps2pi'):
            fn = 'cvt_trunc_i32' if m.startswith('cvtt') else 'cvt_round_i32'
            o.append('{ Xmm s_ = %s; %s.i32[0] = %s(s_.f[0]); %s.i32[1] = %s(s_.f[1]); }' % (
                self.vsrc(insn, ops[1], 8), D, fn, D, fn))
            return True
        if m == 'cvtpi2ps':
            o.append('{ Mmx s_ = %s; %s.f[0] = (float)s_.i32[0]; %s.f[1] = (float)s_.i32[1]; }' % (
                self.vsrc(insn, ops[1], 8, True), D, D))
            return True

        if m in ('comiss', 'ucomiss', 'comisd', 'ucomisd'):
            f, nb = ('f', 4) if m.endswith('ss') else ('d', 8)
            o.append('{ Xmm s_ = %s; fp_compare(%s.%s[0], s_.%s[0], zf, pf, cf); of = sf = af = 0; }' % (
                self.vsrc(insn, ops[1], nb), D, f, f))
            return True

        return self.emit_conversion(insn, m, ops, D)

    def emit_conversion(self, insn, m, ops, D):
        o = self.out
        d = ops[0] if ops else None
        if m in ('cvtsi2ss', 'cvtsi2sd'):
            f = 'f' if m.endswith('ss') else 'd'
            t = 'float' if f == 'f' else 'double'
            o.append('%s.%s[0] = (%s)(int32_t)%s;' % (D, f, t, self.read(insn, ops[1], 4)))
            return True
        if m in ('cvttss2si', 'cvtss2si', 'cvttsd2si', 'cvtsd2si'):
            if d.size != 4:
                raise Unsupported(m + ' to a non-32-bit register')
            f, nb = ('f', 4) if 'ss2' in m else ('d', 8)
            fn = 'cvt_trunc_i32' if m.startswith('cvtt') else 'cvt_round_i32'
            o.append('{ Xmm s_ = %s; %s }' % (self.vsrc(insn, ops[1], nb), self.write(insn, d, '(uint32_t)%s((double)s_.%s[0])' % (fn, f))))
            return True
        if m == 'cvtss2sd':
            o.append('{ Xmm s_ = %s; %s.d[0] = (double)s_.f[0]; }' % (self.vsrc(insn, ops[1], 4), D))
            return True
        if m == 'cvtsd2ss':
            o.append('{ Xmm s_ = %s; %s.f[0] = (float)s_.d[0]; }' % (self.vsrc(insn, ops[1], 8), D))
            return True
        if m == 'cvtps2pd':
            o.append('{ Xmm s_ = %s; Xmm r_; r_.d[0] = (double)s_.f[0]; r_.d[1] = (double)s_.f[1]; %s = r_; }' % (
                self.vsrc(insn, ops[1], 8), D))
            return True
        if m == 'cvtpd2ps':
            o.append('{ Xmm s_ = %s; Xmm r_{}; r_.f[0] = (float)s_.d[0]; r_.f[1] = (float)s_.d[1]; %s = r_; }' % (
                self.vsrc(insn, ops[1], 16), D))
            return True
        if m == 'cvtdq2ps':
            o.append('{ Xmm s_ = %s; Xmm r_; for (int i_ = 0; i_ < 4; ++i_) r_.f[i_] = (float)s_.i32[i_]; %s = r_; }' % (
                self.vsrc(insn, ops[1], 16), D))
            return True
        if m in ('cvttps2dq', 'cvtps2dq'):
            fn = 'cvt_trunc_i32' if m.startswith('cvtt') else 'cvt_round_i32'
            o.append('{ Xmm s_ = %s; Xmm r_; for (int i_ = 0; i_ < 4; ++i_) r_.i32[i_] = %s((double)s_.f[i_]); %s = r_; }' % (
                self.vsrc(insn, ops[1], 16), fn, D))
            return True
        if m == 'cvtdq2pd':
            o.append('{ Xmm s_ = %s; Xmm r_; r_.d[0] = (double)s_.i32[0]; r_.d[1] = (double)s_.i32[1]; %s = r_; }' % (
                self.vsrc(insn, ops[1], 8), D))
            return True
        if m in ('cvttpd2dq', 'cvtpd2dq'):
            fn = 'cvt_trunc_i32' if m.startswith('cvtt') else 'cvt_round_i32'
            o.append('{ Xmm s_ = %s; Xmm r_{}; r_.i32[0] = %s(s_.d[0]); r_.i32[1] = %s(s_.d[1]); %s = r_; }' % (
                self.vsrc(insn, ops[1], 16), fn, fn, D))
            return True
        return False
