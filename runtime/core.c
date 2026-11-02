// Guest memory, ELF loading, dispatch and the CPU helpers generated code calls.
#include "hle.h"
#include "host.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint8_t *M;
int hle_verbose = 1;
int hle_trace;
char game_root[1024];
char save_root[1024];
char elf_dir[1100];

const gen_program *prog;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

// The log, with a message repeated many times (a game's message every frame)
// cut down: the first 20 times in full, then once every 1000 with its count.
// iOS throttles an app that logs too much, and logging costs time.
// what was printed since the last hle_log_stats, by message with its numbers
// left out, for the perf: line (which message fills the log on a device)
static struct { uint32_t hash; unsigned count; char text[96]; } shape[64];
static unsigned printed_lines, printed_bytes;

void hle_log(const char *fmt, ...)
{
    static struct { uint32_t hash; unsigned count; } seen[256];
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uint32_t h = 2166136261u, hs = 2166136261u;
    for (const char *p = buf; *p; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
        if (*p < '0' || *p > '9') hs = (hs ^ (unsigned char)*p) * 16777619u;
    }
    pthread_mutex_lock(&log_lock);
    unsigned *count = &seen[h & 255].count;
    if (seen[h & 255].hash != h) seen[h & 255].hash = h, *count = 0;
    ++*count;
    int out = 0;
    if (*count <= 20) fputs(buf, stderr), out = 1;
    else if (*count % 1000 == 0) fprintf(stderr, "(the next line has been logged %u times)\n%s", *count, buf), out = 1;
    if (out) {
        printed_lines++, printed_bytes += (unsigned)strlen(buf);
        int k = hs & 63;
        if (shape[k].hash != hs) {
            shape[k].hash = hs, shape[k].count = 0;
            snprintf(shape[k].text, sizeof shape[k].text, "%.*s", (int)strcspn(buf, "\n"), buf);
        }
        shape[k].count++;
    }
    fflush(stderr);
    pthread_mutex_unlock(&log_lock);
}

void hle_log_stats(char *buf, size_t size)
{
    pthread_mutex_lock(&log_lock);
    int top = 0;
    for (int k = 1; k < 64; k++)
        if (shape[k].count > shape[top].count) top = k;
    if (shape[top].count > 1)
        snprintf(buf, size, "log: %u lines, %u KB since the last perf: line; most often (%u times): %s", printed_lines,
                 printed_bytes / 1024, shape[top].count, shape[top].text);
    else
        snprintf(buf, size, "log: %u lines, %u KB since the last perf: line", printed_lines, printed_bytes / 1024);
    memset(shape, 0, sizeof shape);
    printed_lines = printed_bytes = 0;
    pthread_mutex_unlock(&log_lock);
}

static void mem_init(void)
{
    // 4 GB of address space, committed lazily
    void *p = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) host_fatal("cannot reserve the guest's 4 GB address space");
    M = p;
}

// ---- ELF ------------------------------------------------------------------
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } Phdr;

// the PT_LOAD segments of an ELF file at base
static int load_segments(const char *path, uint32_t base, uint32_t *entry)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        hle_log("cannot open %s\n", path);
        return -1;
    }
    Ehdr eh;
    if (fread(&eh, sizeof eh, 1, f) != 1) return -1;
    for (int i = 0; i < eh.phnum; i++) {
        Phdr ph;
        fseek(f, eh.phoff + i * eh.phentsize, SEEK_SET);
        if (fread(&ph, sizeof ph, 1, f) != 1) return -1;
        if (ph.type != 1) continue;
        fseek(f, ph.offset, SEEK_SET);
        if (ph.filesz && fread(M + base + ph.vaddr, 1, ph.filesz, f) != ph.filesz) return -1;
        memset(M + base + ph.vaddr + ph.filesz, 0, ph.memsz - ph.filesz);
    }
    fclose(f);
    if (entry) *entry = base + eh.entry;
    return 0;
}

