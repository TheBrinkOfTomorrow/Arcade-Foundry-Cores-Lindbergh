// Make the games' GLSL acceptable to a strict compiler. The cabinets ran
// NVIDIA's driver, which also takes Cg-isms that GLSL 1.10/1.20 forbids:
//   half/half2/half3/half4 types
//   samplers declared at global scope without "uniform"
//   C-style brace initializers:  const mat3 m = {{..},{..},{..}};
//   assignments that change vector size: vec4 = vec2, vec3 = float (Cg pads
//     with 0,1 and smears scalars); the right side goes through an overloaded
//     converter, so the host compiler works out its type
//   shadow2D on a plain sampler2D, and dot(vecN, float)
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; } sbuf;

static void put(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(sbuf *b, const char *s) { put(b, s, strlen(s)); }

static int word_at(const char *s, size_t i, const char *w)
{
    if (s[i] != w[0]) return 0;   // most positions: no strlen, no strncmp
    size_t n = strlen(w);
    if (strncmp(s + i, w, n)) return 0;
    if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_')) return 0;
    return !(isalnum((unsigned char)s[i + n]) || s[i + n] == '_');
}

// the word at s[i] is followed by an argument list
static int is_call(const char *s, size_t i)
{
    while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') i++;
    return s[i] == '(';
}

// copy a brace initializer starting at s[i] == '{' as constructor calls; type is
// "mat3", "vec4", ... for the outer level. Returns the index after the closing brace.
static size_t brace_init(const char *s, size_t i, const char *type, sbuf *o)
{
    char inner[8] = "";
    if (!strncmp(type, "mat", 3)) snprintf(inner, sizeof inner, "vec%c", type[3]);
    puts_(o, type);
    puts_(o, "(");
    i++;
    while (s[i] && s[i] != '}') {
        if (s[i] == '{') {
            i = brace_init(s, i, inner[0] ? inner : "float", o);
            continue;
        }
        put(o, s + i, 1);
        i++;
    }
    puts_(o, ")");
    return s[i] ? i + 1 : i;
}

static char *fix_syntax(const char *src)
{
    sbuf o = {0};
    size_t n = strlen(src);
    int depth = 0, line_start = 1;
    char last_type[16] = "";
    for (size_t i = 0; i < n;) {
        char ch = src[i];
        // comments pass through untouched
        if (ch == '/' && src[i + 1] == '/') {
            size_t j = i;
            while (j < n && src[j] != '\n') j++;
            put(&o, src + i, j - i);
            i = j;
            continue;
        }
        if (ch == '/' && src[i + 1] == '*') {
            const char *e = strstr(src + i + 2, "*/");
            size_t j = e ? (size_t)(e - src) + 2 : n;
            put(&o, src + i, j - i);
            i = j;
            continue;
        }
        if (depth == 0 && line_start && (word_at(src, i, "sampler2D") || word_at(src, i, "sampler1D") ||
                                         word_at(src, i, "sampler3D") || word_at(src, i, "samplerCube") ||
                                         word_at(src, i, "sampler2DShadow") || word_at(src, i, "sampler2DRect")))
            puts_(&o, "uniform ");
        if (isalpha((unsigned char)ch)) {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            if (j - i == 4 && (!strncmp(src + i, "mat", 3) || !strncmp(src + i, "vec", 3)) &&
                src[i + 3] >= '2' && src[i + 3] <= '4') {
                memcpy(last_type, src + i, 4);
                last_type[4] = 0;
            }
            put(&o, src + i, j - i);
            i = j;
            line_start = 0;
            continue;
        }
        if (ch == '=' && last_type[0] && src[i + 1] != '=') {
            size_t j = i + 1;
            while (j < n && isspace((unsigned char)src[j])) j++;
            if (src[j] == '{') {
                puts_(&o, "= ");
                i = brace_init(src, j, last_type, &o);
                last_type[0] = 0;
                continue;
            }
        }
        if (ch == ';') last_type[0] = 0;
        if (ch == '{') depth++;
        if (ch == '}') depth--;
        if (ch == '\n') line_start = 1;
        else if (!isspace((unsigned char)ch)) line_start = 0;
        put(&o, src + i, 1);
        i++;
    }
    return o.p;
}

// ---- vector size conversions --------------------------------------------------------
static const char helpers[] =
    "#version 120\n"
    "#define half float\n#define half2 vec2\n#define half3 vec3\n#define half4 vec4\n"
    // Cg's names, which NVIDIA's GLSL took too (Sega Race TV)
    "#define fixed float\n#define fixed2 vec2\n#define fixed3 vec3\n#define fixed4 vec4\n"
    "#define tex2D texture2D\n#define tex2Dproj texture2DProj\n#define texCUBE textureCube\n"
    "float _lb1(float x){return x;} float _lb1(int x){return float(x);} float _lb1(vec2 x){return x.x;}\n"
    "float _lb1(vec3 x){return x.x;} float _lb1(vec4 x){return x.x;}\n"
    "vec2 _lb2(float x){return vec2(x);} vec2 _lb2(int x){return vec2(float(x));} vec2 _lb2(vec2 x){return x;}\n"
    "vec2 _lb2(vec3 x){return x.xy;} vec2 _lb2(vec4 x){return x.xy;}\n"
    "vec3 _lb3(float x){return vec3(x);} vec3 _lb3(int x){return vec3(float(x));} vec3 _lb3(vec2 x){return vec3(x,0.0);}\n"
    "vec3 _lb3(vec3 x){return x;} vec3 _lb3(vec4 x){return x.xyz;}\n"
    "vec4 _lb4(float x){return vec4(x);} vec4 _lb4(int x){return vec4(float(x));} vec4 _lb4(vec2 x){return vec4(x,0.0,1.0);}\n"
    "vec4 _lb4(vec3 x){return vec4(x,1.0);} vec4 _lb4(vec4 x){return x;}\n"
    // user functions named like a built-in hide it, so these get their own names
    "vec4 _lbshadow2D(sampler2D s,vec3 c){return texture2D(s,c.xy);}\n"
#ifndef LB_IOS
    "vec4 _lbshadow2D(sampler2DShadow s,vec3 c){return shadow2D(s,c);}\n"
#else   // OpenGL ES 2 has no shadow samplers: the comparison by hand (glsl_pp.c)
    "vec4 _lbshadowcmp(sampler2D s,vec3 c){return vec4(step(c.z,texture2D(s,c.xy).r));}\n"
#endif
    "float _lbdot(float a,float b){return a*b;} float _lbdot(vec2 a,vec2 b){return dot(a,b);}\n"
    "float _lbdot(vec3 a,vec3 b){return dot(a,b);} float _lbdot(vec4 a,vec4 b){return dot(a,b);}\n"
    "float _lbdot(vec2 a,float b){return dot(a,vec2(b));} float _lbdot(vec3 a,float b){return dot(a,vec3(b));}\n"
    "float _lbdot(vec4 a,float b){return dot(a,vec4(b));}\n"
    "#line 1\n";

#define MAX_SYMS 512
typedef struct { char name[64]; int size; size_t pos; } sym;   // size 1-4: float/vecN, 0: other

static int is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

static int type_size(const char *t, size_t n)
{
    static const struct { const char *w; int size; } types[] = {
        {"float", 1}, {"half", 1}, {"vec2", 2}, {"half2", 2}, {"vec3", 3}, {"half3", 3},
        {"vec4", 4}, {"half4", 4}, {"int", 0}, {"bool", 0}, {"ivec2", 0}, {"ivec3", 0},
        {"ivec4", 0}, {"bvec2", 0}, {"bvec3", 0}, {"bvec4", 0}, {"mat2", 0}, {"mat3", 0},
        {"mat4", 0}, {"void", 0},
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strlen(types[i].w) == n && !strncmp(t, types[i].w, n)) return types[i].size;
    return -1;
}

// the type of the declaration of name nearest before pos
static int lookup(const sym *t, int n, const char *name, size_t len, size_t pos)
{
    if (len == 11 && !strncmp(name, "gl_TexCoord", len)) return 4;
    if (len >= 3 && !strncmp(name, "gl_", 3)) {
        static const char *v4[] = {"gl_FragColor", "gl_Position", "gl_FragData", "gl_FrontColor",
                                   "gl_BackColor", "gl_FrontSecondaryColor", "gl_ClipVertex"};
        for (size_t i = 0; i < sizeof v4 / sizeof v4[0]; i++)
            if (strlen(v4[i]) == len && !strncmp(name, v4[i], len)) return 4;
        if (len == 11 && !strncmp(name, "gl_FragDepth", len)) return 1;
        return -1;
    }
    for (int i = n - 1; i >= 0; i--)
        if (t[i].pos < pos && strlen(t[i].name) == len && !strncmp(t[i].name, name, len)) return t[i].size;
    return -1;
}

// size of the value an assignment's left side names, or -1 when unknown
static int lhs_size(const char *s, size_t a, size_t b, const sym *t, int nt)
{
    while (a < b && isspace((unsigned char)s[a])) a++;
    // skip control-flow heads: if (...), for (...), while (...), else
    for (;;) {
        size_t w = a;
        while (w < b && is_ident(s[w])) w++;
        if ((w - a == 2 && !strncmp(s + a, "if", 2)) || (w - a == 3 && !strncmp(s + a, "for", 3)) ||
            (w - a == 5 && !strncmp(s + a, "while", 5))) {
            while (w < b && s[w] != '(') w++;
            int d = 0;
            for (; w < b; w++) {
                if (s[w] == '(') d++;
                if (s[w] == ')' && --d == 0) { w++; break; }
            }
            a = w;
        } else if (w - a == 4 && !strncmp(s + a, "else", 4)) {
            a = w;
        } else if (w - a == 5 && !strncmp(s + a, "const", 5)) {
            return -1;      // a constant initializer cannot hold a function call
        } else break;
        while (a < b && isspace((unsigned char)s[a])) a++;
    }
    size_t w = a;
    while (w < b && is_ident(s[w])) w++;
    if (w == a) return -1;
    size_t r = w;
    while (r < b && isspace((unsigned char)s[r])) r++;
    if (r < b && is_ident(s[r])) return type_size(s + a, w - a);     // declaration
    int size = lookup(t, nt, s + a, w - a, a);
    if (size < 0) return -1;
    // an index, then maybe a swizzle
    while (r < b && s[r] == '[') {
        int d = 0;
        for (; r < b; r++) {
            if (s[r] == '[') d++;
            if (s[r] == ']' && --d == 0) { r++; break; }
        }
    }
    while (r < b && isspace((unsigned char)s[r])) r++;
    if (r < b && s[r] == '.') {
        size_t q = ++r, k = 0;
        while (q < b && strchr("xyzwrgbastpq", s[q]) && s[q]) q++, k++;
        while (q < b && isspace((unsigned char)s[q])) q++;
        if (q != b || k < 1 || k > 4) return -1;
        return (int)k;
    }
    while (r < b && isspace((unsigned char)s[r])) r++;
    return r == b ? size : -1;
}

// blank out comments, keeping newlines so line numbers hold
static char *strip_comments(const char *src)
{
    size_t n = strlen(src);
    char *s = strdup(src);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '/' && s[i + 1] == '/')
            while (i < n && s[i] != '\n') s[i++] = ' ';
        else if (s[i] == '/' && s[i + 1] == '*') {
            while (i < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] != '\n') s[i] = ' ';
                i++;
            }
            if (i < n) s[i] = s[i + 1] = ' ';
        }
    }
    return s;
}

