// NVIDIA's options on ARB assembly programs, lowered to plain ARB for the iOS
// build, where gl4es translates ARB programs to GLSL ES and knows no NV option.
// Cg's vp40/fp40 output (Sega Race TV's precompiled programs) and the
// runtime's own NV_vertex_program and NV_fragment_program translations
// (nv_vertex_program.c) carry OPTION NV_vertex_program3 / NV_fragment_program2
// but mostly use ARB instructions with NVIDIA's extras:
//   precision suffixes (MULR, MOVH)            -> MUL, MOV
//   SHORT TEMP / LONG TEMP                      -> TEMP
//   the condition code registers (TEMP RC, HC)  -> dropped
//   a branch label with no branches (BB1:)      -> dropped
//   _SAT in a vertex program                    -> a clamp through a temporary
//   |x|                                         -> ABS through a temporary
//   SGT, SLE                                    -> SLT, SGE with the operands swapped
//   RCC                                         -> RCP (no clamp)
//   a condition code update and a write under   -> the code in a temporary, and
//   a test (GT, NE, ...)                           old + mask * (new - old)
//   ARL A0.xy (NV_vertex_program3)              -> A0.x, and the rest where
//                                                  split_address_components keeps them
//   ARB_fragment_program_shadow's SHADOW2D      -> 2D (ES 2 has no shadow samplers)
// A program that really branches, uses condition codes or other NV-only
// instructions is left as it is (and fails as before).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ARB_vertex_program and ARB_fragment_program instructions
static const char *const arb_ops[] = {
    "ABS", "ADD", "ARL", "CMP", "COS", "DP3", "DP4", "DPH", "DST", "EX2", "EXP", "FLR", "FRC",
    "KIL", "LG2", "LIT", "LOG", "LRP", "MAD", "MAX", "MIN", "MOV", "MUL", "POW", "RCP", "RSQ",
    "SCS", "SGE", "SIN", "SLT", "SUB", "SWZ", "TEX", "TXB", "TXP", "XPD", NULL,
};

static const char *const declarations[] = {
    "PARAM", "TEMP", "ADDRESS", "ATTRIB", "OUTPUT", "ALIAS", "OPTION", "END", NULL,
};

static int starts_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return !strncmp(s, w, n) && !(s[n] == '_' || (s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= '0' && s[n] <= '9'));
}

struct out { char *p; size_t n, cap; };

static void put(struct out *o, const char *s, size_t n)
{
    if (o->n + n + 1 > o->cap) o->p = realloc(o->p, o->cap = (o->n + n + 1) * 2 + 256);
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}
static void puts_(struct out *o, const char *s) { put(o, s, strlen(s)); }

// one instruction's opcode: its ARB base, whether it saturates, updates the
// condition code (C), or is SGT/SLE (SLT/SGE with the operands swapped);
// 0 if not lowerable
static int split_op(const char *op, size_t n, char *base, int *sat, int *cc, int *swap)
{
    *sat = n > 4 && !strncmp(op + n - 4, "_SAT", 4);
    if (*sat) n -= 4;
    *swap = 0;
    // NV instructions as ARB ones: SGT and SLE with the operands swapped;
    // RCC (a reciprocal clamped to 2^-64...2^64) as RCP
    static const char *const swapped[][3] = {{"SGT", "SLT", "1"}, {"SLE", "SGE", "1"}, {"RCC", "RCP", "0"}};
    for (int pass = 0; pass < 2; pass++) {
        int count = pass ? 3 : (int)(sizeof arb_ops / sizeof arb_ops[0]) - 1;
        for (int i = 0; i < count; i++) {
            const char *name = pass ? swapped[i][0] : arb_ops[i];
            size_t b = strlen(name), k = b;
            if (n < b || strncmp(op, name, b)) continue;
            if (k < n && strchr("RHX", op[k])) k++;   // a precision suffix
            *cc = k < n && op[k] == 'C';
            if (*cc) k++;
            if (k != n) continue;
            strcpy(base, pass ? swapped[i][1] : name);
            *swap = pass && swapped[i][2][0] == '1';
            return 1;
        }
    }
    return 0;
}

