// libstdc++.so.6 (GCC 4.x, 32-bit) for programs that link it dynamically:
// its file and string streams, the std::string members and exception classes
// the runtime's string layer (hle_libc.c) lacked. Hummer needs them for its
// shaders, CSV tables, sprite animations and drive logs.
//
// A program inlines much of the library's templates: it builds and destroys
// ifstream, ofstream and ostringstream objects itself, with the library's
// vtables and VTTs, which it copies into its own data (R_386_COPY). There is
// no library here, so those copies are filled at load with vtables whose
// slots are the functions below. The layouts are libstdc++ 6's:
//   streambuf  0x20: vptr, get area (eback, gptr, egptr), put area (pbase,
//              pptr, epptr), locale
//   filebuf    0x88: streambuf, lock (0x20), __basic_file (0x38: the FILE,
//              here a handle), mode (0x40), ...
//   stringbuf  0x28: streambuf, mode (0x20), the string (0x24)
//   ios_base   0x70: vptr, precision (4), width (8), flags (0xc), exceptions
//              (0x10), state (0x14), ..., locale (0x6c)
//   basic_ios  0x88: ios_base, tie (0x70), fill (0x74), fill set (0x75),
//              streambuf (0x78), facets (0x7c..0x84)
//   istream: vptr, gcount, basic_ios virtual; ostream: vptr, basic_ios;
//   ifstream (0x118): istream, filebuf at 8, basic_ios at 0x90;
//   ofstream (0x114): ostream, filebuf at 4, basic_ios at 0x8c;
//   ostringstream (0xb4): ostream, stringbuf at 4, basic_ios at 0x2c.
// A stream finds its basic_ios through the offset at its vptr[-3].
// Nothing is buffered in a filebuf (its get and put areas stay empty), so
// whatever the program inlines goes through the virtual functions; a
// stringbuf's put area is its string's characters, which the program reads
// directly (str() is inlined).
#include "hle.h"
#include "host.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

enum { BADBIT = 1, EOFBIT = 2, FAILBIT = 4 };
enum { M_APP = 1, M_ATE = 2, M_BINARY = 4, M_IN = 8, M_OUT = 0x10, M_TRUNC = 0x20 };

// ---- the program's copies of the library's objects --------------------------------------------
static uint32_t copy_of(const char *name, uint32_t *size)
{
    for (unsigned i = 1; i <= prog->copy_count; i++)
        if (!strcmp(prog->copy_names[i], name)) {
            if (size) *size = prog->copy_size[i];
            return prog->copy_addr[i];
        }
    return 0;
}

// the vtable's words at the program's copy (or in host data): the words before
// the address point, then the functions; returns the address point
static uint32_t make_vtable(const char *name, const uint32_t *pre, int npre, const hle_fn *fns, const char *const *names, int nfns)
{
    uint32_t size = 0, at = name ? copy_of(name, &size) : 0;
    uint32_t need = 4u * (uint32_t)(npre + nfns);
    if (!at || size < need) at = hostdata_alloc(need);
    for (int i = 0; i < npre; i++) wr32(at + 4 * i, pre[i]);
    for (int i = 0; i < nfns; i++) wr32(at + 4 * (npre + i), fns[i] ? magic_register(fns[i], names[i]) : 0);
    return at + 4 * npre;
}

// ---- host files behind filebufs ---------------------------------------------------------------
#define MAX_SFILES 64
static FILE *sfile[MAX_SFILES];
static pthread_mutex_t sfile_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE *fb_file(uint32_t fb)
{
    uint32_t h = rd32(fb + 0x38);
    return h && h <= MAX_SFILES ? sfile[h - 1] : NULL;
}

// ---- streambufs --------------------------------------------------------------------------------
static uint32_t vt_streambuf, vt_filebuf, vt_stringbuf, vt_console;
static uint32_t console_sb[2];   // stdout, stderr
static char console_line[2][512];
static int console_n[2];

static int sb_kind(uint32_t sb)
{
    uint32_t v = sb ? rd32(sb) : 0;
    if (v && v == vt_filebuf) return 'f';
    if (v && v == vt_stringbuf) return 's';
    return 'c';
}

static void console_write(uint32_t sb, const char *p, uint32_t n)
{
    int k = sb == console_sb[1];
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == '\n' || console_n[k] == (int)sizeof console_line[k] - 1) {
            console_line[k][console_n[k]] = 0;
            if (hle_verbose) hle_log("%s: %s\n", k ? "cerr" : "cout", console_line[k]);
            console_n[k] = 0;
            if (p[i] == '\n') continue;
        }
        console_line[k][console_n[k]++] = p[i];
    }
}

// a stringbuf's put area grows in its string (a representation of its own)
static void strbuf_put(uint32_t sb, const char *p, uint32_t n)
{
    uint32_t base = rd32(sb + 0x10), cur = rd32(sb + 0x14), end = rd32(sb + 0x18);
    uint32_t len = cur && base ? cur - base : 0, cap = end && base ? end - base : 0;
    if (!base || len + n > cap) {
        uint32_t ncap = cap * 2 > len + n ? cap * 2 : len + n;
        if (ncap < 256) ncap = 256;
        uint32_t rep = guest_malloc(ncap + 1 + 12);
        wr32(rep + 4, ncap);
        wr32(rep + 8, 0);
        if (len) memmove(G(rep + 12), G(base), len);
        uint32_t old = rd32(sb + 0x24);
        base = rep + 12;
        wr32(sb + 0x24, base);
        if (old) cxx_str_release(old);
        wr32(sb + 0x18, base + ncap);
        wr32(sb + 4, base);   // the get area: what has been written, from the start
        wr32(sb + 8, base);
    }
    memmove(G(base + len), p, n);
    len += n;
    *G(base + len) = 0;
    wr32(base - 12, len);
    wr32(sb + 0x10, base);
    wr32(sb + 0x14, base + len);
    wr32(sb + 0xc, base + len);
}