// ---- writable varyings ----------------------------------------------------------------
// NVIDIA lets a fragment shader write its varyings. Each scalar or vector varying
// the shader writes (directly or through a #define alias) gets a writable copy
// _lbw_name that every use refers to, and main() starts by filling it. A
// declaration inside #if blocks is copied under the same conditions. Varyings that
// are only read stay as they are: Mesa refuses to link a fragment shader that
// reads a varying the vertex shader does not write.
#define MAX_VARY 64
#define MAX_DEPTH 16
typedef struct { char type[16], name[64], cond[1024]; } vary;

// is identifier w (length wn) written anywhere in s: w[...][.swz] followed by an
// assignment operator, or ++/--
static int written(const char *s, const char *w, size_t wn)
{
    size_t n = strlen(s);
    for (const char *p = s; (p = strstr(p, w)); p += wn) {
        size_t i = (size_t)(p - s);
        if ((i && is_ident(s[i - 1])) || is_ident(s[i + wn])) continue;
        size_t j = i + wn;
        while (j < n && isspace((unsigned char)s[j])) j++;
        while (j < n && s[j] == '[') {
            while (j < n && s[j] != ']') j++;
            j++;
        }
        if (j < n && s[j] == '.') {
            j++;
            while (j < n && is_ident(s[j])) j++;
        }
        while (j < n && isspace((unsigned char)s[j])) j++;
        if (j + 1 < n && s[j] == '=' && s[j + 1] != '=') return 1;
        if (j + 1 < n && strchr("+-*/", s[j]) && s[j + 1] == '=') return 1;
        if (j + 1 < n && (s[j] == '+' || s[j] == '-') && s[j + 1] == s[j]) return 1;
    }
    return 0;
}

