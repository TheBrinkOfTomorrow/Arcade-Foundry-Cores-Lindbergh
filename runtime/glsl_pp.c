// The GLSL preprocessor, run on the iOS build before gl4es sees a shader.
// gl4es rewrites desktop GLSL into GLSL ES by its text (gl_TextureMatrix[2]
// becomes a uniform of its own, and so on), so it needs plain code: indices
// and array sizes as numbers, not macros. GLSL ES is also stricter than the
// desktop (a macro that is not defined is an error in #if). The games use
// object-like macros only, which is all this handles: #define, #undef, #if,
// #ifdef, #ifndef, #elif, #else, #endif with C's integer expressions.
// #version, #extension, #pragma and #line pass through; every other
// directive line becomes empty, so line numbers stay.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS 4096

struct macro { struct macro *next; char *name, *body; };
struct pp { struct macro *tab[BUCKETS]; };

static unsigned hash(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h % BUCKETS;
}

static struct macro **find(struct pp *pp, const char *name, size_t n)
{
    struct macro **m = &pp->tab[hash(name, n)];
    for (; *m; m = &(*m)->next)
        if (strlen((*m)->name) == n && !memcmp((*m)->name, name, n)) return m;
    return m;
}

static void define(struct pp *pp, const char *name, size_t n, const char *body)
{
    struct macro **m = find(pp, name, n);
    if (*m) {
        free((*m)->body);
        (*m)->body = strdup(body);
        return;
    }
    struct macro *e = calloc(1, sizeof *e);
    e->name = strndup(name, n);
    e->body = strdup(body);
    *m = e;
}

static void undef(struct pp *pp, const char *name, size_t n)
{
    struct macro **m = find(pp, name, n), *e = *m;
    if (!e) return;
    *m = e->next;
    free(e->name);
    free(e->body);
    free(e);
}

static int ident_start(char c) { return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }

struct buf { char *p; size_t n, cap; };

static void put(struct buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) b->p = realloc(b->p, b->cap = (b->n + n + 1) * 2);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

// expand macros in s[0..n) into out; names being expanded are not expanded again
#define MAX_NEST 64
static void expand(struct pp *pp, const char *s, size_t n, struct buf *out,
                   const struct macro **active, int nactive, int in_if)
{
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c >= '0' && c <= '9') {      // a number, suffix and all
            size_t j = i;
            while (j < n && (ident_char(s[j]) || s[j] == '.')) j++;
            put(out, s + i, j - i);
            i = j;
            continue;
        }
        if (!ident_start(c)) {
            put(out, s + i, 1);
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && ident_char(s[j])) j++;
        if (in_if && j - i == 7 && !memcmp(s + i, "defined", 7)) {
            // defined NAME or defined(NAME): 1 or 0, the name not expanded
            size_t k = j;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            int paren = k < n && s[k] == '(';
            if (paren) k++;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            size_t a = k;
            while (k < n && ident_char(s[k])) k++;
            int def = *find(pp, s + a, k - a) != NULL;
            if (paren) {
                while (k < n && s[k] != ')') k++;
                if (k < n) k++;
            }
            put(out, def ? "1" : "0", 1);
            i = k;
            continue;
        }
        struct macro *m = *find(pp, s + i, j - i);
        int busy = 0;
        for (int a = 0; a < nactive; a++) busy |= active[a] == m;
        if (m && !busy && nactive < MAX_NEST) {
            active[nactive] = m;
            expand(pp, m->body, strlen(m->body), out, active, nactive + 1, in_if);
        } else {
            put(out, s + i, j - i);
        }
        i = j;
    }
}

// #if expressions: C's integer operators, on an expanded line
struct ev { const char *p; int err; };

static void skip(struct ev *e) { while (*e->p == ' ' || *e->p == '\t') e->p++; }
static int64_t ev_cond(struct ev *e);