static uint32_t sb_write(uint32_t sb, const char *p, uint32_t n)
{
    switch (sb_kind(sb)) {
    case 'f': {
        FILE *f = fb_file(sb);
        return f ? (uint32_t)fwrite(p, 1, n, f) : 0;
    }
    case 's':
        strbuf_put(sb, p, n);
        return n;
    default:
        console_write(sb, p, n);
        return n;
    }
}

static uint32_t sb_read(uint32_t sb, char *p, uint32_t n)
{
    switch (sb_kind(sb)) {
    case 'f': {
        FILE *f = fb_file(sb);
        return f ? (uint32_t)fread(p, 1, n, f) : 0;
    }
    case 's': {
        uint32_t cur = rd32(sb + 8), end = rd32(sb + 0xc);
        uint32_t k = cur < end ? end - cur : 0;
        if (k > n) k = n;
        memmove(p, G(cur), k);
        wr32(sb + 8, cur + k);
        return k;
    }
    default:
        return 0;
    }
}

// the next character (consumed or not), or -1
static int sb_getc(uint32_t sb, int take)
{
    if (sb_kind(sb) == 'f') {
        FILE *f = fb_file(sb);
        int ch = f ? fgetc(f) : EOF;
        if (ch != EOF && !take) ungetc(ch, f);
        return ch == EOF ? -1 : ch;
    }
    if (sb_kind(sb) == 's') {
        uint32_t cur = rd32(sb + 8);
        if (cur >= rd32(sb + 0xc)) return -1;
        if (take) wr32(sb + 8, cur + 1);
        return (uint8_t)*G(cur);
    }
    return -1;
}

static void sb_flush(uint32_t sb)
{
    if (sb_kind(sb) == 'f' && fb_file(sb)) fflush(fb_file(sb));
}

// the virtual functions (this first)
static void v_sb_dtor(CPU *c) { (void)c; }
static void v_sb_dtor_delete(CPU *c) { guest_free(ARG(0)); }
static void v_sb_imbue(CPU *c) { (void)c; }
static void v_sb_setbuf(CPU *c) { RET(ARG(0)); }
// seekoff(off (64), dir, mode) and seekpos(pos (16 bytes), mode) return an
// fpos through a hidden pointer the callee pops
static void put_fpos(CPU *c, uint32_t out, int64_t off)
{
    wr64(out, (uint64_t)off);
    wr64(out + 8, 0);
    c->r[ESP] += 4;
    RET(out);
}
static int64_t sb_seek(uint32_t sb, int64_t off, int whence)
{
    if (sb_kind(sb) == 'f') {
        FILE *f = fb_file(sb);
        if (!f || fseeko(f, (off_t)off, whence)) return -1;
        return (int64_t)ftello(f);
    }
    if (sb_kind(sb) == 's') {
        uint32_t base = rd32(sb + 4), cur = rd32(sb + 8), end = rd32(sb + 0xc);
        int64_t at = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)(cur - base) + off : (int64_t)(end - base) + off;
        if (at < 0 || at > (int64_t)(end - base)) return -1;
        wr32(sb + 8, base + (uint32_t)at);
        return at;
    }
    return -1;
}
static void v_sb_seekoff(CPU *c)
{
    static const int whence[3] = {SEEK_SET, SEEK_CUR, SEEK_END};
    uint32_t out = ARG(0), sb = ARG(1), dir = ARG(4);
    put_fpos(c, out, sb_seek(sb, (int64_t)(((uint64_t)ARG(3) << 32) | ARG(2)), dir < 3 ? whence[dir] : SEEK_SET));
}
static void v_sb_seekpos(CPU *c)
{
    uint32_t out = ARG(0), sb = ARG(1);
    put_fpos(c, out, sb_seek(sb, (int64_t)(((uint64_t)ARG(3) << 32) | ARG(2)), SEEK_SET));
}
static void v_sb_sync(CPU *c)
{
    sb_flush(ARG(0));
    RET(0);
}
static void v_sb_showmanyc(CPU *c) { RET(0); }
static void v_sb_xsgetn(CPU *c) { RET(sb_read(ARG(0), G(ARG(1)), ARG(2))); }
static void v_sb_underflow(CPU *c) { RET((uint32_t)sb_getc(ARG(0), 0)); }
static void v_sb_uflow(CPU *c) { RET((uint32_t)sb_getc(ARG(0), 1)); }
static void v_sb_pbackfail(CPU *c)
{
    uint32_t sb = ARG(0);
    int ch = (int)ARG(1);
    FILE *f = sb_kind(sb) == 'f' ? fb_file(sb) : NULL;
    if (f && ch != -1 && ungetc(ch, f) != EOF) RET((uint32_t)ch);
    if (f && ch == -1 && !fseeko(f, -1, SEEK_CUR)) RET(0);
    RET(0xffffffffu);
}
static void v_sb_xsputn(CPU *c) { RET(sb_write(ARG(0), G(ARG(1)), ARG(2))); }
static void v_sb_overflow(CPU *c)
{
    int ch = (int)ARG(1);
    if (ch == -1) RET(0);
    char b = (char)ch;
    RET(sb_write(ARG(0), &b, 1) == 1 ? (uint32_t)(uint8_t)b : 0xffffffffu);
}
static int filebuf_close(uint32_t fb);
static void v_fb_dtor(CPU *c) { filebuf_close(ARG(0)); }
static void v_fb_dtor_delete(CPU *c)
{
    filebuf_close(ARG(0));
    guest_free(ARG(0));
}
static void v_strbuf_dtor(CPU *c)
{
    uint32_t s = rd32(ARG(0) + 0x24);
    if (s) cxx_str_release(s);
    wr32(ARG(0) + 0x24, 0);
}
static void v_strbuf_dtor_delete(CPU *c)
{
    v_strbuf_dtor(c);
    guest_free(ARG(0));
}