// is the varying `name` written, directly or through "#define alias name..."
// (aliases of aliases too)
static int varying_written(const char *s, const char *name, int depth)
{
    size_t nn = strlen(name);
    if (written(s, name, nn)) return 1;
    for (const char *p = s; (p = strstr(p, "#define")); p += 7) {
        const char *a = p + 7;
        while (*a == ' ' || *a == '\t') a++;
        const char *ae = a;
        while (is_ident(*ae)) ae++;
        const char *b = ae;
        while (*b == ' ' || *b == '\t') b++;
        if (ae > a && !strncmp(b, name, nn) && !is_ident(b[nn])) {
            char alias[128];
            snprintf(alias, sizeof alias, "%.*s", (int)(ae - a), a);
            if (depth < 4 && varying_written(s, alias, depth + 1)) return 1;
        }
    }
    return 0;
}

// is name one of the space-separated words in list
static int in_list(const char *list, const char *name, size_t nn)
{
    if (!list) return 0;
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == nn && !strncmp(p, name, nn)) return 1;
        p += len;
        while (*p == ' ') p++;
    }
    return 0;
}

static char *fix_varyings(const char *src, const char *missing)
{
    char *s = strip_comments(src);
    static vary v[MAX_VARY];
    int nv = 0;
    // directive lines of each open #if level, in order
    char levels[MAX_DEPTH][512];
    int depth = 0;
    sbuf o = {0};
    size_t n = strlen(s), main_open = 0;
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && s[e] != '\n') e++;
        const char *l = s + i;
        size_t ln = e - i;
        size_t k = 0;
        while (k < ln && isspace((unsigned char)l[k])) k++;
        if (k < ln && l[k] == '#') {
            size_t d = k + 1;
            while (d < ln && isspace((unsigned char)l[d])) d++;
            if (!strncmp(l + d, "if", 2) && depth < MAX_DEPTH) {
                snprintf(levels[depth++], sizeof levels[0], "%.*s\n", (int)(ln - k), l + k);
            } else if ((!strncmp(l + d, "elif", 4) || !strncmp(l + d, "else", 4)) && depth > 0) {
                size_t used = strlen(levels[depth - 1]);
                snprintf(levels[depth - 1] + used, sizeof levels[0] - used, "%.*s\n", (int)(ln - k), l + k);
            } else if (!strncmp(l + d, "endif", 5) && depth > 0) {
                depth--;
            }
        } else if (word_at(l, k, "varying") && missing) {
            // inputs the vertex shader does not write become plain globals
            size_t a = k + 7;
            while (a < ln && isspace((unsigned char)l[a])) a++;
            while (a < ln && is_ident(l[a])) a++;
            while (a < ln && isspace((unsigned char)l[a])) a++;
            size_t b = a;
            while (b < ln && is_ident(l[b])) b++;
            if (b > a && in_list(missing, l + a, b - a)) {
                put(&o, l, k);
                puts_(&o, "       ");
                put(&o, l + k + 7, ln - k - 7);
                if (e < n) put(&o, "\n", 1);
                i = e + 1;
                continue;
            }
        }
        if (k < ln && l[k] != '#' && word_at(l, k, "varying") && nv < MAX_VARY) {
            // varying T name;  (arrays and lists are left alone)
            size_t a = k + 7, b, c2, d;
            while (a < ln && isspace((unsigned char)l[a])) a++;
            b = a;
            while (b < ln && is_ident(l[b])) b++;
            c2 = b;
            while (c2 < ln && isspace((unsigned char)l[c2])) c2++;
            d = c2;
            while (d < ln && is_ident(l[d])) d++;
            size_t f = d;
            while (f < ln && isspace((unsigned char)l[f])) f++;
            char vname[64];
            snprintf(vname, sizeof vname, "%.*s", (int)(d - c2 < 63 ? d - c2 : 63), l + c2);
            if (b > a && d > c2 && f < ln && l[f] == ';' && b - a < 16 && d - c2 < 60 &&
                varying_written(s, vname, 0)) {
                vary *x = &v[nv++];
                snprintf(x->type, sizeof x->type, "%.*s", (int)(b - a), l + a);
                snprintf(x->name, sizeof x->name, "%s", vname);
                x->cond[0] = 0;
                for (int q = 0; q < depth; q++) {
                    size_t used = strlen(x->cond);
                    snprintf(x->cond + used, sizeof x->cond - used, "%s", levels[q]);
                }
                char line[256];
                snprintf(line, sizeof line, "varying %s \001%s; %s %s;", x->type, x->name, x->type, x->name);
                puts_(&o, line);
                put(&o, l + ln, e < n ? 1 : 0);
                i = e + 1;
                continue;
            }
        } else if (!main_open && word_at(l, k, "void")) {
            // void main(...) {   — the copies go right after the brace
            const char *m = strstr(l, "main");
            if (m && m < l + ln) {
                size_t j = (size_t)(m - s);
                while (j < n && s[j] != '{') j++;
                if (j < n) {
                    put(&o, s + i, j + 1 - i);
                    puts_(&o, "\n");
                    for (int q = 0; q < nv; q++) {
                        int levelsn = 0;
                        for (const char *t = v[q].cond; *t; t++)
                            if (*t == '\n') {
                                const char *ls = t;
                                while (ls > v[q].cond && ls[-1] != '\n') ls--;
                                const char *dd = ls + 1;
                                while (isspace((unsigned char)*dd)) dd++;
                                if (!strncmp(dd, "if", 2)) levelsn++;
                            }
                        puts_(&o, v[q].cond);
                        char line[160];
                        snprintf(line, sizeof line, "%s = \001%s;\n", v[q].name, v[q].name);
                        puts_(&o, line);
                        for (int z = 0; z < levelsn; z++) puts_(&o, "#endif\n");
                    }
                    main_open = 1;
                    i = j + 1;
                    continue;
                }
            }
        }
        put(&o, l, ln);
        if (e < n) put(&o, "\n", 1);
        i = e + 1;
    }
    free(s);
    // every use names the copy; \001 marks the two places that mean the varying
    sbuf r = {0};
    for (size_t i = 0; i < o.n;) {
        if (o.p[i] == 1) { i++; continue; }
        if (is_ident(o.p[i]) && (i == 0 || (!is_ident(o.p[i - 1]) && o.p[i - 1] != 1))) {
            size_t w = i;
            while (w < o.n && is_ident(o.p[w])) w++;
            for (int q = 0; q < nv; q++)
                if (strlen(v[q].name) == w - i && !strncmp(o.p + i, v[q].name, w - i)) {
                    puts_(&r, "_lbw_");
                    break;
                }
            put(&r, o.p + i, w - i);
            i = w;
            continue;
        }
        put(&r, o.p + i, 1);
        i++;
    }
    free(o.p);
    return r.p;
}