static int64_t ev_unary(struct ev *e)
{
    skip(e);
    char c = *e->p;
    if (c == '(') {
        e->p++;
        int64_t v = ev_cond(e);
        skip(e);
        if (*e->p == ')') e->p++;
        else e->err = 1;
        return v;
    }
    if (c == '!') { e->p++; return !ev_unary(e); }
    if (c == '~') { e->p++; return ~ev_unary(e); }
    if (c == '-') { e->p++; return -ev_unary(e); }
    if (c == '+') { e->p++; return ev_unary(e); }
    if (c >= '0' && c <= '9') {
        char *end;
        int64_t v = (int64_t)strtoll(e->p, &end, 0);
        while (ident_char(*end)) end++;  // u, l suffixes
        e->p = end;
        return v;
    }
    if (ident_start(c)) {                // a name not defined: 0
        while (ident_char(*e->p)) e->p++;
        return 0;
    }
    e->err = 1;
    return 0;
}

// binary operators by precedence, lowest first
static const struct { const char *op; int prec; } ops[] = {
    {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6},
    {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8}, {"<", 7}, {">", 7},
    {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10},
};

static int peek_op(struct ev *e, int *prec)
{
    skip(e);
    for (int i = 0; i < (int)(sizeof ops / sizeof ops[0]); i++) {
        size_t l = strlen(ops[i].op);
        if (strncmp(e->p, ops[i].op, l)) continue;
        // "|" is not "||", "&" not "&&", "<" not "<<"/"<="
        if (l == 1 && (e->p[1] == e->p[0] || ((e->p[0] == '<' || e->p[0] == '>') && e->p[1] == '='))) continue;
        *prec = ops[i].prec;
        return i;
    }
    return -1;
}

static int64_t ev_bin(struct ev *e, int min)
{
    int64_t a = ev_unary(e);
    for (;;) {
        int prec, i = peek_op(e, &prec);
        if (i < 0 || prec < min) return a;
        e->p += strlen(ops[i].op);
        int64_t b = ev_bin(e, prec + 1);
        const char *o = ops[i].op;
        if (!strcmp(o, "||")) a = a || b;
        else if (!strcmp(o, "&&")) a = a && b;
        else if (!strcmp(o, "|")) a |= b;
        else if (!strcmp(o, "^")) a ^= b;
        else if (!strcmp(o, "&")) a &= b;
        else if (!strcmp(o, "==")) a = a == b;
        else if (!strcmp(o, "!=")) a = a != b;
        else if (!strcmp(o, "<=")) a = a <= b;
        else if (!strcmp(o, ">=")) a = a >= b;
        else if (!strcmp(o, "<<")) a <<= b;
        else if (!strcmp(o, ">>")) a >>= b;
        else if (!strcmp(o, "<")) a = a < b;
        else if (!strcmp(o, ">")) a = a > b;
        else if (!strcmp(o, "+")) a += b;
        else if (!strcmp(o, "-")) a -= b;
        else if (!strcmp(o, "*")) a *= b;
        else if (!strcmp(o, "/")) a = b ? a / b : 0;
        else if (!strcmp(o, "%")) a = b ? a % b : 0;
    }
}

static int64_t ev_cond(struct ev *e)
{
    int64_t c = ev_bin(e, 1);
    skip(e);
    if (*e->p != '?') return c;
    e->p++;
    int64_t a = ev_cond(e);
    skip(e);
    if (*e->p == ':') e->p++;
    else e->err = 1;
    int64_t b = ev_cond(e);
    return c ? a : b;
}

static int eval(struct pp *pp, const char *s, size_t n)
{
    struct buf x = {0};
    const struct macro *active[MAX_NEST];
    put(&x, "", 0);
    expand(pp, s, n, &x, active, 0, 1);
    struct ev e = {x.p, 0};
    int64_t v = ev_cond(&e);
    free(x.p);
    return v != 0;
}