// the operands of an instruction (after its opcode, up to ';'), split at commas
static int split_args(const char *args, char out[4][512])
{
    int n = 0;
    const char *p = args;
    while (*p && *p != ';' && n < 4) {
        while (*p == ' ' || *p == '\t') p++;
        const char *e = p;
        int depth = 0;
        while (*e && *e != ';' && (*e != ',' || depth)) depth += *e == '{' ? 1 : *e == '}' ? -1 : 0, e++;
        size_t len = (size_t)(e - p);
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        if (len >= 512) return -1;
        memcpy(out[n], p, len);
        out[n++][len] = 0;
        p = *e == ',' ? e + 1 : e;
    }
    return n;
}

// state.fog.color and state.fog.params, which gl4es does not bind: program
// environment parameters 22 and 23 instead, which the runtime keeps equal to
// the fog state (hle_gfx.c, fog_env_update); 1 when the program used them
int arb_fog_env(char *src)
{
    static const struct { const char *state, *env; } map[] = {
        {"state.fog.color", "program.env[22]"}, {"state.fog.params", "program.env[23]"},
    };
    int used = 0;
    for (int i = 0; i < 2; i++) {
        size_t n = strlen(map[i].state);   // both replacements are the same length or shorter
        for (char *p = src; (p = strstr(p, map[i].state)); ) {
            size_t m = strlen(map[i].env);
            memcpy(p, map[i].env, m);
            memmove(p + m, p + n, strlen(p + n) + 1);
            p += m;
            used = 1;
        }
    }
    return used;
}


// A loop as Cg writes it (Let's Go Jungle's lights):
//   BBa: <op>C CC.x, ...; BRA BBe (EQ.x); <body>; BRA BBa; BBe:
// unrolled LOOP_UNROLL times, each body write made conditional on the
// negated test, so the passes after the count change nothing.
#define LOOP_UNROLL 8
static char *unroll_loops(char *src)
{
    if (!strstr(src, "BRA")) return src;
    // the lines
    char **l = NULL;
    int n = 0, cap = 0;
    for (char *p = src; *p;) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (n == cap) l = realloc(l, sizeof(char *) * (size_t)(cap = cap ? cap * 2 : 256));
        l[n++] = strndup(p, len);
        p += len + (e ? 1 : 0);
    }
    static const char *const neg[][2] = {{"EQ", "NE"}, {"NE", "EQ"}, {"LT", "GE"}, {"GE", "LT"}, {"GT", "LE"}, {"LE", "GT"}};
    struct out o = {0};
    puts_(&o, "");
    int ok = 1;
    for (int i = 0; i < n; i++) {
        char *s = l[i];
        while (*s == ' ' || *s == '\t') s++;
        size_t w = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
        // a label followed by a condition update and a conditional branch forward
        if (w && s[w] == ':' && i + 2 < n) {
            char label[32];
            snprintf(label, sizeof label, "%.*s", (int)w, s);
            char *set = l[i + 1], *bra = l[i + 2];
            while (*set == ' ' || *set == '\t') set++;
            while (*bra == ' ' || *bra == '\t') bra++;
            char end[32], test[3], sw[8] = "";
            if (!strncmp(bra, "BRA", 3) && sscanf(bra, "BRA %31s (%2s", end, test) == 2) {
                char *dot = strchr(bra, '(');
                if (dot && dot[3] == '.') snprintf(sw, sizeof sw, "%.*s", (int)strcspn(dot + 3, ")"), dot + 3);
                // the body up to "BRA label;"
                int back = -1;
                for (int k = i + 3; k < n && back < 0; k++) {
                    char *b = l[k];
                    while (*b == ' ' || *b == '\t') b++;
                    char tgt[32];
                    if (!strncmp(b, "BRA", 3) && sscanf(b, "BRA %31[^; ]", tgt) == 1) {
                        if (!strcmp(tgt, label) && !strchr(b, '(')) back = k;
                        else { ok = 0; break; }
                    }
                }
                const char *nt = NULL;
                for (int k = 0; k < 6; k++)
                    if (!strcmp(test, neg[k][0])) nt = neg[k][1];
                if (back < 0 || !nt) { ok = 0; break; }
                for (int r = 0; r < LOOP_UNROLL; r++) {
                    puts_(&o, l[i + 1]);
                    puts_(&o, "\n");
                    for (int k = i + 3; k < back; k++) {
                        char *b = l[k];
                        while (*b == ' ' || *b == '\t') b++;
                        size_t ow = strspn(b, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
                        char *comma = strchr(b, ',');
                        if (!ow || b[ow] == ':' || !strncmp(b, "ARL", 3) || !comma) {   // labels, ARL, no destination
                            if (!(ow && b[ow] == ':')) { puts_(&o, b); puts_(&o, "\n"); }
                            continue;
                        }
                        if (memchr(b, '(', (size_t)(comma - b))) { ok = 0; break; }
                        put(&o, b, (size_t)(comma - b));
                        char c[24];
                        snprintf(c, sizeof c, "(%s%s)", nt, sw);   // sw keeps its dot
                        puts_(&o, c);
                        puts_(&o, comma);
                        puts_(&o, "\n");
                    }
                }
                i = back;   // the end label follows and is dropped as a label
                continue;
            }
        }
        puts_(&o, l[i]);
        puts_(&o, "\n");
    }
    for (int i = 0; i < n; i++) free(l[i]);
    free(l);
    if (!ok) { free(o.p); return src; }
    free(src);
    return o.p;
}

