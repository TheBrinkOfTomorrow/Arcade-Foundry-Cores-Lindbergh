// NV_vertex_program (VP1.x/VP2.0, OutRun 2 SP) as ARB vertex programs with the
// NV_vertex_program3 option, which Apple's GL has: the register files become
// ARB bindings, c[] an array of environment parameters with the tracked
// matrices (glTrackMatrixNV) and Cg's "#const" values in their places.
// NV_fragment_program (FP1.x/FP2.0) likewise as ARB fragment programs with the
// NV_fragment_program2 option.
#include "nv_vertex_program.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; } buf;
static void put(buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(buf *b, const char *s) { put(b, s, strlen(s)); }

// v[N] or v[NAME]: NVIDIA aliases the conventional inputs onto these generic
// attributes, and OutRun 2 SP feeds its programs through the generic ones
// (glVertexAttribPointerARB), so they are read as such; 0 is the position either way
static const char *input(const char *name, char *tmp, size_t n)
{
    static const char *const names[16] = {"OPOS", "WGHT", "NRML", "COL0", "COL1", "FOGC", "6", "7",
                                          "TEX0", "TEX1", "TEX2", "TEX3", "TEX4", "TEX5", "TEX6", "TEX7"};
    int i = -1;
    if (isdigit((unsigned char)name[0])) i = atoi(name);
    else
        for (int k = 0; k < 16; k++)
            if (!strcmp(name, names[k])) i = k;
    if (i <= 0) return "vertex.position";
    snprintf(tmp, n, "vertex.attrib[%d]", i);
    return tmp;
}

static const char *output(const char *name, char *tmp, size_t n)
{
    if (!strcmp(name, "HPOS")) return "result.position";
    if (!strcmp(name, "COL0")) return "result.color.primary";
    if (!strcmp(name, "COL1")) return "result.color.secondary";
    if (!strcmp(name, "BFC0")) return "result.color.back.primary";
    if (!strcmp(name, "BFC1")) return "result.color.back.secondary";
    if (!strcmp(name, "FOGC")) return "result.fogcoord";
    if (!strcmp(name, "PSIZ")) return "result.pointsize";
    if (!strncmp(name, "TEX", 3)) snprintf(tmp, n, "result.texcoord[%d]", atoi(name + 3));
    else if (!strncmp(name, "CLP", 3)) snprintf(tmp, n, "result.clip[%d]", atoi(name + 3));
    else snprintf(tmp, n, "result.%s", name);
    return tmp;
}

static const char *matrix_name(unsigned m)
{
    switch (m) {
    case 0x1700: return "modelview";             // GL_MODELVIEW
    case 0x1701: return "projection";            // GL_PROJECTION
    case 0x1702: return "texture";               // GL_TEXTURE
    case 0x8629: return "mvp";                   // GL_MODELVIEW_PROJECTION_NV
    }
    return NULL;
}
static const char *transform_suffix(unsigned t)
{
    switch (t) {
    case 0x862b: return ".inverse";              // GL_INVERSE_NV
    case 0x862c: return ".transpose";            // GL_TRANSPOSE_NV
    case 0x862d: return ".invtrans";             // GL_INVERSE_TRANSPOSE_NV
    }
    return "";                                   // GL_IDENTITY_NV
}

char *nvvp_to_arb(const char *src, size_t len, const nvvp_track *track)
{
    char *s = malloc(len + 1);
    memcpy(s, src, len);
    s[len] = 0;
    // the constants Cg wrote as comments, and how much of c[] the program reads
    static float konst[NVVP_PARAMS][4];
    static char has_const[NVVP_PARAMS];
    memset(has_const, 0, sizeof has_const);
    int top = 0, relative = 0;
    for (const char *p = s; (p = strstr(p, "c[")); p += 2) {
        if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        const char *q = p + 2;
        while (*q == ' ') q++;
        if (isdigit((unsigned char)*q)) {
            int k = atoi(q);
            if (k + 1 > top) top = k + 1;
        } else if (*q == 'A') relative = 1;
    }
    for (const char *p = s; (p = strstr(p, "#const c[")); p++) {
        int k;
        float v[4];
        if (sscanf(p, "#const c[%d] = %f %f %f %f", &k, &v[0], &v[1], &v[2], &v[3]) == 5 && k >= 0 && k < NVVP_PARAMS) {
            memcpy(konst[k], v, sizeof v);
            has_const[k] = 1;
        }
    }
    if (relative || top > NVVP_PARAMS) top = NVVP_PARAMS;

    buf b = {0};
    char line[256];
    puts_(&b, "!!ARBvp1.0\nOPTION NV_vertex_program3;\n");
    puts_(&b, "TEMP R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13, R14, R15;\n");
    // A1 only where the program has it (NV_vertex_program2): plain ARB, as
    // on Mesa (arb_lower.c), allows one address register
    puts_(&b, strstr(s, "A1.") ? "ADDRESS A0, A1;\n" : "ADDRESS A0;\n");
    if (top) {
        snprintf(line, sizeof line, "PARAM c[%d] = { ", top);
        puts_(&b, line);
        for (int i = 0; i < top;) {
            if (i) puts_(&b, ", ");
            const nvvp_track *t = NULL;
            for (int k = 0; track && k < NVVP_TRACKS; k++)
                if (track[k].matrix && track[k].address == (unsigned)i && matrix_name(track[k].matrix) && i + 4 <= top)
                    t = &track[k];
            if (t) {
                snprintf(line, sizeof line, "state.matrix.%s%s", matrix_name(t->matrix), transform_suffix(t->transform));
                puts_(&b, line);
                i += 4;
            } else if (has_const[i]) {
                snprintf(line, sizeof line, "{ %.9g, %.9g, %.9g, %.9g }", konst[i][0], konst[i][1], konst[i][2], konst[i][3]);
                puts_(&b, line);
                i++;
            } else {
                int j = i + 1;   // a run of plain parameters
                while (j < top && !has_const[j]) {
                    int tracked = 0;
                    for (int k = 0; track && k < NVVP_TRACKS; k++)
                        if (track[k].matrix && track[k].address == (unsigned)j) tracked = 1;
                    if (tracked) break;
                    j++;
                }
                snprintf(line, sizeof line, j - i > 1 ? "program.env[%d..%d]" : "program.env[%d]", i, j - 1);
                puts_(&b, line);
                i = j;
            }
        }
        puts_(&b, " };\n");
    }

    // the body: everything after the header line, v[] and o[] rewritten
    const char *p = strchr(s, '\n');
    p = p ? p + 1 : s + len;
    while (*p) {
        if (*p == '#') {                       // comments as they are
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p + 1) : strlen(p);
            put(&b, p, n);
            p += n;
            continue;
        }
        if ((p[0] == 'v' || p[0] == 'o') && p[1] == '[' && (p == s || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
            const char *e = strchr(p, ']');
            if (e && e - p - 2 < 16) {
                char name[20], tmp[40];
                memcpy(name, p + 2, (size_t)(e - p - 2));
                name[e - p - 2] = 0;
                puts_(&b, p[0] == 'v' ? input(name, tmp, sizeof tmp) : output(name, tmp, sizeof tmp));
                p = e + 1;
                continue;
            }
        }
        put(&b, p, 1);
        p++;
    }
    free(s);
    return b.p;
}

static const char *frag_input(const char *name, char *tmp, size_t n)
{
    if (!strcmp(name, "COL0")) return "fragment.color.primary";
    if (!strcmp(name, "COL1")) return "fragment.color.secondary";
    if (!strcmp(name, "WPOS")) return "fragment.position";
    if (!strcmp(name, "FOGC")) return "fragment.fogcoord";
    if (!strncmp(name, "TEX", 3)) snprintf(tmp, n, "fragment.texcoord[%d]", atoi(name + 3));
    else snprintf(tmp, n, "fragment.%s", name);
    return tmp;
}

char *nvfp_to_arb(const char *src, size_t len)
{
    buf b = {0};
    puts_(&b, "!!ARBfp1.0\nOPTION NV_fragment_program2;\n");
    puts_(&b, "TEMP R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13, R14, R15;\n");
    puts_(&b, "SHORT TEMP H0, H1, H2, H3, H4, H5, H6, H7, H8, H9, H10, H11, H12, H13, H14, H15;\n");
    const char *p = memchr(src, '\n', len), *end = src + len;
    p = p ? p + 1 : end;
    while (p < end) {
        if (*p == '#') {
            const char *e = memchr(p, '\n', (size_t)(end - p));
            size_t n = e ? (size_t)(e - p + 1) : (size_t)(end - p);
            put(&b, p, n);
            p += n;
            continue;
        }
        if (!strncmp(p, "DECLARE", 7) && (p == src || p[-1] == '\n')) {   // a named local constant
            puts_(&b, "PARAM");
            p += 7;
            continue;
        }
        if ((p[0] == 'f' || p[0] == 'o') && p[1] == '[' && !(isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
            const char *e = memchr(p, ']', (size_t)(end - p));
            if (e && e - p - 2 < 16) {
                char name[20], tmp[40];
                memcpy(name, p + 2, (size_t)(e - p - 2));
                name[e - p - 2] = 0;
                if (p[0] == 'f') puts_(&b, frag_input(name, tmp, sizeof tmp));
                else puts_(&b, !strcmp(name, "DEPR") ? "result.depth" : "result.color");
                p = e + 1;
                continue;
            }
        }
        // TEX0..15 as a texture unit (after the coordinate operand)
        if (!strncmp(p, "TEX", 3) && isdigit((unsigned char)p[3]) && !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '[')) {
            char tmp[24];
            int u = atoi(p + 3);
            snprintf(tmp, sizeof tmp, "texture[%d]", u);
            puts_(&b, tmp);
            p += 3;
            while (isdigit((unsigned char)*p)) p++;
            continue;
        }
        // {1}.x: a scalar constant takes no swizzle in ARB
        if (*p == '{') {
            const char *e = memchr(p, '}', (size_t)(end - p));
            if (e && e[1] == '.' && !memchr(p, ',', (size_t)(e - p))) {
                put(&b, p, (size_t)(e - p + 1));
                p = e + 2;
                while (p < end && strchr("xyzwrgba", *p)) p++;
                continue;
            }
        }
        put(&b, p, 1);
        p++;
    }
    return b.p;
}