int load_elf(const char *path, uint32_t *entry)
{
    if (load_segments(path, 0, entry)) return -1;
    // the shared libraries translated with the program sit next to it
    for (unsigned i = 0; i < prog->mod_count; i++) {
        char lib[1024];
        const char *slash = strrchr(path, '/');
        snprintf(lib, sizeof lib, "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, prog->mod_file[i]);
        if (load_segments(lib, prog->mod_base[i], NULL)) return -1;
        hle_log("loaded %s at 0x%08x\n", prog->mod_file[i], prog->mod_base[i]);
    }
    // imports resolve to their PLT stubs, which call the host layer
    for (unsigned i = 0; i < prog->import_count; i++) wr32(prog->import_got[i], prog->import_plt[i]);
    return 0;
}

// ---- dispatch -------------------------------------------------------------
#define MAX_MAGIC 4096
#define MAX_IMPORTS 8192

// Calls into OpenGL and the time spent in them, per entry point, for the
// perf: line (gl_prof: on with it). Imports by index, then the functions the
// game took by address (glXGetProcAddress).
int gl_prof, gl_clock;
static struct { const char *name; unsigned long long n; double t; } glp[MAX_IMPORTS + MAX_MAGIC];
static _Thread_local double gl_spent;   // this thread's time in OpenGL (gl_clock: on)
static double prof_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
double hle_cpu_now(void) { return prof_now() - gl_spent; }

// calls and time per frame over `frames`: the costliest and the most called; then reset
void hle_gl_prof(char *buf, size_t size, int frames)
{
    double total = 0, n = 0;
    int top[6] = {-1, -1, -1, -1, -1, -1}, most[6] = {-1, -1, -1, -1, -1, -1};
    for (int i = 0; i < MAX_IMPORTS + MAX_MAGIC; i++) {
        if (!glp[i].n) continue;
        total += glp[i].t, n += (double)glp[i].n;
        for (int k = 0; k < 6; k++)
            if (top[k] < 0 || glp[i].t > glp[top[k]].t) {
                memmove(top + k + 1, top + k, (5 - k) * sizeof top[0]);
                top[k] = i;
                break;
            }
        for (int k = 0; k < 6; k++)
            if (most[k] < 0 || glp[i].n > glp[most[k]].n) {
                memmove(most + k + 1, most + k, (5 - k) * sizeof most[0]);
                most[k] = i;
                break;
            }
    }
    if (frames < 1) frames = 1;
    int len = snprintf(buf, size, "GL: %.0f calls, %.1f ms a frame; the most time:", n / frames, total * 1e3 / frames);
    for (int k = 0; k < 6 && top[k] >= 0 && len > 0 && (size_t)len < size; k++)
        len += snprintf(buf + len, size - (size_t)len, "%s %s %.0f/%.2f ms", k ? "," : "", glp[top[k]].name,
                        (double)glp[top[k]].n / frames, glp[top[k]].t * 1e3 / frames);
    if (len > 0 && (size_t)len < size) len += snprintf(buf + len, size - (size_t)len, "; the most calls:");
    for (int k = 0; k < 6 && most[k] >= 0 && len > 0 && (size_t)len < size; k++)
        len += snprintf(buf + len, size - (size_t)len, "%s %s %.0f", k ? "," : "", glp[most[k]].name,
                        (double)glp[most[k]].n / frames);
    memset(glp, 0, sizeof glp);
}
static hle_fn magic_fns[MAX_MAGIC];
static const char *magic_names[MAX_MAGIC];
static unsigned magic_count;
static pthread_mutex_t magic_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t magic_register(hle_fn fn, const char *name)
{
    pthread_mutex_lock(&magic_lock);
    for (unsigned i = 0; i < magic_count; i++)
        if (magic_fns[i] == fn) {
            pthread_mutex_unlock(&magic_lock);
            return MAGIC_BASE + i * 16;
        }
    unsigned i = magic_count++;
    magic_fns[i] = fn;
    magic_names[i] = name;
    pthread_mutex_unlock(&magic_lock);
    return MAGIC_BASE + i * 16;
}

guest_fn guest_lookup(uint32_t addr)
{
    unsigned lo = 0, hi = prog->fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (prog->fn_addr[mid] < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < prog->fn_count && prog->fn_addr[lo] == addr) return prog->fn_ptr[lo];
    return NULL;
}

void guest_dispatch(CPU *c, uint32_t addr)
{
    guest_fn f = guest_lookup(addr);
    if (f) {
        f(c);
        return;
    }
    if (addr >= MAGIC_BASE && addr < MAGIC_BASE + MAX_MAGIC * 16 && magic_fns[(addr - MAGIC_BASE) / 16]) {
        unsigned k = (addr - MAGIC_BASE) / 16;
        if ((gl_prof || gl_clock) && magic_names[k] && magic_names[k][0] == 'g' && magic_names[k][1] == 'l') {
            double t0 = prof_now();
            magic_fns[k](c);
            double t = prof_now() - t0;
            gl_spent += t;
            if (gl_prof) {
                glp[MAX_IMPORTS + k].t += t;
                glp[MAX_IMPORTS + k].n++;
                glp[MAX_IMPORTS + k].name = magic_names[k];
            }
        } else
            magic_fns[k](c);
        if (gl_check_hook && magic_names[k] && magic_names[k][0] == 'g' && magic_names[k][1] == 'l')
            gl_check_hook(magic_names[k]);
        c->r[ESP] += 4;
        return;
    }
    char why[128];
    snprintf(why, sizeof why, "no function at 0x%08x (called from 0x%08x)", addr, rd32(c->r[ESP]));
    host_fatal(why);
}

uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args)
{
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    uint32_t sp = c->r[ESP];
    for (int i = nargs - 1; i >= 0; i--) {
        sp -= 4;
        wr32(sp, args[i]);
    }
    sp -= 4;
    wr32(sp, RETURN_MAGIC);
    c->r[ESP] = sp;
    guest_dispatch(c, addr);
    uint32_t r = c->r[EAX];
    // cdecl: callee-saved registers are restored by the callee, the caller
    // pops the arguments
    saved[EAX] = r;
    saved[EDX] = c->r[EDX];
    saved[ECX] = c->r[ECX];
    memcpy(c->r, saved, sizeof saved);
    return r;
}

