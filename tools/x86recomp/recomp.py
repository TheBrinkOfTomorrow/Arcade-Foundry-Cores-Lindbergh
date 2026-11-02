#!/usr/bin/env python3
"""Static recompiler: 32-bit x86 Linux ELF -> portable C.

Every guest function becomes a C function `void F_<addr>(CPU *c)`. Guest
registers live in C locals inside a function and are written back to the CPU
struct around calls; flags are C locals the compiler drops when nothing reads
them. Imports go through PLT stubs that call the host library layer
(`hle_<name>`), and indirect calls go through guest_dispatch().

Usage: recomp.py GAME.elf OUTDIR [--per-file N]
"""
import argparse
import bisect
import collections
import os
import re
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import (X86_OP_REG, X86_OP_IMM, X86_OP_MEM)
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
R8L = ['al', 'cl', 'dl', 'bl']
R8H = ['ah', 'ch', 'dh', 'bh']
UT = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}
ST = {8: 'int8_t', 16: 'int16_t', 32: 'int32_t', 64: 'int64_t'}
MASK = {8: '0xffu', 16: '0xffffu', 32: '0xffffffffu'}

COND = {
    'o': 'OF', 'no': '!OF', 'b': 'CF', 'ae': '!CF', 'e': 'ZF', 'ne': '!ZF',
    'be': '(CF|ZF)', 'a': '(!CF&&!ZF)', 's': 'SF', 'ns': '!SF', 'p': 'PF',
    'np': '!PF', 'l': '(SF!=OF)', 'ge': '(SF==OF)', 'le': '(ZF||SF!=OF)',
    'g': '(!ZF&&SF==OF)',
}
CC_ALIAS = {'c': 'b', 'nae': 'b', 'nc': 'ae', 'nb': 'ae', 'z': 'e', 'nz': 'ne',
            'na': 'be', 'nbe': 'a', 'pe': 'p', 'po': 'np', 'nge': 'l',
            'nl': 'ge', 'ng': 'le', 'nle': 'g'}

SAVE = 'c->r[0]=eax;c->r[1]=ecx;c->r[2]=edx;c->r[3]=ebx;c->r[4]=esp;c->r[5]=ebp;c->r[6]=esi;c->r[7]=edi;c->df=DF;'
LOAD = 'eax=c->r[0];ecx=c->r[1];edx=c->r[2];ebx=c->r[3];esp=c->r[4];ebp=c->r[5];esi=c->r[6];edi=c->r[7];DF=c->df;'


def cc_of(suffix):
    suffix = CC_ALIAS.get(suffix, suffix)
    return COND.get(suffix)


class Unsupported(Exception):
    pass


class Image:
    """An ELF's code, data and symbols at `base` (0 for the program; a
    shared library is placed at a fixed base so its code can be translated
    ahead of time)."""

    def __init__(self, path, base=0):
        self.f = ELFFile(open(path, 'rb'))
        self.file = os.path.basename(path)
        self.base = base
        self.exec_secs = []
        self.data_secs = []
        for s in self.f.iter_sections():
            if s['sh_type'] != 'SHT_PROGBITS' or not (s['sh_flags'] & 2):
                continue
            if s['sh_flags'] & 4:
                if s.name in ('.plt',):
                    continue
                self.exec_secs.append((base + s['sh_addr'], base + s['sh_addr'] + s['sh_size'], s.data(), s.name))
            else:
                self.data_secs.append((base + s['sh_addr'], s.data(), s.name))
        self.exec_secs.sort()
        self.entry = base + self.f.header['e_entry']
        self.syms = {}
        for s in self.f.iter_sections():
            if isinstance(s, SymbolTableSection):
                for sym in s.iter_symbols():
                    if sym['st_info']['type'] == 'STT_FUNC' and sym['st_value'] and sym['st_shndx'] != 'SHN_UNDEF':
                        self.syms.setdefault(base + sym['st_value'], sym.name)
        # PLT: entry i (from 0) of .rel.plt sits at plt + 16*(i+1)
        plt = self.f.get_section_by_name('.plt')
        self.plt_start = base + plt['sh_addr']
        self.plt_end = base + plt['sh_addr'] + plt['sh_size']
        relplt = self.f.get_section_by_name('.rel.plt')
        dsym = self.f.get_section_by_name('.dynsym')
        self.imports = []      # (plt_addr, got_addr, name)
        for i, rel in enumerate(relplt.iter_relocations()):
            name = dsym.get_symbol(rel['r_info_sym']).name
            self.imports.append((self.plt_start + 16 * (i + 1), base + rel['r_offset'], name))
        self.plt_by_addr = {p: n for p, g, n in self.imports}
        reldyn = self.f.get_section_by_name('.rel.dyn')
        self.copies = []       # (addr, name) for R_386_COPY
        self.copy_size = {}
        self.relocs = []       # (addr, type, symbol name or '') the loader applies to a library
        if reldyn:
            for rel in reldyn.iter_relocations():
                t = rel['r_info_type']
                name = dsym.get_symbol(rel['r_info_sym']).name if rel['r_info_sym'] else ''
                if t == 5:
                    self.copies.append((base + rel['r_offset'], name))
                    self.copy_size[name] = dsym.get_symbol(rel['r_info_sym'])['st_size']
                elif base or t == 6:
                    # a library's relocations; the program's own GOT entries for
                    # symbols it takes the address of (Race TV: pthread_create)
                    self.relocs.append((base + rel['r_offset'], t, name))
        # what the dynamic linker would let other modules bind to
        self.exports = []
        for sym in dsym.iter_symbols():
            if (sym.name and sym['st_shndx'] != 'SHN_UNDEF' and sym['st_value'] and
                    sym['st_info']['bind'] in ('STB_GLOBAL', 'STB_WEAK') and
                    sym['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT', 'STT_NOTYPE')):
                self.exports.append((sym.name, base + sym['st_value']))
        self.init = 0
        self.pltgot = 0
        for sec in self.f.iter_sections():
            if sec['sh_type'] == 'SHT_DYNAMIC':
                for t in sec.iter_tags():
                    if t.entry.d_tag == 'DT_INIT':
                        self.init = base + t.entry.d_val
                    elif t.entry.d_tag == 'DT_PLTGOT':
                        self.pltgot = base + t.entry.d_val
        self.objects = {}      # data symbols, for the runtime to find game variables
        self.func_by_name = {}
        for s in self.f.iter_sections():
            if isinstance(s, SymbolTableSection) and s.name == '.symtab':
                for sym in s.iter_symbols():
                    t = sym['st_info']['type']
                    if t == 'STT_OBJECT' and sym['st_value'] and sym['st_shndx'] != 'SHN_UNDEF':
                        self.objects.setdefault(sym.name, base + sym['st_value'])
                    if t == 'STT_FUNC' and sym['st_value'] and sym['st_shndx'] != 'SHN_UNDEF':
                        self.func_by_name.setdefault(sym.name, base + sym['st_value'])
        # words in data that may be code addresses: absolute ones, and for a
        # library also unrelocated ones (base-relative) and PIC jump-table
        # entries (relative to the GOT, which %ebx holds)
        self.ref_bias = [0] + ([base, self.pltgot] if base else [])
        self.libs = []

    def add_library(self, lib):
        """Translate a shared library along with this program."""
        self.libs.append(lib)
        self.exec_secs = sorted(self.exec_secs + lib.exec_secs)
        self.data_secs += lib.data_secs
        for a, n in lib.syms.items():
            self.syms.setdefault(a, n)
        self.imports += lib.imports
        self.plt_by_addr.update(lib.plt_by_addr)
        for n, a in lib.func_by_name.items():
            self.func_by_name.setdefault(n, a)
        self.ref_bias = sorted(set(self.ref_bias + lib.ref_bias))

    def patch(self, addr, data):
        for k, (lo, hi, d, name) in enumerate(self.exec_secs):
            if lo <= addr and addr + len(data) <= hi:
                b = bytearray(d)
                b[addr - lo:addr - lo + len(data)] = data
                self.exec_secs[k] = (lo, hi, bytes(b), name)
                return
        raise Exception('patch outside code: 0x%x' % addr)

    def _sec_index(self, a):
        if getattr(self, '_lows_n', None) != len(self.exec_secs):
            self._lows = [lo for lo, hi, d, n in self.exec_secs]
            self._lows_n = len(self.exec_secs)
        k = bisect.bisect_right(self._lows, a) - 1
        return k if k >= 0 and a < self.exec_secs[k][1] else -1

    def in_exec(self, a):
        return self._sec_index(a) >= 0

    def sec_of(self, a):
        k = self._sec_index(a)
        if k < 0:
            return None
        lo, hi, data, name = self.exec_secs[k]
        return lo, hi, data