static char *fix_sizes(const char *src)
{
    size_t n = strlen(src);
    char *s = strip_comments(src);
    // calls that go to the helpers (only calls: Race TV has a uniform named dot)
    {
        sbuf r = {0};
        for (size_t i = 0; i < n;) {
            if ((i == 0 || !is_ident(s[i - 1])) && (word_at(s, i, "dot") || word_at(s, i, "shadow2D")) &&
                is_call(s, i))
                puts_(&r, "_lb");
            put(&r, s + i, 1);
            i++;
        }
        free(s);
        s = r.p;
        n = r.n;
    }
    // declarations: type name [, name ...]
    static sym t[MAX_SYMS];
    int nt = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_ident(s[i]) || (i && is_ident(s[i - 1]))) continue;
        size_t w = i;
        while (w < n && is_ident(s[w])) w++;
        int size = type_size(s + i, w - i);
        if (size < 0) { i = w - 1; continue; }
        size_t q = w;
        for (;;) {
            while (q < n && isspace((unsigned char)s[q])) q++;
            size_t e = q;
            while (e < n && is_ident(s[e])) e++;
            if (e == q || e - q >= sizeof t[0].name) break;
            size_t f = e;
            while (f < n && isspace((unsigned char)s[f])) f++;
            if (s[f] == '(') break;                      // a function
            if (nt < MAX_SYMS) {
                memcpy(t[nt].name, s + q, e - q);
                t[nt].name[e - q] = 0;
                t[nt].pos = q;
                t[nt++].size = size;
            }
            // skip an array size or initializer to the next ',' or the end
            int d = 0;
            for (q = f; q < n; q++) {
                if (s[q] == '(' || s[q] == '[' || s[q] == '{') d++;
                if (s[q] == ')' || s[q] == ']' || s[q] == '}') { if (--d < 0) break; }
                if (d == 0 && (s[q] == ',' || s[q] == ';')) break;
            }
            if (q < n && s[q] == ',' && d == 0) { q++; continue; }
            break;
        }
        i = w - 1;
    }
    sbuf o = {0};
    puts_(&o, helpers);
    size_t stmt = 0;
    int pd = 0, bd = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '#' ) {                                 // preprocessor line: copy as is
            size_t j = i;
            while (j < n && s[j] != '\n') j++;
            put(&o, s + i, j - i);
            i = j - 1;
            stmt = j;
            continue;
        }
        if (c == '(' || c == '[') pd++;
        if (c == ')' || c == ']') pd--;
        if (pd == 0 && (c == ';' || c == '{' || c == '}')) {
            bd += c == '{' ? 1 : c == '}' ? -1 : 0;
            put(&o, s + i, 1);
            stmt = i + 1;
            continue;
        }
        // file scope is left alone: Apple's linker crashes on a global initializer
        // that calls a function
        if (pd == 0 && bd > 0 && c == '=' && s[i + 1] != '=' && i && !strchr("=!<>", s[i - 1])) {
            size_t lhs_end = strchr("+-*/", s[i - 1]) ? i - 1 : i;
            int size = lhs_size(s, stmt, lhs_end, t, nt);
            // the right side runs to ';' at depth 0; skip it if it holds a ','
            size_t e = i + 1;
            int d = 0, comma = 0;
            for (; e < n; e++) {
                if (s[e] == '(' || s[e] == '[') d++;
                if (s[e] == ')' || s[e] == ']') d--;
                if (d == 0 && s[e] == ',') comma = 1;
                if (d == 0 && s[e] == ';') break;
                if (d < 0 || s[e] == '{') break;
            }
            if (size >= 1 && !comma && e < n && s[e] == ';') {
                char call[16];
                snprintf(call, sizeof call, "_lb%d(", size);
                put(&o, "=", 1);
                puts_(&o, call);
                put(&o, s + i + 1, e - i - 1);
                puts_(&o, ")");
                i = e - 1;
                continue;
            }
        }
        put(&o, s + i, 1);
    }
    free(s);
    return o.p;
}