void guest_unimpl(CPU *c, uint32_t addr, const char *what)
{
    char why[160];
    snprintf(why, sizeof why, "unimplemented at 0x%08x: %s", addr, what);
    host_fatal(why);
}

void guest_bad_return(CPU *c, uint32_t from, uint32_t expected, uint32_t got)
{
    char why[128];
    snprintf(why, sizeof why, "bad return at 0x%08x: expected 0x%08x got 0x%08x", from, expected, got);
    host_fatal(why);
}

void guest_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d)
{
    // a 3 GHz Pentium 4 (Prescott) with SSE3
    switch (*a) {
    case 0:
        *a = 5;
        *b = 0x756e6547;
        *d = 0x49656e69;
        *cc = 0x6c65746e;
        break;
    case 1:
        *a = 0x00000f41;
        *b = 0x00010800;
        *cc = 0x0000641d;
        *d = 0xbfebfbff;
        break;
    default:
        *a = *b = *cc = *d = 0;
        break;
    }
}

uint64_t guest_rdtsc(CPU *c)
{
    // timing loops spin on rdtsc without calling an import: leave them here too
    if (guest_stopping) guest_exit_thread();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 3000000000ull + (uint64_t)ts.tv_nsec * 3;
}

// ---- x87 helpers ------------------------------------------------------------
double x87_load80(uint32_t a)
{
    uint64_t mant = rd64(a);
    uint16_t se = rd16(a + 8);
    int sign = se >> 15;
    int exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void x87_store80(uint32_t a, double v)
{
    uint64_t mant = 0;
    uint16_t se = 0;
    if (signbit(v)) se = 0x8000;
    v = fabs(v);
    if (v == 0.0) {
    } else if (isinf(v)) {
        se |= 0x7fff;
        mant = 1ull << 63;
    } else if (isnan(v)) {
        se |= 0x7fff;
        mant = 0xc000000000000000ull;
    } else {
        int e;
        double m = frexp(v, &e);   // v = m * 2^e, 0.5 <= m < 1
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    wr64(a, mant);
    wr16(a + 8, se);
}

static int64_t round_mode(double v, int mode)
{
    switch (mode & 3) {
    case 0: return (int64_t)nearbyint(v);   // host default is round-to-nearest-even
    case 1: return (int64_t)floor(v);
    case 2: return (int64_t)ceil(v);
    default: return (int64_t)trunc(v);
    }
}

int64_t x87_round(CPU *c, double v) { return round_mode(v, (c->fcw >> 10) & 3); }
int64_t sse_round(CPU *c, double v) { return round_mode(v, (c->mxcsr >> 13) & 3); }

// ---- per-thread CPU ---------------------------------------------------------
static __thread CPU *tls_cpu;
CPU *cpu_current(void) { return tls_cpu; }
void cpu_set_current(CPU *c) { tls_cpu = c; }

CPU *cpu_new(void)
{
    CPU *c = calloc(1, sizeof(CPU));
    c->fcw = 0x37f;
    c->mxcsr = 0x1f80;
    return c;
}

// ---- guest heap ---------------------------------------------------------------
// Size-class free lists for small blocks, an address-ordered free list with
// coalescing for large ones. Every block has an 8-byte header: size, magic.
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t heap_top = HEAP_BASE;
#define NCLASS 64
static uint32_t small_free[NCLASS];   // classes of 16 bytes up to 1 KB
static uint32_t large_free;            // singly linked, sorted by address: [hdr][next]
#define HDR_MAGIC 0x4c424748u
#define ALIGNED_MAGIC 0x4c42414cu   // before a memalign pointer: [block pointer][ALIGNED_MAGIC]

static uint32_t bump(uint32_t n)
{
    uint32_t p = heap_top;
    if (heap_top + n > HEAP_END || heap_top + n < heap_top) {
        host_fatal("guest heap exhausted");
    }
    heap_top += n;
    return p;
}

uint32_t guest_malloc(uint32_t n)
{
    if (n == 0) n = 1;
    uint32_t total = (n + 8 + 15) & ~15u;
    pthread_mutex_lock(&heap_lock);
    uint32_t blk = 0;
    if (total <= NCLASS * 16) {
        int k = total / 16 - 1;
        if (small_free[k]) {
            blk = small_free[k];
            small_free[k] = rd32(blk + 8);
        } else blk = bump(total);
    } else {
        total = (total + 4095) & ~4095u;
        uint32_t prev = 0, cur = large_free;
        while (cur && rd32(cur) < total) {
            prev = cur;
            cur = rd32(cur + 8);
        }
        if (cur) {
            uint32_t sz = rd32(cur), next = rd32(cur + 8);
            if (sz - total >= 4096) {
                uint32_t rest = cur + total;
                wr32(rest, sz - total);
                wr32(rest + 4, 0);
                wr32(rest + 8, next);
                next = rest;
            } else total = sz;
            if (prev) wr32(prev + 8, next);
            else large_free = next;
            blk = cur;
        } else {
            // keep large blocks page-aligned in the heap
            uint32_t pad = (16 - (heap_top & 15)) & 15;
            if (pad) bump(pad);
            blk = bump(total);
        }
    }
    pthread_mutex_unlock(&heap_lock);
    wr32(blk, total);
    wr32(blk + 4, HDR_MAGIC);
    return blk + 8;
}

uint32_t guest_calloc(uint32_t n, uint32_t m)
{
    uint64_t t = (uint64_t)n * m;
    uint32_t p = guest_malloc((uint32_t)t);
    memset(M + p, 0, t);
    return p;
}

// blocks are 16-aligned, so the pointers malloc returns are 8 mod 16: an
// aligned pointer sits further into a bigger block, with the block's own
// pointer and a marker just before it
uint32_t guest_memalign(uint32_t align, uint32_t n)
{
    if (align <= 8) return guest_malloc(n);
    uint32_t p = guest_malloc(n + align + 8);
    uint32_t q = (p + 8 + align - 1) & ~(align - 1);
    wr32(q - 8, p);
    wr32(q - 4, ALIGNED_MAGIC);
    return q;
}

// what a pointer from guest_malloc or guest_memalign can hold
uint32_t guest_usable(uint32_t p)
{
    if (rd32(p - 4) == ALIGNED_MAGIC) {
        uint32_t b = rd32(p - 8);
        return rd32(b - 8) - 8 - (p - b);
    }
    return rd32(p - 8) - 8;
}

void guest_free(uint32_t p)
{
    if (!p) return;
    if (rd32(p - 4) == ALIGNED_MAGIC) {
        wr32(p - 4, 0);
        p = rd32(p - 8);
    }
    uint32_t blk = p - 8;
    if (rd32(blk + 4) != HDR_MAGIC) {
        hle_log("free of bad pointer 0x%08x\n", p);
        return;
    }
    uint32_t total = rd32(blk);
    wr32(blk + 4, 0);
    pthread_mutex_lock(&heap_lock);
    if (total <= NCLASS * 16) {
        int k = total / 16 - 1;
        wr32(blk + 8, small_free[k]);
        small_free[k] = blk;
    } else {
        uint32_t prev = 0, cur = large_free;
        while (cur && cur < blk) {
            prev = cur;
            cur = rd32(cur + 8);
        }
        wr32(blk + 8, cur);
        if (cur && blk + total == cur) {
            wr32(blk, total + rd32(cur));
            wr32(blk + 8, rd32(cur + 8));
        }
        if (prev) {
            wr32(prev + 8, blk);
            if (prev + rd32(prev) == blk) {
                wr32(prev, rd32(prev) + rd32(blk));
                wr32(prev + 8, rd32(blk + 8));
            }
        } else large_free = blk;
    }
    pthread_mutex_unlock(&heap_lock);
}

uint32_t guest_strdup(const char *s)
{
    uint32_t n = strlen(s) + 1;
    uint32_t p = guest_malloc(n);
    memcpy(M + p, s, n);
    return p;
}

static uint32_t hostdata_top = HOSTDATA_BASE;
static uint32_t stacks_top = STACKS_END;
uint32_t hostdata_alloc(uint32_t n)
{
    pthread_mutex_lock(&heap_lock);
    uint32_t p = hostdata_top;
    hostdata_top += (n + 15) & ~15u;
    pthread_mutex_unlock(&heap_lock);
    memset(M + p, 0, n);
    return p;
}

uint32_t stack_alloc(uint32_t size)
{
    pthread_mutex_lock(&heap_lock);
    uint32_t top = stacks_top;
    stacks_top -= (size + 0xffff) & ~0xffffu;
    stacks_top -= 0x10000;   // gap
    pthread_mutex_unlock(&heap_lock);
    return top - 64;
}

// ---- imports ------------------------------------------------------------------
#define MAX_HLE 2048
static const hle_entry *tables[64];
static int ntables;
static hle_fn import_fns[MAX_IMPORTS];
static int import_resolved;

void hle_register(const hle_entry *t)
{
    // registered once: init runs again for each program a core starts
    for (int i = 0; i < ntables; i++)
        if (tables[i] == t) return;
    tables[ntables++] = t;
}

hle_fn hle_find(const char *name)
{
    for (int i = 0; i < ntables; i++)
        for (const hle_entry *e = tables[i]; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
    return NULL;
}

static int import_first(const char *name)
{
    for (unsigned i = 0; i < prog->import_count; i++)
        if (!strcmp(prog->import_names[i], name)) return (int)i;
    return -1;
}

// the modules' exported symbols, hashed by name
#define EXPORT_HASH 65536
static int32_t export_slot[EXPORT_HASH];   // index + 1, 0 empty

static unsigned name_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static void build_exports(void)
{
    memset(export_slot, 0, sizeof export_slot);
    for (unsigned i = 0; i < prog->export_count && i < EXPORT_HASH / 2; i++) {
        unsigned h = name_hash(prog->export_names[i]) % EXPORT_HASH;
        while (export_slot[h]) {
            // the first module that defines a name wins, as with ld.so
            if (!strcmp(prog->export_names[export_slot[h] - 1], prog->export_names[i])) goto next;
            h = (h + 1) % EXPORT_HASH;
        }
        export_slot[h] = (int32_t)i + 1;
    next:;
    }
}

// a symbol another module defines, or 0
uint32_t guest_export(const char *name)
{
    if (!prog->export_count) return 0;
    for (unsigned h = name_hash(name) % EXPORT_HASH; export_slot[h]; h = (h + 1) % EXPORT_HASH)
        if (!strcmp(prog->export_names[export_slot[h] - 1], name)) return prog->export_addr[export_slot[h] - 1];
    return 0;
}

// a symbol's address for a library's data: guest code or data first, then a
// host function (through a magic address) or a host object
static uint32_t symbol_value(const char *name, int *found)
{
    uint32_t a = guest_export(name);
    *found = 1;
    if (a) return a;
    hle_fn fn = hle_find(name);
    if (fn) return magic_register(fn, name);
    if ((a = hle_object(name))) return a;
    *found = 0;
    return 0;
}

uint32_t guest_symbol(const char *name)
{
    int found;
    return symbol_value(name, &found);
}

static void apply_relocs(void)
{
    int unresolved = 0;
    for (unsigned i = 0; i < prog->reloc_count; i++) {
        uint32_t p = prog->reloc_addr[i], base = 0;
        for (unsigned m = 0; m < prog->mod_count; m++)
            if (prog->mod_base[m] <= p && prog->mod_base[m] > base) base = prog->mod_base[m];
        int found = 1;
        uint32_t s = prog->reloc_sym[i][0] ? symbol_value(prog->reloc_sym[i], &found) : 0;
        if (!found) {
            if (hle_verbose) hle_log("relocation: no symbol %s\n", prog->reloc_sym[i]);
            unresolved++;
        }
        switch (prog->reloc_type[i]) {
        case 1: wr32(p, rd32(p) + s); break;          // R_386_32
        case 2: wr32(p, rd32(p) + s - p); break;      // R_386_PC32
        case 6: wr32(p, s); break;                    // R_386_GLOB_DAT
        case 8: wr32(p, rd32(p) + base); break;       // R_386_RELATIVE
        default: hle_log("relocation type %d at 0x%08x not handled\n", prog->reloc_type[i], p);
        }
    }
    if (unresolved) hle_log("%d library relocations name symbols nobody defines (weak ones are 0)\n", unresolved);
    // R_386_COPY of a library's object: the program's copy starts as the library's
    for (unsigned i = 1; i <= prog->copy_count; i++)
        for (unsigned k = 0; k < prog->export_count; k++)
            if (prog->export_addr[k] != prog->copy_addr[i] && !strcmp(prog->export_names[k], prog->copy_names[i])) {
                memcpy(M + prog->copy_addr[i], M + prog->export_addr[k], prog->copy_size[i]);
                break;
            }
}

static uint32_t import_guest[MAX_IMPORTS];

static void resolve_imports(void)
{
    int missing = 0;
    build_exports();
    for (unsigned i = 0; i < prog->import_count; i++) {
        import_fns[i] = NULL;
        // another module's function first: the program's own C++ runtime
        // serves the libraries, and the libraries serve the program
        if ((import_guest[i] = guest_export(prog->import_names[i])) && guest_lookup(import_guest[i])) continue;
        import_guest[i] = 0;
        import_fns[i] = hle_find(prog->import_names[i]);
        if (!import_fns[i] && i == (unsigned)import_first(prog->import_names[i])) {
            hle_log("no host function for import %s\n", prog->import_names[i]);
            missing++;
        }
    }
    if (missing) hle_log("%d imports unresolved\n", missing);
    apply_relocs();
    import_resolved = 1;
}

void (*gl_check_hook)(const char *name);

void hle_call(CPU *c, int i)
{
    if (import_guest[i]) {
        // bound to another module: its code returns to the caller itself
        if (guest_stopping) guest_exit_thread();
        guest_lookup(import_guest[i])(c);
        return;
    }
    hle_fn f = import_fns[i];
    if (!f) {
        char why[160];
        snprintf(why, sizeof why, "call to unimplemented import %s (from 0x%08x)", prog->import_names[i], rd32(c->r[ESP]));
        host_fatal(why);
    }
    if (hle_trace && strncmp(prog->import_names[i], "gl", 2))
        hle_log("call %s %x %x %x %x\n", prog->import_names[i], rd32(c->r[ESP] + 4), rd32(c->r[ESP] + 8),
                rd32(c->r[ESP] + 12), rd32(c->r[ESP] + 16));
    if (guest_stopping) guest_exit_thread();
    if ((gl_prof || gl_clock) && prog->import_names[i][0] == 'g' && prog->import_names[i][1] == 'l') {
        double t0 = prof_now();
        f(c);
        double t = prof_now() - t0;
        gl_spent += t;
        if (gl_prof) {
            glp[i].t += t;
            glp[i].n++;
            glp[i].name = prog->import_names[i];
        }
    } else
        f(c);
    if (gl_check_hook && prog->import_names[i][0] == 'g' && prog->import_names[i][1] == 'l')
        gl_check_hook(prog->import_names[i]);
    if (guest_stopping) guest_exit_thread();
    c->r[ESP] += 4;
}

// The host's own devices stay out of the game's sight: on a PC with gamepads
// HotD4 opened /dev/input/js* and got stuck. The cabinet's devices
// the game uses are emulated (device_for_path); anything else under /dev is
// missing, as it is on a machine without it.
int host_device_hidden(const char *p)
{
    static const char *const shown[] = {"/dev/null", "/dev/zero", "/dev/random", "/dev/urandom"};
    if (strncmp(p, "/dev/", 5)) return 0;
    for (size_t i = 0; i < sizeof shown / sizeof *shown; i++)
        if (!strcmp(p, shown[i])) return 0;
    return 1;
}

// A directory's names, kept for the lookup below while the directory is unchanged
#define NAME_DIRS 64
#ifdef __APPLE__
#define MTIME(st) ((st).st_mtimespec)
#else
#define MTIME(st) ((st).st_mtim)
#endif
static struct { char path[1100]; struct timespec mtime; char **names; int n; } name_dirs[NAME_DIRS];
static int name_dirs_next;
static pthread_mutex_t name_lock = PTHREAD_MUTEX_INITIALIZER;

// `name` in the directory `dir` in any case, written to `out`; 0 if there is none
static int find_any_case(const char *dir, const char *name, char *out, size_t n)
{
    struct stat st;
    if (stat(dir, &st) != 0) return 0;
    pthread_mutex_lock(&name_lock);
    int k;
    for (k = 0; k < NAME_DIRS; k++)
        if (!strcmp(name_dirs[k].path, dir)) break;
    if (k == NAME_DIRS || name_dirs[k].mtime.tv_sec != MTIME(st).tv_sec || name_dirs[k].mtime.tv_nsec != MTIME(st).tv_nsec) {
        if (k == NAME_DIRS) k = name_dirs_next++ % NAME_DIRS;
        for (int i = 0; i < name_dirs[k].n; i++) free(name_dirs[k].names[i]);
        free(name_dirs[k].names);
        name_dirs[k].names = NULL;
        name_dirs[k].n = 0;
        snprintf(name_dirs[k].path, sizeof name_dirs[k].path, "%s", dir);
        name_dirs[k].mtime = MTIME(st);
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));) {
            if (!(name_dirs[k].n & 63)) name_dirs[k].names = realloc(name_dirs[k].names, (size_t)(name_dirs[k].n + 64) * sizeof(char *));
            name_dirs[k].names[name_dirs[k].n++] = strdup(e->d_name);
        }
        if (d) closedir(d);
    }
    int found = 0;
    for (int i = 0; i < name_dirs[k].n && !found; i++)
        if (!strcasecmp(name_dirs[k].names[i], name)) found = snprintf(out, n, "%s", name_dirs[k].names[i]) > 0;
    pthread_mutex_unlock(&name_lock);
    return found;
}