char *arb_lower_nv(char *src)
{
    if (!strstr(src, "OPTION NV_")) return src;
    src = unroll_loops(src);
    int vertex = !strncmp(src, "!!ARBvp", 7);

    struct out o = {0};
    int declared = 0, open_statement = 0;
    char *buf = NULL;
    puts_(&o, "");
    for (char *line = src; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        free(buf);
        buf = malloc(len + 64);   // room for |x| -> lbAbsN (at most three a line)
        memcpy(buf, line, len);
        buf[len] = 0;
        line += len + (end ? 1 : 0);
        if (len && buf[len - 1] == '\r') buf[--len] = 0;

        char *s = buf;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '!' || open_statement) {
            // a comment, or the rest of a statement over several lines (PARAM c[] = { ... };)
            if (open_statement && strchr(s, ';')) open_statement = 0;
            puts_(&o, buf);
            puts_(&o, "\n");
            continue;
        }
        if (starts_word(s, "OPTION")) {
            if (strstr(s, "NV_vertex_program") || strstr(s, "NV_fragment_program") ||
                strstr(s, "ARB_fragment_program_shadow"))
                continue;
            puts_(&o, buf);
            puts_(&o, "\n");
            continue;
        }
        // a label: only when nothing branches to it
        size_t w = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
        if (w && s[w] == ':') continue;
        if (!declared) {   // the clamp's temporary and constants, ahead of everything else
            puts_(&o, "TEMP lbSat, lbAbs0, lbAbs1, lbAbs2, lbHC, lbCnd, lbMask;\n"
                      "PARAM lbZeroOne = { 0, 1, 0, 0 };\n");
            declared = 1;
        }
        if (starts_word(s, "SHORT") || starts_word(s, "LONG")) {
            s += strspn(s, "SHORTLONG");
            while (*s == ' ' || *s == '\t') s++;
        }
        int decl = 0;
        for (int i = 0; declarations[i]; i++) decl |= starts_word(s, declarations[i]);
        if (decl) {
            if (starts_word(s, "TEMP")) {
                // without the condition code registers RC and HC
                char names[1024] = "", *t = s + 4, *tok;
                size_t kept = 0;
                char list[1024];
                snprintf(list, sizeof list, "%s", t);
                char *semi = strchr(list, ';');
                if (semi) *semi = 0;
                for (tok = strtok(list, ", \t"); tok; tok = strtok(NULL, ", \t")) {
                    if (!strcmp(tok, "RC") || !strcmp(tok, "HC") || !strcmp(tok, "CC")) continue;
                    if (kept++) strcat(names, ", ");
                    strcat(names, tok);
                }
                if (kept) {
                    puts_(&o, "TEMP ");
                    puts_(&o, names);
                    puts_(&o, ";\n");
                }
                continue;
            }
            puts_(&o, s);
            puts_(&o, "\n");
            if (!strchr(s, ';')) open_statement = 1;
            continue;
        }
        // an instruction
        char base[8];
        int sat, cc, swap;
        if (!split_op(s, w, base, &sat, &cc, &swap)) goto give_up;
        char *args = s + w;
        // |x| (NV): ABS into a temporary first
        for (int k = 0; strchr(args, '|'); k++) {
            char *a = strchr(args, '|'), *b = a ? strchr(a + 1, '|') : NULL;
            if (!b || k > 2) goto give_up;
            char abs_line[256], name[8];
            snprintf(abs_line, sizeof abs_line, "ABS lbAbs%d, %.*s;\n", k, (int)(b - a - 1), a + 1);
            puts_(&o, abs_line);
            snprintf(name, sizeof name, "lbAbs%d", k);
            size_t nl = strlen(name), rest = strlen(b + 1);
            memmove(a + nl, b + 1, rest + 1);
            memcpy(a, name, nl);
        }
        char *shadow;
        while ((shadow = strstr(args, "SHADOW")) != NULL)   // SHADOW2D -> 2D
            memmove(shadow, shadow + 6, strlen(shadow + 6) + 1);
        char a[4][512];
        int na = split_args(args, a);
        if (na < 1) goto give_up;
        if (swap) {   // SGT d, x, y -> SLT d, y, x
            if (na != 3) goto give_up;
            char t[512];
            strcpy(t, a[1]);
            strcpy(a[1], a[2]);
            strcpy(a[2], t);
        }
        // the condition code (RC, HC) is lbHC, set by the instruction that updates it
        char cc_copy[600] = "";
        if (cc) {
            if (!strncmp(a[0], "HC", 2) || !strncmp(a[0], "RC", 2) || !strncmp(a[0], "CC", 2)) {
                char t[512];
                snprintf(t, sizeof t, "lbHC%s", a[0] + 2);
                strcpy(a[0], t);
            } else {   // a register and the condition code both
                char reg[512];
                strcpy(reg, a[0]);
                char *dot = strchr(reg, '.');
                snprintf(cc_copy, sizeof cc_copy, "MOV lbHC%s, %s;\n", dot ? dot : "", reg);
            }
        }
        // a write under a condition (GT, NE, ...[.swizzle]): the new value in
        // lbCnd, then dst = old + mask * (new - old), mask 1 where the test holds
        char cond[16] = "", test[3] = "";
        char *paren = strchr(a[0], '(');
        if (paren) {
            snprintf(test, sizeof test, "%.2s", paren + 1);
            const char *sw = paren + 3;
            snprintf(cond, sizeof cond, "%.*s", (int)strcspn(sw, ")"), sw);
            *paren = 0;
            if (!strcmp(test, "FL")) continue;           // never written
            if (!strcmp(test, "TR")) paren = NULL;       // always written
            else if (!strncmp(a[0], "result.", 7) || !strncmp(a[0], "o", 1)) goto give_up;   // outputs cannot be read
        }
        char dst[512];
        strcpy(dst, a[0]);
        if (paren) strcpy(a[0], "lbCnd");
        int clamp = vertex && sat;
        if (clamp) strcpy(a[0], "lbSat");
        // ARL A0.xy, s: A0.x from s, the other components where split_address_components keeps them
        if (!strcmp(dst, "A0")) strcpy(dst, "A0.xyzw");
        if (!strcmp(base, "ARL") && !strncmp(dst, "A0.", 3) && strlen(dst) > 4) {
            if (!strstr(src, "lbAI") || na != 2) goto give_up;
            char reg[512], sw[8] = "xyzw";
            strcpy(reg, a[1]);
            char *dot = strrchr(reg, '.');
            if (dot && !strchr(dot, ']')) {
                size_t l = strlen(dot + 1);
                for (int k = 0; k < 4; k++) sw[k] = dot[1 + (l == 1 ? 0 : k < (int)l ? k : (int)l - 1)];
                *dot = 0;
            }
            for (const char *m = dst + 3; *m; m++) {
                int k = *m == 'x' ? 0 : *m == 'y' ? 1 : *m == 'z' ? 2 : 3;
                char l[600];
                if (k == 0) snprintf(l, sizeof l, "ARL A0.x, %s.%c;\n", reg, sw[0]);
                else snprintf(l, sizeof l, "MOV lbAI.%c, %s.%c;\n", *m, reg, sw[k]);
                puts_(&o, l);
            }
            continue;
        }
        // ARL's source is a scalar (Cg writes ARL A0.x, R0)
        if (!strcmp(base, "ARL") && na == 2 && !strchr(strrchr(a[1], ']') ? strrchr(a[1], ']') : a[1], '.'))
            strcat(a[1], ".x");
        puts_(&o, base);
        if (sat && !clamp) puts_(&o, "_SAT");
        for (int k = 0; k < na; k++) {
            puts_(&o, k ? ", " : " ");
            puts_(&o, a[k]);
        }
        puts_(&o, ";\n");
        if (clamp) {   // dst = min(max(lbSat, 0), 1)
            char l[1200];
            snprintf(l, sizeof l, "MAX lbSat, lbSat, lbZeroOne.x;\nMIN %s, lbSat, lbZeroOne.y;\n", dst);
            puts_(&o, l);
        }
        puts_(&o, cc_copy);
        if (paren) {
            static const struct { const char *test, *mask; } tests[] = {
                {"GT", "SLT lbMask, lbZeroOne.x, lbHC;\n"},
                {"LT", "SLT lbMask, lbHC, lbZeroOne.x;\n"},
                {"GE", "SGE lbMask, lbHC, lbZeroOne.x;\n"},
                {"LE", "SGE lbMask, lbZeroOne.x, lbHC;\n"},
                {"NE", "ABS lbMask, lbHC;\nSLT lbMask, lbZeroOne.x, lbMask;\n"},
                {"EQ", "ABS lbMask, lbHC;\nSGE lbMask, lbZeroOne.x, lbMask;\n"},
            };
            const char *mask = NULL;
            for (int k = 0; k < 6; k++)
                if (!strcmp(test, tests[k].test)) mask = tests[k].mask;
            if (!mask) goto give_up;
            char reg[512];
            strcpy(reg, dst);
            char *dot = strchr(reg, '.');
            if (dot) *dot = 0;
            char l[1600];
            snprintf(l, sizeof l, "%sSUB lbCnd, lbCnd, %s;\nMAD %s, lbMask%s, lbCnd, %s;\n", mask, reg, dst, cond, reg);
            puts_(&o, l);
        }
    }
    free(buf);
    free(src);
    return o.p;

