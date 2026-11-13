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
//   NRM                                         -> DP3, RSQ, MUL
//   KIL on a condition (KIL LE.x)               -> a mask from the code, KIL -mask
// A program that really branches, uses condition codes or other NV-only
// instructions is left as it is (and fails as before).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
// Or with the test at the end (2 Spicy's lights, vp40):
//   BRA BBe (EQ.x); BBa: <body, ending <op>C CC.x, ...>; BRA BBa (NE.x); BBe:
// unrolled with a running mask instead (lbRun: 1 while the loop goes on, from
// a copy of each test, lbTest), each write blended in by it, so writes that
// have their own condition inside the body keep it; the condition updates and
// ARL run every pass (a counting loop that has ended tests the same again).
#define LOOP_UNROLL 8
// a set instruction that updates the condition code (SLTC HC.x, a, b), also
// as a plain one into lbTest (the condition registers are dropped later):
// 1 if it was one, and *set says so for the latest update
static int copies;   // lbTest written (unroll_loops declares it)
static int set_copy(struct out *o, const char *b, int *set)
{
    size_t ow = strspn(b, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
    const char *dst = b + ow + strspn(b + ow, " \t");
    if (ow < 2 || b[ow - 1] != 'C' || (strncmp(dst, "HC", 2) && strncmp(dst, "RC", 2))) return 0;
    *set = b[0] == 'S' && strncmp(b, "SWZ", 3) && strncmp(b, "SIN", 3) && strncmp(b, "SCS", 3);
    if (!*set) return 1;
    const char *comma = strchr(dst, ',');
    char d[16];
    snprintf(d, sizeof d, "%.*s", (int)strcspn(dst + 2, " ,("), dst + 2);   // the mask: .x
    char line[256];
    snprintf(line, sizeof line, "%.*s lbTest%s%s\n", (int)ow - 1, b, d, comma ? comma : ";");
    puts_(o, line);
    copies = 1;
    return 1;
}

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
    int ok = 1, loops_run = 0, last_set = 0;
    copies = 0;
    for (int i = 0; i < n; i++) {
        char *s = l[i];
        while (*s == ' ' || *s == '\t') s++;
        size_t w = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
        // a conditional branch forward past a loop that tests at its end
        if (!strncmp(s, "BRA", 3) && i + 1 < n) {
            char end[32], test[3], sw[8] = "", label[32];
            char *lab = l[i + 1];
            while (*lab == ' ' || *lab == '\t') lab++;
            size_t lw = strspn(lab, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
            if (sscanf(s, "BRA %31s (%2s", end, test) == 2 && lw && lab[lw] == ':') {
                char *dot = strchr(s, '(');
                if (dot && dot[3] == '.') snprintf(sw, sizeof sw, "%.*s", (int)strcspn(dot + 3, ")"), dot + 3);
                char *semi = strchr(end, ';');
                if (semi) *semi = 0;
                snprintf(label, sizeof label, "%.*s", (int)lw, lab);
                // the branch back: BRA label (keep.sw); then the end label
                int back = -1;
                char keep[3] = "";
                for (int k = i + 2; k < n && back < 0; k++) {
                    char *b = l[k];
                    while (*b == ' ' || *b == '\t') b++;
                    char tgt[32], t2[3];
                    if (!strncmp(b, "BRA", 3)) {
                        if (sscanf(b, "BRA %31[^; (] (%2s", tgt, t2) == 2 && !strcmp(tgt, label)) {
                            back = k;
                            snprintf(keep, sizeof keep, "%s", t2);
                        } else {
                            ok = 0;
                            break;
                        }
                    }
                }
                char *e = back >= 0 && back + 1 < n ? l[back + 1] : NULL;
                while (e && (*e == ' ' || *e == '\t')) e++;
                const char *skip_neg = NULL;
                for (int k = 0; k < 6; k++)
                    if (!strcmp(test, neg[k][0])) skip_neg = neg[k][1];
                // the guard skips exactly when the loop would not go on, and the
                // test is on a set instruction's 0 or 1 (EQ: 0, NE: 1)
                if (!ok || back < 0 || !e || strncmp(e, end, strlen(end)) || e[strlen(end)] != ':' || !skip_neg ||
                    strcmp(skip_neg, keep) || (strcmp(keep, "NE") && strcmp(keep, "EQ")) || !*sw || !last_set) {
                    ok = 0;
                    break;
                }
                // lbRun.x: 1 while the loop runs; each write lands only then
                char cc[16], run[64];
                snprintf(cc, sizeof cc, "lbTest%s", sw);
                if (!strcmp(keep, "NE")) snprintf(run, sizeof run, "MOV lbRun.x, %s;\n", cc);
                else snprintf(run, sizeof run, "ADD lbRun.x, lbZeroOne.y, -%s;\n", cc);
                puts_(&o, run);
                for (int r = 0; r < LOOP_UNROLL && ok; r++) {
                    for (int k = i + 2; k < back; k++) {
                        char *b = l[k];
                        while (*b == ' ' || *b == '\t') b++;
                        size_t ow = strspn(b, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
                        char *comma = strchr(b, ',');
                        char *dst = b + ow + strspn(b + ow, " \t");
                        if (!ow || b[ow] == ':') continue;   // a label
                        int ccu = ow > 1 && b[ow - 1] == 'C' && (!strncmp(dst, "HC", 2) || !strncmp(dst, "RC", 2));
                        if (!strncmp(b, "ARL", 3) || !comma || ccu) {   // ARL, condition updates: always
                            puts_(&o, b);
                            puts_(&o, "\n");
                            if (ccu) set_copy(&o, b, &last_set);
                            continue;
                        }
                        // the destination: register, mask, its own condition
                        char reg[32] = "", mask[8] = "", cond[16] = "";
                        size_t dl = (size_t)(comma - dst);
                        char d[64];
                        snprintf(d, sizeof d, "%.*s", (int)(dl < 63 ? dl : 63), dst);
                        char *cp = strchr(d, '(');
                        if (cp) {
                            snprintf(cond, sizeof cond, "%.*s", (int)strcspn(cp, ")") + 1, cp);
                            *cp = 0;
                        }
                        for (char *t = d + strlen(d); t > d && (t[-1] == ' ' || t[-1] == '\t'); ) *--t = 0;
                        char *md = strchr(d, '.');
                        if (md) {
                            snprintf(mask, sizeof mask, "%s", md);
                            *md = 0;
                        }
                        snprintf(reg, sizeof reg, "%s", d);
                        if (!reg[0] || strchr(reg, '[') || !strncmp(reg, "result", 6) || !strncmp(reg, "o", 1)) { ok = 0; break; }
                        char line[512];
                        // dst + run * (new - dst): a vertex program has no LRP
                        snprintf(line, sizeof line, "%.*s lbLw%s%s\nADD lbLw%s, lbLw, -%s;\nMAD %s%s%s, lbLw, lbRun.x, %s;\n",
                                 (int)ow, b, mask, comma, mask, reg, reg, mask, cond, reg);
                        puts_(&o, line);
                    }
                    // the test at the end of the pass keeps it going, or not
                    if (!last_set) { ok = 0; break; }
                    char upd[96];
                    if (!strcmp(keep, "NE")) snprintf(upd, sizeof upd, "MUL lbRun.x, lbRun.x, %s;\n", cc);
                    else snprintf(upd, sizeof upd, "MAD lbRun.x, lbRun.x, -%s, lbRun.x;\n", cc);
                    puts_(&o, upd);
                }
                loops_run = 1;
                i = back + 1;   // past the end label
                continue;
            }
        }
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
        set_copy(&o, s, &last_set);
    }
    for (int i = 0; i < n; i++) free(l[i]);
    free(l);
    if (!ok) { free(o.p); return src; }
    free(src);
    if (loops_run || copies) {   // the running mask's temporaries, after the header and its options
        char *p = o.p;
        while (*p == '!' || *p == '#' || !strncmp(p, "OPTION", 6) || *p == '\n') {
            char *e = strchr(p, '\n');
            if (!e) break;
            p = e + 1;
        }
        size_t at = (size_t)(p - o.p);
        const char *decl = "TEMP lbRun, lbLw, lbTest;\n";
        char *r = malloc(strlen(o.p) + strlen(decl) + 1);
        memcpy(r, o.p, at);
        strcpy(r + at, decl);
        strcat(r, o.p + at);
        free(o.p);
        return r;
    }
    return o.p;
}

// NRM (NV_fragment_program2, 2 Spicy's per-pixel lighting): a normalize, as
// DP3, RSQ and MUL through lbNrm
static char *lower_nrm(char *src)
{
    if (!strstr(src, "NRM")) return src;
    struct out o = {0};
    puts_(&o, "");
    for (char *p = src; *p;) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *s = p;
        while (*s == ' ' || *s == '\t') s++;
        char op[16], dst[64], arg[128];
        if (!strncmp(s, "NRM", 3) && sscanf(s, "%15s %63[^,], %127[^;]", op, dst, arg) == 3) {
            char line[512];
            snprintf(line, sizeof line, "DP3 lbNrm.w, %s, %s;\nRSQ lbNrm.w, lbNrm.w;\nMUL%s %s, %s, lbNrm.w;\n", arg, arg,
                     strstr(op, "_SAT") ? "_SAT" : "", dst, arg);
            puts_(&o, line);
        } else {
            put(&o, p, len);
            puts_(&o, "\n");
        }
        p += len + (e ? 1 : 0);
    }
    free(src);
    return o.p;
}

// The loops NV_vertex_program2 writes by hand (Ghost Squad Evolution's
// lights), a counted do-while around a subroutine call:
//   MOVC n.x, count; BRA end (LE.x);
//   start: <body, CAL sub (GT.x)> SUBC n.x, n.x, 1; BRA start (GT.x);
//   end: ... BRA final; sub: <...> RET; ... final:
// Each call inside such a loop is the subroutine's body (its condition is
// the loop's own: nothing in between sets the condition code), and the loop
// is unrolled LOOP_UNROLL times, each pass's writes under the loop's test
// and the count's update after it, so the passes past the count change
// nothing. Apple's GL, on Metal, cannot run the branches (it drew the scene
// unlit, in software); gl4es does not take them either.
static int is_label(const char *s, char *name, size_t n)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t w = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789");
    if (!w || s[w] != ':' || w >= n) return 0;
    snprintf(name, n, "%.*s", (int)w, s);
    return 1;
}
static const char *op_of(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int sets_cc(const char *s)
{
    s = op_of(s);
    size_t w = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_");
    return w > 1 && (s[w - 1] == 'C' || (w > 5 && !strncmp(s + w - 5, "C_SAT", 5))) && strncmp(s, "RCC", 3) && strncmp(s, "TXC", 3);
}
// "BRA target (TEST.sw);": the target and the condition ("" for none)
static int bra(const char *s, char *target, char *cond)
{
    s = op_of(s);
    if (strncmp(s, "BRA", 3) && strncmp(s, "CAL", 3)) return 0;
    cond[0] = 0;
    if (sscanf(s + 3, " %63[A-Za-z0-9_] ( %15[A-Z.xyzw] )", target, cond) < 1) return 0;
    return 1;
}
// The loops inline_calls found in the last program, in order: where each
// count comes from, program.env[arb_loop_env[k]] component arb_loop_comp[k]
// (-1: not an environment parameter). arb_lower_counts unrolls each loop for
// a given count instead of LOOP_UNROLL (hle_gfx.c: Ghost Squad Evolution's
// lights, for the counts of the moment; eight passes of three kinds made
// programs Apple's GL ran in software, a third of the speed).
int arb_loop_env[8], arb_loop_comp[8], arb_nloops;
static const int *want_counts;
static int want_n;

// the count's source: "nlights.x" after "PARAM nlights = program.env[160];"
static void loop_source(char **l, int n, const char *cc_line, int k)
{
    arb_loop_env[k] = arb_loop_comp[k] = -1;
    const char *comma = strchr(cc_line, ',');
    if (!comma) return;
    char arg[64] = "";
    sscanf(comma + 1, " %63[^;]", arg);
    char *dot = strrchr(arg, '.');
    if (!dot || strlen(dot) != 2 || !strchr("xyzw", dot[1])) return;
    int comp = dot[1] == 'x' ? 0 : dot[1] == 'y' ? 1 : dot[1] == 'z' ? 2 : 3;
    *dot = 0;
    int env = -1;
    if (sscanf(arg, "program.env[%d]", &env) != 1)
        for (int i = 0; i < n && env < 0; i++) {
            char name[64];
            const char *d = op_of(l[i]);
            if (strncmp(d, "PARAM", 5) || sscanf(d + 5, " %63[A-Za-z0-9_]", name) != 1 || strcmp(name, arg)) continue;
            const char *e = strstr(d, "program.env[");
            const char *close = e ? strchr(e, ']') : NULL;
            if (close && close[1] != '.') sscanf(e, "program.env[%d]", &env);
        }
    if (env >= 0) arb_loop_env[k] = env, arb_loop_comp[k] = comp;
}

static char *inline_calls(char *src)
{
    arb_nloops = 0;
    if (!strstr(src, "CAL") || !strstr(src, "RET")) return src;
    char **l = NULL;
    int n = 0, cap = 0;
    for (char *p = src; *p;) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (n == cap) l = realloc(l, sizeof(char *) * (size_t)(cap = cap ? cap * 2 : 256));
        l[n] = strndup(p, len);
        char *hash = strchr(l[n], '#');   // comments out: they may hold ";" or ":"
        if (hash) *hash = 0;
        char *cr = strchr(l[n], '\r');    // the games' shader text has CRLF lines
        if (cr) *cr = 0;
        n++;
        p += len + (e ? 1 : 0);
    }
    char *drop = calloc((size_t)n, 1), name[64], t[64], c[16];
    // the subroutines: a label that a CAL names, up to RET
    struct { char name[64]; int from, to; } sub[16];
    int nsub = 0, ok = 1;
    for (int i = 0; i < n && ok; i++) {
        if (!is_label(l[i], name, sizeof name)) continue;
        int called = 0;
        for (int k = 0; k < n && !called; k++) called = bra(l[k], t, c) && !strncmp(op_of(l[k]), "CAL", 3) && !strcmp(t, name);
        if (!called) continue;
        int r = i + 1;
        while (r < n && strncmp(op_of(l[r]), "RET", 3)) r++;
        if (r == n || nsub == 16) { ok = 0; break; }
        snprintf(sub[nsub].name, sizeof sub[nsub].name, "%s", name);
        sub[nsub].from = i + 1, sub[nsub].to = r;
        for (int k = i; k <= r; k++) drop[k] = 1;
        nsub++;
    }
    struct out o = {0};
    puts_(&o, "");
    for (int i = 0; i < n && ok; i++) {
        if (drop[i]) continue;
        // BRA past the subroutines to the label that now follows it
        if (bra(l[i], t, c) && !c[0] && !strncmp(op_of(l[i]), "BRA", 3)) {
            int k = i + 1;
            while (k < n && (drop[k] || !*op_of(l[k]))) k++;
            if (k < n && is_label(l[k], name, sizeof name) && !strcmp(name, t)) continue;
            ok = 0;
            break;
        }
        // the do-while: a CC update (then instructions that keep it), BRA end
        // (test), start:, body, CC update, BRA start (test)
        char end_name[64], c1[16];
        int b = i + 1;
        if (sets_cc(l[i]))
            while (b < n && (!*op_of(l[b]) || (!sets_cc(l[b]) && !bra(l[b], t, c) && !is_label(l[b], name, sizeof name) &&
                                                strncmp(op_of(l[b]), "RET", 3))))
                b++;
        int s0 = b + 1;
        while (s0 < n && !*op_of(l[s0])) s0++;
        if (sets_cc(l[i]) && b < n && s0 < n && bra(l[b], end_name, c1) && c1[0] && !strncmp(op_of(l[b]), "BRA", 3) &&
            is_label(l[s0], name, sizeof name)) {
            for (int k = i; k < b; k++) {   // the count, and what comes before the test
                puts_(&o, l[k]);
                puts_(&o, "\n");
            }
            int j = s0 + 1, m = -1;
            for (; j < n; j++) {
                if (bra(l[j], t, c) && !strncmp(op_of(l[j]), "BRA", 3)) {
                    if (!strcmp(t, name) && j > i + 3 && sets_cc(l[j - 1])) m = j - 1;
                    break;
                }
                if (sets_cc(l[j]) && !(j + 1 < n && bra(l[j + 1], t, c) && !strcmp(t, name))) break;
            }
            // the CC update that closes the loop is the last thing before its BRA
            char test[16];
            if (m < 0 || !bra(l[m + 1], t, test) || !test[0]) { ok = 0; break; }
            int passes = LOOP_UNROLL;
            if (arb_nloops < 8) {
                if (want_counts && arb_nloops < want_n) passes = want_counts[arb_nloops];
                loop_source(l, n, l[i], arb_nloops++);
            }
            for (int r = 0; r < passes; r++) {
                for (int k = s0 + 1; k < m; k++) {
                    const char *b = op_of(l[k]);
                    if (!*b) continue;
                    const char *body[64];
                    int nb = 0;
                    if (!strncmp(b, "CAL", 3)) {   // the subroutine, inline
                        bra(b, t, c);
                        int f = -1;
                        for (int q = 0; q < nsub; q++) if (!strcmp(sub[q].name, t)) f = q;
                        if (f < 0 || strcmp(c, test)) { ok = 0; break; }
                        for (int q = sub[f].from; q < sub[f].to && nb < 64; q++)
                            if (*op_of(l[q])) body[nb++] = op_of(l[q]);
                    } else body[nb++] = b;
                    for (int q = 0; q < nb; q++) {
                        const char *x = body[q], *comma = strchr(x, ',');
                        if (!strncmp(x, "ARL", 3) || !comma) {   // addresses: unconditional
                            puts_(&o, x);
                            puts_(&o, "\n");
                            continue;
                        }
                        if (memchr(x, '(', (size_t)(comma - x))) { ok = 0; break; }
                        put(&o, x, (size_t)(comma - x));
                        char cnd[24];
                        snprintf(cnd, sizeof cnd, " (%s)", test);
                        puts_(&o, cnd);
                        puts_(&o, comma);
                        puts_(&o, "\n");
                    }
                    if (!ok) break;
                }
                puts_(&o, op_of(l[m]));   // the count's update, every pass
                puts_(&o, "\n");
            }
            // past the loop: the end label is dropped as a label
            i = m + 1;
            continue;
        }
        if (!strncmp(op_of(l[i]), "CAL", 3) || !strncmp(op_of(l[i]), "BRA", 3)) { ok = 0; break; }
        if (is_label(l[i], name, sizeof name)) continue;   // nothing branches any more
        puts_(&o, l[i]);
        puts_(&o, "\n");
    }
    for (int i = 0; i < n; i++) free(l[i]);
    free(l);
    free(drop);
    if (!ok) {
        free(o.p);
        return src;
    }
    free(src);
    return o.p;
}

