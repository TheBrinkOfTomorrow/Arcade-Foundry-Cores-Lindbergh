// Host-side stand-ins for the libraries and hardware a Lindbergh game uses.
// A handler is entered the way the guest function would be: the return
// address is at esp, arguments above it. hle_call() pops the return address.
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARG(i) rd32(c->r[ESP] + 4 + 4 * (i))
#define ARGF(i) u2f(ARG(i))
#define ARGD(i) u2d(((uint64_t)ARG((i) + 1) << 32) | ARG(i))
#define ARGQ(i) (((uint64_t)ARG((i) + 1) << 32) | ARG(i))
#define G(a) ((char *)(M + (uint32_t)(a)))
#define RET(v) do { c->r[EAX] = (uint32_t)(v); return; } while (0)
#define RETQ(v) do { uint64_t _v = (uint64_t)(v); c->r[EAX] = (uint32_t)_v; c->r[EDX] = (uint32_t)(_v >> 32); return; } while (0)
#define RETF(v) do { fpush(c, (double)(v)); return; } while (0)

typedef void (*hle_fn)(CPU *c);
typedef struct { const char *name; hle_fn fn; } hle_entry;

void hle_call(CPU *c, int import_index);
hle_fn hle_find(const char *name);
// guest address of a data object the host owns (stdout, ...) for a library's relocation, or 0
uint32_t hle_object(const char *name);
// what a module's symbol or a host function resolves to (for dlsym), or 0
uint32_t guest_symbol(const char *name);
uint32_t guest_export(const char *name);   // only what a module defines, or 0
// LB_GL_CHECK=1: called after every GL import to report GL errors by call
extern void (*gl_check_hook)(const char *name);
void hle_register(const hle_entry *table);
void hle_log(const char *fmt, ...);
void hle_log_stats(char *buf, size_t size);
extern int gl_prof;   // time the GL calls (hle_gl_prof)
extern int gl_clock;  // time the GL calls for hle_cpu_now
double hle_cpu_now(void);   // seconds, less this thread's time in GL calls while gl_clock is on
void hle_gl_prof(char *buf, size_t size, int frames);   // printed since the last call, for the perf: line
extern int hle_verbose;
extern int hle_trace;   // log every import call

// guest memory layout
#define HEAP_BASE 0x10000000u
#define HEAP_END 0xa0000000u
#define STACKS_BASE 0xa0000000u
#define STACKS_END 0xe0000000u
#define HOSTDATA_BASE 0xe0000000u   // guest-visible data the host layer owns
#define HOSTDATA_END 0xf0000000u
#define MAGIC_BASE 0xf0000000u      // addresses that dispatch to host functions
#define RETURN_MAGIC 0xfffffff0u

uint32_t guest_malloc(uint32_t n);
uint32_t guest_calloc(uint32_t n, uint32_t m);
void guest_free(uint32_t p);
uint32_t guest_memalign(uint32_t align, uint32_t n);
uint32_t guest_usable(uint32_t p);   // bytes a heap pointer can hold
uint32_t guest_strdup(const char *s);
uint32_t hostdata_alloc(uint32_t n);   // never freed
uint32_t magic_register(hle_fn fn, const char *name);
uint32_t stack_alloc(uint32_t size);   // returns the top

// game files
extern char game_root[1024];          // the game's disk0 directory's parent
extern char elf_dir[1100];            // relative paths start here; empty: the working directory
extern int segaboot_test;               // the game asked to be started in its test menu (-t)
extern char save_root[1024];          // the game's saves and /tmp; empty: LB_SAVE_DIR or game_root/save
const char *map_path(const char *guest_path, char *buf, size_t n);
int host_device_hidden(const char *guest_path);   // a host device the game must not open

// formatted output with guest varargs
int guest_vformat(char *out, size_t n, const char *fmt, uint32_t args);

// libraries
void hle_libc_init(void);
void hle_thread_init(CPU *main);
void hle_gfx_init(void);
void hle_lindbergh_init(void);
void audio_frame(void);     // once per video frame
CPU *cpu_new(void);
CPU *cpu_current(void);
void cpu_set_current(CPU *c);

// devices behind file descriptors
typedef struct device_ops {
    const char *path;
    int (*open)(void);
    int (*ioctl)(uint32_t req, uint32_t argp);
    int (*read)(uint32_t buf, uint32_t n);
    int (*write)(uint32_t buf, uint32_t n);
    int (*select)(void);
} device_ops;
const device_ops *device_for_path(const char *path);
const device_ops *device_for_fd(int fd);
// read-only files the cabinet's kernel provides (/proc/...); 1 if path is one
int virtual_file(const char *path, const void **data, size_t *size);

// a translated program's tables (gen_tables.c, written by common/tools/x86recomp)
typedef struct gen_program {
    const char *name;
    const uint32_t *fn_addr;
    const guest_fn *fn_ptr;
    unsigned fn_count;
    const char *const *import_names;
    const uint32_t *import_got, *import_plt;
    unsigned import_count;
    const uint32_t *copy_addr;          // [0] unused
    const char *const *copy_names;
    unsigned copy_count;
    const char *const *obj_names;
    const uint32_t *obj_addr;
    unsigned obj_count;
    const uint32_t *copy_size;          // [0] unused
    // shared libraries translated with the program (games/<game>/libs.txt),
    // loaded from the ELF's directory at fixed bases
    const char *const *mod_file;
    const uint32_t *mod_base, *mod_init;
    unsigned mod_count;
    // symbols the modules define, program first, for binding imports and
    // the libraries' relocations the way the dynamic linker would
    const char *const *export_names;
    const uint32_t *export_addr;
    unsigned export_count;
    const uint32_t *reloc_addr;          // the libraries' R_386_32/PC32/GLOB_DAT/RELATIVE, the program's GLOB_DAT
    const uint8_t *reloc_type;
    const char *const *reloc_sym;
    unsigned reloc_count;
} gen_program;
extern const gen_program *prog;        // the program running

int load_elf(const char *path, uint32_t *entry);
// Runs the program's entry point on the calling thread; returns only when the
// game is stopped (guest_stop) or the entry point returns.
void run_game(const gen_program *p, const char *elf, int argc, char **argv);

// Stopping a running game from the host: guest_stop sets guest_stopping,
// after which every guest thread leaves (guest_exit_thread) at its next
// import call or wait, and waits until the game's own threads have ended.
// guest_reset then returns every module to its state before run_game.
extern volatile int guest_stopping;
void guest_exit_thread(void) __attribute__((noreturn));
void guest_sleep_us(uint64_t us);      // a sleep a stop cuts short
void guest_stop(void);
void guest_reset(void);
void hle_thread_stop(void);
void hle_thread_reset(void);
void hle_gfx_reset(void);
void hle_audio_reset(void);
void hle_libc_reset(void);
void hle_lindbergh_reset(void);
void hle_lindbergh_flush(void);

#ifdef __cplusplus
}
#endif