give_up:
    free(buf);
    free(o.p);
    return src;
}

// Rows of a texture matrix in a parameter array (Sega Race TV:
// state.matrix.texture[2].transpose.row[0..1]), which gl4es does not bind:
// the array holds zeros in their places, a second array the whole matrix,
// and the instructions that read those places (c[k]) read its rows instead.
char *arb_matrix_rows(char *src)
{
    char *start = strstr(src, "PARAM c[");
    if (!start || !strstr(src, ".row[")) return src;
    char *open = strchr(start, '{'), *close = open ? strstr(open, "};") : NULL;
    if (!open || !close) return src;
    struct { int pos, row, matrix; } map[16];
    char matrices[4][128];
    int nmap = 0, nmat = 0, pos = 0;
    struct out list = {0};
    puts_(&list, "");
    for (char *p = open + 1; p < close;) {
        while (p < close && strchr(" \t\r\n", *p)) p++;
        char *e = p;
        int depth = 0;
        while (e < close && (*e != ',' || depth)) depth += *e == '{' ? 1 : *e == '}' ? -1 : 0, e++;
        char item[256];
        size_t len = (size_t)(e - p);
        while (len && strchr(" \t\r\n", p[len - 1])) len--;
        if (len >= sizeof item) goto keep;
        memcpy(item, p, len);
        item[len] = 0;
        int count = 1, a, b;
        char *rows = strstr(item, ".row[");
        char *range = strrchr(item, '[');
        if (item[0] == '{') count = 1;
        else if (rows) count = sscanf(rows, ".row[%d..%d]", &a, &b) == 2 ? b - a + 1 : 1;
        else if (!strncmp(item, "state.matrix.", 13)) count = 4;
        else if (range && sscanf(range, "[%d..%d]", &a, &b) == 2) count = b - a + 1;
        if (rows && strstr(item, "state.matrix.texture") && nmat < 4) {
            if (sscanf(rows, ".row[%d..%d]", &a, &b) != 2) b = a = atoi(rows + 5);
            snprintf(matrices[nmat], sizeof matrices[nmat], "%.*s", (int)(rows - item), item);
            for (int r = a; r <= b && nmap < 16; r++, pos++) {
                map[nmap].pos = pos, map[nmap].row = r, map[nmap].matrix = nmat, nmap++;
                if (list.n) puts_(&list, ",\n\t\t");
                puts_(&list, "{ 0, 0, 0, 0 }");
            }
            nmat++;
        } else {
            if (list.n) puts_(&list, ",\n\t\t");
            puts_(&list, item);
            pos += count;
        }
        p = e < close ? e + 1 : e;
    }
    if (!nmap) goto keep;
    struct out o = {0};
    puts_(&o, "");
    put(&o, src, (size_t)(start - src));
    for (int m = 0; m < nmat; m++) {
        char d[256];
        snprintf(d, sizeof d, "PARAM lbRows%d[4] = { %s };\n", m, matrices[m]);
        puts_(&o, d);
    }
    put(&o, start, (size_t)(open + 1 - start));
    puts_(&o, " ");
    puts_(&o, list.p);
    puts_(&o, " ");
    // the instructions after the array: c[k] for a moved row -> lbRowsM[r]
    for (char *p = close; *p;) {
        if (!strncmp(p, "c[", 2) && (p == src || !strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789", p[-1]))) {
            char *e;
            long k = strtol(p + 2, &e, 10);
            int hit = -1;
            if (e > p + 2 && *e == ']')
                for (int i = 0; i < nmap; i++)
                    if (map[i].pos == k) hit = i;
            if (hit >= 0) {
                char r[32];
                snprintf(r, sizeof r, "lbRows%d[%d]", map[hit].matrix, map[hit].row);
                puts_(&o, r);
                p = e + 1;
                continue;
            }
        }
        put(&o, p, 1);
        p++;
    }
    free(list.p);
    free(src);
    return o.p;
keep:
    free(list.p);
    return src;
}

// Constants written in an instruction (ADD R4, {1}, -R3.wwww; the runtime's
// NV_fragment_program translation), which gl4es does not parse: each becomes
// a PARAM of its own, declared after the header and options.
char *arb_inline_constants(char *src)
{
    struct out decl = {0}, body = {0};
    puts_(&decl, "");
    puts_(&body, "");
    int k = 0, open_statement = 0;
    for (char *line = src; *line;) {
        char *e = strchr(line, '\n');
        size_t len = e ? (size_t)(e - line) : strlen(line);
        char *s = line;
        line += len + (e ? 1 : 0);
        while (s < line && (*s == ' ' || *s == '\t')) s++;
        int is_decl = starts_word(s, "PARAM") || starts_word(s, "TEMP") || starts_word(s, "OPTION") ||
                      starts_word(s, "ATTRIB") || starts_word(s, "OUTPUT") || starts_word(s, "ADDRESS") ||
                      starts_word(s, "ALIAS") || *s == '#' || *s == '!' || open_statement;
        const char *nl = e ? "\n" : "";
        if (is_decl || !memchr(s, '{', len - (size_t)(s - (line - len - (e ? 1 : 0))))) {
            size_t start = (size_t)(line - len - (e ? 1 : 0) - src);
            put(&body, src + start, len);
            puts_(&body, nl);
            if (starts_word(s, "PARAM") && !memchr(s, ';', len)) open_statement = 1;
            else if (open_statement && memchr(s, ';', len)) open_statement = 0;
            continue;
        }
        const char *p = line - len - (e ? 1 : 0), *end = p + len;
        while (p < end) {
            if (*p != '{') { put(&body, p, 1); p++; continue; }
            const char *c = memchr(p, '}', (size_t)(end - p));
            if (!c) { put(&body, p, (size_t)(end - p)); break; }
            char d[256];
            snprintf(d, sizeof d, "PARAM lbK%d = %.*s;\n", k, (int)(c - p + 1), p);
            puts_(&decl, d);
            snprintf(d, sizeof d, "lbK%d", k++);
            puts_(&body, d);
            p = c + 1;
        }
        puts_(&body, nl);
    }
    if (!k) {
        free(decl.p);
        free(body.p);
        return src;
    }
    // the declarations go after the header line and any OPTION lines
    struct out o = {0};
    puts_(&o, "");
    char *b = body.p, *at = b;
    for (;;) {
        char *e = strchr(at, '\n');
        if (!e) break;
        char *s = at;
        while (*s == ' ' || *s == '\t') s++;
        if (!(*s == '!' || starts_word(s, "OPTION") || *s == '#' || *s == '\n')) break;
        at = e + 1;
    }
    put(&o, b, (size_t)(at - b));
    puts_(&o, decl.p);
    puts_(&o, at);
    free(decl.p);
    free(body.p);
    free(src);
    return o.p;
}