class Op:
    """Operand helpers bound to one instruction."""

    def __init__(self, ins):
        self.ins = ins

    def rname(self, r):
        return self.ins.reg_name(r)


def regref(name):
    """(local, kind) for a GPR name; kind in 32/16/8l/8h."""
    if name in R32:
        return name, 32
    if name in R16:
        return R32[R16.index(name)], 16
    if name in R8L:
        return R32[R8L.index(name)], '8l'
    if name in R8H:
        return R32[R8H.index(name)], '8h'
    return None, None


class FuncGen:
    def __init__(self, rc, start, end, insns, entries):
        self.rc = rc
        self.start = start
        self.end = end
        self.insns = insns
        self.entries = entries
        self.out = []
        self.tmp = 0
        self.labels = set()
        self.jmpbufs = []

    # ---- operands -------------------------------------------------------
    def addr(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(regref(ins.reg_name(m.base))[0])
        if m.index:
            idx = regref(ins.reg_name(m.index))[0]
            parts.append(idx if m.scale == 1 else '%s*%du' % (idx, m.scale))
        if m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        seg = ins.reg_name(m.segment) if m.segment else None
        if seg in ('fs', 'gs'):
            raise Unsupported('segment ' + seg)
        return '(uint32_t)(' + '+'.join(parts) + ')'

    def rd(self, ins, op, bits=None):
        bits = bits or op.size * 8
        if op.type == X86_OP_REG:
            loc, kind = regref(ins.reg_name(op.reg))
            if loc is None:
                raise Unsupported('reg ' + ins.reg_name(op.reg))
            if kind == 32:
                return loc
            if kind == 16:
                return '(uint16_t)' + loc
            if kind == '8l':
                return '(uint8_t)' + loc
            return '(uint8_t)(%s>>8)' % loc
        if op.type == X86_OP_IMM:
            v = op.imm & ((1 << bits) - 1)
            return '(%s)0x%xu' % (UT[bits], v)
        if op.type == X86_OP_MEM:
            return 'rd%d(%s)' % (bits, self.addr(ins, op))
        raise Unsupported('operand')

    def wr(self, ins, op, val, bits=None):
        bits = bits or op.size * 8
        if op.type == X86_OP_REG:
            loc, kind = regref(ins.reg_name(op.reg))
            if loc is None:
                raise Unsupported('reg ' + ins.reg_name(op.reg))
            if kind == 32:
                return '%s=(uint32_t)(%s);' % (loc, val)
            if kind == 16:
                return '%s=(%s&0xffff0000u)|(uint16_t)(%s);' % (loc, loc, val)
            if kind == '8l':
                return '%s=(%s&0xffffff00u)|(uint8_t)(%s);' % (loc, loc, val)
            return '%s=(%s&0xffff00ffu)|((uint32_t)(uint8_t)(%s)<<8);' % (loc, loc, val)
        if op.type == X86_OP_MEM:
            return 'wr%d(%s,(%s)(%s));' % (bits, self.addr(ins, op), UT[bits], val)
        raise Unsupported('write operand')

    def szf(self, r, bits):
        return 'ZF=(%s)==0;SF=((%s)>>%d)&1;PF=parity8(%s);' % (r, r, bits - 1, r)

    def t(self):
        self.tmp += 1
        return 't%d' % self.tmp

    # ---- control flow helpers -------------------------------------------
    def goto(self, target):
        if self.start <= target < self.end and target in self.inset:
            self.labels.add(target)
            return 'goto L_%x;' % target
        return self.tailcall(target)

    def callname(self, target):
        return self.rc.fn_name(target)

    def tailcall(self, target):
        name = self.callname(target)
        if name is None:
            return '{%s guest_unimpl(c,0x%xu,"tail jump to unknown 0x%x"); return;}' % (SAVE, self.cur, target)
        return '{%s %s(c); return;}' % (SAVE, name)

    # ---- translation ------------------------------------------------------
    def gen(self):
        self.inset = set(i.address for i in self.insns)
        body = []
        for ins in self.insns:
            self.cur = ins.address
            try:
                code = self.one(ins)
            except Unsupported as e:
                self.rc.unsupported[ins.mnemonic + ' ' + str(e)] += 1
                code = '%s guest_unimpl(c,0x%xu,"%s %s"); return;' % (SAVE, ins.address, ins.mnemonic, ins.op_str.replace('"', ''))
            body.append((ins.address, code))
        # labels referenced from data (switch tables) inside this function
        for a in self.rc.data_refs_in(self.start, self.end):
            if a in self.inset:
                self.labels.add(a)
        for ins in self.insns:
            for a in self.rc.jtables.get(ins.address, ()):
                if a in self.inset:
                    self.labels.add(a)
        for e in self.entries:
            self.labels.add(e)
        out = []
        multi = len(self.entries) > 1
        fname = 'B_%x' % self.start if multi else self.rc.fn_name(self.start)
        if multi:
            out.append('static void %s(CPU *c, uint32_t entry){' % fname)
        else:
            out.append('void %s(CPU *c){' % fname)
        out.append('uint32_t eax,ecx,edx,ebx,esp,ebp,esi,edi,DF;uint32_t CF=0,ZF=0,SF=0,OF=0,PF=0;' + LOAD)
        for n in self.jmpbufs:
            out.append('jmp_buf hb_%d;volatile uint32_t jbv_%d;' % (n, n))
        if multi:
            out.append('switch(entry){' + ''.join('case 0x%xu: goto L_%x;' % (e, e) for e in self.entries) + '}')
        last_fallthrough = True
        for a, code in body:
            if a in self.labels:
                out.append('L_%x:' % a)
            out.append(code)
        # code that runs past the end continues in the next function (the
        # Intel compiler splits functions into "name" and "name." this way)
        if self.callname(self.end):
            out.append('%s %s(c); return;' % (SAVE, self.callname(self.end)))
        else:
            out.append(SAVE + 'guest_unimpl(c,0x%xu,"fell off end"); return;' % self.end)
        out.append('}')
        if multi:
            for e in self.entries:
                out.append('void %s(CPU *c){%s(c,0x%xu);}' % (self.rc.fn_name(e), fname, e))
        return '\n'.join(out)

    def one(self, ins):
        mn = ins.mnemonic
        ops = ins.operands
        prefix = None
        if mn.startswith('rep ') or mn.startswith('repe ') or mn.startswith('repne ') or mn.startswith('repz ') or mn.startswith('repnz '):
            prefix, mn = mn.split(' ', 1)
        if mn.startswith('bnd ') or mn.startswith('lock '):
            mn = mn.split(' ', 1)[1]
        b0 = ins.bytes
        # strip prefixes for the opcode byte
        k = 0
        while b0[k] in (0x66, 0x67, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0xf0):
            k += 1
        opc = b0[k]
        if 0xd8 <= opc <= 0xdf:
            return self.x87(ins, opc, b0[k + 1])
        if mn in ('movsd', 'cmpsd') and len(ops) == 2 and ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_MEM:
            mn = mn + '_str'
        h = getattr(self, 'i_' + mn.replace(' ', '_'), None)
        if h:
            return h(ins, ops) if prefix is None else h(ins, ops, prefix)
        if mn.startswith('j') and cc_of(mn[1:]):
            return 'if(%s) %s' % (cc_of(mn[1:]), self.goto(ops[0].imm & 0xffffffff))
        if mn.startswith('set') and cc_of(mn[3:]):
            return self.wr(ins, ops[0], cc_of(mn[3:]) + '?1:0', 8)
        if mn.startswith('cmov') and cc_of(mn[4:]):
            return 'if(%s){%s}' % (cc_of(mn[4:]), self.wr(ins, ops[0], self.rd(ins, ops[1])))
        sse = self.rc.sse.gen(self, ins, mn, ops)
        if sse is not None:
            return sse
        raise Unsupported('no handler')

    # ---- integer ----------------------------------------------------------
    def i_nop(self, ins, ops):
        return ';'

    i_fnop = i_nop
    i_prefetchnta = i_nop
    i_prefetcht0 = i_nop
    i_prefetcht1 = i_nop
    i_prefetcht2 = i_nop
    i_sfence = i_nop
    i_lfence = i_nop
    i_mfence = i_nop
    i_emms = i_nop

    def i_mov(self, ins, ops):
        if ops[0].type == X86_OP_REG and ins.reg_name(ops[0].reg) in ('ds', 'es', 'fs', 'gs', 'ss', 'cs'):
            raise Unsupported('segment write')
        if ops[1].type == X86_OP_REG and ins.reg_name(ops[1].reg) in ('ds', 'es', 'fs', 'gs', 'ss', 'cs'):
            raise Unsupported('segment read')
        return self.wr(ins, ops[0], self.rd(ins, ops[1], ops[0].size * 8))

    def i_movabs(self, ins, ops):
        return self.i_mov(ins, ops)

    def i_movzx(self, ins, ops):
        return self.wr(ins, ops[0], self.rd(ins, ops[1]))

    def i_movsx(self, ins, ops):
        b = ops[1].size * 8
        return self.wr(ins, ops[0], '(%s)(%s)(%s)' % (UT[ops[0].size * 8], ST[b], self.rd(ins, ops[1])))

    def i_lea(self, ins, ops):
        return self.wr(ins, ops[0], self.addr(ins, ops[1]))

    def i_xchg(self, ins, ops):
        t = self.t()
        b = ops[0].size * 8
        return '{%s %s=%s;%s%s}' % (UT[b], t, self.rd(ins, ops[0]), self.wr(ins, ops[0], self.rd(ins, ops[1])), self.wr(ins, ops[1], t))

    def i_xadd(self, ins, ops):
        b = ops[0].size * 8
        u = UT[b]
        flags = 'CF=r<a;OF=((~(a^b)&(a^r))>>%d)&1;' % (b - 1) + self.szf('r', b)
        if ins.mnemonic.startswith('lock ') and ops[0].type == X86_OP_MEM:
            # guest threads are host threads: a locked add must be atomic
            return '{%s b=%s,a=__atomic_fetch_add((%s *)(M+%s),b,__ATOMIC_SEQ_CST),r=(%s)(a+b);%s%s}' % (
                u, self.rd(ins, ops[1]), u, self.addr(ins, ops[0]), u, flags, self.wr(ins, ops[1], 'a'))
        return '{%s a=%s,b=%s,r=(%s)(a+b);%s%s%s}' % (
            u, self.rd(ins, ops[0]), self.rd(ins, ops[1]), u, flags, self.wr(ins, ops[1], 'a'), self.wr(ins, ops[0], 'r'))

    def i_push(self, ins, ops):
        b = ops[0].size * 8
        if ops[0].type == X86_OP_IMM:
            # push imm8/imm32 store 4 bytes; with an operand-size prefix (66) 2
            # (GCC 2.96 builds a short argument with sub esp,2; push word)
            b = 16 if 0x66 in ins.bytes[:ins.bytes.index(ins.opcode[0])] else 32
            v = '0x%xu' % (ops[0].imm & (0xffff if b == 16 else 0xffffffff))
        else:
            v = self.rd(ins, ops[0])
        if b == 16:
            return '{uint16_t t=%s;esp-=2;wr16(esp,t);}' % v
        return '{uint32_t t=%s;esp-=4;wr32(esp,t);}' % v

    def i_pop(self, ins, ops):
        b = ops[0].size * 8
        if b == 16:
            return '{uint16_t t=rd16(esp);esp+=2;%s}' % self.wr(ins, ops[0], 't')
        # pop to a memory operand addressed off esp uses the incremented esp
        return '{uint32_t t=rd32(esp);esp+=4;%s}' % self.wr(ins, ops[0], 't')

    def i_pushal(self, ins, ops):
        raise Unsupported('pushal')

    def i_leave(self, ins, ops):
        return 'esp=ebp;ebp=rd32(esp);esp+=4;'

    def i_ret(self, ins, ops):
        n = 4 + (ops[0].imm if ops else 0)
        return 'esp+=%du;%s return;' % (n, SAVE)

    def i_call(self, ins, ops):
        nxt = ins.address + ins.size
        push = 'esp-=4;wr32(esp,0x%xu);' % nxt
        op = ops[0]
        if op.type == X86_OP_IMM:
            target = op.imm & 0xffffffff
            if target == nxt:
                # call/pop to read eip
                return push
            if self.rc.img.plt_by_addr.get(target) in ('_setjmp', 'setjmp', '__sigsetjmp'):
                return self.setjmp(nxt)
            name = self.callname(target)
            if name is None:
                return '%s%s guest_unimpl(c,0x%xu,"call to unknown 0x%x"); return;' % (push, SAVE, ins.address, target)
            return '%s%s %s(c);%s' % (push, SAVE, name, LOAD)
        t = self.rd(ins, op)
        return '{uint32_t t=%s;%s%s guest_dispatch(c,t);%s}' % (t, push, SAVE, LOAD)

    def setjmp(self, nxt):
        """setjmp in line: the host jmp_buf must live in this function's frame.
        The guest jmp_buf gets glibc's ebx, esi, edi, ebp, esp, pc and, at
        offset 32 (the signal mask, unused by _setjmp), the host jmp_buf for
        longjmp (runtime). After a longjmp the registers come from the guest
        jmp_buf, as the guest's setjmp would return them."""
        n = len(self.jmpbufs)
        self.jmpbufs.append(n)
        return ('jbv_%d=rd32(esp);{uint32_t j=jbv_%d;wr32(j,ebx);wr32(j+4,esi);wr32(j+8,edi);wr32(j+12,ebp);'
                'wr32(j+16,esp);wr32(j+20,0x%xu);jmp_buf *hp=&hb_%d;memcpy(M+j+32,&hp,sizeof hp);}%s'
                '{int r=setjmp(hb_%d);if(r){uint32_t j=jbv_%d;ebx=rd32(j);esi=rd32(j+4);edi=rd32(j+8);'
                'ebp=rd32(j+12);esp=rd32(j+16);eax=(uint32_t)r;}else eax=0;}' % (n, n, nxt, n, SAVE, n, n))

    def i_jmp(self, ins, ops):
        op = ops[0]
        if op.type == X86_OP_IMM:
            return self.goto(op.imm & 0xffffffff)
        t = self.rd(ins, op)
        refs = set(self.rc.data_refs_in(self.start, self.end)) | set(self.rc.jtables.get(ins.address, ()))
        cases = ''.join('case 0x%xu: goto L_%x;' % (a, a) for a in sorted(refs) if a in self.inset)
        return '{uint32_t t=%s;switch(t){%sdefault:%s guest_dispatch(c,t);return;}}' % (t, cases, SAVE)

    def arith(self, ins, ops, kind):
        b = ops[0].size * 8
        a = self.rd(ins, ops[0])
        s = self.rd(ins, ops[1], b) if len(ops) > 1 else None
        u = UT[b]
        msb = b - 1
        pre = '{%s a=%s,b=%s,r;' % (u, a, s) if s else None
        if kind == 'add':
            body = pre + 'r=(%s)(a+b);CF=r<a;OF=((~(a^b)&(a^r))>>%d)&1;' % (u, msb) + self.szf('r', b)
        elif kind in ('sub', 'cmp'):
            body = pre + 'r=(%s)(a-b);CF=a<b;OF=(((a^b)&(a^r))>>%d)&1;' % (u, msb) + self.szf('r', b)
        elif kind == 'adc':
            body = pre + 'uint32_t ci=CF;r=(%s)(a+b+ci);CF=ci?r<=a:r<a;OF=((~(a^b)&(a^r))>>%d)&1;' % (u, msb) + self.szf('r', b)
        elif kind == 'sbb':
            body = pre + 'uint32_t ci=CF;r=(%s)(a-b-ci);CF=ci?a<=b:a<b;OF=(((a^b)&(a^r))>>%d)&1;' % (u, msb) + self.szf('r', b)
        elif kind in ('and', 'test'):
            body = pre + 'r=a&b;CF=0;OF=0;' + self.szf('r', b)
        elif kind == 'or':
            body = pre + 'r=a|b;CF=0;OF=0;' + self.szf('r', b)
        elif kind == 'xor':
            if ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
                return self.wr(ins, ops[0], '0') + 'CF=0;OF=0;ZF=1;SF=0;PF=1;'
            body = pre + 'r=a^b;CF=0;OF=0;' + self.szf('r', b)
        else:
            raise Unsupported(kind)
        if kind not in ('cmp', 'test'):
            body += self.wr(ins, ops[0], 'r')
        return body + '}'

    def i_add(self, ins, ops): return self.arith(ins, ops, 'add')
    def i_sub(self, ins, ops): return self.arith(ins, ops, 'sub')
    def i_cmp(self, ins, ops): return self.arith(ins, ops, 'cmp')
    def i_adc(self, ins, ops): return self.arith(ins, ops, 'adc')
    def i_sbb(self, ins, ops): return self.arith(ins, ops, 'sbb')
    def i_and(self, ins, ops): return self.arith(ins, ops, 'and')
    def i_or(self, ins, ops): return self.arith(ins, ops, 'or')
    def i_xor(self, ins, ops): return self.arith(ins, ops, 'xor')
    def i_test(self, ins, ops): return self.arith(ins, ops, 'test')

    def i_inc(self, ins, ops):
        b = ops[0].size * 8
        u = UT[b]
        return '{%s a=%s,r=(%s)(a+1);OF=r==(%s)(1u<<%d);%s%s}' % (u, self.rd(ins, ops[0]), u, u, b - 1, self.szf('r', b), self.wr(ins, ops[0], 'r'))

    def i_dec(self, ins, ops):
        b = ops[0].size * 8
        u = UT[b]
        return '{%s a=%s,r=(%s)(a-1);OF=a==(%s)(1u<<%d);%s%s}' % (u, self.rd(ins, ops[0]), u, u, b - 1, self.szf('r', b), self.wr(ins, ops[0], 'r'))

    def i_neg(self, ins, ops):
        b = ops[0].size * 8
        u = UT[b]
        return '{%s a=%s,r=(%s)(0-a);CF=a!=0;OF=a==(%s)(1u<<%d);%s%s}' % (u, self.rd(ins, ops[0]), u, u, b - 1, self.szf('r', b), self.wr(ins, ops[0], 'r'))

    def i_not(self, ins, ops):
        return self.wr(ins, ops[0], '~' + self.rd(ins, ops[0]))

    def shift(self, ins, ops, kind):
        b = ops[0].size * 8
        u = UT[b]
        if len(ops) > 1:
            if ops[1].type == X86_OP_IMM:
                cnt = '%du' % (ops[1].imm & 31)
                if ops[1].imm & 31 == 0:
                    return ';'
            else:
                cnt = '(ecx&31u)'
        else:
            cnt = '1u'
        a = self.rd(ins, ops[0])
        m = b - 1
        if kind == 'shl':
            core = 'r=(%s)(a<<n);CF=n<=%d?(a>>(%d-n))&1:0;OF=((r>>%d)&1)^CF;' % (u, b, b, m)
        elif kind == 'shr':
            core = 'r=(%s)(a>>n);CF=n<=%d?(a>>(n-1))&1:0;OF=(a>>%d)&1;' % (u, b, m)
        elif kind == 'sar':
            core = 'r=(%s)((%s)a>>(n>%d?%d:n));CF=((%s)a>>(n>%d?%d:n-1))&1;OF=0;' % (u, ST[b], m, m, ST[b], m, m)
        elif kind == 'rol':
            core = 'uint32_t k=n%%%d;r=(%s)(k?(a<<k)|(a>>(%d-k)):a);CF=r&1;OF=((r>>%d)&1)^CF;' % (b, u, b, m)
            return '{%s a=%s,r;uint32_t n=%s;if(n){%s%s}}' % (u, a, cnt, core, self.wr(ins, ops[0], 'r'))
        elif kind == 'ror':
            core = 'uint32_t k=n%%%d;r=(%s)(k?(a>>k)|(a<<(%d-k)):a);CF=(r>>%d)&1;OF=((r>>%d)^(r>>%d))&1;' % (b, u, b, m, m, m - 1)
            return '{%s a=%s,r;uint32_t n=%s;if(n){%s%s}}' % (u, a, cnt, core, self.wr(ins, ops[0], 'r'))
        else:
            raise Unsupported(kind)
        return '{%s a=%s,r;uint32_t n=%s;if(n){%s%s%s}}' % (u, a, cnt, core, self.szf('r', b), self.wr(ins, ops[0], 'r'))

    def i_shl(self, ins, ops): return self.shift(ins, ops, 'shl')
    def i_sal(self, ins, ops): return self.shift(ins, ops, 'shl')
    def i_shr(self, ins, ops): return self.shift(ins, ops, 'shr')
    def i_sar(self, ins, ops): return self.shift(ins, ops, 'sar')
    def i_rol(self, ins, ops): return self.shift(ins, ops, 'rol')
    def i_ror(self, ins, ops): return self.shift(ins, ops, 'ror')

    def i_shld(self, ins, ops):
        if ops[0].size != 4:
            raise Unsupported('shld16')
        cnt = '%du' % (ops[2].imm & 31) if ops[2].type == X86_OP_IMM else '(ecx&31u)'
        return '{uint32_t a=%s,s=%s,n=%s,r;if(n){r=(a<<n)|(s>>(32-n));CF=(a>>(32-n))&1;OF=((r^a)>>31)&1;%s%s}}' % (
            self.rd(ins, ops[0]), self.rd(ins, ops[1]), cnt, self.szf('r', 32), self.wr(ins, ops[0], 'r'))

    def i_shrd(self, ins, ops):
        if ops[0].size != 4:
            raise Unsupported('shrd16')
        cnt = '%du' % (ops[2].imm & 31) if ops[2].type == X86_OP_IMM else '(ecx&31u)'
        return '{uint32_t a=%s,s=%s,n=%s,r;if(n){r=(a>>n)|(s<<(32-n));CF=(a>>(n-1))&1;OF=((r^a)>>31)&1;%s%s}}' % (
            self.rd(ins, ops[0]), self.rd(ins, ops[1]), cnt, self.szf('r', 32), self.wr(ins, ops[0], 'r'))

    def i_wait(self, ins, ops):
        return ';'

    def loop(self, ins, ops, cond):
        return 'ecx--;if(ecx%s) %s' % (cond, self.goto(ops[0].imm & 0xffffffff))

    def i_loop(self, ins, ops): return self.loop(ins, ops, '')
    def i_loope(self, ins, ops): return self.loop(ins, ops, '&&ZF')
    def i_loopne(self, ins, ops): return self.loop(ins, ops, '&&!ZF')

    def bitmem(self, ins, ops, setbit):
        a = self.addr(ins, ops[0])
        n = self.rd(ins, ops[1], 32)
        s = 'wr8(ba,(uint8_t)(rd8(ba)|(1u<<(n&7))));' if setbit else ''
        return '{int32_t n=(int32_t)%s;uint32_t ba=%s+(uint32_t)(n>>3);CF=(rd8(ba)>>(n&7))&1;%s}' % (n, a, s)

    def i_bt(self, ins, ops):
        if ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_REG:
            return self.bitmem(ins, ops, False)
        b = ops[0].size * 8
        return 'CF=(%s>>(%s&%du))&1;' % (self.rd(ins, ops[0]), self.rd(ins, ops[1], b), b - 1)

    def i_bts(self, ins, ops):
        if ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_REG:
            return self.bitmem(ins, ops, True)
        b = ops[0].size * 8
        return '{%s a=%s;uint32_t n=%s&%du;CF=(a>>n)&1;%s}' % (UT[b], self.rd(ins, ops[0]), self.rd(ins, ops[1], b), b - 1, self.wr(ins, ops[0], 'a|((%s)1<<n)' % UT[b]))

    def i_mul(self, ins, ops):
        b = ops[0].size * 8
        s = self.rd(ins, ops[0])
        if b == 32:
            return '{uint64_t r=(uint64_t)eax*(uint64_t)%s;eax=(uint32_t)r;edx=(uint32_t)(r>>32);CF=OF=edx!=0;}' % s
        if b == 16:
            return '{uint32_t r=(uint32_t)(uint16_t)eax*(uint32_t)%s;eax=(eax&0xffff0000u)|(r&0xffff);edx=(edx&0xffff0000u)|(r>>16);CF=OF=(r>>16)!=0;}' % s
        return '{uint32_t r=(uint32_t)(uint8_t)eax*(uint32_t)%s;eax=(eax&0xffff0000u)|(r&0xffff);CF=OF=(r>>8)!=0;}' % s

    def i_imul(self, ins, ops):
        if len(ops) == 1:
            b = ops[0].size * 8
            s = self.rd(ins, ops[0])
            if b == 32:
                return '{int64_t r=(int64_t)(int32_t)eax*(int64_t)(int32_t)%s;eax=(uint32_t)r;edx=(uint32_t)((uint64_t)r>>32);CF=OF=r!=(int64_t)(int32_t)r;}' % s
            if b == 16:
                return '{int32_t r=(int32_t)(int16_t)eax*(int32_t)(int16_t)%s;eax=(eax&0xffff0000u)|((uint32_t)r&0xffff);edx=(edx&0xffff0000u)|(((uint32_t)r>>16)&0xffff);CF=OF=r!=(int32_t)(int16_t)r;}' % s
            return '{int32_t r=(int32_t)(int8_t)eax*(int32_t)(int8_t)%s;eax=(eax&0xffff0000u)|((uint32_t)r&0xffff);CF=OF=r!=(int32_t)(int8_t)r;}' % s
        b = ops[0].size * 8
        if len(ops) == 2:
            x, y = self.rd(ins, ops[0]), self.rd(ins, ops[1])
        else:
            x, y = self.rd(ins, ops[1]), self.rd(ins, ops[2], b)
        if b == 32:
            return '{int64_t r=(int64_t)(int32_t)%s*(int64_t)(int32_t)%s;CF=OF=r!=(int64_t)(int32_t)r;%s}' % (x, y, self.wr(ins, ops[0], '(uint32_t)r'))
        return '{int32_t r=(int32_t)(int16_t)%s*(int32_t)(int16_t)%s;CF=OF=r!=(int32_t)(int16_t)r;%s}' % (x, y, self.wr(ins, ops[0], '(uint32_t)r'))

    def i_div(self, ins, ops):
        b = ops[0].size * 8
        s = self.rd(ins, ops[0])
        if b == 32:
            return '{uint64_t n=((uint64_t)edx<<32)|eax;uint32_t d=%s;eax=(uint32_t)(n/d);edx=(uint32_t)(n%%d);}' % s
        if b == 16:
            return '{uint32_t n=((edx&0xffff)<<16)|(eax&0xffff);uint32_t d=%s;eax=(eax&0xffff0000u)|((n/d)&0xffff);edx=(edx&0xffff0000u)|((n%%d)&0xffff);}' % s
        return '{uint32_t n=eax&0xffff;uint32_t d=%s;eax=(eax&0xffff0000u)|((n/d)&0xff)|(((n%%d)&0xff)<<8);}' % s

    def i_idiv(self, ins, ops):
        b = ops[0].size * 8
        s = self.rd(ins, ops[0])
        if b == 32:
            return '{int64_t n=(int64_t)(((uint64_t)edx<<32)|eax);int32_t d=(int32_t)%s;eax=(uint32_t)(int32_t)(n/d);edx=(uint32_t)(int32_t)(n%%d);}' % s
        if b == 16:
            return '{int32_t n=(int32_t)(((edx&0xffff)<<16)|(eax&0xffff));int32_t d=(int16_t)%s;eax=(eax&0xffff0000u)|((uint32_t)(n/d)&0xffff);edx=(edx&0xffff0000u)|((uint32_t)(n%%d)&0xffff);}' % s
        return '{int32_t n=(int16_t)eax;int32_t d=(int8_t)%s;eax=(eax&0xffff0000u)|((uint32_t)(n/d)&0xff)|(((uint32_t)(n%%d)&0xff)<<8);}' % s

    def i_cdq(self, ins, ops):
        return 'edx=(uint32_t)((int32_t)eax>>31);'

    def i_cwde(self, ins, ops):
        return 'eax=(uint32_t)(int32_t)(int16_t)eax;'

    def i_cwd(self, ins, ops):
        return 'edx=(edx&0xffff0000u)|(((int16_t)eax<0)?0xffffu:0u);'

    def i_cbw(self, ins, ops):
        return 'eax=(eax&0xffff0000u)|(uint16_t)(int16_t)(int8_t)eax;'

    def i_cld(self, ins, ops):
        return 'DF=0;'

    def i_std(self, ins, ops):
        return 'DF=1;'

    def i_clc(self, ins, ops):
        return 'CF=0;'

    def i_stc(self, ins, ops):
        return 'CF=1;'

    def i_cmc(self, ins, ops):
        return 'CF^=1;'

    def i_sahf(self, ins, ops):
        return '{uint32_t h=(eax>>8)&0xff;CF=h&1;PF=(h>>2)&1;ZF=(h>>6)&1;SF=(h>>7)&1;}'

    def i_lahf(self, ins, ops):
        return 'eax=(eax&0xffff00ffu)|((CF|(PF<<2)|(ZF<<6)|(SF<<7)|2u)<<8);'

    def i_pushfd(self, ins, ops):
        return '{uint32_t f=CF|2u|(PF<<2)|(ZF<<6)|(SF<<7)|(DF<<10)|(OF<<11)|0x200u|c->eflags_id;esp-=4;wr32(esp,f);}'

    def i_popfd(self, ins, ops):
        return '{uint32_t f=rd32(esp);esp+=4;CF=f&1;PF=(f>>2)&1;ZF=(f>>6)&1;SF=(f>>7)&1;DF=(f>>10)&1;OF=(f>>11)&1;c->eflags_id=f&0x200000u;}'

    def i_cpuid(self, ins, ops):
        return '{uint32_t a=eax,b,cc=ecx,d;guest_cpuid(c,&a,&b,&cc,&d);eax=a;ebx=b;ecx=cc;edx=d;}'

    def i_rdtsc(self, ins, ops):
        return '{uint64_t v=guest_rdtsc(c);eax=(uint32_t)v;edx=(uint32_t)(v>>32);}'

    def i_hlt(self, ins, ops):
        return '%s guest_unimpl(c,0x%xu,"hlt"); return;' % (SAVE, ins.address)

    def i_int3(self, ins, ops):
        return '%s guest_unimpl(c,0x%xu,"int3"); return;' % (SAVE, ins.address)

    i_ud2 = i_int3

    def i_out(self, ins, ops):
        port = '(uint16_t)0x%xu' % ops[0].imm if ops[0].type == X86_OP_IMM else '(uint16_t)edx'
        return '%s guest_port_out(c,%s,%s,%d);' % (SAVE, port, self.rd(ins, ops[1]), ops[1].size * 8)

    def i_in(self, ins, ops):
        port = '(uint16_t)0x%xu' % ops[1].imm if ops[1].type == X86_OP_IMM else '(uint16_t)edx'
        return '{%s uint32_t v=guest_port_in(c,%s,%d);%s}' % (SAVE, port, ops[0].size * 8, self.wr(ins, ops[0], 'v'))

    # string instructions: es:edi / ds:esi, flat memory
    def strop(self, ins, size, prefix, kind):
        b = size * 8
        step = 'int32_t st=DF?-%d:%d;' % (size, size)
        acc = {8: '(uint8_t)eax', 16: '(uint16_t)eax', 32: 'eax'}[b]
        if kind == 'movs':
            one = 'wr%d(edi,rd%d(esi));esi+=st;edi+=st;' % (b, b)
        elif kind == 'stos':
            one = 'wr%d(edi,%s);edi+=st;' % (b, acc)
        elif kind == 'lods':
            one = '%s edi+=0;esi+=st;' % ({8: 'eax=(eax&0xffffff00u)|rd8(esi);', 16: 'eax=(eax&0xffff0000u)|rd16(esi);', 32: 'eax=rd32(esi);'}[b])
        elif kind == 'scas':
            one = '{%s a=%s,bb=rd%d(edi),r=(%s)(a-bb);CF=a<bb;OF=(((a^bb)&(a^r))>>%d)&1;%s}edi+=st;' % (UT[b], acc, b, UT[b], b - 1, self.szf('r', b))
        elif kind == 'cmps':
            one = '{%s a=rd%d(esi),bb=rd%d(edi),r=(%s)(a-bb);CF=a<bb;OF=(((a^bb)&(a^r))>>%d)&1;%s}esi+=st;edi+=st;' % (UT[b], b, b, UT[b], b - 1, self.szf('r', b))
        if prefix is None:
            return '{%s%s}' % (step, one)
        if kind in ('movs', 'stos', 'lods'):
            if kind == 'movs' and True:
                return '{%sif(!DF&&ecx){uint32_t n=ecx*%du;memmove(M+edi,M+esi,n);esi+=n;edi+=n;ecx=0;}else while(ecx){%secx--;}}' % (step, size, one)
            return '{%swhile(ecx){%secx--;}}' % (step, one)
        cond = 'ZF' if prefix in ('rep', 'repe', 'repz') else '!ZF'
        return '{%swhile(ecx){%secx--;if(!(%s))break;}}' % (step, one, cond)

    def mkstr(kind, size):
        def f(self, ins, ops, prefix=None):
            return self.strop(ins, size, prefix, kind)
        return f

    i_movsb = mkstr('movs', 1)
    i_movsw = mkstr('movs', 2)
    i_stosb = mkstr('stos', 1)
    i_stosw = mkstr('stos', 2)
    i_stosd = mkstr('stos', 4)
    i_lodsb = mkstr('lods', 1)
    i_lodsd = mkstr('lods', 4)
    i_scasb = mkstr('scas', 1)
    i_scasd = mkstr('scas', 4)
    i_cmpsb = mkstr('cmps', 1)
    i_cmpsd_str = mkstr('cmps', 4)
    i_movsd_str = mkstr('movs', 4)

    # ---- x87, decoded from the opcode bytes --------------------------------
    def x87(self, ins, opc, modrm):
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        ops = ins.operands
        if mod != 3:
            mop = [o for o in ops if o.type == X86_OP_MEM][0]
            a = self.addr(ins, mop)
            arith = {0: '+', 1: '*', 4: '-', 5: 'r-', 6: '/', 7: 'r/'}
            if opc in (0xd8, 0xdc, 0xda, 0xde):
                if opc == 0xd8:
                    v = 'rdf32(%s)' % a
                elif opc == 0xdc:
                    v = 'rdf64(%s)' % a
                elif opc == 0xda:
                    v = '(double)(int32_t)rd32(%s)' % a
                else:
                    v = '(double)(int16_t)rd16(%s)' % a
                if reg in (2, 3):
                    return 'fcompare(c,ST(0),%s);%s' % (v, 'fpop(c);' if reg == 3 else '')
                o = arith[reg]
                if o.startswith('r'):
                    return 'ST(0)=%s%sST(0);' % (v, o[1])
                return 'ST(0)=ST(0)%s%s;' % (o, v)
            if opc == 0xd9:
                if reg == 0:
                    return 'fpush(c,rdf32(%s));' % a
                if reg == 2:
                    return 'wrf32(%s,(float)ST(0));' % a
                if reg == 3:
                    return 'wrf32(%s,(float)ST(0));fpop(c);' % a
                if reg == 5:
                    return 'c->fcw=rd16(%s);' % a
                if reg == 7:
                    return 'wr16(%s,c->fcw);' % a
            if opc == 0xdb:
                if reg == 0:
                    return 'fpush(c,(double)(int32_t)rd32(%s));' % a
                if reg == 1:
                    return 'wr32(%s,(uint32_t)(int32_t)(ST(0)>=-2147483648.0&&ST(0)<2147483648.0?(int32_t)ST(0):(int32_t)0x80000000));fpop(c);' % a
                if reg == 2:
                    return 'wr32(%s,(uint32_t)x87_int(c,ST(0),32));' % a
                if reg == 3:
                    return 'wr32(%s,(uint32_t)x87_int(c,ST(0),32));fpop(c);' % a
                if reg == 5:
                    return 'fpush(c,x87_load80(%s));' % a
                if reg == 7:
                    return 'x87_store80(%s,ST(0));fpop(c);' % a
            if opc == 0xdd:
                if reg == 0:
                    return 'fpush(c,rdf64(%s));' % a
                if reg == 1:      # fisttp m64 (SSE3): truncate, out of range is the integer indefinite
                    return 'wr64(%s,(uint64_t)(ST(0)>=-9223372036854775808.0&&ST(0)<9223372036854775808.0?(int64_t)ST(0):(int64_t)0x8000000000000000ull));fpop(c);' % a
                if reg == 2:
                    return 'wrf64(%s,ST(0));' % a
                if reg == 3:
                    return 'wrf64(%s,ST(0));fpop(c);' % a
                if reg == 7:
                    return 'wr16(%s,fstatus(c));' % a
            if opc == 0xdf:
                if reg == 0:
                    return 'fpush(c,(double)(int16_t)rd16(%s));' % a
                if reg == 1:      # fisttp m16
                    return 'wr16(%s,(uint16_t)(int16_t)(ST(0)>-32769.0&&ST(0)<32768.0?(int16_t)ST(0):(int16_t)0x8000));fpop(c);' % a
                if reg == 2:
                    return 'wr16(%s,(uint16_t)x87_int(c,ST(0),16));' % a
                if reg == 3:
                    return 'wr16(%s,(uint16_t)x87_int(c,ST(0),16));fpop(c);' % a
                if reg == 5:
                    return 'fpush(c,(double)(int64_t)rd64(%s));' % a
                if reg == 7:
                    return 'wr64(%s,(uint64_t)x87_int(c,ST(0),64));fpop(c);' % a
            raise Unsupported('x87 %02x /%d' % (opc, reg))
        i = rm
        if opc == 0xd8:
            if reg in (2, 3):
                return 'fcompare(c,ST(0),ST(%d));%s' % (i, 'fpop(c);' if reg == 3 else '')
            return {0: 'ST(0)=ST(0)+ST(%d);', 1: 'ST(0)=ST(0)*ST(%d);', 4: 'ST(0)=ST(0)-ST(%d);',
                    5: 'ST(0)=ST(%d)-ST(0);', 6: 'ST(0)=ST(0)/ST(%d);', 7: 'ST(0)=ST(%d)/ST(0);'}[reg] % i
        if opc in (0xdc, 0xde):
            pop = 'fpop(c);' if opc == 0xde else ''
            if opc == 0xde and reg == 3 and i == 1:
                return 'fcompare(c,ST(0),ST(1));fpop(c);fpop(c);'
            if reg in (2, 3):
                raise Unsupported('x87 dc/de compare')
            return ({0: 'ST(%d)=ST(%d)+ST(0);', 1: 'ST(%d)=ST(%d)*ST(0);', 4: 'ST(%d)=ST(0)-ST(%d);',
                     5: 'ST(%d)=ST(%d)-ST(0);', 6: 'ST(%d)=ST(0)/ST(%d);', 7: 'ST(%d)=ST(%d)/ST(0);'}[reg] % (i, i)) + pop
        if opc == 0xd9:
            if reg == 0:
                return '{double t=ST(%d);fpush(c,t);}' % i
            if reg == 1:
                return '{double t=ST(0);ST(0)=ST(%d);ST(%d)=t;}' % (i, i)
            x = {0xd0: ';', 0xe0: 'ST(0)=-ST(0);', 0xe1: 'ST(0)=fabs(ST(0));', 0xe4: 'fcompare(c,ST(0),0.0);',
                 0xe8: 'fpush(c,1.0);', 0xe9: 'fpush(c,3.321928094887362347870);', 0xea: 'fpush(c,1.442695040888963407360);',
                 0xeb: 'fpush(c,3.141592653589793238463);', 0xec: 'fpush(c,0.301029995663981195214);',
                 0xed: 'fpush(c,0.693147180559945309417);', 0xee: 'fpush(c,0.0);',
                 0xf0: 'ST(0)=exp2(ST(0))-1.0;', 0xf1: 'ST(1)=ST(1)*log2(ST(0));fpop(c);',
                 0xf2: 'ST(0)=tan(ST(0));fpush(c,1.0);', 0xf3: 'ST(1)=atan2(ST(1),ST(0));fpop(c);',
                 0xf8: 'ST(0)=fmod(ST(0),ST(1));c->fsw&=~0x400;', 0xf5: 'ST(0)=remainder(ST(0),ST(1));c->fsw&=~0x400;',
                 0xfa: 'ST(0)=sqrt(ST(0));', 0xfb: '{double v=ST(0);ST(0)=sin(v);fpush(c,cos(v));}',
                 0xfc: 'ST(0)=(double)x87_round(c,ST(0));', 0xfd: 'ST(0)=ldexp(ST(0),(int)trunc(ST(1)));',
                 0xfe: 'ST(0)=sin(ST(0));', 0xff: 'ST(0)=cos(ST(0));', 0xf6: 'c->ftop=(c->ftop-1)&7;',
                 0xf7: 'c->ftop=(c->ftop+1)&7;', 0xf9: 'ST(1)=ST(1)*log2(ST(0)+1.0);fpop(c);'}.get(modrm)
            if x:
                return x
        if opc == 0xda and modrm == 0xe9:
            return 'fcompare(c,ST(0),ST(1));fpop(c);fpop(c);'
        if opc in (0xda, 0xdb) and modrm < 0xe0:
            # fcmovb/e/be/u, and fcmovnb/ne/nbe/nu with db
            cond = ('CF', 'ZF', '(CF|ZF)', 'PF')[reg]
            return 'if(%s%s)ST(0)=ST(%d);' % ('!' if opc == 0xdb else '', cond, i)
        if opc == 0xdb:
            if modrm == 0xe2:
                return 'c->fsw&=0x7f00;'
            if modrm == 0xe3:
                return 'c->fcw=0x37f;c->fsw=0;c->ftop=0;'
            if 0xe8 <= modrm <= 0xf7:
                return '{double a=ST(0),b=ST(%d);if(a!=a||b!=b){ZF=PF=CF=1;}else{ZF=a==b;CF=a<b;PF=0;}OF=SF=0;}' % i
        if opc == 0xdd:
            if reg == 0:
                return ';'
            if reg == 2:
                return 'ST(%d)=ST(0);' % i
            if reg == 3:
                return 'ST(%d)=ST(0);fpop(c);' % i
            if reg == 4:
                return 'fcompare(c,ST(0),ST(%d));' % i
            if reg == 5:
                return 'fcompare(c,ST(0),ST(%d));fpop(c);' % i
        if opc == 0xdf:
            if modrm == 0xe0:
                return 'eax=(eax&0xffff0000u)|fstatus(c);'
            if 0xe8 <= modrm <= 0xf7:
                return '{double a=ST(0),b=ST(%d);if(a!=a||b!=b){ZF=PF=CF=1;}else{ZF=a==b;CF=a<b;PF=0;}OF=SF=0;fpop(c);}' % i
        raise Unsupported('x87 %02x %02x' % (opc, modrm))


class Recompiler:
    def __init__(self, img, overrides=(), name='game'):
        self.img = img
        self.overrides = {}
        for fname in overrides:
            a = img.func_by_name.get(fname)
            if a:
                self.overrides[a] = fname
        self.unsupported = collections.Counter()
        from sse import SSEGen
        self.sse = SSEGen()
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.entries = {}      # function start -> set of entry addresses
        self.names = {}        # entry address -> C name
        # C names carry the program's name, so several programs link into one library
        self.prog = name
        self.F = name + '_F_%x'
        self.P = name + '_P_%x'

    def fn_name(self, a):
        if a in self.img.plt_by_addr:
            return self.P % a
        return self.names.get(a)

    def find_functions(self):
        img = self.img
        starts = set(a for a in img.syms if img.in_exec(a))
        starts.add(img.entry)
        # a library names only what it exports: its other functions are found
        # from its initialiser, the code pointers its relocations fix up, and
        # (below) the direct calls and jumps of its code found so far (the
        # program has a full symbol table, so its functions stay as they are)
        for lib in img.libs:
            if lib.init:
                starts.add(lib.init)
            for a, t, n in lib.relocs:
                if t == 8:
                    v = (self._word(a) + lib.base) & 0xffffffff
                    if img.in_exec(v):
                        starts.add(v)
        md0 = Cs(CS_ARCH_X86, CS_MODE_32)
        lib_lo = min([l.base for l in img.libs], default=1 << 32)
        work = sorted(a for a in starts if a >= lib_lo)
        while work:
            ordered = sorted(starts)
            new = set()
            for a in work:
                k = bisect.bisect_right(ordered, a)
                lo, hi, data = img.sec_of(a)
                end = min(ordered[k], hi) if k < len(ordered) else hi
                for (addr, size, mnem, opstr) in (x for pa, pe in self.pieces(a, end)
                                                  for x in md0.disasm_lite(data[pa - lo:pe - lo], pa)):
                    if mnem == 'lea' and '[ebx ' in opstr and '*' not in opstr:
                        # PIC: the address of a static function, GOT-relative
                        m = re.search(r'\[ebx ([+-]) (0x[0-9a-f]+|\d+)\]', opstr)
                        lib = self.lib_of(a)
                        if m and lib and lib.pltgot:
                            d = int(m.group(2), 0) * (1 if m.group(1) == '+' else -1)
                            t = (lib.pltgot + d) & 0xffffffff
                            if t not in starts and img.in_exec(t) and self.lib_of(t) is lib:
                                new.add(t)
                        continue
                    if mnem == 'call' or mnem == 'jmp':
                        try:
                            t = int(opstr, 16)
                        except ValueError:
                            continue
                        if mnem == 'jmp' and a <= t < end:
                            continue
                        if (t >= lib_lo and t != addr + size and t not in starts and t not in img.plt_by_addr
                                and img.in_exec(t)):
                            new.add(t)
            starts |= new
            work = sorted(new)
        self.starts = sorted(starts)
        # function extent: up to the next start or the end of its section
        self.extent = {}
        for k, a in enumerate(self.starts):
            lo, hi, _ = img.sec_of(a)
            nxt = self.starts[k + 1] if k + 1 < len(self.starts) else hi
            self.extent[a] = min(nxt, hi)

    def lib_of(self, a):
        for lib in self.img.libs:
            for lo, hi, d, n in lib.exec_secs:
                if lo <= a < hi:
                    return lib
        return None

    def _word(self, a):
        for lo, data, name in self.img.data_secs:
            if lo <= a < lo + len(data) - 3:
                return struct.unpack_from('<I', data, a - lo)[0]
        return 0

    def find_jump_tables(self):
        """PIC switch tables inside a library's code (GCC 2.96 puts them right
        after the jump): bound check, a load from [ebx + index*4 + disp] with
        %ebx holding the GOT, then jmp reg. Entries are GOT-relative."""
        self.jtables = {}      # jmp address -> targets
        ranges = []
        for lib in self.img.libs:
            if not lib.pltgot:
                continue
            for lo, hi, data, name in lib.exec_secs:
                pos = lo
                recent = collections.deque(maxlen=24)
                while pos < hi:
                    restart = False
                    for ins in self.md.disasm(data[pos - lo:hi - lo], pos):
                        pos = ins.address + ins.size
                        recent.append(ins)
                        if ins.mnemonic == 'jmp' and ins.operands and ins.operands[0].type == X86_OP_REG:
                            t = self.match_table(lib, list(recent), data, lo, hi)
                            if t:
                                tlo, thi, targets = t
                                self.jtables[ins.address] = targets
                                ranges.append((tlo, thi))
                                if pos <= tlo < pos + 16:
                                    pos = thi
                                    restart = True
                                    break
                    if not restart and pos < hi:
                        pos += 1           # an undecodable byte
                    recent.clear() if not restart else None
        self.jt_ranges = sorted(ranges)
        self.jt_lows = [r[0] for r in self.jt_ranges]

    def match_table(self, lib, recent, data, lo, hi):
        load = bound = None
        for k in range(len(recent) - 2, -1, -1):
            ins = recent[k]
            if load is None:
                for op in ins.operands:
                    if (op.type == X86_OP_MEM and op.mem.base and ins.reg_name(op.mem.base) == 'ebx'
                            and op.mem.index and op.mem.scale == 4):
                        load = (k, op.mem.disp)
            elif ins.mnemonic in ('ja', 'jae', 'jnb'):
                # the cmp may have moves (no flags) between it and the branch
                for j in range(k - 1, max(k - 7, -1), -1):
                    cmp = recent[j]
                    if cmp.mnemonic == 'cmp' and len(cmp.operands) == 2 and cmp.operands[1].type == X86_OP_IMM:
                        bound = cmp.operands[1].imm + (1 if ins.mnemonic == 'ja' else 0)
                        break
                    if not (cmp.mnemonic in ('lea', 'mov', 'movzx', 'movsx') or
                            (cmp.mnemonic.startswith('f') and 'comi' not in cmp.mnemonic)):
                        break
                break
        if load is None or (bound or 0) > 4096:
            return None
        tlo = (lib.pltgot + load[1]) & 0xffffffff
        jmp_end = recent[-1].address + recent[-1].size
        if not bound:
            # no bound check (a constant index): a table right after the jump
            # runs while its entries lead to code nearby
            if not (jmp_end <= tlo < jmp_end + 16 and lo <= tlo):
                return None
            for sign in (-1, 1):
                n = 0
                while tlo + 4 * (n + 1) <= hi and n < 256:
                    t = (lib.pltgot + sign * struct.unpack_from('<I', data, tlo + 4 * n - lo)[0]) & 0xffffffff
                    if not (lo <= t < hi and abs(t - tlo) < 0x10000):
                        break
                    n += 1
                if n:
                    bound = n
                    break
            if not bound:
                return None
        thi = tlo + 4 * bound
        if not (lo <= tlo and thi <= hi):
            return None
        entries = struct.unpack_from('<%dI' % bound, data, tlo - lo)
        for sign in (-1, 1):
            targets = [(lib.pltgot + sign * e) & 0xffffffff for e in entries]
            if all(lo <= t < hi for t in targets):
                return tlo, thi, targets
        return None

    def pieces(self, a, end):
        """[a, end) without the jump tables inside it"""
        out = []
        k = bisect.bisect_right(self.jt_lows, a) - 1
        k = max(k, 0)
        pos = a
        while k < len(self.jt_ranges) and self.jt_ranges[k][0] < end:
            tlo, thi = self.jt_ranges[k]
            if thi > pos:
                if tlo > pos:
                    out.append((pos, tlo))
                pos = max(pos, thi)
            k += 1
        if pos < end:
            out.append((pos, end))
        return out

    def disasm(self, a, end):
        lo, hi, data = self.img.sec_of(a)
        md = self.md
        out = []
        for pa, pe in self.pieces(a, end):
            pos = pa
            for ins in md.disasm(data[pos - lo:pe - lo], pos):
                out.append(ins)
                pos = ins.address + ins.size
            if pos < pe:
                # undecodable bytes (data in text): stop this function here
                break
        return out

    def scan_data_refs(self):
        refs = set()
        for lo, data, name in self.img.data_secs:
            for off in range(0, len(data) - 3, 4):
                v = struct.unpack_from('<I', data, off)[0]
                for bias in self.img.ref_bias:
                    if self.img.in_exec((v + bias) & 0xffffffff):
                        refs.add((v + bias) & 0xffffffff)
        self.data_refs = sorted(refs)

    def data_refs_in(self, lo, hi):
        i = bisect.bisect_left(self.data_refs, lo)
        j = bisect.bisect_left(self.data_refs, hi)
        return self.data_refs[i:j]

    def run(self, outdir, per_file):
        self.find_jump_tables()
        self.find_functions()
        self.scan_data_refs()
        # pass 1: instruction boundaries and direct branch targets only
        bounds = {}
        branches = {}
        md0 = Cs(CS_ARCH_X86, CS_MODE_32)
        for a in self.starts:
            lo, hi, data = self.img.sec_of(a)
            addrs = []
            br = []
            for (addr, size, mnem, opstr) in (x for pa, pe in self.pieces(a, self.extent[a])
                                              for x in md0.disasm_lite(data[pa - lo:pe - lo], pa)):
                addrs.append(addr)
                if mnem == 'call' or mnem.startswith('j'):
                    try:
                        br.append((mnem, int(opstr, 16)))
                    except ValueError:
                        pass
            bounds[a] = set(addrs)
            branches[a] = br
        self.ninsns = {a: len(bounds[a]) for a in self.starts}
        # entries: function starts, plus jump/call targets that land outside
        # their own function and inside another
        for a in self.starts:
            self.entries[a] = {a}
        for a in self.starts:
            for mnem, t in branches[a]:
                if t in self.img.plt_by_addr:
                    continue
                if mnem != 'call' and a <= t < self.extent[a]:
                    continue
                k = bisect.bisect_right(self.starts, t) - 1
                if k >= 0 and self.starts[k] <= t < self.extent[self.starts[k]]:
                    f = self.starts[k]
                    if t in bounds[f]:
                        self.entries[f].add(t)
        for f, es in self.entries.items():
            for e in es:
                self.names[e] = self.F % e
        os.makedirs(outdir, exist_ok=True)
        files = []
        chunk = []
        n = 0
        decls = []
        for a in self.starts:
            for e in sorted(self.entries[a]):
                decls.append('void %s(CPU *c);' % (self.F % e))
        for p, g, name in self.img.imports:
            decls.append('void %s(CPU *c);' % (self.P % p))
        hdr = ['#include "cpu.h"', '#include "hle.h"', '#include <setjmp.h>'] + decls
        with open(os.path.join(outdir, 'gen.h'), 'w') as fh:
            fh.write('\n'.join(hdr) + '\n')
        count = 0
        for a in self.starts:
            if a in self.overrides:
                chunk.append('void ovr_%s(CPU *c);\nvoid %s(CPU *c){ovr_%s(c);c->r[ESP]+=4;}' % (self.overrides[a], self.F % a, self.overrides[a]))
                for e in sorted(self.entries[a]):
                    if e != a:
                        chunk.append('void %s(CPU *c){guest_unimpl(c,0x%xu,"entry into overridden function");}' % (self.F % e, e))
                continue
            insns = self.disasm(a, self.extent[a])
            fg = FuncGen(self, a, self.extent[a], insns, sorted(self.entries[a]))
            chunk.append(fg.gen())
            count += len(insns)
            del insns, fg
            if len(chunk) >= per_file:
                files.append(chunk)
                chunk = []
        if chunk:
            files.append(chunk)
        for k, ch in enumerate(files):
            with open(os.path.join(outdir, 'gen_%03d.c' % k), 'w') as fh:
                fh.write('#include "gen.h"\n')
                fh.write('\n'.join(ch) + '\n')
        # PLT stubs and the dispatch table
        out = ['#include "gen.h"']
        for p, g, name in self.img.imports:
            out.append('void %s(CPU *c){hle_call(c,%d);}' % (self.P % p, self.import_index(name)))
        tab = []
        for a in self.starts:
            for e in sorted(self.entries[a]):
                tab.append((e, self.F % e))
        for p, g, name in self.img.imports:
            tab.append((p, self.P % p))
        tab.sort()
        objs = sorted(self.img.objects.items())
        libs = self.img.libs
        exports = self.img.exports + [e for lib in libs for e in lib.exports]
        relocs = self.img.relocs + [r for lib in libs for r in lib.relocs]
        arrays = [
            ('const uint32_t', 'fn_addr', ['0x%xu' % a for a, _ in tab]),
            ('const guest_fn', 'fn_ptr', [n for _, n in tab]),
            ('const char *const', 'import_names', ['"%s"' % n for n in self.import_names]),
            ('const uint32_t', 'import_got', ['0x%xu' % g for p, g, n in self.img.imports]),
            ('const uint32_t', 'import_plt', ['0x%xu' % p for p, g, n in self.img.imports]),
            ('const uint32_t', 'copy_addr', ['0'] + ['0x%xu' % a for a, n in self.img.copies]),
            ('const char *const', 'copy_names', ['""'] + ['"%s"' % n for a, n in self.img.copies]),
            ('const char *const', 'obj_names', ['"%s"' % n for n, a in objs] or ['""']),
            ('const uint32_t', 'obj_addr', ['0x%xu' % a for n, a in objs] or ['0']),
            ('const uint32_t', 'copy_size', ['0'] + ['%du' % self.img.copy_size.get(n, 0) for a, n in self.img.copies]),
            ('const char *const', 'mod_file', ['"%s"' % l.file for l in libs] or ['""']),
            ('const uint32_t', 'mod_base', ['0x%xu' % l.base for l in libs] or ['0']),
            ('const uint32_t', 'mod_init', ['0x%xu' % l.init for l in libs] or ['0']),
            ('const char *const', 'export_names', ['"%s"' % n for n, a in exports] or ['""']),
            ('const uint32_t', 'export_addr', ['0x%xu' % a for n, a in exports] or ['0']),
            ('const uint32_t', 'reloc_addr', ['0x%xu' % a for a, t, n in relocs] or ['0']),
            ('const uint8_t', 'reloc_type', ['%d' % t for a, t, n in relocs] or ['0']),
            ('const char *const', 'reloc_sym', ['"%s"' % n for a, t, n in relocs] or ['""']),
        ]
        for t, n, vals in arrays:
            out.append('static %s %s[] = {%s};' % (t, n, ','.join(vals)))
        out.append('const gen_program gen_program_%s = {"%s", fn_addr, fn_ptr, %d, import_names, import_got, '
                   'import_plt, %d, copy_addr, copy_names, %d, obj_names, obj_addr, %d, '
                   'copy_size, mod_file, mod_base, mod_init, %d, export_names, export_addr, %d, reloc_addr, reloc_type, reloc_sym, %d};'
                   % (self.prog, self.prog, len(tab), len(self.img.imports), len(self.img.copies), len(objs),
                      len(libs), len(exports), len(relocs)))
        with open(os.path.join(outdir, 'gen_tables.c'), 'w') as fh:
            fh.write('\n'.join(out) + '\n')
        return count, len(files)

    @property
    def import_names(self):
        return [n for p, g, n in self.img.imports]

    def import_index(self, name):
        # every module's PLT entry for a name calls the same import
        if not hasattr(self, '_import_index'):
            self._import_index = {}
            for i, n in enumerate(self.import_names):
                self._import_index.setdefault(n, i)
        return self._import_index[name]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('out')
    ap.add_argument('--per-file', type=int, default=400)
    ap.add_argument('--name', default='game', help='program name: prefixes its C names, and its table is gen_program_<name>')
    ap.add_argument('--files', type=int, help='write exactly this many gen_NNN.c files (the build lists them up front)')
    ap.add_argument('--overrides', help='file of function names the runtime implements (ovr_<name>)')
    ap.add_argument('--patches', help='file of "symbol+offset hexbytes" lines applied before translation')
    ap.add_argument('--libs', help='file of "library base" lines: shared libraries next to the ELF, translated '
                                   'with it and loaded at those bases')
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    img = Image(a.elf)
    if a.libs:
        for line in open(a.libs):
            line = line.split('#')[0].strip()
            if line:
                name, base = line.split()
                img.add_library(Image(os.path.join(os.path.dirname(os.path.abspath(a.elf)), name), int(base, 16)))
    ovr = []
    if a.overrides:
        ovr = [l.split('#')[0].strip() for l in open(a.overrides)]
        ovr = [l for l in ovr if l]
    if a.patches:
        for line in open(a.patches):
            line = line.split('#')[0].strip()
            if not line:
                continue
            where, hexb = line.split(None, 1)
            sym, off = where.split('+') if '+' in where else (where, '0')
            base = img.func_by_name.get(sym)
            if base is None:
                print('patch: no symbol %s, skipped' % sym)
                continue
            addr = base + int(off, 16)
            img.patch(addr, bytes.fromhex(hexb.replace(' ', '')))
    rc = Recompiler(img, ovr, a.name)
    n, files = rc.run(a.out, a.per_file)
    print('%d functions, %d instructions, %d files' % (len(rc.starts), n, files))
    if a.files is not None:
        if files > a.files:
            sys.exit('recomp: %d files written but the build expects %d; raise GEN_COUNT' % (files, a.files))
        for k in range(files, a.files):
            with open(os.path.join(a.out, 'gen_%03d.c' % k), 'w') as fh:
                fh.write('/* empty */\n')
    if rc.unsupported:
        print('unsupported (%d kinds, %d sites):' % (len(rc.unsupported), sum(rc.unsupported.values())))
        for k, v in rc.unsupported.most_common(60):
            print('  %6d %s' % (v, k))


if __name__ == '__main__':
    main()