// The cabinet's disk tells upper from lower case and a game's names are as it
// asks for them, but a game folder unpacked on a disk that does not (the Mac's)
// keeps one of two names that differ only in case: OutRun 2 SP's data has
// Beach_wave.adx and beach_wave.adx, packed on a Mac as beach_wave.adx, and
// the game asks for Beach_wave.adx. A path under the game folder that is not
// there is looked up again in any case, a folder at a time.
static void fix_case(char *buf, size_t n)
{
    size_t root = strlen(game_root);
    struct stat st;
    if (!root || strncmp(buf, game_root, root) || buf[root] != '/' || stat(buf, &st) == 0 || errno != ENOENT) return;
    char out[2048], name[256];
    snprintf(out, sizeof out, "%s", game_root);
    for (const char *p = buf + root; *p;) {
        while (*p == '/') p++;
        size_t len = strcspn(p, "/");
        if (!len) break;
        if (len >= sizeof name) return;
        memcpy(name, p, len);
        name[len] = 0;
        p += len;
        size_t at = strlen(out);
        snprintf(out + at, sizeof out - at, "/%s", name);
        if (stat(out, &st) == 0) continue;
        out[at] = 0;
        char real[256];
        if (!find_any_case(out, name, real, sizeof real)) return;   // not there in any case: as asked
        snprintf(out + at, sizeof out - at, "/%s", real);
    }
    if (strlen(out) < n) memcpy(buf, out, strlen(out) + 1);
}

