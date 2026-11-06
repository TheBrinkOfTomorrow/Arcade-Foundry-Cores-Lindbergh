// The 8x8 inverse DCT of CRI's MPEG decoder (Sofdec; DCT_IsseTrans in Sega
// Race TV, The House of the Dead 4, Let's Go Jungle!), done natively: the
// game's own is MMX/SSE, which the recompiler runs a helper call per
// instruction, a large share of a frame while a movie plays (issue #49).
// It is the row-table / column algorithm of Intel's application note AP-922,
// computed here exactly as the game's code does (the same 16-bit saturating
// and 32-bit wrapping arithmetic, the same rounding), so the pictures are
// the same to the bit. Its coefficient tables are read from the game, which
// lays them out differently from build to build: `k` points at each (see
// sofdec_idct).
#include <stdint.h>
#include <string.h>

static inline int16_t idct_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline int16_t idct_adds(int16_t a, int16_t b) { return idct_sat16((int32_t)a + b); }
static inline int16_t idct_subs(int16_t a, int16_t b) { return idct_sat16((int32_t)a - b); }
// pmaddwd's lane: two products added, wrapping at 32 bits
static inline uint32_t idct_madd(int16_t a, int16_t b, int16_t c, int16_t d)
{
    return (uint32_t)((int64_t)a * b + (int64_t)c * d);
}
static inline int16_t idct_mulh(int16_t a, int16_t b) { return (int16_t)(((int32_t)a * b) >> 16); }

// one row: x the 8 coefficients, t its table (32 halfwords), rnd the rounder (2 int32)
static inline void idct_row(const int16_t *x, int16_t *y, const int16_t *t, const int32_t *rnd)
{
    // pmaddwd of [x0,x2,x0,x2], [x4,x6,x4,x6], [x1,x3,x1,x3], [x5,x7,x5,x7] with the table
    int32_t a0[2], a1[2], b0[2], b1[2];
    for (int k = 0; k < 2; k++) {
        a0[k] = (int32_t)(idct_madd(x[0], t[0 + 2 * k], x[2], t[1 + 2 * k]) + (uint32_t)rnd[k] +
                          idct_madd(x[4], t[4 + 2 * k], x[6], t[5 + 2 * k]));
        a1[k] = (int32_t)(idct_madd(x[0], t[8 + 2 * k], x[2], t[9 + 2 * k]) + (uint32_t)rnd[k] +
                          idct_madd(x[4], t[12 + 2 * k], x[6], t[13 + 2 * k]));
        b0[k] = (int32_t)(idct_madd(x[1], t[16 + 2 * k], x[3], t[17 + 2 * k]) +
                          idct_madd(x[5], t[20 + 2 * k], x[7], t[21 + 2 * k]));
        b1[k] = (int32_t)(idct_madd(x[1], t[24 + 2 * k], x[3], t[25 + 2 * k]) +
                          idct_madd(x[5], t[28 + 2 * k], x[7], t[29 + 2 * k]));
    }
    y[0] = idct_sat16((int32_t)((uint32_t)a0[0] + (uint32_t)b0[0]) >> 12);
    y[1] = idct_sat16((int32_t)((uint32_t)a0[1] + (uint32_t)b0[1]) >> 12);
    y[2] = idct_sat16((int32_t)((uint32_t)a1[0] + (uint32_t)b1[0]) >> 12);
    y[3] = idct_sat16((int32_t)((uint32_t)a1[1] + (uint32_t)b1[1]) >> 12);
    y[4] = idct_sat16((int32_t)((uint32_t)a1[1] - (uint32_t)b1[1]) >> 12);
    y[5] = idct_sat16((int32_t)((uint32_t)a1[0] - (uint32_t)b1[0]) >> 12);
    y[6] = idct_sat16((int32_t)((uint32_t)a0[1] - (uint32_t)b0[1]) >> 12);
    y[7] = idct_sat16((int32_t)((uint32_t)a0[0] - (uint32_t)b0[0]) >> 12);
}