static const char *const sb_names[14] = {
    "streambuf::~streambuf", "streambuf::~streambuf (delete)", "streambuf::imbue", "streambuf::setbuf",
    "streambuf::seekoff", "streambuf::seekpos", "streambuf::sync", "streambuf::showmanyc", "streambuf::xsgetn",
    "streambuf::underflow", "streambuf::uflow", "streambuf::pbackfail", "streambuf::xsputn", "streambuf::overflow"};

static uint32_t streambuf_vtable(const char *name, hle_fn d1, hle_fn d0)
{
    const uint32_t pre[2] = {0, 0};
    const hle_fn fns[14] = {d1, d0, v_sb_imbue, v_sb_setbuf, v_sb_seekoff, v_sb_seekpos, v_sb_sync, v_sb_showmanyc,
                            v_sb_xsgetn, v_sb_underflow, v_sb_uflow, v_sb_pbackfail, v_sb_xsputn, v_sb_overflow};
    return make_vtable(name, pre, 2, fns, sb_names, 14);
}

// ---- filebuf ----------------------------------------------------------------------------------
static void filebuf_init(uint32_t fb)
{
    memset(G(fb), 0, 0x88);
    wr32(fb, vt_filebuf);
}

static int filebuf_close(uint32_t fb)
{
    uint32_t h = rd32(fb + 0x38);
    if (!h || h > MAX_SFILES) return 0;
    pthread_mutex_lock(&sfile_lock);
    if (sfile[h - 1]) fclose(sfile[h - 1]);
    sfile[h - 1] = NULL;
    pthread_mutex_unlock(&sfile_lock);
    wr32(fb + 0x38, 0);
    wr8(fb + 0x3c, 0);
    return 1;
}

// open(name, mode): as fopen, with libstdc++'s table of modes; this or 0
static uint32_t filebuf_open(uint32_t fb, const char *name, uint32_t mode)
{
    if (rd32(fb + 0x38)) return 0;
    const char *m;
    switch (mode & (M_IN | M_OUT | M_TRUNC | M_APP)) {
    case M_OUT: case M_OUT | M_TRUNC: m = "w"; break;
    case M_OUT | M_APP: case M_APP: m = "a"; break;
    case M_IN: m = "r"; break;
    case M_IN | M_OUT: m = "r+"; break;
    case M_IN | M_OUT | M_TRUNC: m = "w+"; break;
    case M_IN | M_OUT | M_APP: case M_IN | M_APP: m = "a+"; break;
    default: return 0;
    }
    char buf[2048], fm[4];
    snprintf(fm, sizeof fm, "%s%s", m, mode & M_BINARY ? "b" : "");
    if (host_device_hidden(name)) return 0;
    FILE *f = fopen(map_path(name, buf, sizeof buf), fm);
    if (hle_verbose) hle_log("filebuf open %s %s -> %s\n", name, fm, f ? "ok" : "fail");
    if (!f) return 0;
    if ((mode & M_ATE) && fseeko(f, 0, SEEK_END)) {
        fclose(f);
        return 0;
    }
    pthread_mutex_lock(&sfile_lock);
    int i;
    for (i = 0; i < MAX_SFILES && sfile[i]; i++);
    if (i < MAX_SFILES) sfile[i] = f;
    pthread_mutex_unlock(&sfile_lock);
    if (i == MAX_SFILES) {
        fclose(f);
        return 0;
    }
    wr32(fb + 0x38, (uint32_t)i + 1);
    wr8(fb + 0x3c, 1);
    wr32(fb + 0x40, mode);
    return fb;
}

static void h_filebuf_ctor(CPU *c) { filebuf_init(ARG(0)); }
static void h_filebuf_open(CPU *c) { RET(filebuf_open(ARG(0), G(ARG(1)), ARG(2))); }
static void h_filebuf_close(CPU *c) { RET(filebuf_close(ARG(0)) ? ARG(0) : 0); }
// __basic_file<char> is the filebuf's member at 0x38
static void h_basic_file_is_open(CPU *c) { RET(rd32(ARG(0)) != 0); }
static void h_basic_file_dtor(CPU *c) { filebuf_close(ARG(0) - 0x38); }

// ---- ios_base, basic_ios ---------------------------------------------------------------------
// a stream's basic_ios, or 0 for a stream that was never built (a program
// that writes to cout before anything sets it up)
static uint32_t ios_of(uint32_t s)
{
    uint32_t v = s ? rd32(s) : 0;
    if (v < 0x08000000u) return 0;
    return s + rd32(v - 12);
}
static uint32_t ios_sb(uint32_t ios) { return ios ? rd32(ios + 0x78) : 0; }
static void ios_setstate(uint32_t ios, uint32_t bits)
{
    if (ios) wr32(ios + 0x14, rd32(ios + 0x14) | bits);
}
static uint32_t ios_state(uint32_t ios) { return ios ? rd32(ios + 0x14) : 0; }