const char *map_path(const char *p, char *buf, size_t n)
{
    // the cabinet's disk is mounted at /home; game paths are relative to elf/
    if (!strncmp(p, "/home/", 6)) snprintf(buf, n, "%s/%s", game_root, p + 6);
    else if (!strncmp(p, "/tmp/", 5)) snprintf(buf, n, "%s/tmp/%s", save_root[0] ? save_root : game_root, p + 5);
    else if (!strncmp(p, "/var/tmp/", 9)) snprintf(buf, n, "%s/tmp/%s", save_root[0] ? save_root : game_root, p + 9);
    else if (p[0] != '/' && elf_dir[0]) snprintf(buf, n, "%s/%s", elf_dir, p);
    else snprintf(buf, n, "%s", p);
    fix_case(buf, n);
    return buf;
}

// ---- stopping ---------------------------------------------------------------------
volatile int guest_stopping;

void guest_exit_thread(void)
{
    // runs the thread's cleanup handlers; the host thread that called
    // run_game sees it as run_game never returning, and joins it
    host_thread_exit();
    pthread_exit(NULL);
}

void guest_sleep_us(uint64_t us)
{
    while (us && !guest_stopping) {
        uint64_t n = us < 10000 ? us : 10000;
        usleep((useconds_t)n);
        us -= n;
    }
    if (guest_stopping) guest_exit_thread();
}

