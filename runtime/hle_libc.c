// glibc 2.3, libm, libgcc and libstdc++.so.5 functions, on guest memory.
#include "hle.h"
#include "host.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <string.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ---- formatted I/O ------------------------------------------------------------
// Walk the guest format, take each argument from guest varargs at its i386
// size, and format it with the host printf one conversion at a time.
int guest_vformat(char *out, size_t n, const char *fmt, uint32_t ap)
{
    size_t o = 0;
    char spec[64], piece[4096];
#define PUT(s, len) do { size_t _l = (len); for (size_t _i = 0; _i < _l; _i++) { if (o + 1 < n) out[o] = (s)[_i]; o++; } } while (0)
    while (*fmt) {
        if (*fmt != '%') {
            PUT(fmt, 1);
            fmt++;
            continue;
        }
        const char *st = fmt++;
        if (*fmt == '%') {
            PUT("%", 1);
            fmt++;
            continue;
        }
        while (strchr("-+ #0'", *fmt)) fmt++;
        int star_w = 0, star_p = 0;
        if (*fmt == '*') { star_w = 1; fmt++; } else while (isdigit((unsigned char)*fmt)) fmt++;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') { star_p = 1; fmt++; } else while (isdigit((unsigned char)*fmt)) fmt++;
        }
        int lng = 0, half = 0, ld = 0;
        for (;;) {
            if (*fmt == 'l') { lng++; fmt++; }
            else if (*fmt == 'h') { half++; fmt++; }
            else if (*fmt == 'L' || *fmt == 'q') { ld = 1; lng = 2; fmt++; }
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') { fmt++; }
            else break;
        }
        char conv = *fmt ? *fmt++ : 0;
        // rebuild the spec without length modifiers, then add the host ones
        size_t sl = 0;
        for (const char *p = st; p < fmt - 1 && sl < sizeof spec - 4; p++)
            if (!strchr("lhLqztj", *p)) spec[sl++] = *p;
        int w = 0, pr = 0;
        if (star_w) { w = (int32_t)rd32(ap); ap += 4; }
        if (star_p) { pr = (int32_t)rd32(ap); ap += 4; }
        int len = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (lng >= 2) {
                spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
                long long v = (long long)rd64(ap);
                ap += 8;
                if (star_w && star_p) len = snprintf(piece, sizeof piece, spec, w, pr, v);
                else if (star_w) len = snprintf(piece, sizeof piece, spec, w, v);
                else if (star_p) len = snprintf(piece, sizeof piece, spec, pr, v);
                else len = snprintf(piece, sizeof piece, spec, v);
            } else {
                spec[sl++] = conv; spec[sl] = 0;
                int v = (int)rd32(ap);
                ap += 4;
                if (half == 1 && conv != 'c') v = strchr("di", conv) ? (int)(int16_t)v : (int)(uint16_t)v;
                if (half == 2) v = strchr("di", conv) ? (int)(int8_t)v : (int)(uint8_t)v;
                if (star_w && star_p) len = snprintf(piece, sizeof piece, spec, w, pr, v);
                else if (star_w) len = snprintf(piece, sizeof piece, spec, w, v);
                else if (star_p) len = snprintf(piece, sizeof piece, spec, pr, v);
                else len = snprintf(piece, sizeof piece, spec, v);
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            spec[sl++] = conv; spec[sl] = 0;
            double v;
            if (ld) { v = x87_load80(ap); ap += 12; }
            else { v = u2d(rd64(ap)); ap += 8; }
            if (star_w && star_p) len = snprintf(piece, sizeof piece, spec, w, pr, v);
            else if (star_w) len = snprintf(piece, sizeof piece, spec, w, v);
            else if (star_p) len = snprintf(piece, sizeof piece, spec, pr, v);
            else len = snprintf(piece, sizeof piece, spec, v);
            break;
        }
        case 's': {
            spec[sl++] = 's'; spec[sl] = 0;
            uint32_t p = rd32(ap);
            ap += 4;
            const char *s = p ? G(p) : "(null)";
            if (star_w && star_p) len = snprintf(piece, sizeof piece, spec, w, pr, s);
            else if (star_w) len = snprintf(piece, sizeof piece, spec, w, s);
            else if (star_p) len = snprintf(piece, sizeof piece, spec, pr, s);
            else len = snprintf(piece, sizeof piece, spec, s);
            break;
        }
        case 'p':
            len = snprintf(piece, sizeof piece, "0x%x", rd32(ap));
            ap += 4;
            break;
        case 'n':
            wr32(rd32(ap), (uint32_t)o);
            ap += 4;
            break;
        default:
            len = 0;
            break;
        }
        if (len > (int)sizeof piece - 1) len = sizeof piece - 1;
        if (len > 0) PUT(piece, (size_t)len);
    }
    if (n) out[o < n ? o : n - 1] = 0;
    return (int)o;
#undef PUT
}

// sscanf: literal text and whitespace are matched here, each conversion by
// the host sscanf with %n to learn how much it consumed.
static int guest_vscan(const char *str, const char *fmt, uint32_t ap)
{
    const char *s = str;
    int count = 0;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            fmt++;
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (*s != *fmt) break;
            s++;
            fmt++;
            continue;
        }
        const char *st = fmt++;
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        while (isdigit((unsigned char)*fmt)) fmt++;
        int lng = 0, half = 0;
        for (;;) {
            if (*fmt == 'l') { lng++; fmt++; }
            else if (*fmt == 'h') { half++; fmt++; }
            else if (*fmt == 'L' || *fmt == 'q') { lng = 2; fmt++; }
            else break;
        }
        char conv = *fmt;
        const char *cend = fmt + 1;
        if (conv == '[') {
            cend = fmt + 1;
            if (*cend == '^') cend++;
            if (*cend == ']') cend++;
            while (*cend && *cend != ']') cend++;
            if (*cend) cend++;
        }
        char spec[128];
        size_t sl = 0;
        spec[sl++] = '%';
        if (suppress) spec[sl++] = '*';
        for (const char *p = st + 1 + suppress; p < fmt && sl < 100; p++)
            if (isdigit((unsigned char)*p)) spec[sl++] = *p;
        int consumed = -1;
        if (conv == 'n') {
            if (!suppress) { wr32(rd32(ap), (uint32_t)(s - str)); ap += 4; }
            fmt = cend;
            continue;
        }
        if (strchr("diuxXoc", conv)) {
            if (lng >= 2) { spec[sl++] = 'l'; spec[sl++] = 'l'; }
            else if (half == 1) spec[sl++] = 'h';
            else if (half == 2) { spec[sl++] = 'h'; spec[sl++] = 'h'; }
            spec[sl++] = conv;
        } else if (strchr("feEgGaA", conv)) {
            if (lng) spec[sl++] = 'l';
            spec[sl++] = conv;
        } else {
            memcpy(spec + sl, fmt, cend - fmt);
            sl += cend - fmt;
        }
        memcpy(spec + sl, "%n", 3);
        int r;
        if (suppress) {
            r = sscanf(s, spec, &consumed);
            if (consumed < 0) break;
            s += consumed;
            fmt = cend;
            continue;
        }
        uint32_t dst = rd32(ap);
        ap += 4;
        if (strchr("diuxXo", conv)) {
            if (lng >= 2) { long long v; r = sscanf(s, spec, &v, &consumed); if (r == 1) wr64(dst, (uint64_t)v); }
            else if (half == 1) { short v; r = sscanf(s, spec, &v, &consumed); if (r == 1) wr16(dst, (uint16_t)v); }
            else if (half == 2) { signed char v; r = sscanf(s, spec, &v, &consumed); if (r == 1) wr8(dst, (uint8_t)v); }
            else { int v; r = sscanf(s, spec, &v, &consumed); if (r == 1) wr32(dst, (uint32_t)v); }
        } else if (strchr("feEgGaA", conv)) {
            if (lng) { double v; r = sscanf(s, spec, &v, &consumed); if (r == 1) wrf64(dst, v); }
            else { float v; r = sscanf(s, spec, &v, &consumed); if (r == 1) wrf32(dst, v); }
        } else {
            r = sscanf(s, spec, G(dst), &consumed);
        }
        if (r != 1 || consumed < 0) {
            if (count == 0 && !*s) return -1;
            break;
        }
        count++;
        s += consumed;
        fmt = cend;
    }
    return count;
}

// ---- FILE ---------------------------------------------------------------------
#define MAX_FILES 256
static FILE *files[MAX_FILES];
static uint32_t file_handle[MAX_FILES];
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t guest_stdout, guest_stderr;

static FILE *hostfile(uint32_t h)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (file_handle[i] == h) return files[i];
    return NULL;
}

static uint32_t new_handle(FILE *f)
{
    pthread_mutex_lock(&file_lock);
    for (int i = 0; i < MAX_FILES; i++)
        if (!files[i]) {
            files[i] = f;
            if (!file_handle[i]) file_handle[i] = hostdata_alloc(160);
            pthread_mutex_unlock(&file_lock);
            return file_handle[i];
        }
    pthread_mutex_unlock(&file_lock);
    return 0;
}

static void h_fopen(CPU *c)
{
    char buf[2048];
    const void *vdata;
    size_t vsize;
    FILE *f;
    if (virtual_file(G(ARG(0)), &vdata, &vsize)) {
        f = fmemopen((void *)vdata, vsize, "rb");
        if (hle_verbose) hle_log("fopen %s (virtual)\n", G(ARG(0)));
        RET(f ? new_handle(f) : 0);
    }
    if (host_device_hidden(G(ARG(0)))) {
        errno = ENOENT;
        RET(0);
    }
    const char *p = map_path(G(ARG(0)), buf, sizeof buf);
    f = fopen(p, G(ARG(1)));
    if (hle_verbose) hle_log("fopen %s %s -> %s\n", G(ARG(0)), G(ARG(1)), f ? "ok" : "fail");
    RET(f ? new_handle(f) : 0);
}

static void h_fclose(CPU *c)
{
    uint32_t h = ARG(0);
    pthread_mutex_lock(&file_lock);
    for (int i = 0; i < MAX_FILES; i++)
        if (file_handle[i] == h && files[i]) {
            if (files[i] != stdout && files[i] != stderr && files[i] != stdin) fclose(files[i]);
            files[i] = NULL;
        }
    pthread_mutex_unlock(&file_lock);
    RET(0);
}

static void h_fread(CPU *c)
{
    FILE *f = hostfile(ARG(3));
    RET(f ? fread(G(ARG(0)), ARG(1), ARG(2), f) : 0);
}

// The game's other writes to stdout and stderr (fwrite, fputs, putc, vfprintf)
// go to the log too, a line at a time, so their repeats are cut down as well
static void guest_out(const char *s, size_t n)
{
    static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
    static char line[1024];
    static size_t len;
    pthread_mutex_lock(&out_lock);
    for (size_t i = 0; i < n; i++) {
        if (len < sizeof line - 2) line[len++] = s[i];
        if (s[i] == '\n' || len >= sizeof line - 2) {
            if (line[len - 1] != '\n') line[len++] = '\n';
            line[len] = 0;
            hle_log("%s", line);
            len = 0;
        }
    }
    pthread_mutex_unlock(&out_lock);
}
static int is_console(FILE *f) { return f == stdout || f == stderr; }

static void h_fwrite(CPU *c)
{
    FILE *f = hostfile(ARG(3));
    if (is_console(f)) {
        guest_out(G(ARG(0)), (size_t)ARG(1) * ARG(2));
        RET(ARG(2));
        return;
    }
    RET(f ? fwrite(G(ARG(0)), ARG(1), ARG(2), f) : 0);
}

static void h_fseek(CPU *c)
{
    FILE *f = hostfile(ARG(0));
    RET(f ? fseek(f, (int32_t)ARG(1), (int)ARG(2)) : -1);
}

static void h_ftell(CPU *c)
{
    FILE *f = hostfile(ARG(0));
    RET(f ? (uint32_t)ftell(f) : (uint32_t)-1);
}

static void h_rewind(CPU *c)
{
    FILE *f = hostfile(ARG(0));
    if (f) rewind(f);
}

static void h_ferror(CPU *c)
{
    FILE *f = hostfile(ARG(0));
    RET(f ? ferror(f) : 1);
}

static void h_fprintf(CPU *c)
{
    char buf[16384];
    int n = guest_vformat(buf, sizeof buf, G(ARG(1)), c->r[ESP] + 12);
    FILE *f = hostfile(ARG(0));
    if (f == stdout || f == stderr) hle_log("%s", buf);   // the log (repeats cut down)
    else if (f) fputs(buf, f);
    RET(n);
}

static void h_printf(CPU *c)
{
    char buf[16384];
    int n = guest_vformat(buf, sizeof buf, G(ARG(0)), c->r[ESP] + 8);
    hle_log("%s", buf);
    RET(n);
}

static void h_puts(CPU *c)
{
    hle_log("%s\n", G(ARG(0)));
    RET(1);
}

static void h_putchar(CPU *c)
{
    char ch = (char)ARG(0);
    guest_out(&ch, 1);
    RET(ARG(0));
}

static void h_IO_putc(CPU *c)
{
    FILE *f = hostfile(ARG(1));
    char ch = (char)ARG(0);
    if (is_console(f)) guest_out(&ch, 1);
    else if (f) fputc((int)ARG(0), f);
    RET(ARG(0) & 0xff);
}

static void h_sprintf(CPU *c)
{
    char buf[65536];
    int n = guest_vformat(buf, sizeof buf, G(ARG(1)), c->r[ESP] + 12);
    memcpy(G(ARG(0)), buf, (size_t)n + 1);
    RET(n);
}

static void h_snprintf(CPU *c)
{
    uint32_t sz = ARG(1);
    char buf[65536];
    int n = guest_vformat(buf, sizeof buf, G(ARG(2)), c->r[ESP] + 16);
    if (sz) {
        uint32_t k = (uint32_t)n < sz - 1 ? (uint32_t)n : sz - 1;
        memcpy(G(ARG(0)), buf, k);
        G(ARG(0))[k] = 0;
    }
    RET(n);
}

static void h_vsprintf(CPU *c)
{
    char buf[65536];
    int n = guest_vformat(buf, sizeof buf, G(ARG(1)), ARG(2));
    memcpy(G(ARG(0)), buf, (size_t)n + 1);
    RET(n);
}

static void h_vsnprintf(CPU *c)
{
    uint32_t sz = ARG(1);
    char buf[65536];
    int n = guest_vformat(buf, sizeof buf, G(ARG(2)), ARG(3));
    if (sz) {
        uint32_t k = (uint32_t)n < sz - 1 ? (uint32_t)n : sz - 1;
        memcpy(G(ARG(0)), buf, k);
        G(ARG(0))[k] = 0;
    }
    RET(n);
}

static void h_sscanf(CPU *c) { RET(guest_vscan(G(ARG(0)), G(ARG(1)), c->r[ESP] + 12)); }

// ---- file descriptors -----------------------------------------------------------
static int host_flags(uint32_t f)
{
    int h = f & 3;
    if (f & 0100) h |= O_CREAT;
    if (f & 0200) h |= O_EXCL;
    if (f & 0400) h |= O_NOCTTY;
    if (f & 01000) h |= O_TRUNC;
    if (f & 02000) h |= O_APPEND;
    if (f & 04000) h |= O_NONBLOCK;
    if (f & 010000) h |= O_SYNC;
    return h;
}