static void ios_base_init(uint32_t b)
{
    memset(G(b + 4), 0, 0x6c);
    wr32(b + 0x64, 8);          // the local words
    wr32(b + 0x68, b + 0x24);
}
// basic_ios::init(streambuf)
static void basic_ios_init(uint32_t ios, uint32_t sb)
{
    wr32(ios + 4, 6);           // precision
    wr32(ios + 8, 0);
    wr32(ios + 0xc, 0x1002);    // skipws | dec
    wr32(ios + 0x10, 0);
    wr32(ios + 0x14, sb ? 0 : BADBIT);
    wr32(ios + 0x70, 0);
    wr8(ios + 0x74, ' ');
    wr8(ios + 0x75, 1);
    wr32(ios + 0x78, sb);
}
static void h_ios_base_ctor(CPU *c) { ios_base_init(ARG(0)); }
static void h_ios_base_dtor(CPU *c) { (void)c; }
static void h_basic_ios_init(CPU *c) { basic_ios_init(ARG(0), ARG(1)); }
static void h_basic_ios_clear(CPU *c)
{
    uint32_t ios = ARG(0);
    wr32(ios + 0x14, ARG(1) | (rd32(ios + 0x78) ? 0 : BADBIT));
}
static void h_basic_ios_widen(CPU *c) { RET(ARG(1) & 0xff); }

// ---- ostream ------------------------------------------------------------------------------------
static void out_write(uint32_t s, const char *p, uint32_t n)
{
    uint32_t ios = ios_of(s);
    if (!ios) {
        console_write(console_sb[0], p, n);
        return;
    }
    uint32_t sb = ios_sb(ios);
    if (!sb || ios_state(ios)) {
        ios_setstate(ios, BADBIT);
        return;
    }
    if (sb_write(sb, p, n) != n) ios_setstate(ios, BADBIT);
}
// a formatted field: padded to the stream's width with its fill, the width then 0
static void out_field(uint32_t s, const char *p, uint32_t n)
{
    uint32_t ios = ios_of(s), w = ios ? rd32(ios + 8) : 0;
    if (w > n) {
        char pad[256];
        uint32_t k = w - n < sizeof pad ? w - n : sizeof pad;
        memset(pad, ios ? *G(ios + 0x74) : ' ', k);
        if (rd32(ios + 0xc) & 0x20) {   // left
            out_write(s, p, n);
            out_write(s, pad, k);
        } else {
            out_write(s, pad, k);
            out_write(s, p, n);
        }
    } else
        out_write(s, p, n);
    if (ios) wr32(ios + 8, 0);
}
static void out_integer(uint32_t s, int64_t v, int is_signed)
{
    uint32_t ios = ios_of(s), fl = ios ? rd32(ios + 0xc) : 2;
    char b[48];
    const char *pre = "";
    if (fl & 0x200) pre = (fl & 8) ? ((fl & 0x4000) ? "0X" : "0x") : (fl & 0x40) ? "0" : "";   // showbase
    if (fl & 8) snprintf(b, sizeof b, (fl & 0x4000) ? "%s%llX" : "%s%llx", pre, (unsigned long long)(is_signed ? (uint32_t)v : (uint64_t)v));
    else if (fl & 0x40) snprintf(b, sizeof b, "%s%llo", pre, (unsigned long long)(is_signed ? (uint32_t)v : (uint64_t)v));
    else if (is_signed) snprintf(b, sizeof b, (fl & 0x800) ? "%+lld" : "%lld", (long long)v);
    else snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    out_field(s, b, (uint32_t)strlen(b));
}
static void h_os_int(CPU *c) { out_integer(ARG(0), (int32_t)ARG(1), 1); RET(ARG(0)); }
static void h_os_ulong(CPU *c) { out_integer(ARG(0), ARG(1), 0); RET(ARG(0)); }
static void h_os_bool(CPU *c)
{
    uint32_t ios = ios_of(ARG(0));
    if (ios && (rd32(ios + 0xc) & 1)) {   // boolalpha
        const char *t = (ARG(1) & 0xff) ? "true" : "false";
        out_field(ARG(0), t, (uint32_t)strlen(t));
    } else
        out_integer(ARG(0), ARG(1) & 0xff, 1);
    RET(ARG(0));
}
static void h_os_double(CPU *c)
{
    uint32_t s = ARG(0), ios = ios_of(s), fl = ios ? rd32(ios + 0xc) : 0;
    int prec = ios ? (int)rd32(ios + 4) : 6;
    double d = ARGD(1);
    char b[512];
    if ((fl & 0x104) == 4) snprintf(b, sizeof b, "%.*f", prec, d);              // fixed
    else if ((fl & 0x104) == 0x100) snprintf(b, sizeof b, "%.*e", prec, d);     // scientific
    else snprintf(b, sizeof b, "%.*g", prec ? prec : 1, d);
    out_field(s, b, (uint32_t)strlen(b));
    RET(s);
}
static void h_os_cstr(CPU *c)
{
    if (!ARG(1)) ios_setstate(ios_of(ARG(0)), BADBIT);
    else out_field(ARG(0), G(ARG(1)), (uint32_t)strlen(G(ARG(1))));
    RET(ARG(0));
}
static void h_os_string(CPU *c)
{
    uint32_t chars = rd32(ARG(1));
    out_field(ARG(0), G(chars), rd32(chars - 12));
    RET(ARG(0));
}
static void h_os_write(CPU *c)
{
    out_write(ARG(0), G(ARG(1)), ARG(2));
    RET(ARG(0));
}
static void h_os_endl(CPU *c)
{
    out_write(ARG(0), "\n", 1);
    sb_flush(ios_sb(ios_of(ARG(0))));
    RET(ARG(0));
}
// << streambuf*: everything the other streambuf has
static void h_os_streambuf(CPU *c)
{
    uint32_t s = ARG(0), from = ARG(1), total = 0;
    char buf[4096];
    if (!from) {
        ios_setstate(ios_of(s), BADBIT);
        RET(s);
    }
    for (;;) {
        uint32_t n = sb_read(from, buf, sizeof buf);
        if (!n) break;
        out_write(s, buf, n);
        total += n;
    }
    if (!total) ios_setstate(ios_of(s), FAILBIT);
    RET(s);
}