void guest_stop(void)
{
    guest_stopping = 1;
    hle_thread_stop();
}

void guest_reset(void)
{
    hle_thread_reset();
    hle_gfx_reset();
    hle_audio_reset();
    hle_libc_reset();
    hle_lindbergh_reset();
    memset(magic_fns, 0, sizeof magic_fns);
    memset(magic_names, 0, sizeof magic_names);
    magic_count = 0;
    memset(import_fns, 0, sizeof import_fns);
    memset(import_guest, 0, sizeof import_guest);
    import_resolved = 0;
    heap_top = HEAP_BASE;
    memset(small_free, 0, sizeof small_free);
    large_free = 0;
    hostdata_top = HOSTDATA_BASE;
    stacks_top = STACKS_END;
    if (M) munmap(M, 1ull << 32);
    M = NULL;
    prog = NULL;
    guest_stopping = 0;
}

void run_game(const gen_program *p, const char *elf, int argc, char **argv)
{
    prog = p;
    mem_init();
    uint32_t entry;
    if (load_elf(elf, &entry)) host_fatal("cannot load the game's program");
    hle_libc_init();
    hle_gfx_init();
    hle_lindbergh_init();
    resolve_imports();
    CPU *c = cpu_new();
    cpu_set_current(c);
    hle_thread_init(c);
    // initial stack: argc, argv, NULL, envp NULL, auxv NULL
    uint32_t sp = stack_alloc(8 << 20);
    uint32_t av[16];
    for (int i = 0; i < argc && i < 15; i++) av[i] = guest_strdup(argv[i]);
    sp -= 64;
    sp &= ~15u;
    uint32_t q = sp;
    wr32(q, argc);
    q += 4;
    for (int i = 0; i < argc; i++, q += 4) wr32(q, av[i]);
    wr32(q, 0); q += 4;
    wr32(q, 0); q += 4;
    wr32(q, 0); q += 4;
    wr32(q, 0);
    c->r[ESP] = sp;
    // the libraries' initialisers (static constructors) run before the program, as ld.so does
    for (unsigned i = 0; i < prog->mod_count; i++)
        if (prog->mod_init[i]) guest_call(c, prog->mod_init[i], 0, NULL);
    hle_log("starting at 0x%08x\n", entry);
    guest_fn f = guest_lookup(entry);
    // _start does not return: it calls __libc_start_main, which calls exit
    f(c);
    hle_log("entry returned\n");
}
