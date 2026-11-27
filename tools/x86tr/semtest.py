#!/usr/bin/env python3
"""Instruction semantics tests against real x86 (Rosetta 2) as ground truth.

Every case is a short Intel-syntax snippet. It is assembled twice by clang:
for i386 (bytes the translator turns into C++) and for x86_64 (run natively
under Rosetta by a harness). Both start from the same register, flag, XMM,
MMX and memory state; the expected final state recorded from the x86_64 run
becomes the reference the translated code must reproduce on arm64.

ebx is the memory base in every case ({b} in the snippets: rbx on x86_64,
ebx in i386): it points at a 64-byte buffer and is never compared. esp is
not touched by snippets.

    PYTHONPATH=build/pylib python3 tools/x86tr/semtest.py --out build/x86sem
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from cfg import Walker  # noqa: E402
from translate import Translator, FunctionTranslator  # noqa: E402
import semantics as F  # noqa: E402

STATE_SIZE = 320
OFF_FLAGS, OFF_XMM, OFF_MM, OFF_MEM = 32, 48, 176, 256
GUEST_MEM = 0x1000
INPUTS = 8
FLAG_BITS = {F.C: 0x1, F.P: 0x4, F.A: 0x10, F.Z: 0x40, F.S: 0x80, F.O: 0x800}
CCS = ['o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']


def fmask(*undefined):
    m = 0x8D5
    for f in undefined:
        m &= ~FLAG_BITS[f]
    return m


def build_cases():
    """(name, snippet, compared flag mask)."""
    cases = []

    def add(snippet, mask=0x8D5, name=None):
        cases.append((name or snippet, snippet, mask))

    # ---- integer ALU ----
    for op in ('add', 'adc', 'sub', 'sbb', 'cmp', 'and', 'or', 'xor', 'test'):
        logic = op in ('and', 'or', 'xor', 'test')
        m = fmask(F.A) if logic else 0x8D5
        for form in ('eax, ecx', 'al, cl', 'ax, dx', 'ah, cl', 'esi, 0x7f', 'edi, -3', 'cx, 0x8001', 'dl, 0x80',
                     'dword ptr [{b}], eax', 'eax, dword ptr [{b}+4]', 'byte ptr [{b}+1], 0x80', 'word ptr [{b}+2], si'):
            if op == 'test' and form.startswith('eax, dword ptr'):
                form = 'dword ptr [{b}+4], eax'
            add('%s %s' % (op, form), m)
        add('%s eax, eax' % op, m)
    for op in ('inc', 'dec', 'neg', 'not'):
        for form in ('eax', 'cl', 'dx', 'dword ptr [{b}+8]', 'byte ptr [{b}+3]'):
            add('%s %s' % (op, form))
    add('sbb edx, edx')
    add('adc eax, ecx\nadc edx, esi')
    add('sub eax, ecx\nsbb edx, esi')
    add('xadd eax, ecx')
    add('xadd dword ptr [{b}], edx')
    add('lock xadd dword ptr [{b}+4], esi')
    for op, form in (('add', 'dword ptr [{b}], eax'), ('sub', 'dword ptr [{b}+4], ecx'),
                     ('and', 'dword ptr [{b}+8], 0xff00'), ('or', 'byte ptr [{b}+1], 0x80'),
                     ('xor', 'word ptr [{b}+2], si'), ('inc', 'dword ptr [{b}]'), ('dec', 'byte ptr [{b}+3]'),
                     ('neg', 'dword ptr [{b}+8]'), ('not', 'dword ptr [{b}+12]'),
                     ('xadd', 'dword ptr [{b}], edx'), ('bts', 'dword ptr [{b}], 3'),
                     ('btr', 'word ptr [{b}+2], 9'), ('btc', 'dword ptr [{b}+4], 13'),
                     ('cmpxchg', 'dword ptr [{b}], edx'), ('cmpxchg8b', 'qword ptr [{b}]')):
        add('lock %s %s' % (op, form), 0x40 if op.startswith('cmpxchg') else (0x1 if op[:2] == 'bt' else 0x8D5))
    add('xchg dword ptr [{b}], esi')
    add('mfence')
    add('sfence\nlfence')
    # Rosetta sets cmpxchg's other flags from DEST - EAX, real CPUs (per Bochs and QEMU) from
    # EAX - DEST; compiled code reads only ZF. The translation follows the CPUs.
    add('cmpxchg ecx, edx', 0x40)
    add('mov eax, dword ptr [{b}]\ncmpxchg dword ptr [{b}], edx', 0x40)
    add('cmpxchg dword ptr [{b}], edx', 0x40)
    # (the equal case of cmpxchg8b stores ebx, the memory base, so only the unequal case is comparable)
    add('cmpxchg8b qword ptr [{b}]', 0x40)
    # ---- multiply / divide ----
    for form in ('ecx', 'cl', 'dx', 'dword ptr [{b}]'):
        add('mul %s' % form, fmask(F.P, F.A, F.Z, F.S))
        add('imul %s' % form, fmask(F.P, F.A, F.Z, F.S))
    for form in ('ecx, edx', 'si, dx', 'eax, dword ptr [{b}]', 'ecx, edx, 0x1234', 'edi, esi, -7', 'ax, cx, 300'):
        add('imul %s' % form, fmask(F.P, F.A, F.Z, F.S))
    add('xor edx, edx\nor ecx, 1\ndiv ecx', fmask(F.C, F.P, F.A, F.Z, F.S, F.O))
    add('and ecx, 0x7fff\nor ecx, 2\ncdq\nidiv ecx', fmask(F.C, F.P, F.A, F.Z, F.S, F.O))
    add('and eax, 0x3fff\nand ecx, 0xff\nor ecx, 0x80\ndiv cl', fmask(F.C, F.P, F.A, F.Z, F.S, F.O))
    add('movsx eax, al\nand ecx, 0x3f\nor ecx, 0x40\nidiv cl', fmask(F.C, F.P, F.A, F.Z, F.S, F.O))
    add('and edx, 0x7f\nor ecx, 0x8000\ndiv cx', fmask(F.C, F.P, F.A, F.Z, F.S, F.O))
    add('cwd\nand ecx, 0x3fff\nor ecx, 0x4000\nidiv cx', fmask(F.C, F.P, F.A, F.Z, F.S, F.O))
    # ---- shifts / rotates ----
    for op in ('shl', 'shr', 'sar'):
        for dst in ('eax', 'dl', 'si', 'dword ptr [{b}]'):
            add('%s %s, 1' % (op, dst), fmask(F.A))
            add('%s %s, 5' % (op, dst), fmask(F.A, F.O))
            add('%s %s, 31' % (op, dst), fmask(F.A, F.O, F.C) if dst in ('dl', 'si') else fmask(F.A, F.O))
            add('%s %s, cl' % (op, dst), fmask(F.A, F.O) if dst not in ('dl', 'si') else fmask(F.A, F.O, F.C))
    for op in ('rol', 'ror', 'rcl', 'rcr'):
        for dst in ('eax', 'dl', 'si'):
            add('%s %s, 1' % (op, dst), fmask(F.P, F.A, F.Z, F.S) | 0)
            add('%s %s, 3' % (op, dst), 0x1)
            add('and ecx, 7\n%s %s, cl' % (op, dst), 0x1)
    add('shld eax, edx, 7', fmask(F.A, F.O))
    add('shrd eax, edx, 7', fmask(F.A, F.O))
    add('shld esi, edi, cl', fmask(F.A, F.O))
    add('shrd esi, edi, cl', fmask(F.A, F.O))
    add('shld dword ptr [{b}], ecx, 12', fmask(F.A, F.O))
    # ---- bits ----
    for op in ('bt', 'bts', 'btr', 'btc'):
        add('%s eax, ecx' % op, 0x1)
        add('%s esi, 13' % op, 0x1)
        add('%s word ptr [{b}], 9' % op, 0x1)
        add('and ecx, 63\n%s dword ptr [{b}], ecx' % op, 0x1)
    add('bsf eax, ecx', 0x40)
    add('bsr eax, ecx', 0x40)
    add('or ecx, 0x100\nbsf eax, ecx', 0x40)
    add('bsr edx, dword ptr [{b}]', 0x40)
    add('bswap esi')
    # ---- conditions: fused cmp / test, flags from arithmetic, branches ----
    for cc in CCS:
        add('cmp eax, ecx\nset%s dl' % cc)
        add('cmp al, cl\nset%s dl' % cc)
        add('cmp si, 0x7fff\nset%s dl' % cc)
        add('test eax, ecx\nset%s dl' % cc, fmask(F.A))
        add('add eax, ecx\nset%s dl' % cc)
        add('sub esi, edi\ncmov%s eax, edx' % cc)
        add('cmp dword ptr [{b}], ecx\ncmov%s eax, dword ptr [{b}+4]' % cc)
        add('cmp eax, edx\nj%s 1f\nmov esi, 1\njmp 2f\n1:\nmov esi, 2\n2:' % cc)
        add('dec ecx\nj%s 1f\nmov esi, 1\n1:' % cc)
    add('cmp eax, ecx\nsetl dl\nsetb dh')
    add('xor eax, eax\nmov ecx, 5\n1:\nadd eax, ecx\ndec ecx\njnz 1b')
    add('mov ecx, 3\nxor eax, eax\n1:\ninc eax\nloop 1b')
    add('and ecx, 1\njecxz 1f\nmov eax, 7\n1:')
    # ---- moves, conversions, misc ----
    for form in ('movzx eax, cl', 'movzx esi, dx', 'movsx eax, cl', 'movsx edi, dx', 'movzx ecx, byte ptr [{b}+5]',
                 'movsx edx, word ptr [{b}+6]', 'movsx ax, cl', 'lea eax, [ecx + edx*4 + 0x100]', 'lea esi, [esi + esi*2]',
                 'lea edi, [ecx*8 - 4]', 'xchg eax, ecx', 'xchg dl, ah', 'xchg dword ptr [{b}], esi', 'cdq', 'cwde', 'cbw', 'cwd',
                 'mov ah, cl', 'mov word ptr [{b}+2], dx', 'mov byte ptr [{b}+7], ch', 'mov eax, dword ptr [{b} + 12]'):
        add(form)
    add('lahf', 0)
    add('sahf')
    add('stc\ncmc\nsetc al', 0)
    add('clc\nadc eax, 0')
    # ---- strings ----
    for s in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsd'):
        add('lea esi, [{b}]\nlea edi, [{b}+24]\nmov ecx, 5\nrep %s\nsub esi, ebx\nsub edi, ebx' % s, 0)
        add('lea esi, [{b}]\nlea edi, [{b}+24]\n%s\nsub esi, ebx\nsub edi, ebx' % s, 0)
    add('lea esi, [{b}]\nlea edi, [{b}+1]\nmov ecx, 9\nrep movsb\nsub esi, ebx\nsub edi, ebx', 0)
    for s in ('cmpsb', 'cmpsd', 'scasb', 'scasd'):
        for p in ('repe', 'repne'):
            add('lea esi, [{b}]\nlea edi, [{b}+32]\nand ecx, 7\n%s %s\nsub esi, ebx\nsub edi, ebx' % (p, s))
        add('lea esi, [{b}]\nlea edi, [{b}+32]\n%s\nsub esi, ebx\nsub edi, ebx' % s)
    add('lea edi, [{b}+40]\nstd\nmov ecx, 3\nrep stosd\ncld\nsub edi, ebx', 0)
    # ---- SSE / SSE2 float ----
    for op in ('add', 'sub', 'mul', 'div', 'min', 'max', 'sqrt'):
        for f in ('ps', 'ss', 'pd', 'sd'):
            add('%s%s xmm0, xmm1' % (op, f), 0)
            add('%s%s xmm2, xmmword ptr [{b}]' % (op, f) if f in ('ps', 'pd') else
                '%s%s xmm2, %s ptr [{b}+8]' % (op, f, 'dword' if f == 'ss' else 'qword'), 0)
    for op in ('and', 'andn', 'or', 'xor'):
        add('%sps xmm3, xmm4' % op, 0)
        add('%spd xmm5, xmmword ptr [{b}+16]' % op, 0)
    add('xorps xmm6, xmm6', 0)
    for p in ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord'):
        add('cmp%sps xmm0, xmm1' % p, 0)
        add('cmp%ssd xmm2, xmm3' % p, 0)
    for op in ('comiss', 'ucomiss'):
        for cc in ('a', 'ae', 'b', 'be', 'e', 'ne', 'p'):
            add('%s xmm0, xmm1\nset%s al' % (op, cc))
        add('%s xmm2, dword ptr [{b}+8]' % op)
    for op in ('comisd', 'ucomisd'):
        add('%s xmm0, xmm1' % op)
        add('%s xmm3, qword ptr [{b}+8]' % op)
    for form in ('cvtsi2ss xmm0, eax', 'cvtsi2sd xmm1, dword ptr [{b}]', 'cvttss2si eax, xmm2', 'cvtss2si ecx, xmm3',
                 'cvttsd2si edx, xmm4', 'cvtsd2si esi, xmm5', 'cvtss2sd xmm6, xmm7', 'cvtsd2ss xmm0, xmm1',
                 'cvtps2pd xmm2, xmm3', 'cvtpd2ps xmm4, xmm5', 'cvtdq2ps xmm6, xmm7', 'cvttps2dq xmm0, xmm1',
                 'cvtps2dq xmm2, xmm3', 'cvtdq2pd xmm4, xmm5', 'cvttpd2dq xmm6, xmm7', 'cvtpd2dq xmm0, xmm1',
                 'cvttss2si eax, dword ptr [{b}+12]', 'cvtps2pd xmm1, qword ptr [{b}]'):
        add(form, 0)
    for form in ('movss xmm0, xmm1', 'movss xmm2, dword ptr [{b}]', 'movss dword ptr [{b}+4], xmm3', 'movsd xmm4, xmm5',
                 'movsd xmm6, qword ptr [{b}]', 'movsd qword ptr [{b}+8], xmm7', 'movaps xmm0, xmmword ptr [{b}+16]',
                 'movups xmmword ptr [{b}+32], xmm1', 'movdqa xmm2, xmm3', 'movdqu xmm4, xmmword ptr [{b}]',
                 'movd xmm5, eax', 'movd ecx, xmm6', 'movd dword ptr [{b}], xmm7', 'movq xmm0, qword ptr [{b}+8]',
                 'movq qword ptr [{b}], xmm1', 'movq xmm2, xmm3', 'movhlps xmm4, xmm5', 'movlhps xmm6, xmm7',
                 'movlps xmm0, qword ptr [{b}]', 'movhps xmm1, qword ptr [{b}+8]', 'movlps qword ptr [{b}+16], xmm2',
                 'movhps qword ptr [{b}+24], xmm3', 'movmskps eax, xmm4', 'movmskpd ecx, xmm5', 'pmovmskb edx, xmm6',
                 'shufps xmm0, xmm1, 0x1b', 'shufps xmm2, xmm2, 0xe4', 'shufpd xmm3, xmm4, 1', 'unpcklps xmm5, xmm6',
                 'unpckhps xmm7, xmm0', 'unpcklpd xmm1, xmm2', 'unpckhpd xmm3, xmm4', 'movntps xmmword ptr [{b}], xmm5',
                 'movnti dword ptr [{b}+4], esi'):
        add(form, 0)
    # ---- SSE2 integer ----
    for op in sorted(['paddb', 'paddw', 'paddd', 'paddq', 'psubb', 'psubw', 'psubd', 'psubq', 'paddsb', 'paddsw',
                      'paddusb', 'paddusw', 'psubsb', 'psubsw', 'psubusb', 'psubusw', 'pcmpeqb', 'pcmpeqw', 'pcmpeqd',
                      'pcmpgtb', 'pcmpgtw', 'pcmpgtd', 'pmullw', 'pmulhw', 'pmulhuw', 'pmuludq', 'pmaddwd', 'pavgb',
                      'pavgw', 'pminub', 'pmaxub', 'pminsw', 'pmaxsw', 'psadbw', 'pand', 'pandn', 'por', 'pxor',
                      'punpcklbw', 'punpckhbw', 'punpcklwd', 'punpckhwd', 'punpckldq', 'punpckhdq', 'punpcklqdq',
                      'punpckhqdq', 'packsswb', 'packuswb', 'packssdw']):
        add('%s xmm0, xmm1' % op, 0)
        add('%s xmm2, xmmword ptr [{b}+16]' % op, 0)
    for op in ('psllw', 'pslld', 'psllq', 'psrlw', 'psrld', 'psrlq', 'psraw', 'psrad'):
        add('%s xmm0, 3' % op, 0)
        add('%s xmm1, 40' % op, 0)
        add('%s xmm2, xmm3' % op, 0)
    add('pslldq xmm4, 5', 0)
    add('psrldq xmm5, 11', 0)
    add('pshufd xmm0, xmm1, 0x1b', 0)
    add('pshuflw xmm2, xmm3, 0x4e', 0)
    add('pshufhw xmm4, xmm5, 0xb1', 0)
    add('pinsrw xmm6, eax, 5', 0)
    add('pextrw ecx, xmm7, 3', 0)
    add('pinsrw xmm1, word ptr [{b}+2], 7', 0)
    # ---- SSE3 ----
    for op in ('haddps', 'hsubps', 'haddpd', 'hsubpd', 'addsubps', 'addsubpd', 'movsldup', 'movshdup'):
        add('%s xmm0, xmm1' % op, 0)
        add('%s xmm2, xmmword ptr [{b}+16]' % op, 0)
    add('movddup xmm3, xmm4', 0)
    add('movddup xmm5, qword ptr [{b}+8]', 0)
    add('and eax, 0x3f\nxlatb', 0)
    # ---- MMX ----
    add('cvtps2pi mm0, xmm1', 0)
    add('cvttps2pi mm2, qword ptr [{b}+16]', 0)
    add('cvtpi2ps xmm3, mm4', 0)
    for op in ('paddw', 'psubusb', 'pmullw', 'pcmpgtd', 'punpcklbw', 'packuswb', 'pxor', 'pmaddwd'):
        add('%s mm0, mm1' % op, 0)
    add('movq mm2, qword ptr [{b}]\npsllw mm2, 3\nmovq qword ptr [{b}+8], mm2', 0)
    add('movd mm3, eax\nmovd ecx, mm3', 0)
    add('pshufw mm4, mm5, 0x1b', 0)
    # ---- x87 (results through memory; buffer holds doubles at 0 and 8, floats at 16 and 20,
    # int32 at 24 and 28, int16 at 32 and 34) ----
    for op in ('faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp'):
        add('fld qword ptr [{b}]\nfld qword ptr [{b}+8]\n%s st(1), st\nfstp qword ptr [{b}+40]' % op, 0)
    for op in ('fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr'):
        add('fld qword ptr [{b}]\n%s qword ptr [{b}+8]\nfstp qword ptr [{b}+40]' % op, 0)
        add('fld qword ptr [{b}]\n%s dword ptr [{b}+16]\nfstp qword ptr [{b}+40]' % op, 0)
        add('fld qword ptr [{b}]\nfld qword ptr [{b}+8]\n%s st, st(1)\nfstp qword ptr [{b}+40]\nfstp qword ptr [{b}+48]' % op, 0)
        add('fld qword ptr [{b}]\nfld qword ptr [{b}+8]\n%s st(1), st\nfstp qword ptr [{b}+40]\nfstp qword ptr [{b}+48]' % op, 0)
        add('fld qword ptr [{b}]\nfi%s dword ptr [{b}+24]\nfstp qword ptr [{b}+40]' % op[1:], 0)
        add('fld qword ptr [{b}]\nfi%s word ptr [{b}+32]\nfstp qword ptr [{b}+40]' % op[1:], 0)
    for rc in (0x027F, 0x067F, 0x0A7F, 0x0E7F):
        add('mov word ptr [{b}+36], %d\nfldcw word ptr [{b}+36]\nfld qword ptr [{b}]\nfistp dword ptr [{b}+40]\n'
            'fld qword ptr [{b}+8]\nfrndint\nfstp qword ptr [{b}+48]\nfld dword ptr [{b}+16]\nfistp word ptr [{b}+38]\n'
            'fild dword ptr [{b}+24]\nfistp qword ptr [{b}+56]' % rc, 0, name='rounding control %04X' % rc)
    add('fild word ptr [{b}+32]\nfild dword ptr [{b}+24]\nfaddp st(1), st\nfstp dword ptr [{b}+40]', 0)
    add('fld qword ptr [{b}]\nfld qword ptr [{b}+8]\nfcompp\nfnstsw ax\nand eax, 0x4500', 0)
    add('fld qword ptr [{b}]\nfcomp qword ptr [{b}+8]\nfnstsw ax\nand eax, 0x4500', 0)
    add('fld qword ptr [{b}]\nfld qword ptr [{b}+8]\nfucompp\nfnstsw ax\nand eax, 0x4500', 0)
    add('fld qword ptr [{b}]\nficomp dword ptr [{b}+24]\nfnstsw ax\nand eax, 0x4500', 0)
    add('fld qword ptr [{b}]\nftst\nfnstsw ax\nand eax, 0x4500\nfstp st(0)', 0)
    add('fld qword ptr [{b}]\nfxam\nfnstsw ax\nand eax, 0x4700\nfstp st(0)', 0)
    for op in ('fcomip', 'fucomip'):
        add('fld qword ptr [{b}+8]\nfld qword ptr [{b}]\n%s st, st(1)\nfstp st(0)\nseta al\nsetb cl\nsete dl\nsetp ah' % op, 0)
    for op in ('fchs', 'fabs', 'fsqrt'):
        add('fld qword ptr [{b}]\n%s\nfstp qword ptr [{b}+40]' % op, 0)
    add('fld1\nfldz\nfldpi\nfstp qword ptr [{b}+40]\nfstp qword ptr [{b}+48]\nfstp qword ptr [{b}+56]', 0)
    add('fld qword ptr [{b}]\nfld qword ptr [{b}+8]\nfxch st(1)\nfstp qword ptr [{b}+40]\nfstp qword ptr [{b}+48]', 0)
    add('fld qword ptr [{b}]\nfld st(0)\nfmulp st(1), st\nfstp qword ptr [{b}+40]', 0)
    add('fld qword ptr [{b}+8]\nfld qword ptr [{b}]\nfcomi st, st(1)\nfcmovb st, st(1)\nfstp qword ptr [{b}+40]\nfstp st(0)', 0)
    add('fld qword ptr [{b}+8]\nfld qword ptr [{b}]\nfprem\nfstp qword ptr [{b}+40]\nfstp st(0)', 0)
    add('fild word ptr [{b}+34]\nfld qword ptr [{b}]\nfscale\nfstp qword ptr [{b}+40]\nfstp st(0)', 0)
    add('fld qword ptr [{b}]\nfstp dword ptr [{b}+40]\nfld dword ptr [{b}+16]\nfstp qword ptr [{b}+48]', 0)
    add('fnstcw word ptr [{b}+40]', 0)
    add('\n'.join('add eax, ecx\nrol eax, 3\ncmp eax, esi\nadc edx, 0\nmovss xmm0, dword ptr [{b}+%d]\naddss xmm1, xmm0' % (4 * (k % 8))
                  for k in range(24)), name='long straight-line block')
    return cases


# ---------------- inputs ----------------
EDGE32 = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 31, 32, 33]
FLOATS = [0.0, -0.0, 1.0, -1.5, 3.25, 0.1, -1e30, 1e-40, 2.5, -2.5, 65536.75, -7.0e9, float('inf'), float('-inf'),
          float('nan'), 1.0e10, 0.5, -0.5]
DOUBLES = FLOATS + [1e300, 5e-320, 2147483647.5, -2147483648.4, 123456.789, 0.49999999999999994]


def make_inputs(rng):
    states = []
    for k in range(INPUTS):
        s = bytearray(STATE_SIZE)
        for i in range(8):
            v = rng.choice(EDGE32) if rng.random() < 0.5 else rng.getrandbits(32)
            struct.pack_into('<I', s, 4 * i, v)
        flags = 0
        for bit in (0x1, 0x4, 0x10, 0x40, 0x80, 0x800):
            if rng.random() < 0.5:
                flags |= bit
        struct.pack_into('<I', s, OFF_FLAGS, flags | 0x202)
        for i in range(8):
            kind = (i + k) % 3
            if kind == 0:
                v = struct.pack('<4f', *[rng.choice(FLOATS) if rng.random() < 0.6 else rng.uniform(-1000, 1000) for _ in range(4)])
            elif kind == 1:
                v = struct.pack('<2d', *[rng.choice(DOUBLES) if rng.random() < 0.6 else rng.uniform(-1e6, 1e6) for _ in range(2)])
            else:
                v = bytes(rng.getrandbits(8) for _ in range(16))
            s[OFF_XMM + 16 * i:OFF_XMM + 16 * i + 16] = v
        for i in range(8):
            s[OFF_MM + 8 * i:OFF_MM + 8 * i + 8] = bytes(rng.getrandbits(8) for _ in range(8))
        mem = bytearray(rng.getrandbits(8) for _ in range(64))
        struct.pack_into('<2d', mem, 0, rng.choice(DOUBLES[:-2]) if k % 2 else rng.uniform(-1000, 1000),
                         rng.choice(DOUBLES) if k % 3 else rng.uniform(-50, 50))
        struct.pack_into('<2f', mem, 16, rng.choice(FLOATS), rng.uniform(-100, 100))
        struct.pack_into('<2i', mem, 24, rng.choice([0, 1, -1, 7, -300, 2147483647]), rng.randint(-100000, 100000))
        struct.pack_into('<2h', mem, 32, rng.randint(-200, 200), rng.randint(-20, 20))
        s[OFF_MEM:OFF_MEM + 64] = mem
        states.append(bytes(s))
    return states


# ---------------- assembly ----------------
def asm64(cases):
    out = ['.intel_syntax noprefix', '.text']
    for k, (_, snippet, _) in enumerate(cases):
        load = ['mov %s, dword ptr [r15 + %d]' % (r, 4 * i) for i, r in enumerate(['eax', 'ecx', 'edx', None, None, 'ebp', 'esi', 'edi'])
                if r]
        out += ['.p2align 4', '.globl _case_%d' % k, '_case_%d:' % k,
                'push rbx', 'push rbp', 'push r12', 'push r13', 'push r14', 'push r15', 'sub rsp, 8',
                'mov r15, rdi', 'fninit', 'fldcw word ptr [rip + _cw]']
        out += ['movdqu xmm%d, xmmword ptr [r15 + %d]' % (i, OFF_XMM + 16 * i) for i in range(8)]
        mmx = re.search(r'\bmm\d', snippet) is not None
        if mmx:
            out += ['movq mm%d, qword ptr [r15 + %d]' % (i, OFF_MM + 8 * i) for i in range(8)]
        out += load + ['lea rbx, [r15 + %d]' % OFF_MEM, 'push qword ptr [r15 + %d]' % OFF_FLAGS, 'popfq']
        out += snippet.format(b='rbx').replace('esi, ebx', 'rsi, rbx').replace('edi, ebx', 'rdi, rbx').replace(
            'lea esi,', 'lea rsi,').replace('lea edi,', 'lea rdi,').split('\n')
        out += ['pushfq', 'pop qword ptr [r15 + %d]' % OFF_FLAGS]
        out += ['mov dword ptr [r15 + %d], %s' % (4 * i, r) for i, r in enumerate(['eax', 'ecx', 'edx', None, None, 'ebp', 'esi', 'edi']) if r]
        out += ['movdqu xmmword ptr [r15 + %d], xmm%d' % (OFF_XMM + 16 * i, i) for i in range(8)]
        if mmx:
            out += ['movq qword ptr [r15 + %d], mm%d' % (OFF_MM + 8 * i, i) for i in range(8)]
        out += ['emms', 'add rsp, 8', 'pop r15', 'pop r14', 'pop r13', 'pop r12', 'pop rbp', 'pop rbx', 'ret']
    out += ['.data', '.p2align 1', '_cw: .short 0x027F', '']
    return '\n'.join(out)


def asm32(cases):
    out = ['.intel_syntax noprefix', '.text']
    for k, (_, snippet, _) in enumerate(cases):
        out += ['.p2align 4', '.globl case_%d' % k, 'case_%d:' % k]
        out += snippet.format(b='ebx').split('\n')
        out += ['ret']
    out.append('')
    return '\n'.join(out)


def elf32_text_and_symbols(path):
    data = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    sections = []
    for i in range(shnum):
        sections.append(struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize))
    names = sections[shstrndx]

    def name(off, strtab):
        end = data.index(b'\0', strtab[4] + off)
        return data[strtab[4] + off:end].decode()
    text_index = next(i for i, s in enumerate(sections) if name(s[0], names) == '.text')
    text = data[sections[text_index][4]:sections[text_index][4] + sections[text_index][5]]
    symtab = next(s for s in sections if s[1] == 2)
    strtab = sections[symtab[6]]
    symbols = {}
    for off in range(symtab[4], symtab[4] + symtab[5], 16):
        st_name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, off)
        if shndx == text_index and st_name:
            symbols[name(st_name, strtab)] = value
    return text, symbols


class TestImage:
    def __init__(self, base, text):
        self.base, self.text = base, text

    def in_text(self, va):
        return self.base <= va < self.base + len(self.text)

    def contains(self, va, n=1):
        return self.base <= va and va + n <= self.base + len(self.text)

    def bytes(self, va, n):
        off = va - self.base
        return bytes(self.text[off:off + n]).ljust(n, b'\xcc')

    def u8(self, va):
        return self.text[va - self.base]

    def u32(self, va):
        return struct.unpack_from('<I', self.text, va - self.base)[0]

    def imports(self):
        return {}


def run(cmd, binary=False):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        err = r.stderr.decode(errors='replace')
        if binary:
            err = '\n'.join(err.splitlines()[-3:])
        else:
            err = r.stdout.decode(errors='replace') + err
        sys.stderr.write(err + '\n')
        raise SystemExit('command failed (%d): %s' % (r.returncode, ' '.join(cmd)))
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', required=True)
    ap.add_argument('--seed', type=int, default=1994)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    cases = build_cases()
    inputs = make_inputs(random.Random(args.seed))

    # ground truth: x86_64 under Rosetta
    p = lambda n: os.path.join(args.out, n)  # noqa: E731
    open(p('cases64.s'), 'w').write(asm64(cases))
    blob = b''.join(inputs)
    harness = ['#include <stdio.h>', '#include <string.h>',
               'typedef struct { unsigned char b[%d]; } __attribute__((aligned(16))) State;' % STATE_SIZE]
    harness += ['void case_%d(State*);' % k for k in range(len(cases))]
    harness.append('static void (*const cases[])(State*) = {%s};' % ', '.join('case_%d' % k for k in range(len(cases))))
    harness.append('static const unsigned char inputs[] = {%s};' % ','.join(str(b) for b in blob))
    harness.append('int main(void) { for (unsigned k = 0; k < %d; ++k) for (unsigned j = 0; j < %d; ++j) { State s; '
                   'fprintf(stderr, "case %%u input %%u\\n", k, j); '
                   'memcpy(&s, inputs + j * %d, %d); cases[k](&s); fwrite(&s, %d, 1, stdout); } return 0; }' % (
                       len(cases), INPUTS, STATE_SIZE, STATE_SIZE, STATE_SIZE))
    open(p('harness.c'), 'w').write('\n'.join(harness) + '\n')
    run(['clang', '-arch', 'x86_64', '-O1', '-o', p('harness'), p('harness.c'), p('cases64.s')])
    truth = run(['arch', '-x86_64', p('harness')], binary=True)
    assert len(truth) == len(cases) * INPUTS * STATE_SIZE

    # translation of the same snippets assembled for i386
    open(p('cases32.s'), 'w').write(asm32(cases))
    run(['clang', '-target', 'i386-unknown-linux-gnu', '-c', '-o', p('cases32.o'), p('cases32.s')])
    text, symbols = elf32_text_and_symbols(p('cases32.o'))
    base = 0x00400000
    img = TestImage(base, text)
    entries = [base + symbols['case_%d' % k] for k in range(len(cases))]
    tr = Translator(img, {'functions': [{'entry': e} for e in entries]}, flags_live_at_exit=True)
    # cut every case of more than a few instructions into chained parts, so the
    # splitting of oversized game functions is checked against x86 too
    FunctionTranslator.SPLIT_INSNS = 4
    funcs = []
    for e in entries:
        fn = tr.walker.walk(e)
        funcs.append(FunctionTranslator(tr, fn).translate())

    def carr(bs):
        return '{' + ','.join(str(b) for b in bs) + '}'
    src = ['// Generated by tools/x86tr/semtest.py: x86 instruction semantics, expected values from real x86 (Rosetta 2).',
           '#include "recomp/guest.hpp"', '#include <cstdio>', '#include <vector>', '']
    src += funcs
    src.append('static GuestFn const k_cases[] = {%s};' % ', '.join('f_%08X' % e for e in entries))
    src.append('static const char* const k_names[] = {%s};' % ', '.join(
        '"%s"' % name.replace('\n', '; ').replace('"', '\\"') for name, _, _ in cases))
    src.append('static const unsigned k_flag_masks[] = {%s};' % ', '.join(str(m) for _, _, m in cases))
    src.append('static const unsigned char k_inputs[] = %s;' % carr(blob))
    src.append('static const unsigned char k_expected[] = %s;' % carr(truth))
    src.append(r'''
namespace {
constexpr unsigned StateSize = %d, OffFlags = %d, OffXmm = %d, OffMm = %d, OffMem = %d, Inputs = %d;
constexpr std::uint32_t GuestMem = 0x%X, StackTop = 0x10000;
std::uint32_t u32(const unsigned char* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
}

int main() {
    std::vector<std::uint8_t> memory(0x20000);
    unsigned failures = 0, checked = 0;
    for (unsigned k = 0; k < sizeof k_cases / sizeof k_cases[0]; ++k) {
        for (unsigned j = 0; j < Inputs; ++j) {
            const unsigned char* in = k_inputs + j * StateSize;
            const unsigned char* want = k_expected + (k * Inputs + j) * StateSize;
            Cpu c;
            c.mem = memory.data();
            for (int i = 0; i < 8; ++i) c.r[i] = u32(in + 4 * i);
            c.r[Cpu::EBX] = GuestMem;
            c.r[Cpu::ESP] = StackTop - 4;
            st32(c.mem, StackTop - 4, 0xDEADBEEFu);
            c.set_eflags(u32(in + OffFlags));
            std::memcpy(c.xmm, in + OffXmm, 128);
            std::memcpy(c.mm, in + OffMm, 64);
            std::memcpy(c.mem + GuestMem, in + OffMem, 64);
            k_cases[k](c);
            ++checked;
            char what[160] = "";
            static const char* const names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
            for (int i = 0; i < 8 && !what[0]; ++i) {
                if (i == Cpu::EBX || i == Cpu::ESP) continue;
                if (c.r[i] != u32(want + 4 * i))
                    std::snprintf(what, sizeof what, "%%s = %%08X, x86 %%08X", names[i], c.r[i], u32(want + 4 * i));
            }
            unsigned mask = k_flag_masks[k];
            if (!what[0] && ((c.eflags() ^ u32(want + OffFlags)) & mask))
                std::snprintf(what, sizeof what, "flags = %%03X, x86 %%03X (compared %%03X)", c.eflags() & mask,
                              u32(want + OffFlags) & mask, mask);
            if (!what[0] && c.r[Cpu::ESP] != StackTop) std::snprintf(what, sizeof what, "esp = %%08X", c.r[Cpu::ESP]);
            for (int i = 0; i < 8 && !what[0]; ++i)
                if (std::memcmp(&c.xmm[i], want + OffXmm + 16 * i, 16)) {
                    std::uint32_t g[4], w[4];
                    std::memcpy(g, &c.xmm[i], 16);
                    std::memcpy(w, want + OffXmm + 16 * i, 16);
                    std::snprintf(what, sizeof what, "xmm%%d = %%08X %%08X %%08X %%08X, x86 %%08X %%08X %%08X %%08X", i,
                                  g[3], g[2], g[1], g[0], w[3], w[2], w[1], w[0]);
                }
            for (int i = 0; i < 8 && !what[0]; ++i)
                if (std::memcmp(&c.mm[i], want + OffMm + 8 * i, 8)) std::snprintf(what, sizeof what, "mm%%d differs", i);
            for (int i = 0; i < 64 && !what[0]; ++i)
                if (c.mem[GuestMem + i] != want[OffMem + i])
                    std::snprintf(what, sizeof what, "memory +%%d = %%02X, x86 %%02X", i, c.mem[GuestMem + i], want[OffMem + i]);
            if (what[0]) {
                static unsigned last_case = ~0u;
                if (++failures <= 400 && last_case != k) std::printf("FAIL %%s [input %%u]: %%s\n", k_names[k], j, what);
                last_case = k;
            }
        }
    }
    std::printf("%%u / %%u case runs match x86\n", checked - failures, checked);
    return failures ? 1 : 0;
}
''' % (STATE_SIZE, OFF_FLAGS, OFF_XMM, OFF_MM, OFF_MEM, INPUTS, GUEST_MEM))
    target = p('x86_semantics.cpp')
    with open(target + '.partial', 'w') as f:
        f.write('\n'.join(src))
    os.replace(target + '.partial', target)
    print('%d cases x %d inputs -> %s' % (len(cases), INPUTS, target))


if __name__ == '__main__':
    main()