char *arb_lower_nv(char *src);
char *arb_lower_counts(char *src, const int *counts, int n)
{
    want_counts = counts, want_n = n;
    src = arb_lower_nv(src);
    want_counts = NULL, want_n = 0;
    return src;
}

// A vertex program's writes to the back colours (result.color.back.*, and
// the OUTPUT names bound to them) taken out: used only with two-sided
// lighting (GL_VERTEX_PROGRAM_TWO_SIDE), and one output too many crashed
// Apple's Metal shader compiler (MTLCompilerService, in LLVM's register
// allocation) on Ghost Squad Evolution's lights: the game froze waiting for
// it. hle_gfx.c calls it on Apple's GL while the game hasn't turned two-sided
// lighting on.
char *arb_drop_back_colors(char *src)
{
    if (strncmp(src, "!!ARBvp", 7) || !strstr(src, "result.color.back")) return src;
    char names[8][64];
    int nn = 0;
    for (const char *p = src; (p = strstr(p, "OUTPUT")) && nn < 8; p += 6) {
        char name[64], what[96];
        if (sscanf(p + 6, " %63[A-Za-z0-9_] = %95[A-Za-z0-9_.]", name, what) == 2 && !strncmp(what, "result.color.back", 17))
            snprintf(names[nn++], sizeof names[0], "%s", name);
    }
    struct out o = {0};
    puts_(&o, "");
    for (const char *line = src; *line;) {
        const char *e = strchr(line, '\n');
        size_t len = e ? (size_t)(e - line + 1) : strlen(line);
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        int drop = 0;
        if (starts_word(s, "OUTPUT")) {
            drop = strstr(s, "result.color.back") && (size_t)(strstr(s, "result.color.back") - line) < len;
        } else if (*s != '#' && *s != '!') {
            // an instruction: its destination is the first operand
            const char *d = s + strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
            while (*d == ' ' || *d == '\t') d++;
            if (d > s && (size_t)(d - line) < len) {
                if (!strncmp(d, "result.color.back", 17)) drop = 1;
                for (int k = 0; k < nn && !drop; k++) {
                    size_t l = strlen(names[k]);
                    drop = !strncmp(d, names[k], l) && !isalnum((unsigned char)d[l]) && d[l] != '_';
                }
            }
        }
        if (!drop) put(&o, line, len);
        line += len;
    }
    free(src);
    return o.p;
}