// the column pass on 4 columns of b (8 rows of 8 halfwords), in place;
// k: the game's column constants, 4 halfwords each, in the order of sofdec_idct
static inline void idct_cols(int16_t *b, const int16_t *const *k)
{
    const int16_t *c830 = k[0], *c838 = k[1], *c840 = k[2], *c848 = k[3], *c850 = k[4], *c858 = k[5], *c868 = k[6];
    for (int i = 0; i < 4; i++) {
#define R(n) b[8 * (n) + i]
        int16_t r1 = R(1), r2 = R(2), r3 = R(3), r5 = R(5), r6 = R(6), r7 = R(7), r0 = R(0), r4 = R(4);
        int16_t m0 = idct_mulh(r5, c838[i]), m1 = idct_mulh(r3, c838[i]);
        int16_t m4 = idct_mulh(r7, c848[i]), m5 = idct_mulh(r1, c848[i]);
        m0 = idct_adds(m0, r5);
        m1 = idct_adds(m1, r3);
        int16_t m7 = idct_mulh(r6, c840[i]);
        m0 = idct_adds(m0, r3);
        int16_t m2 = idct_subs(r5, m1);
        int16_t m3 = idct_mulh(r2, c840[i]);
        m5 = idct_subs(m5, r7);
        m4 = idct_adds(m4, r1);
        int16_t m0s = m0;
        m0 = idct_adds(m0, m4);
        m0 = idct_adds(m0, c868[i]);
        m4 = idct_subs(m4, m0s);
        m7 = idct_adds(m7, r2);
        int16_t m6 = m5;
        m3 = idct_subs(m3, r6);
        m5 = idct_subs(m5, m2);
        m5 = idct_adds(m5, c868[i]);
        m6 = idct_adds(m6, m2);
        int16_t t7 = m0;           // stored to row 7, read back below
        int16_t t3 = m6;           // stored to row 3
        int16_t m1b = m4;
        m4 = idct_adds(m4, m5);
        int16_t p = idct_mulh(m4, c830[i]);
        m1b = idct_subs(m1b, m5);
        int16_t q = idct_mulh(m1b, c830[i]);
        m4 = idct_adds(m4, p);
        m4 = (int16_t)(m4 | c868[i]);
        int16_t s5 = idct_adds(r0, r4);
        int16_t s6 = idct_subs(r0, r4);
        q = idct_adds(q, m1b);
        q = (int16_t)(q | c868[i]);
        int16_t s2 = s5;
        s5 = idct_adds(s5, m7);
        int16_t s1 = s6;
        s5 = idct_adds(s5, c858[i]);
        s2 = idct_subs(s2, m7);
        s6 = idct_adds(s6, m3);
        s6 = idct_adds(s6, c858[i]);
        int16_t o0 = idct_adds(t7, s5);
        s1 = idct_subs(s1, m3);
        s2 = idct_adds(s2, c850[i]);
        int16_t s3 = s6;
        s1 = idct_adds(s1, c850[i]);
        int16_t o1 = idct_adds(s6, m4);
        int16_t s7 = s1;
        int16_t o2 = idct_adds(s1, q);
        int16_t o3 = idct_adds(t3, s2);
        int16_t o4 = idct_subs(s2, t3);
        int16_t o5 = idct_subs(s7, q);
        int16_t o7 = idct_subs(s5, t7);
        int16_t o6 = idct_subs(s3, m4);
        R(0) = (int16_t)(o0 >> 5); R(1) = (int16_t)(o1 >> 5); R(2) = (int16_t)(o2 >> 5); R(3) = (int16_t)(o3 >> 5);
        R(4) = (int16_t)(o4 >> 5); R(5) = (int16_t)(o5 >> 5); R(6) = (int16_t)(o6 >> 5); R(7) = (int16_t)(o7 >> 5);
#undef R
    }
}

// src: 64 coefficients; dst: 64 results; full: 0 for a DC-only block.
// t: the game's tables: [0..3] the row tables (32 halfwords) of rows 0, 1,
// 2 and 3 (rows 4-7 take 0, 3, 2, 1), [4] the row rounder (two 32-bit
// words), [5..11] the column constants (in Race TV at 0x884a830, 838, 840,
// 848, 850, 858, 868)
static inline void sofdec_idct(const int16_t *src, int16_t *dst, int full, const int16_t *const *t)
{
    if (!full) {
        int v = src[0];
        int d = v >= 0 ? (v + 4) >> 3 : -((4 - v) >> 3);
        for (int i = 0; i < 64; i++) dst[i] = (int16_t)d;
        return;
    }
    static const int order[8] = {0, 1, 2, 3, 0, 3, 2, 1};
    for (int r = 0; r < 8; r++) idct_row(src + 8 * r, dst + 8 * r, t[order[r]], (const int32_t *)t[4]);
    idct_cols(dst, t + 5);
    idct_cols(dst + 4, t + 5);
}