// ---- istream -------------------------------------------------------------------------------------
static void h_is_read(CPU *c)
{
    uint32_t s = ARG(0), ios = ios_of(s), n = ARG(2), got = 0;
    if (ios && !ios_state(ios)) {
        got = sb_read(ios_sb(ios), G(ARG(1)), n);
        if (got < n) ios_setstate(ios, EOFBIT | FAILBIT);
    } else
        ios_setstate(ios, FAILBIT);
    wr32(s + 4, got);
    RET(s);
}
// seekg(fpos (16 bytes by value)), seekg(off (64), dir): the end-of-file flag
// stays as it was, as in libstdc++ before C++11
static void seekg(uint32_t s, int64_t off, int whence)
{
    uint32_t ios = ios_of(s);
    if (!ios || (ios_state(ios) & (BADBIT | FAILBIT))) return;
    if (sb_seek(ios_sb(ios), off, whence) < 0) ios_setstate(ios, FAILBIT);
}
static void h_is_seekg_pos(CPU *c)
{
    seekg(ARG(0), (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1)), SEEK_SET);
    RET(ARG(0));
}
static void h_is_seekg_off(CPU *c)
{
    static const int whence[3] = {SEEK_SET, SEEK_CUR, SEEK_END};
    seekg(ARG(0), (int64_t)(((uint64_t)ARG(2) << 32) | ARG(1)), ARG(3) < 3 ? whence[ARG(3)] : SEEK_SET);
    RET(ARG(0));
}
// tellg(): an fpos through a hidden pointer
static void h_is_tellg(CPU *c)
{
    uint32_t out = ARG(0), ios = ios_of(ARG(1));
    int64_t at = -1;
    if (ios && !(ios_state(ios) & (BADBIT | FAILBIT))) at = sb_seek(ios_sb(ios), 0, SEEK_CUR);
    put_fpos(c, out, at);
}
// getline(istream &, string &, delimiter)
static void h_getline(CPU *c)
{
    uint32_t s = ARG(0), str = ARG(1), ios = ios_of(s), sb = ios_sb(ios);
    int delim = (int)(ARG(2) & 0xff);
    uint32_t n = 0, cap = 256;
    char *b = malloc(cap);
    int ch = -1, took = 0;
    if (ios && !ios_state(ios))
        while ((ch = sb_getc(sb, 1)) != -1) {
            took++;
            if (ch == delim) break;
            if (n + 1 >= cap) b = realloc(b, cap *= 2);
            b[n++] = (char)ch;
        }
    uint32_t old = rd32(str);
    wr32(str, cxx_str_new(b, n));
    cxx_str_release(old);
    free(b);
    if (ch == -1) ios_setstate(ios, EOFBIT);
    if (!took) ios_setstate(ios, FAILBIT);
    if (ios) wr32(s + 4, (uint32_t)took);
    RET(s);
}

// ---- the stream classes -----------------------------------------------------------------------------
// vtables: {vbase offset, 0, typeinfo, D1, D0} and the basic_ios part
// {vcall offset, -vbase, typeinfo, D1 thunk, D0 thunk}
static uint32_t vt_ifstream, vt_ofstream, vt_ostrstream, vt_ostream, vt_ios;

// the stream a destructor's this belongs to: as called, or through the basic_ios
static uint32_t stream_full(uint32_t p) { return p; }
static uint32_t stream_from_ios(uint32_t ios) { return ios + rd32(rd32(ios) - 12); }
static void close_stream_buf(uint32_t s)
{
    uint32_t sb = ios_sb(ios_of(s));
    if (sb_kind(sb) == 'f') filebuf_close(sb);
    else if (sb_kind(sb) == 's') {
        uint32_t str = rd32(sb + 0x24);
        if (str) cxx_str_release(str);
        wr32(sb + 0x24, 0);
    }
}
static void v_stream_dtor(CPU *c) { close_stream_buf(stream_full(ARG(0))); }
static void v_stream_dtor_delete(CPU *c)
{
    close_stream_buf(ARG(0));
    guest_free(ARG(0));
}
static void v_stream_dtor_thunk(CPU *c) { close_stream_buf(stream_from_ios(ARG(0))); }
static void v_stream_dtor_delete_thunk(CPU *c)
{
    uint32_t s = stream_from_ios(ARG(0));
    close_stream_buf(s);
    guest_free(s);
}
static void v_nop(CPU *c) { (void)c; }