// GLSL 1.20 attributes are floating point; NVIDIA and Mesa take "attribute ivec4 X",
// Apple does not. The attribute keeps its name (the game looks it up) as vecN, and
// every later use reads ivecN(X). The game feeds it with glVertexAttribPointer, so
// the values arrive as floats either way.
static char *fix_int_attributes(const char *src)
{
    static const char *itypes[] = {"int", "ivec2", "ivec3", "ivec4"};
    static const char *ftypes[] = {"float", "vec2", "vec3", "vec4"};
    char names[16][64];
    int kinds[16], nn = 0;
    sbuf o = {0};
    size_t n = strlen(src);
    for (size_t i = 0; i < n;) {
        if (!is_ident(src[i]) || (i && is_ident(src[i - 1]))) {
            put(&o, src + i, 1);
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && is_ident(src[j])) j++;
        if (word_at(src, i, "attribute")) {
            size_t t = j;
            while (t < n && isspace((unsigned char)src[t])) t++;
            for (int k = 0; k < 4; k++) {
                if (!word_at(src, t, itypes[k])) continue;
                size_t a = t + strlen(itypes[k]), e;
                while (a < n && isspace((unsigned char)src[a])) a++;
                for (e = a; e < n && is_ident(src[e]); e++);
                if (e == a || e - a >= sizeof names[0]) break;
                int seen = 0;
                for (int m = 0; m < nn; m++) seen |= strlen(names[m]) == e - a && !strncmp(names[m], src + a, e - a);
                if (!seen && nn < 16) {
                    memcpy(names[nn], src + a, e - a);
                    names[nn][e - a] = 0;
                    kinds[nn++] = k;
                }
                puts_(&o, "attribute ");
                puts_(&o, ftypes[k]);
                put(&o, src + t + strlen(itypes[k]), e - t - strlen(itypes[k]));
                i = e;
                break;
            }
            if (i > j) continue;
        }
        int k = -1;
        for (int m = 0; m < nn; m++)
            if (strlen(names[m]) == j - i && !strncmp(names[m], src + i, j - i)) k = kinds[m];
        if (k >= 0) {
            puts_(&o, itypes[k]);
            puts_(&o, "(");
        }
        put(&o, src + i, j - i);
        if (k >= 0) puts_(&o, ")");
        i = j;
    }
    return o.p;
}