// comments out, as the preprocessor does first (newlines in block comments kept)
static char *strip_comments(const char *s)
{
    size_t n = strlen(s);
    char *o = malloc(n + 1), *q = o;
    for (size_t i = 0; i < n; ) {
        if (s[i] == '/' && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
        } else if (s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') *q++ = '\n';
                i++;
            }
            i += 2;
            *q++ = ' ';
        } else if (s[i] == '\r') {
            i++;
        } else if (s[i] == '\\' && s[i + 1] == '\n') {   // a continued line: kept as one
            i += 2;
        } else {
            *q++ = s[i++];
        }
    }
    *q = 0;
    return o;
}

// gl_Name[ ( 0) ] as gl_Name[0]: gl4es finds the built-in arrays' indices
// (gl_TextureMatrix[N] and the like) only as plain numbers
static void fold_builtin_indices(struct buf *b)
{
    struct buf o = {0};
    put(&o, "", 0);
    const char *s = b->p;
    while (*s) {
        const char *g = strstr(s, "gl_");
        if (!g) break;
        const char *n = g + 3;
        while (ident_char(*n)) n++;
        const char *x = n;
        while (*x == ' ' || *x == '\t') x++;
        int plain = g == b->p || !ident_char(g[-1]);
        if (plain && *x == '[') {
            const char *e = x + 1;
            int depth = 0, constant = 1;
            for (; *e && (*e != ']' || depth); e++) {
                if (*e == '[') depth++;
                else if (*e == ']') depth--;
                else if (!strchr("0123456789 \t()+-*/", *e)) constant = 0;
            }
            if (*e == ']' && constant) {
                char *t = strndup(x + 1, (size_t)(e - x - 1));
                struct ev ev = {t, 0};
                int64_t v = ev_cond(&ev);
                skip(&ev);
                if (!ev.err && !*ev.p) {
                    char num[32];
                    put(&o, s, (size_t)(n - s));
                    snprintf(num, sizeof num, "[%lld]", (long long)v);
                    put(&o, num, strlen(num));
                    s = e + 1;
                    free(t);
                    continue;
                }
                free(t);
            }
        }
        put(&o, s, (size_t)(n - s));
        s = n;
    }
    put(&o, s, strlen(s));
    free(b->p);
    *b = o;
}

// Rectangle textures, which OpenGL ES does not have: gl4es keeps one as a 2D
// texture, sampled with coordinates 0..1. Each sampler2DRect becomes a
// sampler2D with a uniform vec2 lbRectScale_<name>, 1 / the texture's size,
// which the runtime sets (hle_gfx.c, rect_update), and texture2DRect(s, c)
// becomes texture2D(s, (c) * lbRectScale_s).
static void fix_rect_textures(struct buf *b)
{
    if (!strstr(b->p, "2DRect")) return;
    struct buf o = {0};
    put(&o, "", 0);
    const char *s = b->p;
    while (*s) {
        if (!strncmp(s, "#extension GL_ARB_texture_rectangle", 35)) {   // no longer needed
            while (*s && *s != '\n') s++;
            continue;
        }
        int word = s == b->p || !ident_char(s[-1]);
        if (word && !strncmp(s, "uniform sampler2DRect", 21) && !ident_char(s[21])) {
            const char *n = s + 21;
            while (*n == ' ' || *n == '\t') n++;
            const char *e = n;
            while (ident_char(*e)) e++;
            char decl[256];
            snprintf(decl, sizeof decl, "uniform sampler2D %.*s; uniform vec2 lbRectScale_%.*s",
                     (int)(e - n), n, (int)(e - n), n);
            put(&o, decl, strlen(decl));
            s = e;
            continue;
        }
        if (word && !strncmp(s, "texture2DRect", 13) && !ident_char(s[13])) {
            const char *p = s + 13;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '(') {
                const char *n = p + 1;
                while (*n == ' ' || *n == '\t') n++;
                const char *e = n;
                while (ident_char(*e)) e++;
                const char *c = e;
                while (*c == ' ' || *c == '\t') c++;
                if (*c == ',' && e > n) {
                    const char *x = c + 1;   // the coordinates, up to the matching ')'
                    int depth = 0;
                    for (; *x && (*x != ')' || depth); x++) depth += *x == '(' ? 1 : *x == ')' ? -1 : 0;
                    if (*x == ')') {
                        char head[160];
                        snprintf(head, sizeof head, "texture2D(%.*s, (", (int)(e - n), n);
                        put(&o, head, strlen(head));
                        put(&o, c + 1, (size_t)(x - c - 1));
                        snprintf(head, sizeof head, ") * lbRectScale_%.*s)", (int)(e - n), n);
                        put(&o, head, strlen(head));
                        s = x + 1;
                        continue;
                    }
                }
            }
        }
        put(&o, s, 1);
        s++;
    }
    free(b->p);
    *b = o;
}