char *arb_lower_nv(char *src)
{
    if (!strstr(src, "OPTION NV_")) return src;
    src = lower_nrm(unroll_loops(inline_calls(src)));
    int vertex = !strncmp(src, "!!ARBvp", 7);

    // outputs written under a condition (Let's Go Jungle's
    // "MUL result.texcoord[0].xyz(GT.x), ..."): the old value is needed and
    // an output cannot be read, so they are written in a temporary each
    // (lbOut0...) and copied out at the end
    char outs[8][64];
    int nouts = 0;
    for (const char *l = src; *l && nouts < 8;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        const char *r = l;
        while (*r == ' ' || *r == '\t') r++;
        r += strspn(r, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789");
        while (*r == ' ' || *r == '\t') r++;
        size_t dl = strcspn(r, ",;\n");
        if (r < l + len && !strncmp(r, "result.", 7) && memchr(r, '(', dl)) {
            char tok[64];
            snprintf(tok, sizeof tok, "%.*s", (int)strcspn(r, "("), r);
            char *dot = strrchr(tok, '.');
            if (dot && dot[1] && strspn(dot + 1, "xyzw") == strlen(dot + 1) && !strchr(dot, ']')) *dot = 0;
            int seen = 0;
            for (int k = 0; k < nouts; k++) seen |= !strcmp(outs[k], tok);
            if (!seen) snprintf(outs[nouts++], sizeof outs[0], "%s", tok);
        }
        l += len + (e ? 1 : 0);
    }

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
            puts_(&o, "TEMP lbSat, lbAbs0, lbAbs1, lbAbs2, lbHC, lbCnd, lbMask, lbNrm;\n"
                      "PARAM lbZeroOne = { 0, 1, 0, 0 };\n");
#ifndef LB_IOS
            if (vertex && (strstr(src, "SIN") || strstr(src, "COS")))
                puts_(&o, "TEMP lbSC;\n"
                          "PARAM lbSinA = { 1, -0.16666667, 0.0083333333, -0.00019841270 };\n"
                          "PARAM lbSinB = { 2.7557319e-6, -2.5052108e-8, 1.6059044e-10, -3.14159265 };\n"
                          "PARAM lbSinC = { 0.15915494, 0.5, 0.75, 6.2831853 };\n");
#endif
            for (int k = 0; k < nouts; k++) {
                char l[64];
                snprintf(l, sizeof l, "TEMP lbOut%d;\n", k);
                puts_(&o, l);
            }
            declared = 1;
        }
        if (nouts && starts_word(s, "END")) {
            for (int k = 0; k < nouts; k++) {
                char l[160];
                snprintf(l, sizeof l, "MOV %s, lbOut%d;\n", outs[k], k);
                puts_(&o, l);
            }
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
        // KIL on a condition (NV_fragment_program2: KIL LE.x, Harley-Davidson's
        // alpha tests): 1 where the test holds, and ARB's KIL of its negation
        // (it discards where a component is below 0)
        if (!strcmp(base, "KIL") && na == 1 && strlen(a[0]) >= 2 && isupper((unsigned char)a[0][0]) &&
            isupper((unsigned char)a[0][1]) && (!a[0][2] || a[0][2] == '.')) {
            static const struct { const char *test, *mask; } kil_tests[] = {
                {"GT", "SLT lbMask, lbZeroOne.x, lbHC;\n"},
                {"LT", "SLT lbMask, lbHC, lbZeroOne.x;\n"},
                {"GE", "SGE lbMask, lbHC, lbZeroOne.x;\n"},
                {"LE", "SGE lbMask, lbZeroOne.x, lbHC;\n"},
                {"NE", "ABS lbMask, lbHC;\nSLT lbMask, lbZeroOne.x, lbMask;\n"},
                {"EQ", "ABS lbMask, lbHC;\nSGE lbMask, lbZeroOne.x, lbMask;\n"},
                {"TR", "MOV lbMask, lbZeroOne.y;\n"},
            };
            const char *mask = NULL;
            for (int k = 0; k < 7; k++)
                if (!strncmp(a[0], kil_tests[k].test, 2)) mask = kil_tests[k].mask;
            if (!strncmp(a[0], "FL", 2)) continue;   // never
            if (!mask) goto give_up;
            char l[600];
            snprintf(l, sizeof l, "%sKIL -lbMask%s;\n", mask, a[0] + 2);
            puts_(&o, l);
            continue;
        }
        for (int k = 0; k < nouts; k++) {
            size_t tl = strlen(outs[k]);
            const char *rest = a[0] + tl;   // nothing, a write mask, a condition
            if (!strncmp(a[0], outs[k], tl) &&
                (!*rest || *rest == '(' || (*rest == '.' && strchr("(", rest[1 + strspn(rest + 1, "xyzw")]) &&
                                            strspn(rest + 1, "xyzw")))) {
                char t[520];
                snprintf(t, sizeof t, "lbOut%d%s", k, rest);
                snprintf(a[0], sizeof a[0], "%s", t);
            }
        }
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
#ifndef LB_IOS
        // SIN and COS, which vp40 has in vertex programs and ARB doesn't
        // (Mesa: Let's Go Jungle's frogs): the angle brought into -pi..pi,
        // then sin's series to x^13 (gl4es takes them as they are)
        if (vertex && (!strcmp(base, "SIN") || !strcmp(base, "COS")) && na == 2) {
            char *br = strrchr(a[1], ']');
            if (!strchr(br ? br : a[1], '.')) strcat(a[1], ".x");
            char l[1600];
            snprintf(l, sizeof l,
                     "MAD lbSC.x, %s, lbSinC.x, lbSinC.%c;\n"
                     "FRC lbSC.x, lbSC.x;\n"
                     "MAD lbSC.x, lbSC.x, lbSinC.w, lbSinB.w;\n"
                     "MUL lbSC.y, lbSC.x, lbSC.x;\n"
                     "MAD lbSC.z, lbSC.y, lbSinB.z, lbSinB.y;\n"
                     "MAD lbSC.z, lbSC.z, lbSC.y, lbSinB.x;\n"
                     "MAD lbSC.z, lbSC.z, lbSC.y, lbSinA.w;\n"
                     "MAD lbSC.z, lbSC.z, lbSC.y, lbSinA.z;\n"
                     "MAD lbSC.z, lbSC.z, lbSC.y, lbSinA.y;\n"
                     "MAD lbSC.z, lbSC.z, lbSC.y, lbSinA.x;\n"
                     "MUL%s %s, lbSC.z, lbSC.x;\n",
                     a[1], base[0] == 'C' ? 'z' : 'y', sat && !clamp ? "_SAT" : "", a[0]);
            puts_(&o, l);
        } else {
#endif
        puts_(&o, base);
        if (sat && !clamp) puts_(&o, "_SAT");
        for (int k = 0; k < na; k++) {
            puts_(&o, k ? ", " : " ");
            puts_(&o, a[k]);
        }
        puts_(&o, ";\n");
#ifndef LB_IOS
        }
#endif
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

// Relative addressing (c[A0.x], c[A0.x+8], c[A0.x +1]): gl4es's ARB parser
// takes only c[A0.x + n], with the sign spaced out, and rejects the program
// otherwise ("Invalid state"; Let's Go Jungle's and Sega Race TV's skinning,
// Ghost Squad Evolution's lights through its own address registers). Every
// [<register>.x ...] is written that way, + 0 where it has no offset.
char *arb_address_offsets(char *src)
{
    if (!strstr(src, ".x")) return src;
    struct out o = {0};
    puts_(&o, "");
    int changed = 0;
    for (char *p = src; *p;) {
        if (*p == '[') {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            const char *r = q;
            while (*r == '_' || (*r >= '0' && *r <= '9') || (*r >= 'A' && *r <= 'Z') || (*r >= 'a' && *r <= 'z')) r++;
            if (r > q && !(*q >= '0' && *q <= '9') && r[0] == '.' && r[1] == 'x') {
                const char *t = r + 2;
                while (*t == ' ' || *t == '\t') t++;
                char sign = '+';
                if (*t == '+' || *t == '-') sign = *t++;
                while (*t == ' ' || *t == '\t') t++;
                const char *d = t;
                while (*t >= '0' && *t <= '9') t++;
                const char *e = t;
                while (*e == ' ' || *e == '\t') e++;
                if (*e == ']' && (d < t || sign == '+') && (d < t || t == r + 2 + strspn(r + 2, " \t"))) {
                    char line[160];
                    snprintf(line, sizeof line, "[%.*s.x %c %.*s]", (int)(r - q), q, sign, d < t ? (int)(t - d) : 1, d < t ? d : "0");
                    if (strncmp(p, line, strlen(line)) || p + strlen(line) != e + 1) changed = 1;
                    puts_(&o, line);
                    p = (char *)e + 1;
                    continue;
                }
            }
        }
        put(&o, p, 1);
        p++;
    }
    if (!changed) { free(o.p); return src; }
    free(src);
    return o.p;
}

// Shadow lookups (OutRun 2 SP's car and scenery, #78). NVIDIA compares a
// 2D lookup's r with the depth texture's value when the texture's compare
// mode is on (ARB_shadow), whatever the program says; gl4es writes every 2D
// lookup as texture2D(sampler, coord.xy), and OpenGL ES 3 compares only
// through a shadow sampler. Each 2D TEX and TXP on units 0-3 of a fragment
// program also makes the comparison (the stored depth at least the
// reference: GL_LEQUAL) and keeps it where program environment parameter
// 18 + unit has x 1, which the runtime sets while a compare-mode texture is
// bound there (hle_gfx.c); the texture itself is sampled with no compare.
// Outside [0, 1] on an axis the texture clamps to its border (z for s, w for
// t: OutRun 2 SP's shadow map, CLAMP_TO_BORDER, which gl4es makes
// CLAMP_TO_EDGE) the comparison is with the border depth (y) instead.
#define SHADOW_ENV 18
char *arb_shadow_lookups(char *src)
{
    if (strncmp(src, "!!ARBfp", 7)) return src;
    for (int u = 0; u < 4; u++) {
        char env[32];
        snprintf(env, sizeof env, "program.env[%d]", SHADOW_ENV + u);
        if (strstr(src, env)) return src;   // the program's own: left alone
    }
    struct out o = {0};
    puts_(&o, "");
    int any = 0;
    const char *decl_at;   // where TEMPs may go: after the header and its OPTIONs
    const char *p = strchr(src, '\n');
    decl_at = p ? p + 1 : src + strlen(src);
    for (const char *q = decl_at; (q = strstr(q, "OPTION")); q += 6) {
        const char *e = strchr(q, ';');
        if (e) decl_at = e + 1;
    }
    put(&o, src, (size_t)(decl_at - src));
    puts_(&o, "\nTEMP _shT, _shS, _shC, _shB, _shO;\nPARAM _shK = {0, 1, 0, 0};\n");
    for (const char *s = decl_at; *s;) {
        const char *e = strchr(s, ';');
        if (!e) { puts_(&o, s); break; }
        const char *t = s;
        while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
        int txp = starts_word(t, "TXP") || starts_word(t, "TXP_SAT");
        int tex = starts_word(t, "TEX") || starts_word(t, "TEX_SAT");
        char a[4][512];
        int unit = -1;
        if ((tex || txp) && split_args(t + (strncmp(t + 3, "_SAT", 4) ? 3 : 7), a) == 4 && !strcmp(a[3], "2D")
            && sscanf(a[2], "texture[%d]", &unit) == 1 && unit >= 0 && unit < 4) {
            int sat = !strncmp(t + 3, "_SAT", 4);
            char line[4096], env[32];
            snprintf(env, sizeof env, "program.env[%d]", SHADOW_ENV + unit);
            put(&o, s, (size_t)(t - s));
            // the coordinate, divided for TXP
            snprintf(line, sizeof line, "MOV _shS, %s;\n%s", a[1],
                     txp ? "RCP _shB.w, _shS.w;\nMUL _shS, _shS, _shB.wwww;\n" : "");
            puts_(&o, line);
            snprintf(line, sizeof line,
                     "TEX _shT, _shS, %s, 2D;\n"
                     "SGE _shC, _shT.xxxx, _shS.zzzz;\n"          // the texel's comparison
                     "SGE _shB, %s.yyyy, _shS.zzzz;\n"            // the border's
                     "SLT _shO.x, _shS.x, _shK.x;\nSLT _shO.y, _shK.y, _shS.x;\n"
                     "SLT _shO.z, _shS.y, _shK.x;\nSLT _shO.w, _shK.y, _shS.y;\n"
                     "MAX _shO.xz, _shO.xxzz, _shO.yyww;\n"       // outside on s, on t
                     "MUL _shO.xz, _shO, %s.zzww;\n"              // and that axis clamps to the border
                     "MAX _shO.x, _shO.x, _shO.z;\n"
                     "LRP _shC, _shO.xxxx, _shB, _shC;\n"
                     "LRP _shT, %s.xxxx, _shC, _shT;\n"
                     "MOV%s %s, _shT",
                     a[2], env, env, env, sat ? "_SAT" : "", a[0]);
            puts_(&o, line);
            any = 1;
        } else
            put(&o, s, (size_t)(e - s));
        puts_(&o, ";");
        s = e + 1;
    }
    if (!any) {
        free(o.p);
        return src;
    }
    free(src);
    return o.p;
}

// state.matrix.program[n] (Ghost Squad Evolution's vertex programs declare
// three, with their inverse transposes), which gl4es does not bind and
// rejects the program for: a PARAM of one that the program never reads is
// left out. One that is read stays, and the program fails as before.
char *arb_program_matrices(char *src)
{
    if (strncmp(src, "!!ARBvp", 7) || !strstr(src, "state.matrix.program")) return src;
    struct out o = {0};
    puts_(&o, "");
    int dropped = 0;
    for (const char *s = src; *s;) {
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s + 1) : strlen(s);
        const char *t = s;
        while (*t == ' ' || *t == '\t') t++;
        char name[64];
        int unused = 0;
        if (starts_word(t, "PARAM") && sscanf(t + 5, " %63[A-Za-z0-9_]", name) == 1) {
            const char *semi = strchr(t, ';');
            const char *m = strstr(t, "state.matrix.program");
            if (semi && m && m < semi) {
                unused = 1;   // read anywhere but its own line?
                size_t nl = strlen(name);
                for (const char *q = src; (q = strstr(q, name)); q += nl) {
                    int word = (q == src || !(q[-1] == '_' || (q[-1] >= '0' && q[-1] <= '9') || (q[-1] >= 'A' && q[-1] <= 'Z') || (q[-1] >= 'a' && q[-1] <= 'z')))
                               && !(q[nl] == '_' || (q[nl] >= '0' && q[nl] <= '9') || (q[nl] >= 'A' && q[nl] <= 'Z') || (q[nl] >= 'a' && q[nl] <= 'z'));
                    if (word && (q < s || q >= s + len)) { unused = 0; break; }
                }
            }
        }
        if (unused) dropped = 1;
        else put(&o, s, len);
        s += len;
    }
    if (!dropped) { free(o.p); return src; }
    free(src);
    return o.p;
}

// Rectangle textures in fragment programs (TEX, TXP, TXB ..., RECT: Ghost
// Squad Evolution's 2D and post effects), which gl4es rejects ("Invalid
// texture sampler target"): gl4es keeps a rectangle texture as a 2D one,
// sampled with coordinates 0..1, so the lookup becomes a 2D one with its
// s and t scaled by program environment parameter 24 + unit, which the
// runtime keeps at 1 / the size of the rectangle texture bound on units 0-7
// (hle_gfx.c, rect_env_update). Scaling before TXP's divide is the same
// as after it.
#define RECT_ENV 24
char *arb_rect_lookups(char *src)
{
    if (strncmp(src, "!!ARBfp", 7) || !strstr(src, "RECT")) return src;
    struct out o = {0};
    puts_(&o, "");
    int any = 0;
    const char *decl_at;   // where TEMPs may go: after the header and its OPTIONs
    const char *p = strchr(src, '\n');
    decl_at = p ? p + 1 : src + strlen(src);
    for (const char *q = decl_at; (q = strstr(q, "OPTION")); q += 6) {
        const char *e = strchr(q, ';');
        if (e) decl_at = e + 1;
    }
    put(&o, src, (size_t)(decl_at - src));
    puts_(&o, "\nTEMP _rcS;\n");
    for (const char *s = decl_at; *s;) {
        const char *e = strchr(s, ';');
        if (!e) { puts_(&o, s); break; }
        const char *t = s;
        while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
        const char *op = NULL;
        static const char *const ops[] = {"TEX", "TXP", "TXB", "TEX_SAT", "TXP_SAT", "TXB_SAT"};
        for (int k = 0; k < 6; k++)
            if (starts_word(t, ops[k])) op = ops[k];
        char a[4][512];
        int unit = -1;
        if (op && split_args(t + strlen(op), a) == 4 && !strcmp(a[3], "RECT")) {
            char tex[512], *d = tex;
            for (const char *q = a[2]; *q; q++)
                if (*q != ' ' && *q != '\t') *d++ = *q;
            *d = 0;
            if (sscanf(tex, "texture[%d]", &unit) != 1 || unit < 0 || unit > 7) unit = -1;
        }
        if (unit >= 0) {
            char line[2048];
            put(&o, s, (size_t)(t - s));
            snprintf(line, sizeof line, "MOV _rcS, %s;\nMUL _rcS.xy, _rcS, program.env[%d];\n%s %s, _rcS, %s, 2D",
                     a[1], RECT_ENV + unit, op, a[0], a[2]);
            puts_(&o, line);
            any = 1;
        } else
            put(&o, s, (size_t)(e - s));
        puts_(&o, ";");
        s = e + 1;
    }
    if (!any) {
        free(o.p);
        return src;
    }
    free(src);
    return o.p;
}

// OUTPUT aliases (OUTPUT oTxc0 = result.texcoord[0]: every one of Ghost
// Squad Evolution's programs), which gl4es handles badly: an indexed result
// is refused ("Invalid token" at the '['), result.pointsize copied to an
// expression (vec4(gl_Point.size, ...) = oPtsize) and result.depth through a
// misspelt name (gl4es_FragDe), so the shader does not compile. The
// declaration is dropped and the alias written as its binding wherever the
// program uses it.
static int ident_char(char ch) { return ch == '_' || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); }
char *arb_output_aliases(char *src)
{
    if (!strstr(src, "OUTPUT")) return src;
    char names[32][64], binds[32][96];
    int n = 0;
    struct out o = {0};
    puts_(&o, "");
    for (const char *s = src; *s;) {   // the declarations, kept out
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s + 1) : strlen(s);
        const char *t = s;
        while (*t == ' ' || *t == '\t') t++;
        char name[64], bind[96];
        if (n < 32 && starts_word(t, "OUTPUT") && sscanf(t + 6, " %63[A-Za-z0-9_] = %95[^; \t]", name, bind) == 2) {
            strcpy(names[n], name), strcpy(binds[n++], bind);
            const char *semi = strchr(t, ';');
            if (semi && semi < s + len) put(&o, semi + 1, (size_t)(s + len - semi - 1));   // a comment after it
        } else
            put(&o, s, len);
        s += len;
    }
    if (!n) { free(o.p); return src; }
    free(src);
    struct out r = {0};
    puts_(&r, "");
    for (const char *p = o.p; *p;) {   // each use of an alias
        if (*p == '#') {   // a comment runs to the line's end
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            put(&r, p, len);
            p += len;
            continue;
        }
        if (ident_char(*p) && (p == o.p || !ident_char(p[-1]))) {
            size_t w = 0;
            while (ident_char(p[w])) w++;
            int k = -1;
            for (int i = 0; i < n && k < 0; i++)
                if (strlen(names[i]) == w && !strncmp(p, names[i], w)) k = i;
            if (k >= 0) puts_(&r, binds[k]);
            else put(&r, p, w);
            p += w;
            continue;
        }
        put(&r, p, 1);
        p++;
    }
    free(o.p);
    return r.p;
}
