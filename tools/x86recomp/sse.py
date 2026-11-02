"""SSE/SSE2/SSE3 and MMX translation for recomp.py."""
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM

CMP_PRED = {'eq': 0, 'lt': 1, 'le': 2, 'unord': 3, 'neq': 4, 'nlt': 5, 'nle': 6, 'ord': 7}

# packed integer ops: field, lanes per 128 bits, C expression of a and b
PINT = {
    'paddb': ('b', 16, '(uint8_t)(a+b)'),
    'paddw': ('w', 8, '(uint16_t)(a+b)'),
    'paddd': ('u', 4, 'a+b'),
    'paddq': ('q', 2, 'a+b'),
    'psubb': ('b', 16, '(uint8_t)(a-b)'),
    'psubw': ('w', 8, '(uint16_t)(a-b)'),
    'psubd': ('u', 4, 'a-b'),
    'psubq': ('q', 2, 'a-b'),
    'paddsw': ('sw', 8, 'sat16((int32_t)a+(int32_t)b)'),
    'psubsw': ('sw', 8, 'sat16((int32_t)a-(int32_t)b)'),
    'pmulhw': ('sw', 8, '(int16_t)(((int32_t)a*(int32_t)b)>>16)'),
    'pmullw': ('sw', 8, '(int16_t)((int32_t)a*(int32_t)b)'),
    'pcmpeqb': ('b', 16, 'a==b?0xffu:0u'),
    'pcmpeqw': ('w', 8, 'a==b?0xffffu:0u'),
    'pcmpeqd': ('u', 4, 'a==b?0xffffffffu:0u'),
    'pcmpgtd': ('i', 4, 'a>b?-1:0'),
    'pand': ('q', 2, 'a&b'),
    'por': ('q', 2, 'a|b'),
    'pxor': ('q', 2, 'a^b'),
    'pandn': ('q', 2, '~a&b'),
    'andps': ('u', 4, 'a&b'), 'andpd': ('u', 4, 'a&b'),
    'orps': ('u', 4, 'a|b'), 'orpd': ('u', 4, 'a|b'),
    'xorps': ('u', 4, 'a^b'), 'xorpd': ('u', 4, 'a^b'),
    'andnps': ('u', 4, '~a&b'), 'andnpd': ('u', 4, '~a&b'),
    'addps': ('f', 4, 'a+b'), 'subps': ('f', 4, 'a-b'), 'mulps': ('f', 4, 'a*b'), 'divps': ('f', 4, 'a/b'),
    'minps': ('f', 4, 'sse_minf(a,b)'), 'maxps': ('f', 4, 'sse_maxf(a,b)'),
    'addpd': ('d', 2, 'a+b'), 'subpd': ('d', 2, 'a-b'), 'mulpd': ('d', 2, 'a*b'), 'divpd': ('d', 2, 'a/b'),
    'pavgb': ('b', 16, '(uint8_t)((a+b+1)>>1)'),
    'pminsw': ('sw', 8, 'a<b?a:b'), 'pmaxsw': ('sw', 8, 'a>b?a:b'),
}
SCALAR = {
    'addss': ('f', 'a+b'), 'subss': ('f', 'a-b'), 'mulss': ('f', 'a*b'), 'divss': ('f', 'a/b'),
    'minss': ('f', 'sse_minf(a,b)'), 'maxss': ('f', 'sse_maxf(a,b)'), 'sqrtss': ('f', 'sqrtf(b)'),
    'rcpss': ('f', '1.0f/b'), 'rsqrtss': ('f', '1.0f/sqrtf(b)'),
    'addsd': ('d', 'a+b'), 'subsd': ('d', 'a-b'), 'mulsd': ('d', 'a*b'), 'divsd': ('d', 'a/b'),
    'minsd': ('d', 'sse_mind(a,b)'), 'maxsd': ('d', 'sse_maxd(a,b)'), 'sqrtsd': ('d', 'sqrt(b)'),
}