// missing: space-separated varyings the vertex shader leaves unwritten (from a
// failed link), which a fragment shader then treats as zero-initialised globals
// Cg's rectangle-texture names, which NVIDIA's GLSL took too (Sega Race TV):
// samplerRECT and texRECT() are GLSL's sampler2DRect and texture2DRect()
static char *fix_rect_textures(const char *src)
{
    sbuf o = {0};
    size_t n = strlen(src);
    for (size_t i = 0; i < n;) {
        if (word_at(src, i, "samplerRECT")) {
            puts_(&o, "sampler2DRect");
            i += 11;
        } else if (word_at(src, i, "texRECT")) {
            puts_(&o, "texture2DRect");
            i += 7;
        } else put(&o, src + i++, 1);
    }
    put(&o, "", 1);
    o.n--;
    return o.p;
}

// a `const` initialised from variables (NVIDIA allowed it; GLSL wants a
// constant expression): the declaration loses `const`
static char *fix_const_locals(const char *src)
{
    sbuf o = {0};
    size_t n = strlen(src);
    for (size_t i = 0; i < n;) {
        if (word_at(src, i, "const")) {
            // a declaration's initializer: up to ';' (a parameter list ends at ')' first)
            const char *eq = NULL, *end = src + i + strcspn(src + i, ";){");
            if (*end != ';') end = NULL;
            for (const char *q = src + i; end && q < end; q++)
                if (*q == '=') { eq = q; break; }
            int variable = 0;
            for (const char *q = eq ? eq + 1 : NULL; q && q < end && !variable;) {
                if (isalpha((unsigned char)*q) || *q == '_') {
                    const char *w = q;
                    while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
                    const char *r = q;
                    while (r < end && isspace((unsigned char)*r)) r++;
                    int number = w > src && (isdigit((unsigned char)w[-1]) || w[-1] == '.');   // 1.0f, 2e5
                    if (*r != '(' && !number && strncmp(w, "true", 4) && strncmp(w, "false", 5)) variable = 1;
                } else q++;
            }
            if (variable) {
                i += 5;
                continue;
            }
        }
        put(&o, src + i, 1);
        i++;
    }
    put(&o, "", 1);
    o.n--;
    return o.p;
}