// min(8, (3) + (3) + (0)) with integer constants, as its value: GLSL ES has
// no integer min and max (and a loop's bound must be a constant)
static void fold_const_minmax(struct buf *b)
{
    if (!strstr(b->p, "min") && !strstr(b->p, "max")) return;
    struct buf o = {0};
    put(&o, "", 0);
    const char *s = b->p;
    while (*s) {
        int word = s == b->p || !ident_char(s[-1]);
        int ismin = !strncmp(s, "min", 3), ismax = !strncmp(s, "max", 3);
        if (word && (ismin || ismax) && !ident_char(s[3])) {
            const char *p = s + 3;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '(') {
                const char *x = p + 1, *comma = NULL;
                int depth = 0, constant = 1;
                for (; *x && (*x != ')' || depth); x++) {
                    if (*x == '(') depth++;
                    else if (*x == ')') depth--;
                    else if (*x == ',' && !depth) comma = comma ? (constant = 0, comma) : x;
                    else if (!strchr("0123456789 \t+-*/", *x)) constant = 0;
                }
                if (*x == ')' && comma && constant) {
                    char *a = strndup(p + 1, (size_t)(comma - p - 1)), *c = strndup(comma + 1, (size_t)(x - comma - 1));
                    struct ev ea = {a, 0}, ec = {c, 0};
                    int64_t va = ev_cond(&ea), vc = ev_cond(&ec);
                    skip(&ea);
                    skip(&ec);
                    if (!ea.err && !ec.err && !*ea.p && !*ec.p) {
                        char num[32];
                        snprintf(num, sizeof num, "%lld", (long long)(ismin ? (va < vc ? va : vc) : (va > vc ? va : vc)));
                        put(&o, num, strlen(num));
                        s = x + 1;
                        free(a);
                        free(c);
                        continue;
                    }
                    free(a);
                    free(c);
                }
            }
        }
        put(&o, s, 1);
        s++;
    }
    free(b->p);
    *b = o;
}

// Shadow samplers, which OpenGL ES 2 does not have (Sega Race TV): each
// sampler2DShadow uniform becomes a sampler2D, and _lbshadow2D (glsl_fix.c's
// shadow2D) on it _lbshadowcmp, which compares the depth itself.
static void fix_shadow_samplers(struct buf *b)
{
    if (!strstr(b->p, "sampler2DShadow")) return;
    char names[16][64];
    int n = 0;
    for (const char *p = b->p; (p = strstr(p, "uniform sampler2DShadow")) && n < 16; p++) {
        const char *q = p + 23;
        while (*q == ' ' || *q == '\t') q++;
        size_t l = 0;
        while (ident_char(q[l]) && l < 63) l++;
        memcpy(names[n], q, l);
        names[n++][l] = 0;
    }
    struct buf o = {0};
    put(&o, "", 0);
    for (const char *s = b->p; *s;) {
        int word = s == b->p || !ident_char(s[-1]);
        if (word && !strncmp(s, "sampler2DShadow", 15) && !ident_char(s[15])) {
            put(&o, "sampler2D", 9);
            s += 15;
            continue;
        }
        if (word && !strncmp(s, "_lbshadow2D", 11) && !ident_char(s[11])) {
            const char *a = s + 11;
            while (*a == ' ' || *a == '\t' || *a == '(') a++;
            int shadow = 0;
            for (int i = 0; i < n && !shadow; i++) {
                size_t l = strlen(names[i]);
                shadow = !strncmp(a, names[i], l) && !ident_char(a[l]);
            }
            if (shadow) {
                put(&o, "_lbshadowcmp", 12);
                s += 11;
                continue;
            }
        }
        put(&o, s, 1);
        s++;
    }
    free(b->p);
    *b = o;
}

