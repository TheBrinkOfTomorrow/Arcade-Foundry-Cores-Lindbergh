#!/usr/bin/env python3
"""Find the functions of a program with symbols in one without them, by code.

    tools/match_functions.py REF.elf TARGET.elf REGEX

For each function of REF.elf whose name matches REGEX, the places in
TARGET.elf's .text where the same instructions are, with absolute addresses
masked and call and jump targets compared through the functions already
matched (so wrappers that differ only in what they call come apart). Sega's
AM library is linked statically into each game, and a stripped program's copy
of a build is the same code as a program with symbols that has that build
(amDongle, amDipsw, amJvs version strings tell): Harley-Davidson's is Race
TV's. The result gives overrides.txt its "0xaddress name" lines; functions
the code cannot tell apart (one-line getters) are placed by their order in the
library, which is the same in both.

Needs capstone and pyelftools (the recompiler's). Prints one line a function:
its name and the address found, or the count when it is not one.
"""
import re
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from elftools.elf.elffile import ELFFile

md = Cs(CS_ARCH_X86, CS_MODE_32)
PADDING = ('nop ', 'lea esi, [esi]', 'lea edi, [edi]', 'mov esi, esi', 'lea esi, [esi + eiz]', 'lea edi, [edi + eiz]')


def load(path):
    f = ELFFile(open(path, 'rb'))
    t = f.get_section_by_name('.text')
    return f, t['sh_addr'], t.data()


def insns(code, addr):
    """(address, the instruction with addresses masked, its call or jump target)"""
    out = []
    for a, size, m, op in md.disasm_lite(code, addr):
        tgt = None
        if m == 'call' or m.startswith('j'):
            try:
                tgt = int(op, 16)
            except ValueError:
                pass
            key = m + ' T'
        else:
            key = m + ' ' + re.sub(r'0x[0-9a-f]{6,}', 'A', op)
        out.append((a, key, tgt))
    while out and out[-1][1] in PADDING:   # the alignment after the function
        out.pop()
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    rf, rlo, rd = load(sys.argv[1])
    tf, tlo, td = load(sys.argv[2])
    pat = re.compile(sys.argv[3])
    syms = [s for s in rf.get_section_by_name('.symtab').iter_symbols()
            if s['st_info']['type'] == 'STT_FUNC' and s['st_size'] > 0 and rlo <= s['st_value'] < rlo + len(rd)
            and pat.search(s.name)]
    target = insns(td, tlo)
    first = {}
    for i, (a, k, t) in enumerate(target):
        first.setdefault(k, []).append(i)
    cand = {}
    for s in syms:
        v = s['st_value']
        n = insns(rd[v - rlo:v - rlo + s['st_size']], v)
        if len(n) < 3:
            continue
        keys = [x[1] for x in n]
        hits = [i for i in first.get(keys[0], []) if [x[1] for x in target[i:i + len(n)]] == keys]
        cand[s.name] = (v, n, hits)
    known = {}   # REF address -> TARGET address, for the functions matched once
    for _ in range(4):
        for name, (v, n, hits) in cand.items():
            ok = [i for i in hits
                  if all(rt is None or rt not in known or known[rt] == tt
                         for (ra, rk, rt), (ta, tk, tt) in zip(n, target[i:i + len(n)]))]
            if len(ok) == 1:
                known[v] = target[ok[0]][0]
            cand[name] = (v, n, ok)
    for name in sorted(cand):
        v, n, hits = cand[name]
        found = ' '.join(hex(target[i][0]) for i in hits[:4])
        print('%-48s %s%s' % (name, found, '' if len(hits) == 1 else ' (%d)' % len(hits)))


if __name__ == '__main__':
    main()