#define MAX_FDS 1024
static const device_ops *fd_dev[MAX_FDS];
static uint8_t fd_guest[MAX_FDS];   // opened by the game: closed when a core stops it
static uint8_t fd_sock[MAX_FDS];    // a socket (see sockets below)

static int guest_fd(int fd, const device_ops *d)
{
    if (fd >= 0 && fd < MAX_FDS) {
        fd_dev[fd] = d;
        fd_guest[fd] = 1;
    }
    return fd;
}

const device_ops *device_for_fd(int fd) { return fd >= 0 && fd < MAX_FDS ? fd_dev[fd] : NULL; }

static void h_open(CPU *c)
{
    const char *gp = G(ARG(0));
    const device_ops *d = device_for_path(gp);
    if (d) {
        int fd = guest_fd(open("/dev/null", O_RDWR), d);
        if (d->open && d->open() < 0) {
            if (fd >= 0 && fd < MAX_FDS) fd_dev[fd] = NULL, fd_guest[fd] = 0;
            close(fd);
            RET(-1);
        }
        hle_log("open %s -> device fd %d\n", gp, fd);
        RET(fd);
    }
    if (host_device_hidden(gp)) {
        if (hle_verbose) hle_log("open %s: host device, not shown\n", gp);
        errno = ENOENT;
        RET(-1);
    }
    char buf[2048];
    int fd = guest_fd(open(map_path(gp, buf, sizeof buf), host_flags(ARG(1)), (mode_t)ARG(2)), NULL);
    if (hle_verbose) hle_log("open %s -> %d\n", gp, fd);
    RET(fd);
}

// Games with the test menu inside create this when they exit (Race TV: its TEST
// switch exits); the cabinet's boot loader then starts them with -t, or again
// without it after the menu.
int segaboot_test;

static void h_creat(CPU *c)
{
    char buf[2048];
    if (!strcmp(G(ARG(0)), "/tmp/segaboot/test")) segaboot_test = 1;
    int fd = guest_fd(creat(map_path(G(ARG(0)), buf, sizeof buf), (mode_t)ARG(1)), NULL);
    RET(fd);
}

static void h_close(CPU *c)
{
    int fd = (int)ARG(0);
    if (fd >= 0 && fd < MAX_FDS) {
        // the game's stdio descriptors are the host's: keep them open
        if (!fd_guest[fd]) RET(0);
        fd_dev[fd] = NULL;
        fd_guest[fd] = 0;
        fd_sock[fd] = 0;
    }
    RET(close(fd));
}

static void h_read(CPU *c)
{
    const device_ops *d = device_for_fd((int)ARG(0));
    if (d) RET(d->read ? d->read(ARG(1), ARG(2)) : 0);
    RET((uint32_t)read((int)ARG(0), G(ARG(1)), ARG(2)));
}

static void h_write(CPU *c)
{
    const device_ops *d = device_for_fd((int)ARG(0));
    if (d) RET(d->write ? d->write(ARG(1), ARG(2)) : (int)ARG(2));
    int fd = (int)ARG(0);
    RET((uint32_t)write(fd == 1 ? 2 : fd, G(ARG(1)), ARG(2)));
}

static void h_lseek(CPU *c) { RET((uint32_t)lseek((int)ARG(0), (int32_t)ARG(1), (int)ARG(2))); }
static void h_lseek64(CPU *c) { RETQ((uint64_t)lseek((int)ARG(0), (off_t)(int64_t)ARGQ(1), (int)ARG(3))); }
static void h_fsync(CPU *c) { RET(0); }
// ---- sockets ------------------------------------------------------------------------------
// Real host sockets, kept on the loopback: a cabinet on its own talks to itself
// (Sega Race TV stand-alone sets up a master on 127.0.0.1 and waits for its own
// client). Binds to any address bind 127.0.0.1; sends to a broadcast or to the
// cabinet's own address go to 127.0.0.1. The interface ioctls describe two
// interfaces, lo (127.0.0.1/8) and eth0 (192.168.1.2/24). Linux's sockaddr,
// option and flag numbers are translated to the host's; any 127.x.x.x is 127.0.0.1.
static int is_sock(int fd) { return fd >= 0 && fd < MAX_FDS && fd_sock[fd]; }

static const struct { const char *name; uint32_t addr, mask, bcast, flags; } ifaces[] = {
    {"lo", 0x0100007fu, 0x000000ffu, 0x00000000u, 0x0049},     // up, loopback, running
    {"eth0", 0x0201a8c0u, 0x00ffffffu, 0xff01a8c0u, 0x1043},   // up, broadcast, running, multicast
};
static int iface_of(uint32_t ifr)   // the interface an ifreq names (eth0 when unknown)
{
    for (int i = 0; i < 2; i++)
        if (!strncmp(G(ifr), ifaces[i].name, 16)) return i;
    return 1;
}
static void put_ifaddr(uint32_t ifr, uint32_t addr)
{
    memset(M + ifr + 16, 0, 16);
    wr16(ifr + 16, 2);   // AF_INET
    wr32(ifr + 20, addr);
}

// Linux errno numbers, which the game compares with
static int linux_errno(int e)
{
#ifdef __APPLE__
    switch (e) {
    case EAGAIN: return 11;
    case EINPROGRESS: return 115;
    case EALREADY: return 114;
    case ENOTSOCK: return 88;
    case EMSGSIZE: return 90;
    case EOPNOTSUPP: return 95;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETUNREACH: return 101;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case EHOSTUNREACH: return 113;
    case ENOSYS: return 38;
    }
#endif
    return e;
}

// guest sockaddr_in {family 16, port, address, zero[8]} <-> the host's
static int to_host_addr(uint32_t ga, uint32_t len, struct sockaddr_in *h, int binding)
{
    memset(h, 0, sizeof *h);
    if (!ga || len < 8 || rd16(ga) != 2) return -1;
#ifdef __APPLE__
    h->sin_len = sizeof *h;
#endif
    h->sin_family = AF_INET;
    memcpy(&h->sin_port, M + ga + 2, 2);
    uint32_t a;
    memcpy(&a, M + ga + 4, 4);
    if (binding ? a == 0 : (a == ifaces[1].addr || a == ifaces[1].bcast || a == 0xffffffffu)) a = ifaces[0].addr;
    if ((a & 0xff) == 127) a = ifaces[0].addr;   // all of 127/8 is local on Linux; the host may have only 127.0.0.1
    memcpy(&h->sin_addr, &a, 4);
    return 0;
}
static void to_guest_addr(const struct sockaddr_in *h, uint32_t ga, uint32_t lenp)
{
    if (ga) {
        memset(M + ga, 0, 16);
        wr16(ga, 2);
        memcpy(M + ga + 2, &h->sin_port, 2);
        memcpy(M + ga + 4, &h->sin_addr, 4);
    }
    if (lenp) wr32(lenp, 16);
}
static int host_msg_flags(uint32_t f)
{
    int h = f & 7;                     // OOB, PEEK, DONTROUTE
    if (f & 0x40) h |= MSG_DONTWAIT;
    if (f & 0x100) h |= MSG_WAITALL;
    return h;                          // MSG_NOSIGNAL: SO_NOSIGPIPE is set at creation
}
#define SOCK_RET(r) do { long _r = (r); if (_r < 0) errno = linux_errno(errno); RET((uint32_t)_r); } while (0)
// LB_VERBOSE: what the game does on the network
static void net_log(const char *what, int fd, const struct sockaddr_in *a, long r)
{
    if (!hle_verbose) return;
    uint32_t ip;
    memcpy(&ip, &a->sin_addr, 4);
    hle_log("net %s fd %d %u.%u.%u.%u:%u -> %ld%s\n", what, fd, ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
            ntohs(a->sin_port), r, r < 0 ? strerror(errno) : "");
}

static void h_socket(CPU *c)
{
    if (ARG(0) != 2) RET((uint32_t)-1);   // AF_INET only
    int fd = guest_fd(socket(AF_INET, (int)(ARG(1) & 0xf), (int)ARG(2)), NULL);
    if (fd >= 0 && fd < MAX_FDS) {
        fd_sock[fd] = 1;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }
    if (hle_verbose) hle_log("socket(%u, %u, %u) -> %d\n", ARG(0), ARG(1), ARG(2), fd);
    SOCK_RET(fd);
}
static void h_bind(CPU *c)
{
    struct sockaddr_in a;
    if (to_host_addr(ARG(1), ARG(2), &a, 1)) RET((uint32_t)-1);
    long r = bind((int)ARG(0), (struct sockaddr *)&a, sizeof a);
    net_log("bind", (int)ARG(0), &a, r);
    SOCK_RET(r);
}
static void h_connect(CPU *c)
{
    struct sockaddr_in a;
    if (to_host_addr(ARG(1), ARG(2), &a, 0)) RET((uint32_t)-1);
    long r = connect((int)ARG(0), (struct sockaddr *)&a, sizeof a);
    net_log("connect", (int)ARG(0), &a, r);
    SOCK_RET(r);
}
static void h_listen(CPU *c)
{
    if (hle_verbose) hle_log("net listen fd %d\n", (int)ARG(0));
    SOCK_RET(listen((int)ARG(0), (int)ARG(1)));
}
static void h_accept(CPU *c)
{
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    int fd = guest_fd(accept((int)ARG(0), (struct sockaddr *)&a, &n), NULL);
    if (fd >= 0) {
        if (fd < MAX_FDS) fd_sock[fd] = 1;
        to_guest_addr(&a, ARG(1), ARG(2));
    }
    SOCK_RET(fd);
}
static void h_shutdown(CPU *c) { SOCK_RET(shutdown((int)ARG(0), (int)ARG(1))); }
static void h_getsockname(CPU *c)
{
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    int r = getsockname((int)ARG(0), (struct sockaddr *)&a, &n);
    if (!r) to_guest_addr(&a, ARG(1), ARG(2));
    SOCK_RET(r);
}
// Linux's SOL_SOCKET options and their host numbers
static int host_sockopt(uint32_t level, uint32_t name, int *hlevel, int *hname)
{
    if (level == 1) {
        static const int map[][2] = {{2, SO_REUSEADDR}, {6, SO_BROADCAST}, {7, SO_SNDBUF}, {8, SO_RCVBUF},
                                     {9, SO_KEEPALIVE}, {4, SO_ERROR}, {15, SO_REUSEPORT}, {20, SO_RCVTIMEO},
                                     {21, SO_SNDTIMEO}, {13, SO_LINGER}, {3, SO_TYPE}};
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (map[i][0] == (int)name) {
                *hlevel = SOL_SOCKET;
                *hname = map[i][1];
                return 0;
            }
        return -1;
    }
    if (level == 6 && name == 1) { *hlevel = IPPROTO_TCP; *hname = TCP_NODELAY; return 0; }
    return -1;   // IP-level options (multicast, TTL): not needed on the loopback
}
static void h_setsockopt(CPU *c)
{
    int hl, hn;
    if (host_sockopt(ARG(1), ARG(2), &hl, &hn)) RET(0);
    if (hn == SO_RCVTIMEO || hn == SO_SNDTIMEO) {   // Linux i386 timeval: two 32-bit longs
        struct timeval tv = {(time_t)(int32_t)rd32(ARG(3)), (suseconds_t)(int32_t)rd32(ARG(3) + 4)};
        SOCK_RET(setsockopt((int)ARG(0), hl, hn, &tv, sizeof tv));
    }
    if (hn == SO_REUSEADDR) {   // SO_REUSEPORT too: several sockets on one loopback port
        int v = (int)rd32(ARG(3));
        setsockopt((int)ARG(0), SOL_SOCKET, SO_REUSEPORT, &v, sizeof v);
    }
    int v = ARG(4) >= 4 ? (int)rd32(ARG(3)) : 0;
    SOCK_RET(setsockopt((int)ARG(0), hl, hn, &v, sizeof v));
}
static void h_getsockopt(CPU *c)
{
    int hl, hn, v = 0;
    socklen_t n = sizeof v;
    if (host_sockopt(ARG(1), ARG(2), &hl, &hn) || hn == SO_RCVTIMEO || hn == SO_SNDTIMEO) {
        if (ARG(3)) wr32(ARG(3), 0);
        if (ARG(4)) wr32(ARG(4), 4);
        RET(0);
    }
    int r = getsockopt((int)ARG(0), hl, hn, &v, &n);
    if (hn == SO_ERROR) v = linux_errno(v);
    if (!r && ARG(3)) wr32(ARG(3), (uint32_t)v);
    if (ARG(4)) wr32(ARG(4), 4);
    SOCK_RET(r);
}
static void h_send(CPU *c) { SOCK_RET(send((int)ARG(0), G(ARG(1)), ARG(2), host_msg_flags(ARG(3)))); }
static void h_recv(CPU *c) { SOCK_RET(recv((int)ARG(0), G(ARG(1)), ARG(2), host_msg_flags(ARG(3)))); }
static void h_sendto(CPU *c)
{
    struct sockaddr_in a;
    if (!ARG(4)) SOCK_RET(send((int)ARG(0), G(ARG(1)), ARG(2), host_msg_flags(ARG(3))));
    if (to_host_addr(ARG(4), ARG(5), &a, 0)) RET(ARG(2));   // not IPv4: dropped
    long r = sendto((int)ARG(0), G(ARG(1)), ARG(2), host_msg_flags(ARG(3)), (struct sockaddr *)&a, sizeof a);
    net_log("sendto", (int)ARG(0), &a, r);
    SOCK_RET(r);
}
static void h_recvfrom(CPU *c)
{
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    long r = recvfrom((int)ARG(0), G(ARG(1)), ARG(2), host_msg_flags(ARG(3)), (struct sockaddr *)&a, &n);
    if (r >= 0) net_log("recvfrom", (int)ARG(0), &a, r);
    if (r >= 0 && ARG(4)) to_guest_addr(&a, ARG(4), ARG(5));
    SOCK_RET(r);
}
// Linux i386 msghdr: {name, namelen, iov, iovlen, control, controllen, flags}, iovec {base, len}
static int host_msg(uint32_t gm, struct msghdr *m, struct iovec *iov, struct sockaddr_in *a)
{
    memset(m, 0, sizeof *m);
    uint32_t n = rd32(gm + 12);
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) {
        iov[i].iov_base = G(rd32(rd32(gm + 8) + 8 * i));
        iov[i].iov_len = rd32(rd32(gm + 8) + 8 * i + 4);
    }
    m->msg_iov = iov;
    m->msg_iovlen = (int)n;
    if (rd32(gm)) {
        m->msg_name = a;
        m->msg_namelen = sizeof *a;
        return 1;
    }
    return 0;
}
static void h_sendmsg(CPU *c)
{
    struct msghdr m;
    struct iovec iov[64];
    struct sockaddr_in a;
    if (host_msg(ARG(1), &m, iov, &a) && to_host_addr(rd32(ARG(1)), rd32(ARG(1) + 4), &a, 0)) {
        m.msg_name = NULL;
        m.msg_namelen = 0;
    }
    long r = sendmsg((int)ARG(0), &m, host_msg_flags(ARG(2)));
    if (m.msg_name) net_log("sendmsg", (int)ARG(0), &a, r);
    else if (hle_verbose) hle_log("net sendmsg fd %d -> %ld\n", (int)ARG(0), r);
    SOCK_RET(r);
}
static void h_recvmsg(CPU *c)
{
    struct msghdr m;
    struct iovec iov[64];
    struct sockaddr_in a;
    int named = host_msg(ARG(1), &m, iov, &a);
    long r = recvmsg((int)ARG(0), &m, host_msg_flags(ARG(2)));
    if (r >= 0 && named) net_log("recvmsg", (int)ARG(0), &a, r);
    if (r >= 0) {
        if (named) to_guest_addr(&a, rd32(ARG(1)), ARG(1) + 4);
        wr32(ARG(1) + 20, 0);   // no control messages
        wr32(ARG(1) + 24, 0);
    }
    SOCK_RET(r);
}
static int socket_ioctl(int fd, uint32_t req, uint32_t a)
{
    switch (req) {
    case 0x8912: {               // SIOCGIFCONF: {length, buffer}, 32-byte ifreqs
        uint32_t buf = rd32(a + 4), room = rd32(a) / 32, n = 0;
        for (int i = 0; i < 2 && (!buf || n < room); i++, n++)
            if (buf) {
                memset(M + buf + 32 * n, 0, 32);
                strcpy(G(buf + 32 * n), ifaces[i].name);
                put_ifaddr(buf + 32 * n, ifaces[i].addr);
            }
        wr32(a, 32 * n);
        return 0;
    }
    case 0x8915: put_ifaddr(a, ifaces[iface_of(a)].addr); return 0;     // SIOCGIFADDR
    case 0x891b: put_ifaddr(a, ifaces[iface_of(a)].mask); return 0;     // SIOCGIFNETMASK
    case 0x8919: put_ifaddr(a, ifaces[iface_of(a)].bcast); return 0;    // SIOCGIFBRDADDR
    case 0x8913: wr16(a + 16, (uint16_t)ifaces[iface_of(a)].flags); return 0;   // SIOCGIFFLAGS
    case 0x8927: {                                                      // SIOCGIFHWADDR
        static const uint8_t mac[6] = {0x00, 0xd0, 0xf1, 0x00, 0x00, 0x02};
        memset(M + a + 16, 0, 16);
        wr16(a + 16, iface_of(a) ? 1 : 772);   // ARPHRD_ETHER, ARPHRD_LOOPBACK
        if (iface_of(a)) memcpy(M + a + 18, mac, 6);
        return 0;
    }
    case 0x8933: wr32(a + 16, (uint32_t)iface_of(a) + 1); return 0;     // SIOCGIFINDEX
    case 0x5421: {                                                      // FIONBIO
        int v = (int)rd32(a);
        return ioctl(fd, FIONBIO, &v);
    }
    case 0x541b: {                                                      // FIONREAD
        int v = 0;
        int r = ioctl(fd, FIONREAD, &v);
        wr32(a, (uint32_t)v);
        return r;
    }
    }
    hle_log("socket ioctl 0x%x ignored\n", req);
    return 0;
}