class SSEGen:
    def kind(self, ins, op):
        if op.type != X86_OP_REG:
            return None, None
        n = ins.reg_name(op.reg)
        if n.startswith('xmm'):
            return 'x', int(n[3:])
        if n.startswith('mm'):
            return 'm', int(n[2:])
        return None, None

    def vec_of(self, fg, ins, op, width):
        """Temp S holding the operand: width 16 (xmm) or 8 (mmx)."""
        k, n = self.kind(ins, op)
        t = 'xmm_t' if width == 16 else 'mmx_t'
        if k == 'x':
            return 'xmm_t S=c->xmm[%d];' % n
        if k == 'm':
            return 'mmx_t S=c->mm[%d];' % n
        if op.type == X86_OP_MEM:
            a = fg.addr(ins, op)
            if op.size == 16:
                return 'xmm_t S;rd128(&S,%s);' % a
            if op.size == 8:
                return '%s S;memset(&S,0,sizeof S);memcpy(&S,M+%s,8);' % (t, a)
            if op.size == 4:
                return '%s S;memset(&S,0,sizeof S);memcpy(&S,M+%s,4);' % (t, a)
            if op.size == 2:
                return '%s S;memset(&S,0,sizeof S);memcpy(&S,M+%s,2);' % (t, a)
        if op.type == X86_OP_REG:
            return '%s S;memset(&S,0,sizeof S);S.u[0]=%s;' % (t, fg.rd(ins, op))
        raise Exception('vec operand')

    def dst(self, ins, op):
        k, n = self.kind(ins, op)
        if k == 'x':
            return 'c->xmm[%d]' % n, 16
        if k == 'm':
            return 'c->mm[%d]' % n, 8
        return None, None

    def gen(self, fg, ins, mn, ops):
        from recomp import Unsupported
        h = getattr(self, 'g_' + mn, None)
        if h:
            return '{' + h(fg, ins, ops) + '}'
        if mn in PINT:
            field, lanes, expr = PINT[mn]
            D, w = self.dst(ins, ops[0])
            if D is None:
                raise Unsupported('pint dst')
            if w == 8:
                lanes //= 2
                if field == 'f' or field == 'd':
                    raise Unsupported('mmx float')
                if field == 'q':
                    return '{%s{uint64_t a=%s.q,b=S.q;%s.q=%s;}}' % (self.vec_of(fg, ins, ops[1], 8), D, D, expr)
            return '{%sfor(int k=0;k<%d;k++){__typeof__(S.%s[0]) a=%s.%s[k],b=S.%s[k];%s.%s[k]=%s;}}' % (
                self.vec_of(fg, ins, ops[1], w), lanes, field, D, field, field, D, field, expr)
        if mn in SCALAR:
            field, expr = SCALAR[mn]
            D, w = self.dst(ins, ops[0])
            return '{%s{__typeof__(S.%s[0]) a=%s.%s[0],b=S.%s[0];%s.%s[0]=%s;}}' % (
                self.vec_of(fg, ins, ops[1], 16), field, D, field, field, D, field, expr)
        for suf, field in (('ss', 'f'), ('sd', 'd'), ('ps', 'f'), ('pd', 'd')):
            if mn.startswith('cmp') and mn.endswith(suf) and mn[3:-2] in CMP_PRED:
                p = CMP_PRED[mn[3:-2]]
                D, w = self.dst(ins, ops[0])
                lanes = 1 if suf[0] == 's' else (4 if field == 'f' else 2)
                ufield = 'u' if field == 'f' else 'q'
                ones = '0xffffffffu' if field == 'f' else '0xffffffffffffffffull'
                return '{%sfor(int k=0;k<%d;k++){%s.%s[k]=sse_cmp(%s.%s[k],S.%s[k],%d)?%s:0;}}' % (
                    self.vec_of(fg, ins, ops[1], 16), lanes, D, ufield, D, field, field, p, ones)
        return None

    # ---- moves --------------------------------------------------------------
    def mov128(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        if D is not None:
            return '%s%s=S;' % (self.vec_of(fg, ins, ops[1], w), D)
        S, w = self.dst(ins, ops[1])
        if w == 16:
            return 'wr128(%s,&%s);' % (fg.addr(ins, ops[0]), S)
        return 'wr64(%s,%s.q);' % (fg.addr(ins, ops[0]), S)

    g_movaps = g_movapd = g_movdqa = g_movdqu = g_movups = g_movupd = g_lddqu = mov128
    g_movntdq = g_movntps = g_movntq = mov128

    def g_movss(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        if D is not None:
            if ops[1].type == X86_OP_MEM:
                return '%s.u[0]=rd32(%s);%s.u[1]=%s.u[2]=%s.u[3]=0;' % (D, fg.addr(ins, ops[1]), D, D, D)
            S, _ = self.dst(ins, ops[1])
            return '%s.u[0]=%s.u[0];' % (D, S)
        S, _ = self.dst(ins, ops[1])
        return 'wr32(%s,%s.u[0]);' % (fg.addr(ins, ops[0]), S)

    def g_movsd(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        if D is not None:
            if ops[1].type == X86_OP_MEM:
                return '%s.q[0]=rd64(%s);%s.q[1]=0;' % (D, fg.addr(ins, ops[1]), D)
            S, _ = self.dst(ins, ops[1])
            return '%s.q[0]=%s.q[0];' % (D, S)
        S, _ = self.dst(ins, ops[1])
        return 'wr64(%s,%s.q[0]);' % (fg.addr(ins, ops[0]), S)

    def g_movq(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        if D is not None:
            if w == 16:
                if ops[1].type == X86_OP_MEM:
                    return '%s.q[0]=rd64(%s);%s.q[1]=0;' % (D, fg.addr(ins, ops[1]), D)
                S, sw = self.dst(ins, ops[1])
                if sw == 16:
                    return '%s.q[0]=%s.q[0];%s.q[1]=0;' % (D, S, D)
                return '%s.q[0]=%s.q;%s.q[1]=0;' % (D, S, D)
            if ops[1].type == X86_OP_MEM:
                return '%s.q=rd64(%s);' % (D, fg.addr(ins, ops[1]))
            S, sw = self.dst(ins, ops[1])
            return '%s.q=%s;' % (D, S + ('.q[0]' if sw == 16 else '.q'))
        S, sw = self.dst(ins, ops[1])
        return 'wr64(%s,%s);' % (fg.addr(ins, ops[0]), S + ('.q[0]' if sw == 16 else '.q'))

    def g_movd(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        if D is not None:
            v = fg.rd(ins, ops[1], 32)
            if w == 16:
                return '{uint32_t v=%s;memset(&%s,0,16);%s.u[0]=v;}' % (v, D, D)
            return '{uint32_t v=%s;%s.q=v;}' % (v, D)
        S, sw = self.dst(ins, ops[1])
        return fg.wr(ins, ops[0], '%s.u[0]' % S, 32)

    def g_movhlps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        S, _ = self.dst(ins, ops[1])
        return '%s.q[0]=%s.q[1];' % (D, S)

    def g_movlhps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        S, _ = self.dst(ins, ops[1])
        return '%s.q[1]=%s.q[0];' % (D, S)

    def g_movhps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        if D is not None:
            return '%s.q[1]=rd64(%s);' % (D, fg.addr(ins, ops[1]))
        S, _ = self.dst(ins, ops[1])
        return 'wr64(%s,%s.q[1]);' % (fg.addr(ins, ops[0]), S)

    g_movhpd = g_movhps

    def g_movlps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        if D is not None:
            return '%s.q[0]=rd64(%s);' % (D, fg.addr(ins, ops[1]))
        S, _ = self.dst(ins, ops[1])
        return 'wr64(%s,%s.q[0]);' % (fg.addr(ins, ops[0]), S)

    g_movlpd = g_movlps

    # ---- compares and conversions ---------------------------------------------
    def comi(self, fg, ins, ops, field):
        D, _ = self.dst(ins, ops[0])
        return '%s{__typeof__(S.%s[0]) a=%s.%s[0],b=S.%s[0];if(a!=a||b!=b){ZF=PF=CF=1;}else{ZF=a==b;CF=a<b;PF=0;}OF=SF=0;}' % (
            self.vec_of(fg, ins, ops[1], 16), field, D, field, field)

    def g_comiss(self, fg, ins, ops): return self.comi(fg, ins, ops, 'f')
    def g_ucomiss(self, fg, ins, ops): return self.comi(fg, ins, ops, 'f')
    def g_comisd(self, fg, ins, ops): return self.comi(fg, ins, ops, 'd')
    def g_ucomisd(self, fg, ins, ops): return self.comi(fg, ins, ops, 'd')

    def g_cvtsi2ss(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s.f[0]=(float)(int32_t)%s;' % (D, fg.rd(ins, ops[1], 32))

    def g_cvtsi2sd(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s.d[0]=(double)(int32_t)%s;' % (D, fg.rd(ins, ops[1], 32))

    def to_int(self, fg, ins, ops, field, trunc):
        v = self.vec_of(fg, ins, ops[1], 16)
        f = 'sse_cvtt32(S.%s[0])' % field if trunc else 'sse_cvt32(c,S.%s[0])' % field
        return v + fg.wr(ins, ops[0], '(uint32_t)' + f, 32)

    def g_cvttss2si(self, fg, ins, ops): return self.to_int(fg, ins, ops, 'f', True)
    def g_cvtss2si(self, fg, ins, ops): return self.to_int(fg, ins, ops, 'f', False)
    def g_cvttsd2si(self, fg, ins, ops): return self.to_int(fg, ins, ops, 'd', True)
    def g_cvtsd2si(self, fg, ins, ops): return self.to_int(fg, ins, ops, 'd', False)

    def g_cvtss2sd(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s%s.d[0]=(double)S.f[0];' % (self.vec_of(fg, ins, ops[1], 16), D)

    def g_cvtsd2ss(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s%s.f[0]=(float)S.d[0];' % (self.vec_of(fg, ins, ops[1], 16), D)

    def g_cvtps2pd(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s%s.d[0]=(double)S.f[0];%s.d[1]=(double)S.f[1];' % (self.vec_of(fg, ins, ops[1], 16), D, D)

    def g_cvtpd2ps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s%s.f[0]=(float)S.d[0];%s.f[1]=(float)S.d[1];%s.q[1]=0;' % (self.vec_of(fg, ins, ops[1], 16), D, D, D)

    def g_cvtdq2ps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%sfor(int k=0;k<4;k++)%s.f[k]=(float)S.i[k];' % (self.vec_of(fg, ins, ops[1], 16), D)

    def g_cvttps2dq(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%sfor(int k=0;k<4;k++)%s.i[k]=sse_cvtt32(S.f[k]);' % (self.vec_of(fg, ins, ops[1], 16), D)

    def g_rcpps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%sfor(int k=0;k<4;k++)%s.f[k]=1.0f/S.f[k];' % (self.vec_of(fg, ins, ops[1], 16), D)

    def g_sqrtps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%sfor(int k=0;k<4;k++)%s.f[k]=sqrtf(S.f[k]);' % (self.vec_of(fg, ins, ops[1], 16), D)

    def g_haddps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%sxmm_t A=%s;%s.f[0]=A.f[0]+A.f[1];%s.f[1]=A.f[2]+A.f[3];%s.f[2]=S.f[0]+S.f[1];%s.f[3]=S.f[2]+S.f[3];' % (
            self.vec_of(fg, ins, ops[1], 16), D, D, D, D, D)

    # ---- shuffles -----------------------------------------------------------------
    def g_shufps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        i = ops[2].imm
        return '%sxmm_t A=%s;%s.u[0]=A.u[%d];%s.u[1]=A.u[%d];%s.u[2]=S.u[%d];%s.u[3]=S.u[%d];' % (
            self.vec_of(fg, ins, ops[1], 16), D, D, i & 3, D, (i >> 2) & 3, D, (i >> 4) & 3, D, (i >> 6) & 3)

    def g_pshufd(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        i = ops[2].imm
        return '%s%s.u[0]=S.u[%d];%s.u[1]=S.u[%d];%s.u[2]=S.u[%d];%s.u[3]=S.u[%d];' % (
            self.vec_of(fg, ins, ops[1], 16), D, i & 3, D, (i >> 2) & 3, D, (i >> 4) & 3, D, (i >> 6) & 3)

    def g_pshuflw(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        i = ops[2].imm
        return '%s%s=S;%s.w[0]=S.w[%d];%s.w[1]=S.w[%d];%s.w[2]=S.w[%d];%s.w[3]=S.w[%d];' % (
            self.vec_of(fg, ins, ops[1], 16), D, D, i & 3, D, (i >> 2) & 3, D, (i >> 4) & 3, D, (i >> 6) & 3)

    def g_pshufhw(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        i = ops[2].imm
        return '%s%s=S;%s.w[4]=S.w[%d];%s.w[5]=S.w[%d];%s.w[6]=S.w[%d];%s.w[7]=S.w[%d];' % (
            self.vec_of(fg, ins, ops[1], 16), D, D, 4 + (i & 3), D, 4 + ((i >> 2) & 3), D, 4 + ((i >> 4) & 3), D, 4 + ((i >> 6) & 3))

    def g_pshufw(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        i = ops[2].imm
        return '%s%s.w[0]=S.w[%d];%s.w[1]=S.w[%d];%s.w[2]=S.w[%d];%s.w[3]=S.w[%d];' % (
            self.vec_of(fg, ins, ops[1], 8), D, i & 3, D, (i >> 2) & 3, D, (i >> 4) & 3, D, (i >> 6) & 3)

    def g_unpcklpd(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s%s.q[1]=S.q[0];' % (self.vec_of(fg, ins, ops[1], 16), D)

    g_punpcklqdq = g_unpcklpd

    def g_haddpd(self, fg, ins, ops):
        # SSE3: each half is the sum of a pair (the destination's, then the source's)
        D, _ = self.dst(ins, ops[0])
        return '%s{double h=%s.d[0]+%s.d[1];%s.d[1]=S.d[0]+S.d[1];%s.d[0]=h;}' % (
            self.vec_of(fg, ins, ops[1], 16), D, D, D, D)

    def g_unpckhpd(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%s%s.q[0]=%s.q[1];%s.q[1]=S.q[1];' % (self.vec_of(fg, ins, ops[1], 16), D, D, D)

    g_punpckhqdq = g_unpckhpd

    def g_unpcklps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%sxmm_t A=%s;%s.u[0]=A.u[0];%s.u[1]=S.u[0];%s.u[2]=A.u[1];%s.u[3]=S.u[1];' % (self.vec_of(fg, ins, ops[1], 16), D, D, D, D, D)

    def g_unpckhps(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        return '%sxmm_t A=%s;%s.u[0]=A.u[2];%s.u[1]=S.u[2];%s.u[2]=A.u[3];%s.u[3]=S.u[3];' % (self.vec_of(fg, ins, ops[1], 16), D, D, D, D, D)

    def punpckl(self, fg, ins, ops, field, n):
        D, w = self.dst(ins, ops[0])
        lanes = n if w == 16 else n // 2
        T = 'xmm_t' if w == 16 else 'mmx_t'
        return '%s%s A=%s;for(int k=0;k<%d;k++){%s.%s[2*k]=A.%s[k];%s.%s[2*k+1]=S.%s[k];}' % (
            self.vec_of(fg, ins, ops[1], w), T, D, lanes // 2, D, field, field, D, field, field)

    def g_punpcklbw(self, fg, ins, ops): return self.punpckl(fg, ins, ops, 'b', 16)
    def g_punpcklwd(self, fg, ins, ops): return self.punpckl(fg, ins, ops, 'w', 8)
    def g_punpckldq(self, fg, ins, ops): return self.punpckl(fg, ins, ops, 'u', 4)

    def g_packssdw(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        if w == 16:
            return '%sxmm_t A=%s;for(int k=0;k<4;k++){%s.sw[k]=sat16(A.i[k]);%s.sw[4+k]=sat16(S.i[k]);}' % (self.vec_of(fg, ins, ops[1], 16), D, D, D)
        return '%smmx_t A=%s;for(int k=0;k<2;k++){%s.sw[k]=sat16(A.i[k]);%s.sw[2+k]=sat16(S.i[k]);}' % (self.vec_of(fg, ins, ops[1], 8), D, D, D)

    def g_pmaddwd(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        n = 4 if w == 16 else 2
        T = 'xmm_t' if w == 16 else 'mmx_t'
        return '%s%s A=%s;for(int k=0;k<%d;k++)%s.i[k]=(int32_t)((uint32_t)((int32_t)A.sw[2*k]*S.sw[2*k])+(uint32_t)((int32_t)A.sw[2*k+1]*S.sw[2*k+1]));' % (
            self.vec_of(fg, ins, ops[1], w), T, D, n, D)

    def shiftv(self, fg, ins, ops, field, bits, kind):
        D, w = self.dst(ins, ops[0])
        lanes = (128 // bits) if w == 16 else (64 // bits)
        if ops[1].type == X86_OP_IMM:
            cnt = 'uint64_t n=%du;' % ops[1].imm
        else:
            cnt = '%suint64_t n=S.q%s;' % (self.vec_of(fg, ins, ops[1], w), '[0]' if w == 16 else '')
        if kind == 'l':
            e = 'n>=%d?0:(%s.%s[k]<<n)' % (bits, D, field)
        elif kind == 'r':
            e = 'n>=%d?0:(%s.%s[k]>>n)' % (bits, D, field)
        else:
            e = '%s.%s[k]>>(n>=%d?%d:n)' % (D, field, bits, bits - 1)
        if w == 8 and bits == 64:
            e = e.replace('.q[k]', '.q')
            return '%s%s.q=%s;' % (cnt, D, e)
        return '%sfor(int k=0;k<%d;k++)%s.%s[k]=%s;' % (cnt, lanes, D, field, e)

    def g_psllq(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'q', 64, 'l')
    def g_psrlq(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'q', 64, 'r')
    def g_pslld(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'u', 32, 'l')
    def g_psrld(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'u', 32, 'r')
    def g_psrad(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'i', 32, 'a')
    def g_psllw(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'w', 16, 'l')
    def g_psrlw(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'w', 16, 'r')
    def g_psraw(self, fg, ins, ops): return self.shiftv(fg, ins, ops, 'sw', 16, 'a')

    def g_psrldq(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        n = min(ops[1].imm, 16)
        return 'xmm_t A=%s;memset(&%s,0,16);memcpy(%s.b,A.b+%d,%d);' % (D, D, D, n, 16 - n)

    def g_pslldq(self, fg, ins, ops):
        D, _ = self.dst(ins, ops[0])
        n = min(ops[1].imm, 16)
        return 'xmm_t A=%s;memset(&%s,0,16);memcpy(%s.b+%d,A.b,%d);' % (D, D, D, n, 16 - n)

    def g_pextrw(self, fg, ins, ops):
        S, w = self.dst(ins, ops[1])
        n = ops[2].imm & (7 if w == 16 else 3)
        return fg.wr(ins, ops[0], '(uint32_t)%s.w[%d]' % (S, n), 32)

    def g_pinsrw(self, fg, ins, ops):
        D, w = self.dst(ins, ops[0])
        n = ops[2].imm & (7 if w == 16 else 3)
        return '%s.w[%d]=(uint16_t)%s;' % (D, n, fg.rd(ins, ops[1], 16 if ops[1].type == X86_OP_MEM else 32))

    def g_pmovmskb(self, fg, ins, ops):
        S, w = self.dst(ins, ops[1])
        return '{uint32_t m=0;for(int k=0;k<%d;k++)m|=((%s.b[k]>>7)&1u)<<k;%s}' % (w, S, fg.wr(ins, ops[0], 'm', 32))

    def g_movmskpd(self, fg, ins, ops):
        S, _ = self.dst(ins, ops[1])
        return fg.wr(ins, ops[0], '(uint32_t)((%s.q[0]>>63)|((%s.q[1]>>63)<<1))' % (S, S), 32)

    def g_movmskps(self, fg, ins, ops):
        S, _ = self.dst(ins, ops[1])
        return fg.wr(ins, ops[0], '(uint32_t)((%s.u[0]>>31)|((%s.u[1]>>31)<<1)|((%s.u[2]>>31)<<2)|((%s.u[3]>>31)<<3))' % (S, S, S, S), 32)

    def g_stmxcsr(self, fg, ins, ops):
        return 'wr32(%s,c->mxcsr);' % fg.addr(ins, ops[0])

    def g_ldmxcsr(self, fg, ins, ops):
        return 'c->mxcsr=rd32(%s);' % fg.addr(ins, ops[0])

    def g_fxsave(self, fg, ins, ops):
        return 'memset(M+%s,0,512);wr16(%s,c->fcw);wr32(%s+24u,c->mxcsr);' % (fg.addr(ins, ops[0]), fg.addr(ins, ops[0]), fg.addr(ins, ops[0]))