// An array constructor initialising a local array (GLSL 1.20; GLSL ES 1.00
// has none): T name[N] = T[N](a, b, ...); becomes T name[N]; name[0] = a;
// name[1] = b; ... (the newlines kept, so line numbers stay)
static void fix_array_constructors(struct buf *b)
{
    if (!strstr(b->p, "](")) return;
    struct buf o = {0};
    put(&o, "", 0);
    int depth = 0;
    const char *stmt = b->p;   // where the current statement starts in the output so far
    size_t stmt_out = 0;
    for (const char *s = b->p; *s;) {
        char c = *s;
        if (c == '{' || c == '}' || c == ';') {
            depth += c == '{' ? 1 : c == '}' ? -1 : 0;
            put(&o, s, 1);
            s++;
            stmt = s;
            stmt_out = o.n;
            continue;
        }
        if (c == '=' && depth > 0 && s[1] != '=' && s > b->p && !strchr("=!<>+-*/", s[-1])) {
            // after '=': T [ N ] (
            const char *p = s + 1;
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            const char *t = p;
            while (ident_char(*p)) p++;
            const char *te = p;
            while (*p == ' ' || *p == '\t') p++;
            int ok = te > t && *p == '[';
            if (ok) {
                p++;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                while (*p == ' ' || *p == '\t') p++;
                ok = *p == '(';
            }
            // the declaration before '=': ... name [ N ]
            char decl[256] = "", name[128] = "";
            const char *lead = stmt;   // the blanks and newlines before the declaration stay
            while (lead < s && strchr(" \t\r\n", *lead)) lead++;
            if (ok) {
                size_t dl = (size_t)(s - lead);
                ok = dl < sizeof decl;
                if (ok) {
                    memcpy(decl, lead, dl);
                    decl[dl] = 0;
                    char *br = strchr(decl, '[');
                    ok = br != NULL;
                    if (ok) {
                        char *ne = br;
                        while (ne > decl && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
                        char *nb = ne;
                        while (nb > decl && ident_char(nb[-1])) nb--;
                        snprintf(name, sizeof name, "%.*s", (int)(ne - nb), nb);
                        ok = *name != 0;
                    }
                }
            }
            if (ok) {
                // the arguments, split at top-level commas
                const char *a = p + 1, *e = a;
                int d = 0, newlines = 0;
                for (; *e && (*e != ')' || d); e++) {
                    d += *e == '(' ? 1 : *e == ')' ? -1 : 0;
                    newlines += *e == '\n';
                }
                const char *semi = *e == ')' ? e + 1 : NULL;
                while (semi && (*semi == ' ' || *semi == '\t')) semi++;
                if (semi && *semi == ';') {
                    // the declaration, without const (the elements are assigned)
                    o.n = stmt_out;
                    o.p[o.n] = 0;
                    put(&o, stmt, (size_t)(lead - stmt));
                    char *cst = strstr(decl, "const ");
                    if (cst) memmove(cst, cst + 6, strlen(cst + 6) + 1);
                    put(&o, decl, strlen(decl));
                    put(&o, ";", 1);
                    int idx = 0;
                    const char *q = a;
                    d = 0;
                    for (const char *r = a;; r++) {
                        if (*r == '(') d++;
                        if (*r == ')') d--;
                        if ((*r == ',' && !d) || r == e) {
                            char head[160];
                            snprintf(head, sizeof head, " %s[%d] = ", name, idx++);
                            put(&o, head, strlen(head));
                            for (const char *k = q; k < r; k++)
                                if (*k != '\n') put(&o, k, 1);
                            put(&o, ";", 1);
                            q = r + 1;
                        }
                        if (r == e) break;
                    }
                    for (int i = 0; i < newlines; i++) put(&o, "\n", 1);
                    s = semi + 1;
                    stmt = s;
                    stmt_out = o.n;
                    continue;
                }
            }
        }
        put(&o, s, 1);
        s++;
    }
    free(b->p);
    *b = o;
}

char *glsl_preprocess(const char *src)
{
    struct pp *pp = calloc(1, sizeof *pp);
    char *s = strip_comments(src);
    struct buf out = {0};
    put(&out, "", 0);
    // each #if level: is its group on, has a group of the chain been on, was the enclosing level on
    enum { MAX_DEPTH = 256 };
    struct { char on, done, outer; } st[MAX_DEPTH];
    int depth = 0, on = 1;
    const struct macro *active[MAX_NEST];
    for (char *p = s; *p; ) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = p;
        p += len + (e ? 1 : 0);

        char *d = line, *end = line + len;
        while (d < end && (*d == ' ' || *d == '\t')) d++;
        if (d == end || *d != '#') {
            if (on) expand(pp, line, len, &out, active, 0, 0);
            put(&out, "\n", 1);
            continue;
        }
        d++;
        while (d < end && (*d == ' ' || *d == '\t')) d++;
        char *w = d;
        while (w < end && ident_char(*w)) w++;
        size_t wn = (size_t)(w - d);
        char *a = w;
        while (a < end && (*a == ' ' || *a == '\t')) a++;
        char *an = a;
        while (an < end && ident_char(*an)) an++;
#define IS(k) (wn == strlen(k) && !memcmp(d, k, wn))
        if (IS("if") || IS("ifdef") || IS("ifndef")) {
            int c = 0;
            if (on) {
                if (IS("if")) c = eval(pp, w, (size_t)(end - w));
                else c = (*find(pp, a, (size_t)(an - a)) != NULL) == IS("ifdef");
            }
            if (depth < MAX_DEPTH) st[depth].outer = on, st[depth].on = c, st[depth].done = c;
            depth++;
            on = on && c;
        } else if (IS("elif") || IS("else")) {
            if (depth && depth <= MAX_DEPTH) {
                int i = depth - 1, c = 0;
                if (st[i].outer && !st[i].done) c = IS("else") || eval(pp, w, (size_t)(end - w));
                st[i].on = c;
                st[i].done |= c;
                on = st[i].outer && c;
            }
        } else if (IS("endif")) {
            if (depth) depth--;
            if (depth < MAX_DEPTH) on = st[depth].outer;
        } else if (IS("define")) {
            if (on && an > a) {
                char *b = an;
                while (b < end && (*b == ' ' || *b == '\t')) b++;
                char *be = end;
                while (be > b && (be[-1] == ' ' || be[-1] == '\t')) be--;
                char *body = strndup(b, (size_t)(be - b));
                define(pp, a, (size_t)(an - a), body);
                free(body);
            }
        } else if (IS("undef")) {
            if (on) undef(pp, a, (size_t)(an - a));
        } else if (IS("version") || IS("extension") || IS("pragma") || IS("line")) {
            if (on) put(&out, line, len);
        }
#undef IS
        put(&out, "\n", 1);
    }
    free(s);
    fold_builtin_indices(&out);
    fix_rect_textures(&out);
    fix_shadow_samplers(&out);
    fix_array_constructors(&out);
    fold_const_minmax(&out);
    // runs of blanks as one space: gl4es's own preprocessor keeps a run in a
    // fixed 500-byte token and overruns it on longer ones
    char *w = out.p;
    for (const char *r = out.p; *r; r++) {
        int blank = *r == ' ' || *r == '\t';
        if (blank && w > out.p && w[-1] == ' ') continue;
        *w++ = blank ? ' ' : *r;
    }
    *w = 0;
    for (int i = 0; i < BUCKETS; i++)
        for (struct macro *m = pp->tab[i], *n; m; m = n) {
            n = m->next;
            free(m->name);
            free(m->body);
            free(m);
        }
    free(pp);
    return out.p;
}