// C's array initializer, which NVIDIA took: T name[N] = { a, b, };
// becomes GLSL 1.20's array constructor T name[N] = T[N](a, b);
static char *fix_array_inits(const char *src)
{
    sbuf o = {0};
    size_t n = strlen(src);
    for (size_t i = 0; i < n;) {
        // at "type name [ size ] = {"
        if ((i == 0 || !is_ident(src[i - 1])) && isalpha((unsigned char)src[i])) {
            size_t t0 = i, t1 = i;
            while (t1 < n && is_ident(src[t1])) t1++;
            size_t q = t1;
            while (q < n && isspace((unsigned char)src[q])) q++;
            size_t n0 = q;
            while (q < n && is_ident(src[q])) q++;
            size_t n1 = q;
            while (q < n && isspace((unsigned char)src[q])) q++;
            if (n1 > n0 && q < n && src[q] == '[') {
                size_t b0 = q + 1, b1 = b0;
                while (b1 < n && src[b1] != ']') b1++;
                q = b1 + 1;
                while (q < n && isspace((unsigned char)src[q])) q++;
                if (q < n && src[q] == '=') {
                    q++;
                    while (q < n && isspace((unsigned char)src[q])) q++;
                    if (q < n && src[q] == '{') {
                        // the matching brace, then the elements without a trailing comma
                        size_t e = q + 1;
                        int d = 1;
                        while (e < n && d) {
                            if (src[e] == '{') d++;
                            if (src[e] == '}') d--;
                            e++;
                        }
                        size_t last = e - 1;   // the closing brace
                        size_t k = last;
                        while (k > q + 1 && isspace((unsigned char)src[k - 1])) k--;
                        size_t inner_end = k > q + 1 && src[k - 1] == ',' ? k - 1 : last;
                        put(&o, src + t0, t1 - t0);
                        put(&o, " ", 1);
                        put(&o, src + n0, n1 - n0);
                        put(&o, "[", 1);
                        put(&o, src + b0, b1 - b0);
                        put(&o, "] = ", 4);
                        put(&o, src + t0, t1 - t0);
                        put(&o, "[", 1);
                        put(&o, src + b0, b1 - b0);
                        put(&o, "](", 2);
                        put(&o, src + q + 1, inner_end - (q + 1));
                        put(&o, ")", 1);
                        i = e;
                        continue;
                    }
                }
            }
            put(&o, src + t0, t1 - t0);
            i = t1;
            continue;
        }
        put(&o, src + i, 1);
        i++;
    }
    put(&o, "", 1);
    o.n--;
    return o.p;
}