// F_GETFL/F_SETFL carry O_NONBLOCK on sockets; elsewhere nothing to do
static void h_fcntl(CPU *c)
{
    int fd = (int)ARG(0);
    if (!is_sock(fd)) RET(0);
    if (ARG(1) == 3) {   // F_GETFL
        int f = fcntl(fd, F_GETFL);
        RET(2 | (f & O_NONBLOCK ? 04000 : 0));
    }
    if (ARG(1) == 4) {   // F_SETFL
        int f = fcntl(fd, F_GETFL);
        f = ARG(2) & 04000 ? f | O_NONBLOCK : f & ~O_NONBLOCK;
        SOCK_RET(fcntl(fd, F_SETFL, f));
    }
    RET(0);
}

static void h_ioctl(CPU *c)
{
    const device_ops *d = device_for_fd((int)ARG(0));
    if (d && d->ioctl) RET(d->ioctl(ARG(1), ARG(2)));
    if (is_sock((int)ARG(0))) SOCK_RET(socket_ioctl((int)ARG(0), ARG(1), ARG(2)));
    hle_log("ioctl fd %d req 0x%x ignored\n", ARG(0), ARG(1));
    RET(-1);
}

static void h_xstat(CPU *c)
{
    char buf[2048];
    struct stat st;
    const char *gp = G(ARG(1));
    if (stat(map_path(gp, buf, sizeof buf), &st)) {
        if (hle_verbose) hle_log("stat %s -> fail\n", gp);
        RET(-1);
    }
    uint32_t b = ARG(2);
    memset(M + b, 0, 88);
    wr64(b, st.st_dev);
    wr32(b + 12, (uint32_t)st.st_ino);
    wr32(b + 16, st.st_mode);
    wr32(b + 20, (uint32_t)st.st_nlink);
    wr32(b + 24, st.st_uid);
    wr32(b + 28, st.st_gid);
    wr32(b + 44, (uint32_t)st.st_size);
    wr32(b + 48, (uint32_t)st.st_blksize);
    wr32(b + 52, (uint32_t)st.st_blocks);
    wr32(b + 56, (uint32_t)st.st_atime);
    wr32(b + 64, (uint32_t)st.st_mtime);
    wr32(b + 72, (uint32_t)st.st_ctime);
    RET(0);
}

static void h_select(CPU *c)
{
    int nfds = (int)ARG(0), socks = 0;
    uint32_t rf = ARG(1), wf = ARG(2), tv = ARG(4);
    for (int fd = 0; fd < nfds && fd < MAX_FDS; fd++) {
        int inr = rf && ((rd32(rf + 4 * (fd / 32)) >> (fd % 32)) & 1);
        int inw = wf && ((rd32(wf + 4 * (fd / 32)) >> (fd % 32)) & 1);
        if ((inr || inw) && fd_dev[fd]) RET(fd_dev[fd]->select ? fd_dev[fd]->select() : 1);
        if ((inr || inw) && fd_sock[fd]) socks = 1;
    }
    uint64_t us = tv ? (uint64_t)rd32(tv) * 1000000 + rd32(tv + 4) : ~0ull;
    if (!socks) {
        if (tv && us) guest_sleep_us(us);
        RET(0);
    }
    // sockets: the host's select, in slices so a stopping game is not held up
    // (fd_set has the same layout: 32-bit words of bits)
    for (;;) {
        fd_set r, w;
        FD_ZERO(&r);
        FD_ZERO(&w);
        for (int fd = 0; fd < nfds && fd < MAX_FDS && fd < FD_SETSIZE; fd++) {
            if (!fd_sock[fd]) continue;
            if (rf && ((rd32(rf + 4 * (fd / 32)) >> (fd % 32)) & 1)) FD_SET(fd, &r);
            if (wf && ((rd32(wf + 4 * (fd / 32)) >> (fd % 32)) & 1)) FD_SET(fd, &w);
        }
        uint64_t slice = us < 100000 ? us : 100000;
        struct timeval t = {(time_t)(slice / 1000000), (suseconds_t)(slice % 1000000)};
        int n = select(nfds, &r, &w, NULL, &t);
        if (n != 0 || us <= slice || guest_stopping) {
            for (int fd = 0; fd < nfds && fd < MAX_FDS; fd++) {
                uint32_t bit = 1u << (fd % 32);
                if (rf) wr32(rf + 4 * (fd / 32), (rd32(rf + 4 * (fd / 32)) & ~bit) | (fd < FD_SETSIZE && FD_ISSET(fd, &r) ? bit : 0));
                if (wf) wr32(wf + 4 * (fd / 32), (rd32(wf + 4 * (fd / 32)) & ~bit) | (fd < FD_SETSIZE && FD_ISSET(fd, &w) ? bit : 0));
            }
            if (ARG(3)) memset(M + ARG(3), 0, (size_t)((nfds + 31) / 32) * 4);
            if (guest_stopping) guest_exit_thread();
            SOCK_RET(n);
        }
        us -= slice;
    }
}