// The lines of src (numbered from its "#line 1") that the compiler's log
// faults for mixing int and float, with their integer constants written as
// floats (2 -> 2.0) and the loops' int counters as float(i), outside [] and
// loop headers; NULL when there is none.
char *glsl_float_literals(const char *src, const char *log)
{
    enum { MAX_LINES = 64 };
    int lines[MAX_LINES], n = 0;
    for (const char *e = log; (e = strstr(e, "ERROR: 0:")) && n < MAX_LINES; e++) {
        const char *eol = strchr(e, '\n');
        size_t len = eol ? (size_t)(eol - e) : strlen(e);
        char msg[512];
        snprintf(msg, sizeof msg, "%.*s", (int)(len < sizeof msg ? len : sizeof msg - 1), e);
        // (OpenGL ES numbers the line after "#line 1" as 2)
        if (strstr(msg, "'int'") || strstr(msg, "(int") || strstr(msg, ", int")) lines[n++] = atoi(e + 9) - 1;
    }
    if (!n) return NULL;
    const char *base = strstr(src, "#line 1\n");
    base = base ? base + 8 : src;
    // the loops' int counters, which a faulted line uses as float(i)
    char counters[8][64];
    int nc = 0;
    for (const char *f = src; (f = strstr(f, "for")) && nc < 8; f += 3) {
        if (f > src && ident_char(f[-1])) continue;
        const char *q = f + 3;
        while (*q == ' ' || *q == '\t') q++;
        if (*q++ != '(') continue;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, "int", 3) || ident_char(q[3])) continue;
        q += 3;
        while (*q == ' ' || *q == '\t') q++;
        size_t l = 0;
        while (ident_char(q[l]) && l < 63) l++;
        if (l) {
            memcpy(counters[nc], q, l);
            counters[nc++][l] = 0;
        }
    }
    struct buf o = {0};
    put(&o, src, (size_t)(base - src));
    int line = 1, changed = 0;
    for (const char *p = base; *p; line++) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p + 1) : strlen(p);
        int fault = 0;
        for (int i = 0; i < n; i++) fault |= lines[i] == line;
        int loop = 0;
        for (size_t i = 0; i + 3 <= len; i++)
            if (!strncmp(p + i, "for", 3) && (i == 0 || !ident_char(p[i - 1])) && !ident_char(p[i + 3])) loop = 1;
        if (!fault || loop || *p == '#') {
            put(&o, p, len);
            p += len;
            continue;
        }
        int bracket = 0;
        for (size_t i = 0; i < len;) {
            char c = p[i];
            if (c == '[') bracket++;
            if (c == ']' && bracket) bracket--;
            if (ident_start(c)) {
                size_t j = i;
                while (j < len && ident_char(p[j])) j++;
                int counter = 0;
                for (int k = 0; k < nc && !bracket; k++)
                    counter |= strlen(counters[k]) == j - i && !strncmp(p + i, counters[k], j - i);
                if (counter) {
                    put(&o, "float(", 6);
                    put(&o, p + i, j - i);
                    put(&o, ")", 1);
                    changed = 1;
                } else {
                    put(&o, p + i, j - i);
                }
                i = j;
                continue;
            }
            if (c >= '0' && c <= '9' && !(i && p[i - 1] == '.')) {
                size_t j = i;
                while (j < len && (ident_char(p[j]) || p[j] == '.')) j++;
                int is_int = 1;
                for (size_t k = i; k < j; k++) is_int &= p[k] >= '0' && p[k] <= '9';
                put(&o, p + i, j - i);
                if (is_int && !bracket) {
                    put(&o, ".0", 2);
                    changed = 1;
                }
                i = j;
                continue;
            }
            put(&o, p + i, 1);
            i++;
        }
        p += len;
    }
    if (!changed) {
        free(o.p);
        return NULL;
    }
    return o.p;
}