static int uses_rect_textures(const char *s)
{
    for (const char *p = s; (p = strstr(p, "2DRect")); p++)
        if (p > s + 6 && (!strncmp(p - 7, "sampler", 7) || !strncmp(p - 7, "texture", 7))) return 1;
    return 0;
}

#ifdef LB_IOS
char *glsl_preprocess(const char *src);   // glsl_pp.c
#endif

char *glsl_fix(const char *src, int fragment, const char *missing)
{
    char *r = fix_rect_textures(src), *k0 = fix_const_locals(r), *k = fix_array_inits(k0);
    free(r);
    free(k0);
    char *a = fix_syntax(k);
    free(k);
    if (fragment) {
        char *v = fix_varyings(a, missing);
        free(a);
        a = v;
    } else {
        char *v = fix_int_attributes(a);
        free(a);
        a = v;
    }
    char *b = fix_sizes(a);
    free(a);
    if (uses_rect_textures(b)) {   // the extension goes right after #version
        char *nl = strchr(b, '\n');
        const char *ext = "#extension GL_ARB_texture_rectangle : enable\n";
        size_t head = nl ? (size_t)(nl - b + 1) : 0, n = strlen(b);
        char *e = malloc(n + strlen(ext) + 1);
        memcpy(e, b, head);
        strcpy(e + head, ext);
        strcpy(e + head + strlen(ext), b + head);
        free(b);
        b = e;
    }
#ifdef LB_IOS
    char *pre = glsl_preprocess(b);   // gl4es needs plain code (glsl_pp.c)
    free(b);
    b = pre;
#endif
    return b;
}