static uint32_t stream_vtable(const char *name, uint32_t vbase, uint32_t *ios_point)
{
    static const char *const names[5] = {"stream::~stream", "stream::~stream (delete)", NULL,
                                         "stream::~stream (virtual thunk)", "stream::~stream (delete, virtual thunk)"};
    const uint32_t pre[3] = {vbase, 0, 0};
    // ten words: the primary part, then the basic_ios part's vcall offset,
    // offset to top and typeinfo as three "functions" written over below
    const hle_fn fns[7] = {v_stream_dtor, v_stream_dtor_delete, NULL, NULL, NULL, v_stream_dtor_thunk, v_stream_dtor_delete_thunk};
    const char *const fn_names[7] = {names[0], names[1], NULL, NULL, NULL, names[3], names[4]};
    uint32_t at = make_vtable(name, pre, 3, fns, fn_names, 7);
    wr32(at + 8, (uint32_t)-(int32_t)vbase);    // vcall offset: back to the stream
    wr32(at + 12, (uint32_t)-(int32_t)vbase);   // offset to top
    wr32(at + 16, 0);
    *ios_point = at + 20;
    return at;
}
static uint32_t ios_point_ifstream, ios_point_ofstream, ios_point_ostrstream, ios_point_ostream;

// a VTT: the stream's vptr, the construction vtables (the same here), its basic_ios's
static void fill_vtt(const char *name, uint32_t vt, uint32_t ios_vt)
{
    uint32_t size = 0, at = copy_of(name, &size);
    if (!at || size < 16) return;
    wr32(at, vt);
    wr32(at + 4, vt);
    wr32(at + 8, ios_vt);
    wr32(at + 12, ios_vt);
}

// basic_ofstream(name, mode): the whole object, built here (0x114 bytes)
static void h_ofstream_ctor(CPU *c)
{
    uint32_t s = ARG(0), ios = s + 0x8c, fb = s + 4;
    ios_base_init(ios);
    wr32(s, vt_ofstream);
    wr32(ios, ios_point_ofstream);
    filebuf_init(fb);
    basic_ios_init(ios, fb);
    if (!filebuf_open(fb, G(ARG(1)), ARG(2) | M_OUT)) ios_setstate(ios, FAILBIT);
}

// cout and cerr: ostreams writing to the log
static void make_console(const char *name, int k)
{
    uint32_t size = 0, s = copy_of(name, &size);
    console_sb[k] = hostdata_alloc(0x20);
    wr32(console_sb[k], vt_console);
    if (!s || size < 0x8c) return;
    wr32(s, vt_ostream);
    ios_base_init(s + 4);
    wr32(s + 4, ios_point_ostream);
    basic_ios_init(s + 4, console_sb[k]);
}

// ---- strings --------------------------------------------------------------------------------------
static uint32_t slen(uint32_t str) { return rd32(rd32(str) - 12); }
static const char *sdat(uint32_t str) { return G(rd32(str)); }
// the string's characters replaced by n from p
static void sset(uint32_t str, const char *p, uint32_t n)
{
    char *tmp = malloc(n + 1);
    memcpy(tmp, p, n);
    uint32_t old = rd32(str);
    wr32(str, cxx_str_new(tmp, n));
    cxx_str_release(old);
    free(tmp);
}
#define NPOS 0xffffffffu
static void h_str_find_first_of(CPU *c)   // (s, pos, n)
{
    uint32_t len = slen(ARG(0)), n = ARG(3);
    const char *d = sdat(ARG(0)), *set = G(ARG(1));
    for (uint32_t i = ARG(2); i < len && n; i++)
        if (memchr(set, d[i], n)) RET(i);
    RET(NPOS);
}
static void h_str_find_first_not_of_char(CPU *c)   // (ch, pos)
{
    uint32_t len = slen(ARG(0));
    const char *d = sdat(ARG(0));
    for (uint32_t i = ARG(2); i < len; i++)
        if (d[i] != (char)ARG(1)) RET(i);
    RET(NPOS);
}
static void h_str_find_n(CPU *c)   // (s, pos, n)
{
    uint32_t len = slen(ARG(0)), pos = ARG(2), n = ARG(3);
    const char *d = sdat(ARG(0)), *w = G(ARG(1));
    if (!n) RET(pos <= len ? pos : NPOS);
    for (uint32_t i = pos; n <= len && i <= len - n; i++)
        if (!memcmp(d + i, w, n)) RET(i);
    RET(NPOS);
}
static void h_str_find_char(CPU *c)   // (ch, pos)
{
    uint32_t len = slen(ARG(0));
    const char *d = sdat(ARG(0));
    for (uint32_t i = ARG(2); i < len; i++)
        if (d[i] == (char)ARG(1)) RET(i);
    RET(NPOS);
}
static void h_str_rfind_char(CPU *c)   // (ch, pos)
{
    uint32_t len = slen(ARG(0));
    const char *d = sdat(ARG(0));
    if (!len) RET(NPOS);
    for (uint32_t i = ARG(2) < len - 1 ? ARG(2) : len - 1;; i--) {
        if (d[i] == (char)ARG(1)) RET(i);
        if (!i) break;
    }
    RET(NPOS);
}
static void h_str_compare_cstr(CPU *c)
{
    uint32_t len = slen(ARG(0));
    const char *w = G(ARG(1));
    size_t k = strlen(w);
    int r = memcmp(sdat(ARG(0)), w, len < k ? len : k);
    if (!r) r = len < k ? -1 : len > k ? 1 : 0;
    RET((uint32_t)r);
}
// a string about to be written through its characters: its own, unshared,
// and not to be shared again
static void h_str_leak_hard(CPU *c)
{
    uint32_t str = ARG(0);
    sset(str, sdat(str), slen(str));
    wr32(rd32(str) - 4, (uint32_t)-1);
}
// _S_construct(n, ch, allocator): the characters of a new string
static void h_str_s_construct(CPU *c)
{
    uint32_t n = ARG(0);
    char *tmp = malloc(n + 1);
    memset(tmp, (int)(ARG(1) & 0xff), n);
    uint32_t r = n ? cxx_str_new(tmp, n) : cxx_str_empty();
    free(tmp);
    RET(r);
}
static void h_str_assign_n(CPU *c)
{
    sset(ARG(0), G(ARG(1)), ARG(2));
    RET(ARG(0));
}
static void h_str_assign(CPU *c)
{
    uint32_t self = ARG(0), other = ARG(1);
    if (rd32(self) != rd32(other)) {
        uint32_t old = rd32(self);
        wr32(self, cxx_str_grab(other));
        cxx_str_release(old);
    }
    RET(self);
}
// _M_mutate(pos, len1, len2): len1 characters at pos make room for len2 (left
// for the caller to write), in an unshared representation
static void h_str_mutate(CPU *c)
{
    uint32_t str = ARG(0), pos = ARG(1), n1 = ARG(2), n2 = ARG(3), len = slen(str);
    if (pos > len) pos = len;
    if (n1 > len - pos) n1 = len - pos;
    uint32_t nlen = len - n1 + n2;
    char *tmp = calloc(1, nlen + 1);
    memcpy(tmp, sdat(str), pos);
    memcpy(tmp + pos + n2, sdat(str) + pos + n1, len - pos - n1);
    sset(str, tmp, nlen);
    free(tmp);
}
static void h_str_ctor_sub(CPU *c)   // (const string &, pos, n)
{
    uint32_t len = slen(ARG(1)), pos = ARG(2), n = ARG(3);
    if (pos > len) host_fatal("std::string: position out of range");
    if (n > len - pos) n = len - pos;
    wr32(ARG(0), cxx_str_new(sdat(ARG(1)) + pos, n));
}
static void h_str_ctor_fill(CPU *c)   // (n, ch, allocator)
{
    uint32_t n = ARG(1);
    char *tmp = malloc(n + 1);
    memset(tmp, (int)(ARG(2) & 0xff), n);
    wr32(ARG(0), cxx_str_new(tmp, n));
    free(tmp);
}