// ---- memory and strings --------------------------------------------------------------
static void h_malloc(CPU *c) { RET(guest_malloc(ARG(0))); }
static void h_calloc(CPU *c) { RET(guest_calloc(ARG(0), ARG(1))); }
static void h_free(CPU *c) { guest_free(ARG(0)); }
static void h_memcpy(CPU *c) { memmove(G(ARG(0)), G(ARG(1)), ARG(2)); RET(ARG(0)); }
static void h_memmove(CPU *c) { memmove(G(ARG(0)), G(ARG(1)), ARG(2)); RET(ARG(0)); }
static void h_memset(CPU *c) { memset(G(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
static void h_strcpy(CPU *c) { strcpy(G(ARG(0)), G(ARG(1))); RET(ARG(0)); }
static void h_strncpy(CPU *c) { strncpy(G(ARG(0)), G(ARG(1)), ARG(2)); RET(ARG(0)); }
static void h_strcat(CPU *c) { strcat(G(ARG(0)), G(ARG(1))); RET(ARG(0)); }
static void h_strncat(CPU *c) { strncat(G(ARG(0)), G(ARG(1)), ARG(2)); RET(ARG(0)); }
static void h_strcmp(CPU *c) { RET(strcmp(G(ARG(0)), G(ARG(1)))); }
static void h_strncmp(CPU *c) { RET(strncmp(G(ARG(0)), G(ARG(1)), ARG(2))); }
static void h_strspn(CPU *c) { RET(strspn(G(ARG(0)), G(ARG(1)))); }
static void h_strcspn(CPU *c) { RET(strcspn(G(ARG(0)), G(ARG(1)))); }

static uint32_t gptr(const char *base_host, uint32_t base_guest, const char *p)
{
    return p ? base_guest + (uint32_t)(p - base_host) : 0;
}

static void h_strchr(CPU *c) { RET(gptr(G(ARG(0)), ARG(0), strchr(G(ARG(0)), (int)ARG(1)))); }
static void h_strrchr(CPU *c) { RET(gptr(G(ARG(0)), ARG(0), strrchr(G(ARG(0)), (int)ARG(1)))); }
static void h_strstr(CPU *c) { RET(gptr(G(ARG(0)), ARG(0), strstr(G(ARG(0)), G(ARG(1))))); }
static void h_strpbrk(CPU *c) { RET(gptr(G(ARG(0)), ARG(0), strpbrk(G(ARG(0)), G(ARG(1))))); }

static uint32_t strtok_save;
static void h_strtok(CPU *c)
{
    uint32_t s = ARG(0) ? ARG(0) : strtok_save;
    const char *d = G(ARG(1));
    if (!s) RET(0);
    s += (uint32_t)strspn(G(s), d);
    if (!*G(s)) {
        strtok_save = 0;
        RET(0);
    }
    uint32_t e = s + (uint32_t)strcspn(G(s), d);
    if (*G(e)) {
        *G(e) = 0;
        strtok_save = e + 1;
    } else strtok_save = 0;
    RET(s);
}

static void h_isdigit(CPU *c) { RET(isdigit((int)ARG(0)) ? 0x800 : 0); }
static void h_atoi(CPU *c) { RET(atoi(G(ARG(0)))); }
static void h_atof(CPU *c) { RETF(atof(G(ARG(0)))); }

static void h_strtol_internal(CPU *c)
{
    char *end;
    long v = strtol(G(ARG(0)), &end, (int)ARG(2));
    if (v > 0x7fffffffL) v = 0x7fffffffL;
    if (v < -0x80000000L) v = -0x80000000L;
    if (ARG(1)) wr32(ARG(1), ARG(0) + (uint32_t)(end - G(ARG(0))));
    RET((uint32_t)v);
}

static void h_strtoul_internal(CPU *c)
{
    char *end;
    unsigned long v = strtoul(G(ARG(0)), &end, (int)ARG(2));
    if (v > 0xffffffffUL) v = 0xffffffffUL;
    if (ARG(1)) wr32(ARG(1), ARG(0) + (uint32_t)(end - G(ARG(0))));
    RET((uint32_t)v);
}

// glibc's ctype table: 384 uint16 entries for -128..255, pointer at +128
static uint32_t ctype_ptr;
static void build_ctype(void)
{
    uint32_t tab = hostdata_alloc(384 * 2);
    for (int i = -128; i < 256; i++) {
        int ch = i & 0xff;
        uint16_t v = 0;
        if (i >= 0 && i < 128) {
            if (isupper(ch)) v |= 0x100;
            if (islower(ch)) v |= 0x200;
            if (isalpha(ch)) v |= 0x400;
            if (isdigit(ch)) v |= 0x800;
            if (isxdigit(ch)) v |= 0x1000;
            if (isspace(ch)) v |= 0x2000;
            if (isprint(ch)) v |= 0x4000;
            if (isgraph(ch)) v |= 0x8000;
            if (ch == ' ' || ch == '\t') v |= 0x1;
            if (iscntrl(ch)) v |= 0x2;
            if (ispunct(ch)) v |= 0x4;
            if (isalnum(ch)) v |= 0x8;
        }
        wr16(tab + 2 * (i + 128), v);
    }
    ctype_ptr = hostdata_alloc(4);
    wr32(ctype_ptr, tab + 256);
}
static void h_ctype_b_loc(CPU *c) { RET(ctype_ptr); }

// ---- glibc random() TYPE_3, so rand() sequences match the cabinet ------------------
static int32_t rtab[34 + 310];
static int rfront, rrear;
static pthread_mutex_t rand_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t r_state[31];
static int r_f, r_r;

static void glibc_srand(uint32_t seed)
{
    if (seed == 0) seed = 1;
    r_state[0] = (int32_t)seed;
    for (int i = 1; i < 31; i++) {
        int32_t hi = r_state[i - 1] / 127773, lo = r_state[i - 1] % 127773;
        int32_t w = 16807 * lo - 2836 * hi;
        if (w < 0) w += 2147483647;
        r_state[i] = w;
    }
    r_f = 3;
    r_r = 0;
    for (int i = 0; i < 310; i++) {
        r_state[r_f] += r_state[r_r];
        r_f = (r_f + 1) % 31;
        r_r = (r_r + 1) % 31;
    }
    (void)rtab; (void)rfront; (void)rrear;
}

static int32_t glibc_rand(void)
{
    r_state[r_f] += r_state[r_r];
    int32_t res = (int32_t)((uint32_t)r_state[r_f] >> 1);
    r_f = (r_f + 1) % 31;
    r_r = (r_r + 1) % 31;
    return res;
}

static void h_srand(CPU *c) { pthread_mutex_lock(&rand_lock); glibc_srand(ARG(0)); pthread_mutex_unlock(&rand_lock); }
static void h_rand(CPU *c) { pthread_mutex_lock(&rand_lock); int32_t v = glibc_rand(); pthread_mutex_unlock(&rand_lock); RET(v); }

// ---- qsort: glibc uses a stable merge sort ---------------------------------------------
static void msort(CPU *c, uint32_t base, uint32_t n, uint32_t sz, uint32_t cmp, uint32_t tmp)
{
    if (n <= 1) return;
    uint32_t n1 = n / 2, n2 = n - n1;
    uint32_t b1 = base, b2 = base + n1 * sz;
    msort(c, b1, n1, sz, cmp, tmp);
    msort(c, b2, n2, sz, cmp, tmp);
    uint32_t t = tmp;
    while (n1 && n2) {
        uint32_t a[2] = {b1, b2};
        if ((int32_t)guest_call(c, cmp, 2, a) <= 0) {
            memmove(M + t, M + b1, sz); b1 += sz; n1--;
        } else {
            memmove(M + t, M + b2, sz); b2 += sz; n2--;
        }
        t += sz;
    }
    if (n1) memmove(M + t, M + b1, n1 * sz);
    memmove(M + base, M + tmp, (n - n2) * sz);
}

static void h_qsort(CPU *c)
{
    uint32_t base = ARG(0), n = ARG(1), sz = ARG(2), cmp = ARG(3);
    if (n < 2) return;
    uint32_t tmp = guest_malloc(n * sz);
    msort(c, base, n, sz, cmp, tmp);
    guest_free(tmp);
}

// ---- math (results in st0) ----------------------------------------------------------
static void h_cos(CPU *c) { RETF(cos(ARGD(0))); }
static void h_pow(CPU *c) { RETF(pow(ARGD(0), ARGD(2))); }
static void h_hypot(CPU *c) { RETF(hypot(ARGD(0), ARGD(2))); }
static void h_log10(CPU *c) { RETF(log10(ARGD(0))); }
static void h_cosf(CPU *c) { RETF(cosf(ARGF(0))); }
static void h_sinf(CPU *c) { RETF(sinf(ARGF(0))); }
static void h_coshf(CPU *c) { RETF(coshf(ARGF(0))); }
static void h_sinhf(CPU *c) { RETF(sinhf(ARGF(0))); }
static void h_tanhf(CPU *c) { RETF(tanhf(ARGF(0))); }
static void h_logf(CPU *c) { RETF(logf(ARGF(0))); }
static void h_powf(CPU *c) { RETF(powf(ARGF(0), ARGF(1))); }

static void h_divdi3(CPU *c) { int64_t a = (int64_t)ARGQ(0), b = (int64_t)ARGQ(2); RETQ(b ? a / b : 0); }
static void h_udivdi3(CPU *c) { uint64_t a = ARGQ(0), b = ARGQ(2); RETQ(b ? a / b : 0); }
static void h_umoddi3(CPU *c) { uint64_t a = ARGQ(0), b = ARGQ(2); RETQ(b ? a % b : 0); }

// ---- time -----------------------------------------------------------------------------------
static void h_time(CPU *c)
{
    uint32_t t = (uint32_t)time(NULL);
    if (ARG(0)) wr32(ARG(0), t);
    RET(t);
}

static void h_gettimeofday(CPU *c)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (ARG(0)) {
        wr32(ARG(0), (uint32_t)tv.tv_sec);
        wr32(ARG(0) + 4, (uint32_t)tv.tv_usec);
    }
    RET(0);
}

static uint32_t tm_buf;
static void h_localtime(CPU *c)
{
    time_t t = (time_t)(int32_t)rd32(ARG(0));
    struct tm tm;
    localtime_r(&t, &tm);
    if (!tm_buf) tm_buf = hostdata_alloc(64);
    int v[9] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    for (int i = 0; i < 9; i++) wr32(tm_buf + 4 * i, (uint32_t)v[i]);
    wr32(tm_buf + 36, (uint32_t)tm.tm_gmtoff);
    wr32(tm_buf + 40, 0);
    RET(tm_buf);
}

static void h_usleep(CPU *c) { guest_sleep_us(ARG(0)); RET(0); }
static void h_sleep(CPU *c) { guest_sleep_us(ARG(0) * 1000000ull); RET(0); }

// ---- process --------------------------------------------------------------------------------
// what the games' launch scripts on the cabinet's disk export
static const char *const cabinet_env[][2] = {
    {"TEA_DIR", "/home/disk0"},          // Let's Go Jungle: where its shaders are
    {"GLIBCPP_FORCE_NEW", "1"},          // libstdc++ allocates with new, not its pools
};
static uint32_t env_value[sizeof cabinet_env / sizeof cabinet_env[0]];

static void h_getenv(CPU *c)
{
    if (hle_verbose) hle_log("getenv %s\n", G(ARG(0)));
    for (unsigned i = 0; i < sizeof cabinet_env / sizeof cabinet_env[0]; i++)
        if (!strcmp(G(ARG(0)), cabinet_env[i][0])) {
            if (!env_value[i]) env_value[i] = guest_strdup(cabinet_env[i][1]);
            RET(env_value[i]);
        }
    RET(0);
}

static void h_signal(CPU *c) { RET(0); }
static void h_system(CPU *c)
{
    // games mark one-time screens as seen with "touch"; nothing else runs
    const char *cmd = G(ARG(0));
    if (!strncmp(cmd, "touch ", 6)) {
        char buf[2048];
        FILE *f = fopen(map_path(cmd + 6, buf, sizeof buf), "a");
        if (f) fclose(f);
        RET(0);
    }
    hle_log("system(\"%s\") ignored\n", cmd);
    RET(0);
}
static void h_popen(CPU *c) { hle_log("popen(\"%s\") ignored\n", G(ARG(0))); RET(0); }
static void h_pclose(CPU *c) { RET(0); }
static void h_sysctl(CPU *c) { RET(-1); }
static void h_iopl(CPU *c) { RET(0); }
static void h_nop(CPU *c) { RET(0); }

static void h_exit(CPU *c)
{
    hle_log("exit(%d)\n", (int)ARG(0));
    host_exit((int)ARG(0));
}

static void h_raise(CPU *c)
{
    hle_log("raise(%d)\n", (int)ARG(0));
    host_exit(128 + (int)ARG(0));
}

static void h_libc_start_main(CPU *c)
{
    uint32_t main_fn = ARG(0), argc = ARG(1), argv = ARG(2), init = ARG(3);
    if (init) guest_call(c, init, 0, NULL);
    uint32_t args[3] = {argc, argv, argv + 4 * (argc + 1)};
    uint32_t r = guest_call(c, main_fn, 3, args);
    hle_log("main returned %d\n", (int)r);
    host_exit((int)r);
}

// ---- network address helpers ------------------------------------------------------------------
static void h_inet_addr(CPU *c)
{
    unsigned a, b, cc, d;
    if (sscanf(G(ARG(0)), "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) RET(0xffffffffu);
    RET(a | (b << 8) | (cc << 16) | (d << 24));
}

static void h_inet_aton(CPU *c)
{
    unsigned a, b, cc, d;
    if (sscanf(G(ARG(0)), "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) RET(0);
    wr32(ARG(1), a | (b << 8) | (cc << 16) | (d << 24));
    RET(1);
}

static uint32_t ntoa_buf;
static void h_inet_ntoa(CPU *c)
{
    uint32_t v = ARG(0);
    if (!ntoa_buf) ntoa_buf = hostdata_alloc(32);
    snprintf(G(ntoa_buf), 32, "%u.%u.%u.%u", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    RET(ntoa_buf);
}

// ---- libstdc++.so.5 -----------------------------------------------------------------------------
// std::string is GCC 3.3's reference-counted string: the object holds a
// pointer to the characters, preceded by {length, capacity, references}.
static uint32_t empty_rep;   // address of the empty _Rep

static void h_new(CPU *c) { RET(guest_malloc(ARG(0))); }
static void h_delete(CPU *c) { guest_free(ARG(0)); }
static void h_alloc_allocate(CPU *c) { RET(guest_malloc(ARG(0))); }
static void h_alloc_deallocate(CPU *c) { guest_free(ARG(0)); }

static void h_string_ctor(CPU *c)
{
    wr32(empty_rep + 8, rd32(empty_rep + 8) + 1);
    wr32(ARG(0), empty_rep + 12);
}

static void rep_dispose(uint32_t rep)
{
    if (rep == empty_rep) {
        wr32(rep + 8, rd32(rep + 8) - 1);
        return;
    }
    int32_t refs = (int32_t)rd32(rep + 8);
    wr32(rep + 8, (uint32_t)(refs - 1));
    if (refs <= 0) guest_free(rep);
}

static void h_string_dtor(CPU *c) { rep_dispose(rd32(ARG(0)) - 12); }

static void h_string_assign_cstr(CPU *c)
{
    uint32_t self = ARG(0);
    const char *s = G(ARG(1));
    uint32_t n = strlen(s);
    uint32_t rep = guest_malloc(n + 1 + 12);
    wr32(rep, n);
    wr32(rep + 4, n);
    wr32(rep + 8, 0);
    memmove(G(rep + 12), s, n);
    *G(rep + 12 + n) = 0;
    uint32_t old = rd32(self) - 12;
    wr32(self, rep + 12);
    rep_dispose(old);
    RET(self);
}

static void h_cxa_atexit(CPU *c) { RET(0); }
static void h_cxa_guard_acquire(CPU *c) { RET(rd8(ARG(0)) ? 0 : 1); }
static void h_cxa_guard_release(CPU *c) { wr8(ARG(0), 1); }

static void h_cxa_fatal(CPU *c)
{
    char why[96];
    snprintf(why, sizeof why, "C++ exception or pure virtual call (ret 0x%08x)", rd32(c->r[ESP]));
    host_fatal(why);
}

// The array helpers allocate with operator new[] and delete[]: the program's own
// when it defines them (Race TV's come from its memory pools), as the dynamic
// linker would bind them.
static uint32_t array_new(CPU *c, uint32_t n)
{
    uint32_t f = guest_export("_Znaj");
    return f ? guest_call(c, f, 1, &n) : guest_malloc(n);
}

static void array_delete(CPU *c, uint32_t p)
{
    uint32_t f = guest_export("_ZdaPv");
    if (f) guest_call(c, f, 1, &p);
    else guest_free(p);
}

static void h_cxa_vec_new(CPU *c)
{
    uint32_t n = ARG(0), sz = ARG(1), pad = ARG(2), ctor = ARG(3);
    uint32_t p = array_new(c, n * sz + pad) + pad;
    if (pad) wr32(p - 4, n);
    if (ctor)
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = p + i * sz;
            guest_call(c, ctor, 1, &a);
        }
    RET(p);
}

static void h_cxa_vec_ctor(CPU *c)
{
    uint32_t p = ARG(0), n = ARG(1), sz = ARG(2), ctor = ARG(3);
    if (ctor)
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = p + i * sz;
            guest_call(c, ctor, 1, &a);
        }
}

static void h_cxa_vec_cctor(CPU *c)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2), sz = ARG(3), cc = ARG(4);
    if (cc)
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a[2] = {d + i * sz, s + i * sz};
            guest_call(c, cc, 2, a);
        }
}

static void h_cxa_vec_dtor(CPU *c)
{
    uint32_t p = ARG(0), n = ARG(1), sz = ARG(2), dtor = ARG(3);
    if (dtor)
        for (uint32_t i = n; i-- > 0;) {
            uint32_t a = p + i * sz;
            guest_call(c, dtor, 1, &a);
        }
}

static void h_cxa_vec_delete(CPU *c)
{
    uint32_t p = ARG(0), sz = ARG(1), pad = ARG(2), dtor = ARG(3);
    if (!p) return;
    if (pad && dtor) {
        uint32_t n = rd32(p - 4);
        for (uint32_t i = n; i-- > 0;) {
            uint32_t a = p + i * sz;
            guest_call(c, dtor, 1, &a);
        }
    }
    array_delete(c, p - pad);
}

// ---- more libc, libm and libgcc (Let's Go Jungle and the libraries it ships) ------------
// glibc i386 layouts: struct dirent (ino, off, reclen, type, name), struct tm (9 ints,
// gmtoff, zone), struct lconv (10 pointers, then chars), wchar_t is 32 bits as on the host.
static void h_strlen(CPU *c) { RET(strlen(G(ARG(0)))); }
static void h_memchr(CPU *c)
{
    const char *p = memchr(G(ARG(0)), (int)ARG(1), ARG(2));
    RET(p ? (uint32_t)(p - (const char *)M) : 0);
}
static void h_strcasecmp(CPU *c) { RET(strcasecmp(G(ARG(0)), G(ARG(1)))); }
static void h_strncasecmp(CPU *c) { RET(strncasecmp(G(ARG(0)), G(ARG(1)), ARG(2))); }
static void h_strdup(CPU *c) { RET(guest_strdup(G(ARG(0)))); }
static void h_strcoll(CPU *c) { RET(strcmp(G(ARG(0)), G(ARG(1)))); }
static void h_strxfrm(CPU *c)
{
    size_t n = strlen(G(ARG(1)));
    if (ARG(2)) {
        strncpy(G(ARG(0)), G(ARG(1)), ARG(2));
        if (n >= ARG(2)) G(ARG(0))[ARG(2) - 1] = 0;
    }
    RET(n);
}
static void h_isspace(CPU *c) { RET(isspace((int)ARG(0)) ? 0x2000 : 0); }
static uint32_t errstr;
static void h_strerror(CPU *c)
{
    if (!errstr) errstr = hostdata_alloc(256);
    snprintf(G(errstr), 256, "%s", strerror((int)ARG(0)));
    RET(errstr);
}
static void h_strtod_internal(CPU *c)
{
    char *end;
    double v = strtod(G(ARG(0)), &end);
    if (ARG(1)) wr32(ARG(1), ARG(0) + (uint32_t)(end - G(ARG(0))));
    RETF(v);
}

// heap: realloc keeps the old contents
static void h_realloc(CPU *c)
{
    uint32_t p = ARG(0), n = ARG(1);
    if (!p) RET(guest_malloc(n));
    if (!n) {
        guest_free(p);
        RET(0);
    }
    uint32_t old = guest_usable(p);
    uint32_t q = guest_malloc(n);
    memcpy(M + q, M + p, old < n ? old : n);
    guest_free(p);
    RET(q);
}
static void h_memalign(CPU *c) { RET(guest_memalign(ARG(0), ARG(1))); }

// libm
static void h_acos(CPU *c) { RETF(acos(ARGD(0))); }
static void h_acosf(CPU *c) { RETF(acosf(ARGF(0))); }
static void h_asin(CPU *c) { RETF(asin(ARGD(0))); }
static void h_asinf(CPU *c) { RETF(asinf(ARGF(0))); }
static void h_cosh(CPU *c) { RETF(cosh(ARGD(0))); }
static void h_exp(CPU *c) { RETF(exp(ARGD(0))); }
static void h_expf(CPU *c) { RETF(expf(ARGF(0))); }
static void h_log(CPU *c) { RETF(log(ARGD(0))); }
static void h_sin(CPU *c) { RETF(sin(ARGD(0))); }
static void h_sinh(CPU *c) { RETF(sinh(ARGD(0))); }
static void h_tan(CPU *c) { RETF(tan(ARGD(0))); }
static void h_tanf(CPU *c) { RETF(tanf(ARGF(0))); }
static void h_tanh(CPU *c) { RETF(tanh(ARGD(0))); }
static void h_frexp(CPU *c)
{
    int e;
    double m = frexp(ARGD(0), &e);
    wr32(ARG(2), (uint32_t)e);
    RETF(m);
}
static void h_modf(CPU *c)
{
    double ip, f = modf(ARGD(0), &ip);
    wrf64(ARG(2), ip);
    RETF(f);
}
static void h_isinf(CPU *c) { double v = ARGD(0); RET(isinf(v) ? (v > 0 ? 1 : -1) : 0); }
static void h_isnan(CPU *c) { RET(isnan(ARGD(0)) ? 1 : 0); }
static void h_finite(CPU *c) { RET(isfinite(ARGD(0)) ? 1 : 0); }
static void h_finitef(CPU *c) { RET(isfinite(ARGF(0)) ? 1 : 0); }
static void h_fixunssfdi(CPU *c) { float f = ARGF(0); RETQ(f <= 0 ? 0 : (uint64_t)f); }
// div_t comes back through a hidden pointer the callee pops (ret $4)
static void h_div(CPU *c)
{
    uint32_t out = ARG(0);
    int32_t a = (int32_t)ARG(1), b = (int32_t)ARG(2);
    wr32(out, (uint32_t)(b ? a / b : 0));
    wr32(out + 4, (uint32_t)(b ? a % b : 0));
    c->r[ESP] += 4;
    RET(out);
}

// ctype, wide characters and the C locale
static uint32_t lower_cell, upper_cell, lconv_addr, c_locale;
static void build_case_tables(void)
{
    uint32_t lo = hostdata_alloc(384 * 4), up = hostdata_alloc(384 * 4);
    for (int i = -128; i < 256; i++) {
        int ch = i < 0 ? i : i & 0xff;
        wr32(lo + 4 * (i + 128), (uint32_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
        wr32(up + 4 * (i + 128), (uint32_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
    }
    lower_cell = hostdata_alloc(4);
    upper_cell = hostdata_alloc(4);
    wr32(lower_cell, lo + 128 * 4);
    wr32(upper_cell, up + 128 * 4);
    c_locale = guest_strdup("C");
    uint32_t dot = guest_strdup("."), empty = guest_strdup("");
    lconv_addr = hostdata_alloc(56);
    for (int i = 0; i < 10; i++) wr32(lconv_addr + 4 * i, i == 0 ? dot : empty);
    for (int i = 40; i < 56; i++) wr8(lconv_addr + i, 127);   // CHAR_MAX: not available
}
static void h_tolower_loc(CPU *c) { RET(lower_cell); }
static void h_toupper_loc(CPU *c) { RET(upper_cell); }
static void h_mb_cur_max(CPU *c) { RET(1); }
static void h_setlocale(CPU *c) { RET(c_locale); }
static void h_localeconv(CPU *c) { RET(lconv_addr); }
static void h_mblen(CPU *c) { RET(ARG(0) ? (G(ARG(0))[0] ? 1 : 0) : 0); }
static void h_mbstowcs(CPU *c)
{
    const unsigned char *s = (const unsigned char *)G(ARG(1));
    uint32_t n = 0;
    for (; s[n] && (!ARG(0) || n < ARG(2)); n++)
        if (ARG(0)) wr32(ARG(0) + 4 * n, s[n]);
    if (ARG(0) && n < ARG(2)) wr32(ARG(0) + 4 * n, 0);
    RET(n);
}
static void h_wcstombs(CPU *c)
{
    uint32_t n = 0;
    for (; rd32(ARG(1) + 4 * n) && (!ARG(0) || n < ARG(2)); n++) {
        uint32_t w = rd32(ARG(1) + 4 * n);
        if (w > 255) RET((uint32_t)-1);
        if (ARG(0)) wr8(ARG(0) + n, (uint8_t)w);
    }
    if (ARG(0) && n < ARG(2)) wr8(ARG(0) + n, 0);
    RET(n);
}
static void h_mbrtowc(CPU *c)
{
    if (!ARG(1)) RET(0);
    if (!ARG(2)) RET((uint32_t)-2);
    uint8_t b = rd8(ARG(1));
    if (b >= 0x80) RET((uint32_t)-1);   // EILSEQ in the C locale
    if (ARG(0)) wr32(ARG(0), b);
    RET(b ? 1 : 0);
}
static void h_wcrtomb(CPU *c)
{
    if (!ARG(0)) RET(1);
    if (ARG(1) > 255) RET((uint32_t)-1);
    wr8(ARG(0), (uint8_t)ARG(1));
    RET(1);
}
static void h_btowc(CPU *c) { RET((int32_t)ARG(0) < 0 || ARG(0) > 127 ? (uint32_t)-1 : ARG(0)); }
static void h_wctob(CPU *c) { RET(ARG(0) <= 127 ? ARG(0) : (uint32_t)-1); }
static void h_towlower(CPU *c) { RET(ARG(0) < 128 ? (uint32_t)tolower((int)ARG(0)) : ARG(0)); }
static void h_towupper(CPU *c) { RET(ARG(0) < 128 ? (uint32_t)toupper((int)ARG(0)) : ARG(0)); }
static void h_iswspace(CPU *c) { RET(ARG(0) < 128 && isspace((int)ARG(0))); }
// wctype descriptors: 1-12 for the classes below
static const char *const wclass[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                     "lower", "print", "punct", "space", "upper", "xdigit"};
static void h_wctype(CPU *c)
{
    for (int i = 0; i < 12; i++)
        if (!strcmp(G(ARG(0)), wclass[i])) RET(i + 1);
    RET(0);
}
static void h_iswctype(CPU *c)
{
    uint32_t w = ARG(0);
    if (w >= 128 || !ARG(1) || ARG(1) > 12) RET(0);
    int ch = (int)w, r = 0;
    switch (ARG(1)) {
    case 1: r = isalnum(ch); break;
    case 2: r = isalpha(ch); break;
    case 3: r = ch == ' ' || ch == '\t'; break;
    case 4: r = iscntrl(ch); break;
    case 5: r = isdigit(ch); break;
    case 6: r = isgraph(ch); break;
    case 7: r = islower(ch); break;
    case 8: r = isprint(ch); break;
    case 9: r = ispunct(ch); break;
    case 10: r = isspace(ch); break;
    case 11: r = isupper(ch); break;
    case 12: r = isxdigit(ch); break;
    }
    RET(r != 0);
}
static void h_wcslen(CPU *c)
{
    uint32_t n = 0;
    while (rd32(ARG(0) + 4 * n)) n++;
    RET(n);
}
static void h_wmemchr(CPU *c)
{
    for (uint32_t i = 0; i < ARG(2); i++)
        if (rd32(ARG(0) + 4 * i) == ARG(1)) RET(ARG(0) + 4 * i);
    RET(0);
}
static void h_wmemcmp(CPU *c)
{
    for (uint32_t i = 0; i < ARG(2); i++) {
        int32_t a = (int32_t)rd32(ARG(0) + 4 * i), b = (int32_t)rd32(ARG(1) + 4 * i);
        if (a != b) RET(a < b ? -1 : 1);
    }
    RET(0);
}
static void h_wmemcpy(CPU *c) { memmove(G(ARG(0)), G(ARG(1)), 4 * (size_t)ARG(2)); RET(ARG(0)); }
static void h_wmemset(CPU *c)
{
    for (uint32_t i = 0; i < ARG(2); i++) wr32(ARG(0) + 4 * i, ARG(1));
    RET(ARG(0));
}
static void h_wcscoll(CPU *c)
{
    for (uint32_t i = 0;; i++) {
        int32_t a = (int32_t)rd32(ARG(0) + 4 * i), b = (int32_t)rd32(ARG(1) + 4 * i);
        if (a != b) RET(a < b ? -1 : 1);
        if (!a) RET(0);
    }
}
static void h_wcsxfrm(CPU *c)
{
    uint32_t n = 0;
    while (rd32(ARG(1) + 4 * n)) n++;
    for (uint32_t i = 0; ARG(0) && i < ARG(2); i++) {
        wr32(ARG(0) + 4 * i, i < n ? rd32(ARG(1) + 4 * i) : 0);
        if (i >= n) break;
    }
    RET(n);
}
static void guest_tm(uint32_t g, struct tm *t)
{
    memset(t, 0, sizeof *t);
    t->tm_sec = (int)rd32(g);
    t->tm_min = (int)rd32(g + 4);
    t->tm_hour = (int)rd32(g + 8);
    t->tm_mday = (int)rd32(g + 12);
    t->tm_mon = (int)rd32(g + 16);
    t->tm_year = (int)rd32(g + 20);
    t->tm_wday = (int)rd32(g + 24);
    t->tm_yday = (int)rd32(g + 28);
    t->tm_isdst = (int)rd32(g + 32);
}
static void h_strftime(CPU *c)
{
    struct tm t;
    guest_tm(ARG(3), &t);
    RET(strftime(G(ARG(0)), ARG(1), G(ARG(2)), &t));
}
static void h_wcsftime(CPU *c)
{
    char fmt[512], out[1024];
    uint32_t n = 0;
    for (; n < sizeof fmt - 1 && rd32(ARG(2) + 4 * n); n++) fmt[n] = (char)rd32(ARG(2) + 4 * n);
    fmt[n] = 0;
    struct tm t;
    guest_tm(ARG(3), &t);
    size_t k = strftime(out, sizeof out, fmt, &t);
    if (k >= ARG(1)) RET(0);
    for (size_t i = 0; i <= k; i++) wr32(ARG(0) + 4 * (uint32_t)i, (uint8_t)out[i]);
    RET(k);
}

// errno: one cell per thread, showing the host's errno of the thread's last call
static __thread uint32_t errno_cell;
static void h_errno_location(CPU *c)
{
    if (!errno_cell) errno_cell = hostdata_alloc(4);
    wr32(errno_cell, (uint32_t)linux_errno(errno));
    RET(errno_cell);
}

// stdio
static uint32_t guest_stdin;
static void h_IO_getc(CPU *c)
{
    FILE *f = hostfile(ARG(0));
    RET(f ? (uint32_t)fgetc(f) : (uint32_t)-1);
}
static void h_fgets(CPU *c)
{
    FILE *f = hostfile(ARG(2));
    RET(f && fgets(G(ARG(0)), (int)ARG(1), f) ? ARG(0) : 0);
}
static FILE *out_file(uint32_t h)
{
    FILE *f = hostfile(h);
    return f == stdout ? stderr : f;
}
static void h_fputc(CPU *c)
{
    FILE *f = out_file(ARG(1));
    char ch = (char)ARG(0);
    if (is_console(f)) {
        guest_out(&ch, 1);
        RET(ARG(0) & 0xff);
        return;
    }
    RET(f ? (uint32_t)fputc((int)ARG(0), f) : (uint32_t)-1);
}
static void h_fputs(CPU *c)
{
    FILE *f = out_file(ARG(1));
    if (is_console(f)) {
        guest_out(G(ARG(0)), strlen(G(ARG(0))));
        RET(1);
        return;
    }
    RET(f ? (uint32_t)fputs(G(ARG(0)), f) : (uint32_t)-1);
}
static void h_fflush(CPU *c)
{
    FILE *f = ARG(0) ? hostfile(ARG(0)) : NULL;
    RET(fflush(f));
}
static void h_feof(CPU *c)
{
    FILE *f = hostfile(ARG(0));
    RET(f ? feof(f) : 1);
}
static void h_vfprintf(CPU *c)
{
    char buf[16384];
    int n = guest_vformat(buf, sizeof buf, G(ARG(1)), ARG(2));
    FILE *f = out_file(ARG(0));
    if (is_console(f)) guest_out(buf, strlen(buf));
    else if (f) fputs(buf, f);
    RET(n);
}
// a line at a time: enough for the settings files games read this way
static void h_fscanf(CPU *c)
{
    FILE *f = hostfile(ARG(0));
    char line[4096];
    if (!f || !fgets(line, sizeof line, f)) RET((uint32_t)-1);
    RET(guest_vscan(line, G(ARG(1)), c->r[ESP] + 12));
}
static void h_fdopen(CPU *c)
{
    FILE *f = fdopen((int)ARG(0), G(ARG(1)));
    RET(f ? new_handle(f) : 0);
}

// files and directories
#define MAX_DIRS 16
static DIR *dirs[MAX_DIRS];
static uint32_t dir_ent[MAX_DIRS];
static void h_opendir(CPU *c)
{
    char buf[2048];
    DIR *d = opendir(map_path(G(ARG(0)), buf, sizeof buf));
    if (!d) RET(0);
    for (int i = 0; i < MAX_DIRS; i++)
        if (!dirs[i]) {
            dirs[i] = d;
            if (!dir_ent[i]) dir_ent[i] = hostdata_alloc(268);
            RET(i + 1);
        }
    closedir(d);
    RET(0);
}
static void h_readdir(CPU *c)
{
    uint32_t i = ARG(0) - 1;
    if (i >= MAX_DIRS || !dirs[i]) RET(0);
    struct dirent *e = readdir(dirs[i]);
    if (!e) RET(0);
    uint32_t g = dir_ent[i];
    memset(M + g, 0, 268);
    wr32(g, (uint32_t)e->d_ino);
    wr16(g + 8, 268);
    wr8(g + 10, e->d_type);
    snprintf(G(g + 11), 256, "%s", e->d_name);
    RET(g);
}
static void h_closedir(CPU *c)
{
    uint32_t i = ARG(0) - 1;
    if (i < MAX_DIRS && dirs[i]) {
        closedir(dirs[i]);
        dirs[i] = NULL;
    }
    RET(0);
}
static void h_mkdir(CPU *c)
{
    char buf[2048];
    RET(mkdir(map_path(G(ARG(0)), buf, sizeof buf), (mode_t)ARG(1)));
}
static void h_unlink(CPU *c)
{
    char buf[2048];
    RET(unlink(map_path(G(ARG(0)), buf, sizeof buf)));
}
static void h_utime(CPU *c) { RET(0); }
static void h_dup(CPU *c) { RET(dup((int)ARG(0))); }
// the game runs in /home/disk0 on the cabinet
static void h_getcwd(CPU *c)
{
    const char *cwd = "/home/disk0";
    uint32_t buf = ARG(0) ? ARG(0) : guest_malloc(ARG(1) > 64 ? ARG(1) : 64);
    snprintf(G(buf), ARG(1) ? ARG(1) : 64, "%s", cwd);
    RET(buf);
}
static void h_realpath(CPU *c)
{
    const char *p = G(ARG(0));
    uint32_t out = ARG(1) ? ARG(1) : guest_malloc(4096);
    if (p[0] == '/') snprintf(G(out), 4096, "%s", p);
    else snprintf(G(out), 4096, "/home/disk0/%s", p[0] == '.' && p[1] == '/' ? p + 2 : p);
    RET(out);
}
static void h_ftime(CPU *c)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    wr32(ARG(0), (uint32_t)tv.tv_sec);
    wr16(ARG(0) + 4, (uint16_t)(tv.tv_usec / 1000));
    wr16(ARG(0) + 6, 0);
    wr16(ARG(0) + 8, 0);
    RET(0);
}
static void h_nanosleep(CPU *c)
{
    guest_sleep_us((uint64_t)rd32(ARG(0)) * 1000000 + rd32(ARG(0) + 4) / 1000);
    if (ARG(1)) wr64(ARG(1), 0);
    RET(0);
}

// processes, dynamic loading and sockets: the game gets the answers of a
// cabinet without them
static void h_getpid(CPU *c) { RET(1000); }
static void h_fail(CPU *c) { RET((uint32_t)-1); }
static void h_null(CPU *c) { RET(0); }
static void h_dlopen(CPU *c)
{
    hle_log("dlopen %s\n", ARG(0) ? G(ARG(0)) : "(self)");
    RET(1);
}
static void h_dlsym(CPU *c)
{
    uint32_t a = guest_symbol(G(ARG(1)));
    if (!a) hle_log("dlsym %s: not found\n", G(ARG(1)));
    RET(a);
}
static void h_abort(CPU *c) { host_fatal("abort() called"); }
static void h_assert_fail(CPU *c)
{
    char why[512];
    snprintf(why, sizeof why, "assertion failed: %s (%s:%u, %s)", G(ARG(0)), G(ARG(1)), ARG(2), G(ARG(3)));
    host_fatal(why);
}

// setjmp is translated in line (recomp.py): the guest jmp_buf holds, at offset
// 32, the host jmp_buf in the frame of the function that called it
static void h_longjmp(CPU *c)
{
    jmp_buf *hp;
    memcpy(&hp, M + ARG(0) + 32, sizeof hp);
    int v = (int)ARG(1);
    longjmp(*hp, v ? v : 1);
}

// C++ exceptions: frames are registered but nothing unwinds translated code yet
static void h_unwind_raise(CPU *c) { host_fatal("C++ exception thrown (unwinding translated code is not supported yet)"); }

static const hle_entry libc_table2[] = {
    {"strlen", h_strlen}, {"memchr", h_memchr}, {"index", h_strchr}, {"strcasecmp", h_strcasecmp},
    {"strncasecmp", h_strncasecmp}, {"strdup", h_strdup}, {"strcoll", h_strcoll}, {"strxfrm", h_strxfrm},
    {"isspace", h_isspace}, {"strerror", h_strerror}, {"__strtod_internal", h_strtod_internal},
    {"realloc", h_realloc}, {"memalign", h_memalign},
    {"acos", h_acos}, {"acosf", h_acosf}, {"asin", h_asin}, {"asinf", h_asinf}, {"cosh", h_cosh},
    {"exp", h_exp}, {"expf", h_expf}, {"log", h_log}, {"sin", h_sin}, {"sinh", h_sinh}, {"tan", h_tan},
    {"tanf", h_tanf}, {"tanh", h_tanh}, {"frexp", h_frexp}, {"modf", h_modf}, {"isinf", h_isinf},
    {"isnan", h_isnan}, {"finite", h_finite}, {"finitef", h_finitef}, {"__fixunssfdi", h_fixunssfdi},
    {"div", h_div},
    {"__ctype_tolower_loc", h_tolower_loc}, {"__ctype_toupper_loc", h_toupper_loc},
    {"__ctype_get_mb_cur_max", h_mb_cur_max}, {"setlocale", h_setlocale}, {"localeconv", h_localeconv},
    {"mblen", h_mblen}, {"mbstowcs", h_mbstowcs}, {"wcstombs", h_wcstombs}, {"mbrtowc", h_mbrtowc},
    {"wcrtomb", h_wcrtomb}, {"btowc", h_btowc}, {"wctob", h_wctob}, {"towlower", h_towlower},
    {"towupper", h_towupper}, {"iswspace", h_iswspace}, {"wctype", h_wctype}, {"iswctype", h_iswctype},
    {"wcslen", h_wcslen}, {"wmemchr", h_wmemchr}, {"wmemcmp", h_wmemcmp}, {"wmemcpy", h_wmemcpy},
    {"wmemmove", h_wmemcpy}, {"wmemset", h_wmemset}, {"wcscoll", h_wcscoll}, {"wcsxfrm", h_wcsxfrm},
    {"strftime", h_strftime}, {"wcsftime", h_wcsftime}, {"__errno_location", h_errno_location},
    {"_IO_getc", h_IO_getc}, {"fgets", h_fgets}, {"fputc", h_fputc}, {"fputs", h_fputs},
    {"fflush", h_fflush}, {"feof", h_feof}, {"vfprintf", h_vfprintf}, {"fscanf", h_fscanf},
    {"fdopen", h_fdopen},
    {"opendir", h_opendir}, {"readdir", h_readdir}, {"closedir", h_closedir}, {"mkdir", h_mkdir},
    {"unlink", h_unlink}, {"utime", h_utime}, {"dup", h_dup}, {"getcwd", h_getcwd},
    {"realpath", h_realpath}, {"ftime", h_ftime}, {"nanosleep", h_nanosleep},
    {"getpid", h_getpid}, {"fork", h_fail}, {"execlp", h_fail}, {"kill", h_null},
    {"dlopen", h_dlopen}, {"dlsym", h_dlsym}, {"dlclose", h_null}, {"dlerror", h_null},
    {"gethostbyname", h_null},
    {"gethostbyaddr", h_null}, {"abort", h_abort}, {"__assert_fail", h_assert_fail},
    {"__cxa_finalize", h_null}, {"__libc_freeres", h_null},
    {"__register_frame_info", h_null}, {"__register_frame_info_bases", h_null},
    {"__deregister_frame_info", h_null}, {"__deregister_frame_info_bases", h_null},
    {"_Unwind_RaiseException", h_unwind_raise}, {"_Unwind_Resume_or_Rethrow", h_unwind_raise},
    {"_Unwind_DeleteException", h_null}, {"_Unwind_GetIP", h_null}, {"_Unwind_SetIP", h_null},
    {"_Unwind_GetGR", h_null}, {"_Unwind_SetGR", h_null}, {"_Unwind_GetRegionStart", h_null},
    {"_Unwind_GetLanguageSpecificData", h_null}, {"_Unwind_GetDataRelBase", h_null},
    {"_Unwind_GetTextRelBase", h_null}, {"longjmp", h_longjmp}, {"_longjmp", h_longjmp},
    {"siglongjmp", h_longjmp},
    {NULL, NULL},
};

// ---- Sega Race TV: the Intel compiler's runtime, libstdc++.so.6, more libc ---------------
// The game links libimf/libirc/libsvml (Intel C++). libimf is not position
// independent (relocations in its code), so the few functions the game calls
// are here. __libm_sse2_* take their arguments in xmm0 (xmm1) and return in
// xmm0; sincos returns the sine in xmm0 and the cosine in xmm1.
static void h_intel_init(CPU *c) { RET(0); }
static void h_intel_memcpy(CPU *c) { memmove(G(ARG(0)), G(ARG(1)), ARG(2)); RET(ARG(0)); }
static void h_intel_memset(CPU *c) { memset(G(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
static void h_intel_memcmp(CPU *c)
{
    int r = memcmp(G(ARG(0)), G(ARG(1)), ARG(2));
    RET(r < 0 ? -1 : r > 0);
}
#define SSE_D(name, expr) static void name(CPU *c) { double x = c->xmm[0].d[0], y = c->xmm[1].d[0]; (void)y; c->xmm[0].d[0] = (expr); }
#define SSE_F(name, expr) static void name(CPU *c) { float x = c->xmm[0].f[0]; c->xmm[0].f[0] = (expr); }
SSE_D(h_sse2_acos, acos(x))
SSE_D(h_sse2_atan, atan(x))
SSE_D(h_sse2_atan2, atan2(x, y))
SSE_D(h_sse2_sin, sin(x))
SSE_F(h_sse2_acosf, acosf(x))
SSE_F(h_sse2_asinf, asinf(x))
SSE_F(h_sse2_atanf, atanf(x))
SSE_F(h_sse2_cosf, cosf(x))
SSE_F(h_sse2_sinf, sinf(x))
SSE_F(h_sse2_tanf, tanf(x))
SSE_F(h_sse2_logf, logf(x))
SSE_F(h_sse2_log10f, log10f(x))
static void h_sse2_sincos(CPU *c)
{
    double x = c->xmm[0].d[0];
    c->xmm[0].d[0] = sin(x);
    c->xmm[1].d[0] = cos(x);
}
static void h_sse2_sincosf(CPU *c)
{
    float x = c->xmm[0].f[0];
    c->xmm[0].f[0] = sinf(x);
    c->xmm[1].f[0] = cosf(x);
}
static void h_finitef2(CPU *c) { RET(isfinite(ARGF(0)) ? 1 : 0); }
static void h_atan2(CPU *c) { RETF(atan2(ARGD(0), ARGD(2))); }
static void h_atan2f(CPU *c) { RETF(atan2f(ARGF(0), ARGF(1))); }
static void h_atanf(CPU *c) { RETF(atanf(ARGF(0))); }
static void h_fmod(CPU *c) { RETF(fmod(ARGD(0), ARGD(2))); }
static void h_fmodf(CPU *c) { RETF(fmodf(ARGF(0), ARGF(1))); }
static void h_sqrt(CPU *c) { RETF(sqrt(ARGD(0))); }
static void h_sqrtf(CPU *c) { RETF(sqrtf(ARGF(0))); }

// libstdc++.so.6 (GCC 3.4): the same reference-counted string as .so.5
static uint32_t str_len(uint32_t self) { return rd32(rd32(self) - 12); }
static uint32_t new_rep(const char *s, uint32_t n)
{
    uint32_t rep = guest_malloc(n + 1 + 12);
    wr32(rep, n);
    wr32(rep + 4, n);
    wr32(rep + 8, 0);
    if (n) memmove(G(rep + 12), s, n);
    *G(rep + 12 + n) = 0;
    return rep;
}
// another reference to other's characters, or a copy of an unshareable string
static uint32_t grab(uint32_t other)
{
    uint32_t rep = rd32(other) - 12;
    if ((int32_t)rd32(rep + 8) < 0) return new_rep(G(rep + 12), rd32(rep)) + 12;
    wr32(rep + 8, rd32(rep + 8) + 1);
    return rep + 12;
}
static void h_string_ctor_cstr(CPU *c) { wr32(ARG(0), new_rep(G(ARG(1)), strlen(G(ARG(1)))) + 12); }
static void h_string_ctor_copy(CPU *c) { wr32(ARG(0), grab(ARG(1))); }
static void h_string_assign(CPU *c)
{
    uint32_t self = ARG(0), other = ARG(1);
    if (rd32(self) != rd32(other)) {
        uint32_t old = rd32(self) - 12;
        wr32(self, grab(other));
        rep_dispose(old);
    }
    RET(self);
}
static void h_string_append_cstr(CPU *c)
{
    uint32_t self = ARG(0), n = str_len(self), k = strlen(G(ARG(1)));
    uint32_t rep = new_rep(G(rd32(self)), n + k);
    wr32(rep, n + k);
    memmove(G(rep + 12 + n), G(ARG(1)), k);
    *G(rep + 12 + n + k) = 0;
    uint32_t old = rd32(self) - 12;
    wr32(self, rep + 12);
    rep_dispose(old);
    RET(self);
}
static void h_string_length(CPU *c) { RET(str_len(ARG(0))); }
static void h_string_c_str(CPU *c) { RET(rd32(ARG(0))); }
static void h_string_find(CPU *c)
{
    uint32_t n = str_len(ARG(0)), pos = ARG(2);
    const char *s = G(rd32(ARG(0))), *w = G(ARG(1));
    size_t k = strlen(w);
    for (uint32_t i = pos; i + k <= n; i++)
        if (!memcmp(s + i, w, k)) RET(i);
    RET(0xffffffffu);
}
static void h_string_rfind(CPU *c)
{
    uint32_t n = str_len(ARG(0)), pos = ARG(2);
    const char *s = G(rd32(ARG(0))), *w = G(ARG(1));
    size_t k = strlen(w);
    if (k > n) RET(0xffffffffu);
    uint32_t i = n - (uint32_t)k < pos ? n - (uint32_t)k : pos;
    for (;; i--) {
        if (!memcmp(s + i, w, k)) RET(i);
        if (!i) break;
    }
    RET(0xffffffffu);
}
// the result comes back through a hidden pointer the callee pops (ret $4)
static void h_string_substr(CPU *c)
{
    uint32_t out = ARG(0), self = ARG(1), pos = ARG(2), n = ARG(3), len = str_len(self);
    if (pos > len) host_fatal("std::string::substr: position out of range");
    if (n > len - pos) n = len - pos;
    wr32(out, new_rep(G(rd32(self) + pos), n) + 12);
    c->r[ESP] += 4;
    RET(out);
}

// std::list's node base: {next, prev}
#define NEXT(n) rd32(n)
#define PREV(n) rd32((n) + 4)
static void h_list_hook(CPU *c)
{
    uint32_t self = ARG(0), pos = ARG(1);
    wr32(self, pos);
    wr32(self + 4, PREV(pos));
    wr32(PREV(pos), self);
    wr32(pos + 4, self);
}
static void h_list_unhook(CPU *c)
{
    uint32_t self = ARG(0);
    wr32(PREV(self), NEXT(self));
    wr32(NEXT(self) + 4, PREV(self));
}
static void h_list_transfer(CPU *c)   // move [first, last) in front of self
{
    uint32_t self = ARG(0), first = ARG(1), last = ARG(2);
    if (self == last) return;
    wr32(PREV(last), self);
    wr32(PREV(first), last);
    wr32(PREV(self), first);
    uint32_t tmp = PREV(self);
    wr32(self + 4, PREV(last));
    wr32(last + 4, PREV(first));
    wr32(first + 4, tmp);
}
static void list_take(uint32_t to, uint32_t from)   // to gets from's nodes; from becomes empty
{
    wr32(to, NEXT(from));
    wr32(to + 4, PREV(from));
    wr32(NEXT(to) + 4, to);
    wr32(PREV(to), to);
    wr32(from, from);
    wr32(from + 4, from);
}
static void h_list_swap(CPU *c)
{
    uint32_t x = ARG(0), y = ARG(1);
    int xe = NEXT(x) == x, ye = NEXT(y) == y;
    if (!xe && !ye) {
        uint32_t xn = NEXT(x), xp = PREV(x);
        wr32(x, NEXT(y));
        wr32(x + 4, PREV(y));
        wr32(y, xn);
        wr32(y + 4, xp);
        wr32(NEXT(x) + 4, x);
        wr32(PREV(x), x);
        wr32(NEXT(y) + 4, y);
        wr32(PREV(y), y);
    } else if (!xe) list_take(y, x);
    else if (!ye) list_take(x, y);
}

// std::_Rb_tree's node base: {color (0 red, 1 black), parent, left, right};
// the header's parent is the root, its left the leftmost, its right the rightmost
#define COLOR(n) rd32(n)
#define PARENT(n) rd32((n) + 4)
#define LEFT(n) rd32((n) + 8)
#define RIGHT(n) rd32((n) + 12)
#define SET_COLOR(n, v) wr32(n, v)
#define SET_PARENT(n, v) wr32((n) + 4, v)
#define SET_LEFT(n, v) wr32((n) + 8, v)
#define SET_RIGHT(n, v) wr32((n) + 12, v)
enum { RED = 0, BLACK = 1 };
static uint32_t rb_increment(uint32_t x)
{
    if (RIGHT(x)) {
        x = RIGHT(x);
        while (LEFT(x)) x = LEFT(x);
    } else {
        uint32_t y = PARENT(x);
        while (x == RIGHT(y)) {
            x = y;
            y = PARENT(y);
        }
        if (RIGHT(x) != y) x = y;
    }
    return x;
}
static uint32_t rb_decrement(uint32_t x)
{
    if (COLOR(x) == RED && PARENT(PARENT(x)) == x) x = RIGHT(x);   // the header: the rightmost
    else if (LEFT(x)) {
        x = LEFT(x);
        while (RIGHT(x)) x = RIGHT(x);
    } else {
        uint32_t y = PARENT(x);
        while (x == LEFT(y)) {
            x = y;
            y = PARENT(y);
        }
        x = y;
    }
    return x;
}
static void h_rb_increment(CPU *c) { RET(rb_increment(ARG(0))); }
static void h_rb_decrement(CPU *c) { RET(rb_decrement(ARG(0))); }
static void rb_rotate_left(uint32_t x, uint32_t header)
{
    uint32_t y = RIGHT(x);
    SET_RIGHT(x, LEFT(y));
    if (LEFT(y)) SET_PARENT(LEFT(y), x);
    SET_PARENT(y, PARENT(x));
    if (x == PARENT(header)) SET_PARENT(header, y);
    else if (x == LEFT(PARENT(x))) SET_LEFT(PARENT(x), y);
    else SET_RIGHT(PARENT(x), y);
    SET_LEFT(y, x);
    SET_PARENT(x, y);
}
static void rb_rotate_right(uint32_t x, uint32_t header)
{
    uint32_t y = LEFT(x);
    SET_LEFT(x, RIGHT(y));
    if (RIGHT(y)) SET_PARENT(RIGHT(y), x);
    SET_PARENT(y, PARENT(x));
    if (x == PARENT(header)) SET_PARENT(header, y);
    else if (x == RIGHT(PARENT(x))) SET_RIGHT(PARENT(x), y);
    else SET_LEFT(PARENT(x), y);
    SET_RIGHT(y, x);
    SET_PARENT(x, y);
}
// (insert_left, x, p, header): x becomes p's left or right child
static void h_rb_insert(CPU *c)
{
    uint32_t left = ARG(0) & 0xff, x = ARG(1), p = ARG(2), header = ARG(3);
    SET_PARENT(x, p);
    SET_LEFT(x, 0);
    SET_RIGHT(x, 0);
    SET_COLOR(x, RED);
    if (left) {
        SET_LEFT(p, x);
        if (p == header) {
            SET_PARENT(header, x);
            SET_RIGHT(header, x);
        } else if (p == LEFT(header)) SET_LEFT(header, x);
    } else {
        SET_RIGHT(p, x);
        if (p == RIGHT(header)) SET_RIGHT(header, x);
    }
    while (x != PARENT(header) && COLOR(PARENT(x)) == RED) {
        uint32_t xp = PARENT(x), xpp = PARENT(xp);
        if (xp == LEFT(xpp)) {
            uint32_t y = RIGHT(xpp);
            if (y && COLOR(y) == RED) {
                SET_COLOR(xp, BLACK);
                SET_COLOR(y, BLACK);
                SET_COLOR(xpp, RED);
                x = xpp;
            } else {
                if (x == RIGHT(xp)) {
                    x = xp;
                    rb_rotate_left(x, header);
                }
                SET_COLOR(PARENT(x), BLACK);
                SET_COLOR(xpp, RED);
                rb_rotate_right(xpp, header);
            }
        } else {
            uint32_t y = LEFT(xpp);
            if (y && COLOR(y) == RED) {
                SET_COLOR(xp, BLACK);
                SET_COLOR(y, BLACK);
                SET_COLOR(xpp, RED);
                x = xpp;
            } else {
                if (x == LEFT(xp)) {
                    x = xp;
                    rb_rotate_right(x, header);
                }
                SET_COLOR(PARENT(x), BLACK);
                SET_COLOR(xpp, RED);
                rb_rotate_left(xpp, header);
            }
        }
    }
    SET_COLOR(PARENT(header), BLACK);
}
// (z, header): unlinks z, rebalances, returns the node to free (z)
static void h_rb_erase(CPU *c)
{
    uint32_t z = ARG(0), header = ARG(1), y = z, x, xp;
    if (!LEFT(y)) x = RIGHT(y);
    else if (!RIGHT(y)) x = LEFT(y);
    else {
        y = RIGHT(y);
        while (LEFT(y)) y = LEFT(y);
        x = RIGHT(y);
    }
    if (y != z) {   // z's successor y takes z's place
        SET_PARENT(LEFT(z), y);
        SET_LEFT(y, LEFT(z));
        if (y != RIGHT(z)) {
            xp = PARENT(y);
            if (x) SET_PARENT(x, PARENT(y));
            SET_LEFT(PARENT(y), x);
            SET_RIGHT(y, RIGHT(z));
            SET_PARENT(RIGHT(z), y);
        } else xp = y;
        if (PARENT(header) == z) SET_PARENT(header, y);
        else if (LEFT(PARENT(z)) == z) SET_LEFT(PARENT(z), y);
        else SET_RIGHT(PARENT(z), y);
        SET_PARENT(y, PARENT(z));
        uint32_t t = COLOR(y);
        SET_COLOR(y, COLOR(z));
        SET_COLOR(z, t);
        y = z;
    } else {
        xp = PARENT(y);
        if (x) SET_PARENT(x, PARENT(y));
        if (PARENT(header) == z) SET_PARENT(header, x);
        else if (LEFT(PARENT(z)) == z) SET_LEFT(PARENT(z), x);
        else SET_RIGHT(PARENT(z), x);
        if (LEFT(header) == z) {
            if (!RIGHT(z)) SET_LEFT(header, PARENT(z));
            else {
                uint32_t m = x;
                while (LEFT(m)) m = LEFT(m);
                SET_LEFT(header, m);
            }
        }
        if (RIGHT(header) == z) {
            if (!LEFT(z)) SET_RIGHT(header, PARENT(z));
            else {
                uint32_t m = x;
                while (RIGHT(m)) m = RIGHT(m);
                SET_RIGHT(header, m);
            }
        }
    }
    if (COLOR(y) != RED) {
        while (x != PARENT(header) && (!x || COLOR(x) == BLACK)) {
            if (x == LEFT(xp)) {
                uint32_t w = RIGHT(xp);
                if (COLOR(w) == RED) {
                    SET_COLOR(w, BLACK);
                    SET_COLOR(xp, RED);
                    rb_rotate_left(xp, header);
                    w = RIGHT(xp);
                }
                if ((!LEFT(w) || COLOR(LEFT(w)) == BLACK) && (!RIGHT(w) || COLOR(RIGHT(w)) == BLACK)) {
                    SET_COLOR(w, RED);
                    x = xp;
                    xp = PARENT(xp);
                } else {
                    if (!RIGHT(w) || COLOR(RIGHT(w)) == BLACK) {
                        SET_COLOR(LEFT(w), BLACK);
                        SET_COLOR(w, RED);
                        rb_rotate_right(w, header);
                        w = RIGHT(xp);
                    }
                    SET_COLOR(w, COLOR(xp));
                    SET_COLOR(xp, BLACK);
                    if (RIGHT(w)) SET_COLOR(RIGHT(w), BLACK);
                    rb_rotate_left(xp, header);
                    break;
                }
            } else {
                uint32_t w = LEFT(xp);
                if (COLOR(w) == RED) {
                    SET_COLOR(w, BLACK);
                    SET_COLOR(xp, RED);
                    rb_rotate_right(xp, header);
                    w = LEFT(xp);
                }
                if ((!RIGHT(w) || COLOR(RIGHT(w)) == BLACK) && (!LEFT(w) || COLOR(LEFT(w)) == BLACK)) {
                    SET_COLOR(w, RED);
                    x = xp;
                    xp = PARENT(xp);
                } else {
                    if (!LEFT(w) || COLOR(LEFT(w)) == BLACK) {
                        SET_COLOR(RIGHT(w), BLACK);
                        SET_COLOR(w, RED);
                        rb_rotate_left(w, header);
                        w = LEFT(xp);
                    }
                    SET_COLOR(w, COLOR(xp));
                    SET_COLOR(xp, BLACK);
                    if (LEFT(w)) SET_COLOR(LEFT(w), BLACK);
                    rb_rotate_right(xp, header);
                    break;
                }
            }
        }
        if (x) SET_COLOR(x, BLACK);
    }
    RET(y);
}

// __gnu_cxx's atomics on guest memory
static void h_atomic_add(CPU *c) { __atomic_fetch_add((int32_t *)G(ARG(0)), (int32_t)ARG(1), __ATOMIC_SEQ_CST); }
static void h_exchange_and_add(CPU *c) { RET(__atomic_fetch_add((int32_t *)G(ARG(0)), (int32_t)ARG(1), __ATOMIC_SEQ_CST)); }

// dynamic_cast (Itanium C++ ABI): from the most derived object, look for the
// target class among its bases. The type_info kinds are told apart by their
// vtables, which the program holds as copies of libstdc++'s.
static uint32_t ti_si, ti_vmi;
static int same_type(uint32_t a, uint32_t b)
{
    return a == b || !strcmp(G(rd32(a + 4)), G(rd32(b + 4)));
}
// offset of target within an object of type t at obj, or -1
static int64_t find_base(uint32_t t, uint32_t obj, uint32_t target, int depth)
{
    if (same_type(t, target)) return obj;
    if (depth > 32) return -1;
    uint32_t vp = rd32(t);
    if (ti_si && vp == ti_si) return find_base(rd32(t + 8), obj, target, depth + 1);
    if (ti_vmi && vp == ti_vmi) {
        uint32_t n = rd32(t + 12);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t base = rd32(t + 16 + 8 * i);
            int32_t flags = (int32_t)rd32(t + 20 + 8 * i), off = flags >> 8;
            if (!(flags & 2)) continue;                    // not public
            uint32_t sub = flags & 1 ? obj + rd32(rd32(obj) + off) : obj + off;   // virtual: from the vtable
            int64_t r = find_base(base, sub, target, depth + 1);
            if (r >= 0) return r;
        }
    }
    return -1;
}
static void h_dynamic_cast(CPU *c)
{
    uint32_t p = ARG(0), dst = ARG(2);
    if (!p) RET(0);
    uint32_t vptr = rd32(p);
    uint32_t whole = p + rd32(vptr - 8), whole_type = rd32(vptr - 4);
    int64_t r = find_base(whole_type, whole, dst, 0);
    RET(r < 0 ? 0 : (uint32_t)r);
}

// C++ exceptions: nothing unwinds translated code yet, so a throw stops the game
static void h_cxa_allocate_exception(CPU *c) { RET(guest_calloc(1, ARG(0) + 128) + 128); }
static void h_cxa_throw(CPU *c)
{
    char why[256];
    uint32_t ti = ARG(1);
    snprintf(why, sizeof why, "C++ exception %s thrown (from 0x%08x)", ti ? G(rd32(ti + 4)) : "?", rd32(c->r[ESP]));
    host_fatal(why);
}
static void h_throw_msg(CPU *c)
{
    char why[256];
    snprintf(why, sizeof why, "C++ library error: %s", ARG(0) ? G(ARG(0)) : "?");
    host_fatal(why);
}

// files, time, processes
static void write_stat(uint32_t b, const struct stat *st)
{
    memset(M + b, 0, 88);
    wr64(b, st->st_dev);
    wr32(b + 12, (uint32_t)st->st_ino);
    wr32(b + 16, st->st_mode);
    wr32(b + 20, (uint32_t)st->st_nlink);
    wr32(b + 24, st->st_uid);
    wr32(b + 28, st->st_gid);
    wr32(b + 44, (uint32_t)st->st_size);
    wr32(b + 48, (uint32_t)st->st_blksize);
    wr32(b + 52, (uint32_t)st->st_blocks);
    wr32(b + 56, (uint32_t)st->st_atime);
    wr32(b + 64, (uint32_t)st->st_mtime);
    wr32(b + 72, (uint32_t)st->st_ctime);
}
static void h_fxstat(CPU *c)
{
    struct stat st;
    if (fstat((int)ARG(1), &st)) RET((uint32_t)-1);
    write_stat(ARG(2), &st);
    RET(0);
}
static void h_lxstat(CPU *c)
{
    char buf[2048];
    struct stat st;
    if (lstat(map_path(G(ARG(1)), buf, sizeof buf), &st)) RET((uint32_t)-1);
    write_stat(ARG(2), &st);
    RET(0);
}
static void h_remove(CPU *c)
{
    char buf[2048];
    RET(remove(map_path(G(ARG(0)), buf, sizeof buf)));
}
static void h_rename(CPU *c)
{
    char a[2048], b[2048];
    RET(rename(map_path(G(ARG(0)), a, sizeof a), map_path(G(ARG(1)), b, sizeof b)));
}
static void h_localtime_r(CPU *c)
{
    time_t t = (time_t)(int32_t)rd32(ARG(0));
    struct tm tm;
    localtime_r(&t, &tm);
    uint32_t o = ARG(1);
    const int v[9] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    for (int i = 0; i < 9; i++) wr32(o + 4 * i, (uint32_t)v[i]);
    wr32(o + 36, (uint32_t)tm.tm_gmtoff);
    wr32(o + 40, 0);
    RET(o);
}
static void h_strtoll(CPU *c)
{
    char *end;
    long long v = strtoll(G(ARG(0)), &end, (int)ARG(2));
    if (ARG(1)) wr32(ARG(1), ARG(0) + (uint32_t)(end - G(ARG(0))));
    RETQ((uint64_t)v);
}
static void h_toupper(CPU *c) { RET((uint32_t)toupper((int)ARG(0))); }
static void h_exit_now(CPU *c) { host_exit((int)ARG(0)); }
static void h_affinity(CPU *c)
{
    if (ARG(2) && ARG(1)) memset(G(ARG(2)), 0xff, ARG(1) < 128 ? ARG(1) : 128);
    RET(0);
}

// ---- OutRun 2 SP SDX ----------------------------------------------------------------------------
static void h_sigfillset(CPU *c) { memset(G(ARG(0)), 0xff, 128); RET(0); }
static void h_isnanf(CPU *c) { RET(isnan(ARGF(0)) ? 1 : 0); }
static void h_perror(CPU *c) { hle_log("perror: %s\n", ARG(0) ? G(ARG(0)) : ""); RET(0); }
static void h_tolower(CPU *c) { RET((uint32_t)tolower((int)ARG(0))); }
static void h_syslog(CPU *c) { hle_log("syslog: %s\n", G(ARG(1))); RET(0); }
static void h_fixunsdfdi(CPU *c) { double d = ARGD(0); RETQ(d <= 0 ? 0 : (uint64_t)d); }
static void h_log10f(CPU *c) { RETF(log10f(ARGF(0))); }
// the game's directory: relative paths start there
static void h_chdir(CPU *c)
{
    char buf[2048], real[PATH_MAX];
    const char *p = map_path(G(ARG(0)), buf, sizeof buf);
    if (!realpath(p, real)) RET(0xffffffffu);
    if (hle_verbose) hle_log("chdir %s -> %s\n", G(ARG(0)), real);
    snprintf(elf_dir, sizeof elf_dir, "%s", real);
    RET(0);
}
// iostreams are not used beyond their static initialiser
static void h_ios_init(CPU *c) { RET(0); }
static void h_string_append_n(CPU *c)
{
    uint32_t self = ARG(0), n = str_len(self), k = ARG(2);
    uint32_t rep = new_rep(G(rd32(self)), n + k);
    memmove(G(rep + 12 + n), G(ARG(1)), k);
    *G(rep + 12 + n + k) = 0;
    uint32_t old = rd32(self) - 12;
    wr32(self, rep + 12);
    rep_dispose(old);
    RET(self);
}
// replace(pos, n1, s, n2)
static void h_string_replace(CPU *c)
{
    uint32_t self = ARG(0), n = str_len(self), pos = ARG(1), n1 = ARG(2), k = ARG(4);
    if (pos > n) pos = n;
    if (n1 > n - pos) n1 = n - pos;
    char *tmp = malloc(n - n1 + k + 1);
    memcpy(tmp, G(rd32(self)), pos);
    memcpy(tmp + pos, G(ARG(3)), k);
    memcpy(tmp + pos + k, G(rd32(self)) + pos + n1, n - pos - n1);
    uint32_t rep = new_rep(tmp, n - n1 + k);
    free(tmp);
    uint32_t old = rd32(self) - 12;
    wr32(self, rep + 12);
    rep_dispose(old);
    RET(self);
}
static void h_rep_destroy(CPU *c) { guest_free(ARG(0)); RET(0); }

static const hle_entry libc_table3[] = {
    {"sigfillset", h_sigfillset}, {"sigaction", h_nop}, {"__isnanf", h_isnanf}, {"perror", h_perror},
    {"tolower", h_tolower}, {"syslog", h_syslog}, {"openlog", h_nop}, {"closelog", h_nop}, {"chdir", h_chdir},
    {"__fixunsdfdi", h_fixunsdfdi}, {"log10f", h_log10f}, {"tcflush", h_nop}, {"alarm", h_nop}, {"sync", h_nop},
    {"_ZNSt8ios_base4InitC1Ev", h_ios_init}, {"_ZNSt8ios_base4InitD1Ev", h_ios_init},
    {"_ZNSs6appendEPKcj", h_string_append_n}, {"_ZNSs7replaceEjjPKcj", h_string_replace},
    {"_ZNSs4_Rep10_M_destroyERKSaIcE", h_rep_destroy},
    {"__intel_new_proc_init_P", h_intel_init}, {"_intel_fast_memcpy", h_intel_memcpy},
    {"_intel_fast_memset", h_intel_memset}, {"_intel_fast_memcmp", h_intel_memcmp},
    {"__intel_VEC_memcpy", h_intel_memcpy}, {"__intel_VEC_memzero", h_intel_memset},
    {"__libm_sse2_acos", h_sse2_acos}, {"__libm_sse2_atan", h_sse2_atan}, {"__libm_sse2_atan2", h_sse2_atan2},
    {"__libm_sse2_sin", h_sse2_sin}, {"__libm_sse2_sincos", h_sse2_sincos}, {"__libm_sse2_acosf", h_sse2_acosf},
    {"__libm_sse2_asinf", h_sse2_asinf}, {"__libm_sse2_atanf", h_sse2_atanf}, {"__libm_sse2_cosf", h_sse2_cosf},
    {"__libm_sse2_sinf", h_sse2_sinf}, {"__libm_sse2_tanf", h_sse2_tanf}, {"__libm_sse2_logf", h_sse2_logf},
    {"__libm_sse2_log10f", h_sse2_log10f}, {"__libm_sse2_sincosf", h_sse2_sincosf},
    {"__finitef", h_finitef2}, {"atan2", h_atan2}, {"atan2f", h_atan2f}, {"atanf", h_atanf}, {"fmod", h_fmod},
    {"fmodf", h_fmodf}, {"sqrt", h_sqrt}, {"sqrtf", h_sqrtf},
    {"_ZNSsC1EPKcRKSaIcE", h_string_ctor_cstr}, {"_ZNSsC1ERKSs", h_string_ctor_copy}, {"_ZNSsaSERKSs", h_string_assign},
    {"_ZNSspLEPKc", h_string_append_cstr}, {"_ZNKSs6lengthEv", h_string_length}, {"_ZNKSs5c_strEv", h_string_c_str},
    {"_ZNKSs4findEPKcj", h_string_find}, {"_ZNKSs5rfindEPKcj", h_string_rfind}, {"_ZNKSs6substrEjj", h_string_substr},
    {"_ZNSaIcEC1Ev", h_nop}, {"_ZNSaIcED1Ev", h_nop}, {"_ZNSt9bad_allocD1Ev", h_nop},
    {"_ZNSt15_List_node_base4hookEPS_", h_list_hook}, {"_ZNSt15_List_node_base6unhookEv", h_list_unhook},
    {"_ZNSt15_List_node_base8transferEPS_S0_", h_list_transfer}, {"_ZNSt15_List_node_base4swapERS_S0_", h_list_swap},
    {"_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", h_rb_increment},
    {"_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", h_rb_increment},
    {"_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", h_rb_decrement},
    {"_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", h_rb_insert},
    {"_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", h_rb_erase},
    {"_ZN9__gnu_cxx12__atomic_addEPVii", h_atomic_add}, {"_ZN9__gnu_cxx18__exchange_and_addEPVii", h_exchange_and_add},
    {"__dynamic_cast", h_dynamic_cast}, {"__cxa_allocate_exception", h_cxa_allocate_exception},
    {"__cxa_throw", h_cxa_throw}, {"__cxa_bad_typeid", h_cxa_fatal}, {"__cxa_guard_abort", h_nop},
    {"_ZSt10unexpectedv", h_cxa_fatal}, {"_ZSt19__throw_logic_errorPKc", h_throw_msg},
    {"_ZSt20__throw_length_errorPKc", h_throw_msg}, {"_ZSt20__throw_out_of_rangePKc", h_throw_msg},
    {"__fxstat", h_fxstat}, {"__lxstat", h_lxstat}, {"__xmknod", h_fail}, {"remove", h_remove}, {"rename", h_rename},
    {"localtime_r", h_localtime_r}, {"strtoll", h_strtoll}, {"toupper", h_toupper}, {"_exit", h_exit_now},
    {"wait", h_fail}, {"daemon", h_null}, {"flock", h_null}, {"dl_iterate_phdr", h_null}, {"setrlimit", h_null},
    {"sched_getaffinity", h_affinity}, {"sched_setaffinity", h_null}, {"pthread_key_delete", h_null},
    {"inet_pton", h_null},
    // the network (sockets, on the loopback) and serial ports without a device behind them
    {"socket", h_socket}, {"bind", h_bind}, {"listen", h_listen}, {"connect", h_connect}, {"accept", h_accept},
    {"shutdown", h_shutdown}, {"getsockname", h_getsockname}, {"getsockopt", h_getsockopt},
    {"setsockopt", h_setsockopt}, {"recv", h_recv}, {"recvfrom", h_recvfrom}, {"recvmsg", h_recvmsg},
    {"send", h_send}, {"sendto", h_sendto}, {"sendmsg", h_sendmsg}, {"tcgetattr", h_null}, {"tcsetattr", h_null}, {"cfmakeraw", h_nop},
    {"cfsetispeed", h_null}, {"cfsetospeed", h_null},
    {NULL, NULL},
};

static const hle_entry libc_table[] = {
    {"fopen", h_fopen}, {"fopen64", h_fopen}, {"fclose", h_fclose}, {"fread", h_fread},
    {"fwrite", h_fwrite}, {"fseek", h_fseek}, {"ftell", h_ftell}, {"rewind", h_rewind},
    {"ferror", h_ferror}, {"fprintf", h_fprintf}, {"printf", h_printf}, {"puts", h_puts},
    {"_IO_putc", h_IO_putc}, {"sprintf", h_sprintf}, {"snprintf", h_snprintf},
    {"vsprintf", h_vsprintf}, {"vsnprintf", h_vsnprintf}, {"sscanf", h_sscanf},
    {"open", h_open}, {"open64", h_open}, {"close", h_close}, {"read", h_read}, {"write", h_write},
    {"lseek", h_lseek}, {"lseek64", h_lseek64}, {"fsync", h_fsync}, {"fcntl", h_fcntl},
    {"ioctl", h_ioctl}, {"__xstat", h_xstat}, {"select", h_select},
    {"malloc", h_malloc}, {"calloc", h_calloc}, {"free", h_free}, {"memcpy", h_memcpy},
    {"memmove", h_memmove}, {"memset", h_memset}, {"strcpy", h_strcpy}, {"strncpy", h_strncpy},
    {"strcat", h_strcat}, {"strncat", h_strncat}, {"strcmp", h_strcmp}, {"strncmp", h_strncmp},
    {"strspn", h_strspn}, {"strcspn", h_strcspn}, {"strchr", h_strchr}, {"strrchr", h_strrchr},
    {"strstr", h_strstr}, {"strpbrk", h_strpbrk}, {"strtok", h_strtok}, {"isdigit", h_isdigit},
    {"atoi", h_atoi}, {"atof", h_atof}, {"__strtol_internal", h_strtol_internal},
    {"__strtoul_internal", h_strtoul_internal}, {"__ctype_b_loc", h_ctype_b_loc},
    {"srand", h_srand}, {"rand", h_rand}, {"qsort", h_qsort},
    {"cos", h_cos}, {"pow", h_pow}, {"hypot", h_hypot}, {"log10", h_log10}, {"cosf", h_cosf},
    {"sinf", h_sinf}, {"coshf", h_coshf}, {"sinhf", h_sinhf}, {"tanhf", h_tanhf},
    {"logf", h_logf}, {"powf", h_powf},
    {"__divdi3", h_divdi3}, {"__udivdi3", h_udivdi3}, {"__umoddi3", h_umoddi3},
    {"time", h_time}, {"gettimeofday", h_gettimeofday}, {"localtime", h_localtime},
    {"usleep", h_usleep}, {"sleep", h_sleep},
    {"getenv", h_getenv}, {"signal", h_signal}, {"system", h_system}, {"raise", h_raise},
    {"creat", h_creat}, {"putchar", h_putchar}, {"popen", h_popen},
    {"pclose", h_pclose}, {"sysctl", h_sysctl}, {"iopl", h_iopl}, {"exit", h_exit},
    {"__libc_start_main", h_libc_start_main}, {"__gmon_start__", h_nop},
    {"_Jv_RegisterClasses", h_nop},
    {"inet_addr", h_inet_addr}, {"inet_aton", h_inet_aton}, {"inet_ntoa", h_inet_ntoa},
    {"_Znwj", h_new}, {"_Znaj", h_new}, {"_ZdlPv", h_delete}, {"_ZdaPv", h_delete},
    {"_ZNSt24__default_alloc_templateILb1ELi0EE8allocateEj", h_alloc_allocate},
    {"_ZNSt24__default_alloc_templateILb1ELi0EE10deallocateEPvj", h_alloc_deallocate},
    {"_ZNSsC1Ev", h_string_ctor}, {"_ZNSsD1Ev", h_string_dtor}, {"_ZNSsaSEPKc", h_string_assign_cstr},
    {"__cxa_atexit", h_cxa_atexit}, {"__cxa_guard_acquire", h_cxa_guard_acquire},
    {"__cxa_guard_release", h_cxa_guard_release}, {"__cxa_pure_virtual", h_cxa_fatal},
    {"__cxa_begin_catch", h_cxa_fatal}, {"__cxa_end_catch", h_cxa_fatal},
    {"__cxa_rethrow", h_cxa_fatal}, {"_Unwind_Resume", h_cxa_fatal},
    {"__gxx_personality_v0", h_cxa_fatal},
    {"__cxa_vec_new", h_cxa_vec_new}, {"__cxa_vec_ctor", h_cxa_vec_ctor},
    {"__cxa_vec_cctor", h_cxa_vec_cctor}, {"__cxa_vec_dtor", h_cxa_vec_dtor},
    {"__cxa_vec_delete", h_cxa_vec_delete},
    {NULL, NULL},
};


void hle_libc_init(void)
{
    hle_register(libc_table);
    hle_register(libc_table2);
    hle_register(libc_table3);
    // the type_info vtables the program copies from libstdc++ (for dynamic_cast)
    ti_si = ti_vmi = 0;
    for (unsigned i = 1; i <= prog->copy_count; i++) {
        if (!strcmp(prog->copy_names[i], "_ZTVN10__cxxabiv120__si_class_type_infoE")) ti_si = prog->copy_addr[i] + 8;
        if (!strcmp(prog->copy_names[i], "_ZTVN10__cxxabiv121__vmi_class_type_infoE")) ti_vmi = prog->copy_addr[i] + 8;
    }
    build_ctype();
    build_case_tables();
    empty_rep = hostdata_alloc(16);
    guest_stdout = new_handle(stdout);
    guest_stderr = new_handle(stderr);
    guest_stdin = new_handle(stdin);
    for (unsigned i = 1; i <= prog->copy_count; i++) {
        if (!strcmp(prog->copy_names[i], "stdout")) wr32(prog->copy_addr[i], guest_stdout);
        else if (!strcmp(prog->copy_names[i], "stderr")) wr32(prog->copy_addr[i], guest_stderr);
    }
}

uint32_t hle_object(const char *name)
{
    uint32_t v;
    // the FILE objects themselves
    if (!strcmp(name, "_IO_2_1_stdout_")) return guest_stdout;
    if (!strcmp(name, "_IO_2_1_stderr_")) return guest_stderr;
    if (!strcmp(name, "_IO_2_1_stdin_")) return guest_stdin;
    // glibc 2.1's ctype tables are pointers in data
    if (!strcmp(name, "__ctype_b")) return ctype_ptr;
    if (!strcmp(name, "__ctype_tolower")) return lower_cell;
    if (!strcmp(name, "__ctype_toupper")) return upper_cell;
    if (!strcmp(name, "stdout")) v = guest_stdout;
    else if (!strcmp(name, "stderr")) v = guest_stderr;
    else if (!strcmp(name, "stdin")) v = guest_stdin;
    else return 0;
    uint32_t cell = hostdata_alloc(4);
    wr32(cell, v);
    return cell;
}

// a core stopped the game: close what it left open, forget its handles
void hle_libc_reset(void)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i] && files[i] != stdout && files[i] != stderr && files[i] != stdin) fclose(files[i]);
    memset(files, 0, sizeof files);
    memset(file_handle, 0, sizeof file_handle);
    guest_stdout = guest_stderr = guest_stdin = 0;
    for (int i = 0; i < MAX_DIRS; i++)
        if (dirs[i]) closedir(dirs[i]);
    memset(dirs, 0, sizeof dirs);
    memset(dir_ent, 0, sizeof dir_ent);
    memset(env_value, 0, sizeof env_value);
    errstr = lower_cell = upper_cell = lconv_addr = c_locale = errno_cell = 0;
    for (int fd = 0; fd < MAX_FDS; fd++)
        if (fd_guest[fd]) close(fd);
    memset(fd_dev, 0, sizeof fd_dev);
    memset(fd_guest, 0, sizeof fd_guest);
    memset(fd_sock, 0, sizeof fd_sock);
    strtok_save = ctype_ptr = tm_buf = ntoa_buf = empty_rep = 0;
    memset(r_state, 0, sizeof r_state);
    r_f = r_r = 0;
}