// ---- exceptions ---------------------------------------------------------------------------------
// logic_error, runtime_error and the like: {vptr, the message}
static uint32_t vt_runtime_error;
static void h_exception_dtor(CPU *c) { (void)c; }
static void h_exception_what(CPU *c) { RET(guest_strdup("std::exception")); }
static void h_error_what(CPU *c) { RET(rd32(ARG(0) + 4)); }
static void h_error_dtor(CPU *c)
{
    uint32_t m = rd32(ARG(0) + 4);
    if (m) cxx_str_release(m);
    wr32(ARG(0) + 4, 0);
}
static void h_error_dtor_delete(CPU *c)
{
    h_error_dtor(c);
    guest_free(ARG(0));
}
static void h_runtime_error_ctor(CPU *c)
{
    wr32(ARG(0), vt_runtime_error);
    wr32(ARG(0) + 4, cxx_str_grab(ARG(1)));
}
static void h_out_of_range_ctor(CPU *c)
{
    uint32_t vt = guest_export("_ZTVSt12out_of_range");
    wr32(ARG(0), vt ? vt + 8 : vt_runtime_error);
    wr32(ARG(0) + 4, cxx_str_grab(ARG(1)));
}
static void h_cxa_free_exception(CPU *c) { guest_free(ARG(0) - 128); }

// ---- C library odds and ends Hummer imports -----------------------------------------------------
static void h_bzero(CPU *c) { memset(G(ARG(0)), 0, ARG(1)); }
// setjmp: the first return (nothing longjmps back in what is translated)
static void h_setjmp(CPU *c) { RET(0); }

const hle_entry cxx_table[] = {
    {"_ZNSt8ios_baseC2Ev", h_ios_base_ctor}, {"_ZNSt8ios_baseD2Ev", h_ios_base_dtor},
    {"_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E", h_basic_ios_init},
    {"_ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate", h_basic_ios_clear},
    {"_ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc", h_basic_ios_widen},
    {"_ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev", h_filebuf_ctor},
    {"_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode", h_filebuf_open},
    {"_ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv", h_filebuf_close},
    {"_ZNKSt12__basic_fileIcE7is_openEv", h_basic_file_is_open}, {"_ZNSt12__basic_fileIcED1Ev", h_basic_file_dtor},
    {"_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode", h_ofstream_ctor},
    {"_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc", h_os_cstr},
    {"_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKh", h_os_cstr},
    {"_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E", h_os_string},
    {"_ZNSolsEi", h_os_int}, {"_ZNSolsEl", h_os_int}, {"_ZNSolsEm", h_os_ulong}, {"_ZNSolsEj", h_os_ulong},
    {"_ZNSolsEb", h_os_bool}, {"_ZNSolsEd", h_os_double},
    {"_ZNSolsEPSt15basic_streambufIcSt11char_traitsIcEE", h_os_streambuf},
    {"_ZNSo5writeEPKci", h_os_write}, {"_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_", h_os_endl},
    {"_ZNSi4readEPci", h_is_read}, {"_ZNSi5seekgESt4fposI11__mbstate_tE", h_is_seekg_pos},
    {"_ZNSi5seekgExSt12_Ios_Seekdir", h_is_seekg_off}, {"_ZNSi5tellgEv", h_is_tellg},
    {"_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_", h_getline},
    {"_ZNKSs13find_first_ofEPKcjj", h_str_find_first_of}, {"_ZNKSs17find_first_not_ofEcj", h_str_find_first_not_of_char},
    {"_ZNKSs4findEPKcjj", h_str_find_n}, {"_ZNKSs4findEcj", h_str_find_char}, {"_ZNKSs5rfindEcj", h_str_rfind_char},
    {"_ZNKSs7compareEPKc", h_str_compare_cstr}, {"_ZNSs12_M_leak_hardEv", h_str_leak_hard},
    {"_ZNSs12_S_constructEjcRKSaIcE", h_str_s_construct}, {"_ZNSs6assignEPKcj", h_str_assign_n},
    {"_ZNSs6assignERKSs", h_str_assign}, {"_ZNSs9_M_mutateEjjj", h_str_mutate},
    {"_ZNSsC1ERKSsjj", h_str_ctor_sub}, {"_ZNSsC1EjcRKSaIcE", h_str_ctor_fill},
    {"_ZNSt9exceptionD2Ev", h_exception_dtor}, {"_ZNKSt9exception4whatEv", h_exception_what},
    {"_ZNKSt11logic_error4whatEv", h_error_what}, {"_ZNKSt13runtime_error4whatEv", h_error_what},
    {"_ZNSt11logic_errorD2Ev", h_error_dtor}, {"_ZNSt13runtime_errorD2Ev", h_error_dtor},
    {"_ZNSt13runtime_errorC2ERKSs", h_runtime_error_ctor}, {"_ZNSt12out_of_rangeC2ERKSs", h_out_of_range_ctor},
    {"__cxa_free_exception", h_cxa_free_exception}, {"bzero", h_bzero}, {"_setjmp", h_setjmp},
    {NULL, NULL},
};

void hle_cxx_init(void)
{
    memset(console_n, 0, sizeof console_n);
    pthread_mutex_lock(&sfile_lock);
    for (int i = 0; i < MAX_SFILES; i++) {
        if (sfile[i]) fclose(sfile[i]);
        sfile[i] = NULL;
    }
    pthread_mutex_unlock(&sfile_lock);
    // the library's constants
    uint32_t a;
    if ((a = copy_of("_ZNSs4_Rep11_S_max_sizeE", NULL))) wr32(a, 0x3ffffffc);
    if ((a = copy_of("_ZNSs4_Rep11_S_terminalE", NULL))) wr8(a, 0);
    // streambufs
    vt_streambuf = streambuf_vtable("_ZTVSt15basic_streambufIcSt11char_traitsIcEE", v_sb_dtor, v_sb_dtor_delete);
    vt_filebuf = streambuf_vtable("_ZTVSt13basic_filebufIcSt11char_traitsIcEE", v_fb_dtor, v_fb_dtor_delete);
    vt_stringbuf = streambuf_vtable("_ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE", v_strbuf_dtor, v_strbuf_dtor_delete);
    vt_console = streambuf_vtable(NULL, v_sb_dtor, v_nop);
    // basic_ios: {0, typeinfo, D1, D0}
    {
        static const char *const n[2] = {"basic_ios::~basic_ios", "basic_ios::~basic_ios (delete)"};
        const uint32_t pre[2] = {0, 0};
        const hle_fn fns[2] = {v_nop, v_nop};
        vt_ios = make_vtable("_ZTVSt9basic_iosIcSt11char_traitsIcEE", pre, 2, fns, n, 2);
    }
    // the streams and their VTTs
    vt_ifstream = stream_vtable("_ZTVSt14basic_ifstreamIcSt11char_traitsIcEE", 0x90, &ios_point_ifstream);
    vt_ofstream = stream_vtable("_ZTVSt14basic_ofstreamIcSt11char_traitsIcEE", 0x8c, &ios_point_ofstream);
    vt_ostrstream = stream_vtable("_ZTVSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE", 0x2c, &ios_point_ostrstream);
    vt_ostream = stream_vtable(NULL, 4, &ios_point_ostream);
    fill_vtt("_ZTTSt14basic_ifstreamIcSt11char_traitsIcEE", vt_ifstream, ios_point_ifstream);
    fill_vtt("_ZTTSt14basic_ofstreamIcSt11char_traitsIcEE", vt_ofstream, ios_point_ofstream);
    fill_vtt("_ZTTSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE", vt_ostrstream, ios_point_ostrstream);
    make_console("_ZSt4cout", 0);
    make_console("_ZSt4cerr", 1);
    // runtime_error: {0, typeinfo, D1, D0, what}
    {
        static const char *const n[3] = {"runtime_error::~runtime_error", "runtime_error::~runtime_error (delete)",
                                         "runtime_error::what"};
        const uint32_t pre[2] = {0, 0};
        const hle_fn fns[3] = {h_error_dtor, h_error_dtor_delete, h_error_what};
        vt_runtime_error = make_vtable("_ZTVSt13runtime_error", pre, 2, fns, n, 3);
    }
}
