// The game as an Arcade Foundry core (common/include/afcore.h): the front end calls
// run_frame, the game runs on threads of its own and hands over each frame
// it finishes in host_present, where it waits for the next run_frame.
//
// The cabinet's two programs, the game and its test menu, are both in the
// library. When one exits (TEST pressed in the game, EXIT in the menu) the
// core ends all its threads, puts the runtime back to power-on state and
// starts the other, as the cabinet's start script does.
#include "af_debug.h"
#include "afcore.h"
#ifdef CORE_FILELIST
#include "afc_filelist.h" // filelists/<set>.h: every file of a known-good copy
#endif
#include "hle.h"
#include "host.h"
#include "input.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STR2(x) #x
#define STR(x) STR2(x)
#define API_FN2(r) afcore_api_##r
#define API_FN(r) API_FN2(r)

// the programs this core was built from, as paths under the game folder
// (CMake): the game, and the cabinet's test menu when it is a separate program
extern const gen_program gen_program_game;
#ifdef CORE_TEST_ELF
extern const gen_program gen_program_test;
static const gen_program *const programs[] = {&gen_program_game, &gen_program_test};
static const char *const program_files[] = {CORE_GAME_ELF, CORE_TEST_ELF};
static const long long program_sizes[] = {CORE_GAME_ELF_SIZE, CORE_TEST_ELF_SIZE};
#else
static const gen_program *const programs[] = {&gen_program_game};
static const char *const program_files[] = {CORE_GAME_ELF};
static const long long program_sizes[] = {CORE_GAME_ELF_SIZE};
#endif
#define NPROGRAMS ((int)(sizeof programs / sizeof programs[0]))
void host_gl_readback(uint32_t *dst);
void host_gl_close(void);
#ifdef LB_IOS
int host_gl_surfaces(void);   // host_core_ios.m
void *host_gl_surface_frame(void);
void host_gl_surface_read(void *surface, uint32_t *dst, int w, int h);
void host_gl_frame_pool(void);   // drains the drawing thread's autorelease pool
#endif

AFCORE_EXPORT const afcore_api *API_FN(CORE_ROMSET)(void);

enum { STOP_NONE, STOP_EXIT, STOP_FATAL, STOP_DESTROY };

#define AUDIO_RATE 48000
#define AUDIO_CAP (AUDIO_RATE * 2)     // frames: two seconds

struct afcore {
    char root[1024], elf[2][1100];
    pthread_t super;
    int which;                          // 0 the game, 1 the test menu
    int in_test;                        // the game was started in its test menu (-t)
    char error[512];
};

static afcore *the_core;   // the runtime is one per process
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int stop_reason, destroying;
static int go;                          // run_frame's permit for the game to pass its next present
static unsigned frame_seq;
// surface: the frame as the host's GPU buffer (screen_surface, iOS), its
// pixels copied out into px only when screen() asks (px_from_surface)
typedef struct { uint32_t *px; size_t cap; int w, h; void *surface; int px_from_surface; } framebuf;
static framebuf frames[2];              // [0] screen() returns it, [1] the game's latest
static int surfaces_on;                 // the front end takes surfaces: no readback
static uint32_t *readback;
static size_t readback_cap;
static int fresh;                       // frames[1] is newer than [0]
static afcore_input pending;
static uint32_t last_buttons;
static uint16_t lever;     // driving: the gear lever's switch (Race TV)
static double lever_until; // monotonic seconds the lever stays out after a press
static float audio[AUDIO_CAP * 2];
static size_t audio_head, audio_len;

// ---- what the runtime asks of its host ----------------------------------------------------
// under lock: the game must stop (it exited, or failed, or the front end is leaving)
static void request_stop(int why)
{
    if (!stop_reason) stop_reason = why;
    guest_stopping = 1;
    pthread_cond_broadcast(&cv);
}

// Where a frame's time goes, averaged over 300 frames and logged: the game
// (its code and GL calls up to the present, waits included) with the CPU time
// its thread used of that, the readback of the finished frame, the copy for
// the front end, the wait for its next run_frame, and on iOS the GPU's time
// per frame. Always on iOS (for the device's console), with LB_PERF=1 elsewhere.
static double perf_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static struct { double last_end, last_cpu, game, cpu, readback, copy, wait, gpu; int frames, gpu_n, on; } perf;

#ifdef LB_IOS
void host_gl_perf(char *buf, size_t size);   // host_core_ios.m
#endif

static double thread_cpu(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void perf_frame(double start, double cpu_start, double read, double copied, double waited)
{
    if (!perf.on) {
#ifdef LB_IOS
        perf.on = 1;
#else
        perf.on = af_debug_env("LB_PERF") ? 1 : -1;
#endif
        gl_prof = perf.on > 0;
    }
    if (perf.on < 0) return;
    if (perf.last_end > 0) {
        perf.game += start - perf.last_end;
        perf.cpu += cpu_start - perf.last_cpu;
        perf.readback += read - start;
        perf.copy += copied - read;
        perf.wait += waited - copied;
        if (++perf.frames == 300) {
            double n = perf.frames * 1e-3, total = perf.game + perf.readback + perf.copy + perf.wait;
            char gpu[192] = "";
#ifdef LB_IOS
            host_gl_perf(gpu, sizeof gpu);
#endif
            hle_log("perf: %.1f frames/s; per frame: game %.1f ms (its thread's CPU %.1f ms), readback %.1f ms, "
                    "copy %.1f ms, waiting for the front end %.1f ms%s\n", perf.frames / total, perf.game / n,
                    perf.cpu / n, perf.readback / n, perf.copy / n, perf.wait / n, gpu);
            char logged[192], gl[768];
            hle_gl_prof(gl, sizeof gl, perf.frames);
            hle_log("%s\n", gl);
            hle_log_stats(logged, sizeof logged);
            hle_log("%s\n", logged);
            perf.game = perf.cpu = perf.readback = perf.copy = perf.wait = 0;
            perf.frames = 0;
        }
    }
    perf.last_end = waited;
    perf.last_cpu = thread_cpu();
}

// the game's boot screen (LB_BOOT_TEXT, hle_libc.c): its frames don't wait
// for the front end, which is shown the latest when it asks
static int booting(void)
{
#ifdef LB_BOOT_TEXT
    extern int lb_booting;
    return lb_booting > 0;
#else
    return 0;
#endif
}

void host_present(void)
{
    double t_start = perf_now(), cpu_start = thread_cpu();
    int w, h;
    host_gl_size(&w, &h);
    size_t n = (size_t)w * h;
#ifdef LB_IOS
    host_gl_frame_pool();
    pthread_mutex_lock(&lock);
    int surfaces = surfaces_on;
    pthread_mutex_unlock(&lock);
    if (surfaces && !af_debug_env("LB_NO_SURFACES") && host_gl_surfaces()) {
        // the GPU copies the frame for the front end: nothing comes back to the CPU
        void *s = host_gl_surface_frame();
        double t_read = perf_now();
        pthread_mutex_lock(&lock);
        if (s) {
            frames[1].surface = s, frames[1].px_from_surface = 0;
            frames[1].w = w, frames[1].h = h;
            fresh = 1;
        }
        frame_seq++;
        double t_copy = perf_now();
        pthread_cond_broadcast(&cv);
        while (!go && !guest_stopping && !booting()) pthread_cond_wait(&cv, &lock);
        go = 0;
        pthread_mutex_unlock(&lock);
        perf_frame(t_start, cpu_start, t_read, t_copy, perf_now());
        if (guest_stopping) guest_exit_thread();
        return;
    }
#endif
    if (booting()) {
        // the front end has the last frame until it asks for another
        pthread_mutex_lock(&lock);
        int asked = go;
        pthread_mutex_unlock(&lock);
        if (!asked) {
            perf_frame(t_start, cpu_start, t_start, t_start, perf_now());
            return;
        }
    }
    if (readback_cap < n) {
        free(readback);
        readback = calloc(n, 4);   // black until the first frame is read back
        readback_cap = n;
    }
    host_gl_readback(readback);
    double t_read = perf_now();
    pthread_mutex_lock(&lock);
    // frames[0] is the front end's until its next run_frame: only [1] changes here
    framebuf *f = &frames[1];
    if (f->cap < n) {
        free(f->px);
        f->px = malloc(n * 4);
        f->cap = n;
    }
    memcpy(f->px, readback, n * 4);
    f->w = w, f->h = h;
    f->surface = NULL;
    fresh = 1;
    frame_seq++;
    double t_copy = perf_now();
    pthread_cond_broadcast(&cv);
    // lockstep with the front end: wait for the next run_frame (not while booting)
    while (!go && !guest_stopping && !booting()) pthread_cond_wait(&cv, &lock);
    go = 0;
    pthread_mutex_unlock(&lock);
    perf_frame(t_start, cpu_start, t_read, t_copy, perf_now());
    if (guest_stopping) guest_exit_thread();
}

static uint16_t gun_axis(float v)
{
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return (uint16_t)(v * 65535.0f + 0.5f);
}

// a gun axis between the raw values at its two edges (LB_GUN_RANGE: the
// game's own calibration, which ignores what lies outside it)
#ifdef LB_GUN_RANGE
static const uint16_t gun_edges[4] = {LB_GUN_RANGE};
#else
static const uint16_t gun_edges[4] = {0, 0xffff, 0, 0xffff};
#endif
static uint16_t gun_raw(float v, int y)
{
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    float a = gun_edges[2 * y], b = gun_edges[2 * y + 1];
    return (uint16_t)(a + v * (b - a) + 0.5f);
}

static int on_screen(float x, float y) { return x >= 0 && x <= 1 && y >= 0 && y <= 1; }

void host_input(void)
{
    pthread_mutex_lock(&lock);
    afcore_input in = pending;
    pthread_mutex_unlock(&lock);
    uint32_t b = in.buttons, rise = b & ~last_buttons;
    last_buttons = b;
    uint16_t p[2] = {0, 0};
#ifdef CORE_DRIVING
    // Sega Race TV's cabinet: steering, gas and brake on analogs 0-2, the gear
    // lever (up, neutral, down) and boost on player 2's switches
    jvs_in.analog[0] = gun_axis((in.steer + 1) / 2);
    jvs_in.analog[1] = gun_axis(in.accel);
    jvs_in.analog[2] = gun_axis(in.brake);
    // the gear lever is sequential and springs back to neutral: each push up or
    // down shifts one gear. It is out while the shift button is held, and for at
    // least LEVER_MIN_S after a press, so a quick paddle tap is not missed (by
    // time, not calls: host_input runs from XPending too, several times a frame).
    // Holding it out (the old latch) gave the game one edge each way, so it only
    // shifted between 1st and 2nd, and name entry read it as player 2 up held.
    // gear is ignored: front ends send it with the shift buttons.
    const double LEVER_MIN_S = 0.05;
    double now = perf_now();
    if (rise & AFCORE_SHIFT_UP) lever = SW_UP, lever_until = now + LEVER_MIN_S;
    else if (rise & AFCORE_SHIFT_DOWN) lever = SW_DOWN, lever_until = now + LEVER_MIN_S;
    else if (now >= lever_until && !(b & (lever == SW_UP ? AFCORE_SHIFT_UP : AFCORE_SHIFT_DOWN))) lever = 0;
    p[1] |= lever;
    if (b & (AFCORE_BUTTON2 | AFCORE_VIEW2)) p[1] |= SW_1;   // boost
    if (b & AFCORE_VIEW1) p[0] |= SW_DOWN;
#else
    const uint32_t trigger[2] = {AFCORE_TRIGGER1, AFCORE_TRIGGER2};
    for (int i = 0; i < 2; i++) {
#ifdef LB_GUN_SWAP_XY
        jvs_in.analog[2 * i] = gun_raw(in.gun_y[i], 1);
        jvs_in.analog[2 * i + 1] = gun_raw(in.gun_x[i], 0);
#else
        jvs_in.analog[2 * i] = gun_raw(in.gun_x[i], 0);
        jvs_in.analog[2 * i + 1] = gun_raw(in.gun_y[i], 1);
#endif
        // as in the other gun cores, a shot off screen reloads (the cabinet's
        // reload is a switch in the gun)
        if (!on_screen(in.gun_x[i], in.gun_y[i]) && (b & trigger[i])) p[i] |= SW_2;
    }
    if (b & AFCORE_TRIGGER1) p[0] |= SW_1;
    if (b & AFCORE_TRIGGER2) p[1] |= SW_1;
    if (b & AFCORE_BUTTON2) p[0] |= SW_2;
    // the gun's button (HotD4: a grenade), player 2's in buttons2
    if (b & AFCORE_BUTTON3) p[0] |= SW_3;
    if (in.buttons2 & AFCORE_BUTTON3) p[1] |= SW_3;
#endif
    if (b & AFCORE_START) p[0] |= SW_START;
    if (b & AFCORE_START2) p[1] |= SW_START;
    if (b & AFCORE_SERVICE) p[0] |= SW_SERVICE;
    if (b & AFCORE_UP) p[0] |= SW_UP;
    if (b & AFCORE_DOWN) p[0] |= SW_DOWN;
    if (b & AFCORE_LEFT) p[0] |= SW_LEFT;
    if (b & AFCORE_RIGHT) p[0] |= SW_RIGHT;
    jvs_in.player[0] = p[0];
    jvs_in.player[1] = p[1];
    if (rise & AFCORE_COIN) jvs_in.coins[0]++;
    if (b & AFCORE_TEST) jvs_in.system |= SYS_TEST;
    else jvs_in.system &= ~SYS_TEST;
}

int host_audio_open(int rate)
{
    (void)rate;   // the mixer runs at AUDIO_RATE
    return 0;     // mixed once per video frame and pushed here
}

void host_audio_push(const int16_t *s, int frames)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < frames; i++) {
        if (audio_len == AUDIO_CAP) {   // the front end is not reading: drop the oldest
            audio_head = (audio_head + 1) % AUDIO_CAP;
            audio_len--;
        }
        size_t at = (audio_head + audio_len) % AUDIO_CAP;
        audio[2 * at] = s[2 * i] / 32768.0f;
        audio[2 * at + 1] = s[2 * i + 1] / 32768.0f;
        audio_len++;
    }
    pthread_mutex_unlock(&lock);
}

void host_exit(int status)
{
    hle_log("program exited (%d)\n", status);
    pthread_mutex_lock(&lock);
    request_stop(STOP_EXIT);
    pthread_mutex_unlock(&lock);
    guest_exit_thread();
}

void host_fatal(const char *why)
{
    hle_log("fatal: %s\n", why);
    pthread_mutex_lock(&lock);
    if (the_core && !the_core->error[0]) snprintf(the_core->error, sizeof the_core->error, "%s", why);
    request_stop(STOP_FATAL);
    pthread_mutex_unlock(&lock);
    guest_exit_thread();
}

// ---- the programs' threads ------------------------------------------------------------------
static void *guest_main(void *p)
{
    afcore *c = p;
    const char *elf = c->elf[c->which];
    // the game leaves /tmp/segaboot/test whenever it exits: after the game the
    // boot loader starts its test menu, after the menu the game
    char *argv[] = {(char *)elf, "-t"};
    c->in_test = segaboot_test && !c->in_test;
    segaboot_test = 0;
    run_game(programs[c->which], elf, c->in_test ? 2 : 1, argv);
    // the entry point returned: as an exit
    host_thread_exit();
    pthread_mutex_lock(&lock);
    request_stop(STOP_EXIT);
    pthread_mutex_unlock(&lock);
    return NULL;
}

// starts a program, waits for it to stop, cleans up, and starts the next
static void *supervise(void *p)
{
    afcore *c = p;
    for (;;) {
        snprintf(game_root, sizeof game_root, "%s", c->root);
        // relative paths start in the program's directory, as on the cabinet
        snprintf(elf_dir, sizeof elf_dir, "%s", c->elf[c->which]);
        if (strrchr(elf_dir, '/')) *strrchr(elf_dir, '/') = 0;
        hle_log("starting %s\n", c->elf[c->which]);
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 16 << 20);
        pthread_t t;
        pthread_create(&t, &at, guest_main, c);
        pthread_attr_destroy(&at);

        pthread_mutex_lock(&lock);
        while (!stop_reason && !destroying) pthread_cond_wait(&cv, &lock);
        int why = stop_reason;
        pthread_mutex_unlock(&lock);

        guest_stop();      // every thread the game started has ended
        pthread_join(t, NULL);
        host_gl_close();
        guest_reset();
        pthread_mutex_lock(&lock);
        stop_reason = STOP_NONE;
        go = 0;
        int leaving = destroying;
        pthread_mutex_unlock(&lock);
        if (leaving || why != STOP_EXIT) break;
        // the game and its test menu hand over to each other; a game with the
        // menu inside starts again
        // (or, asked for with /tmp/segaboot/test, in its menu)
        c->which = (c->which + 1) % NPROGRAMS;
        // the TEST switch may still be held from entering the menu; the next
        // program sees it as the front end reports it
    }
    return NULL;
}

// ---- the interface ---------------------------------------------------------------------------
static int is_file(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISREG(st.st_mode);
}

static off_t file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) ? -1 : st.st_size;
}

static afcore *create(const char *rom_path, const char *save_dir, char *err, size_t err_size)
{
    if (the_core) {
        snprintf(err, err_size, "only one Lindbergh game can run at a time");
        return NULL;
    }
    afcore *c = calloc(1, sizeof *c);
    segaboot_test = 0;   // a new core starts with the game
    lever = 0;
    lever_until = 0;
    // rom_path: the game's folder, the one holding disk0
    snprintf(c->root, sizeof c->root, "%s", rom_path);
    size_t len = strlen(c->root);
    while (len > 1 && c->root[len - 1] == '/') c->root[--len] = 0;
    for (int i = 0; i < NPROGRAMS; i++) {
        snprintf(c->elf[i], sizeof c->elf[i], "%s/%s", c->root, program_files[i]);
        if (!is_file(c->elf[i])) {
            snprintf(err, err_size, "%s is not a %s game folder: no %s", rom_path, API_FN(CORE_ROMSET)()->title, program_files[i]);
            free(c);
            return NULL;
        }
        if (file_size(c->elf[i]) != program_sizes[i]) {
            snprintf(err, err_size, "%s is not the version this core was built from", program_files[i]);
            free(c);
            return NULL;
        }
        // the shared libraries the game ships next to its program
        for (unsigned k = 0; k < programs[i]->mod_count; k++) {
            char lib[1200];
            snprintf(lib, sizeof lib, "%s", c->elf[i]);
            snprintf(strrchr(lib, '/') + 1, sizeof lib - (size_t)(strrchr(lib, '/') + 1 - lib), "%s", programs[i]->mod_file[k]);
            if (!is_file(lib)) {
                snprintf(err, err_size, "%s is not a complete %s game folder: no %s next to %s", rom_path,
                         API_FN(CORE_ROMSET)()->title, programs[i]->mod_file[k], program_files[i]);
                free(c);
                return NULL;
            }
        }
    }
    if (save_dir) {
        snprintf(save_root, sizeof save_root, "%s", save_dir);
        mkdir(save_root, 0755);
    } else {
        // nothing kept: the saves go to a new scratch folder of their own
        snprintf(save_root, sizeof save_root, "%s/lindbergh-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        if (!mkdtemp(save_root)) {
            snprintf(err, err_size, "no scratch folder for the saves (%s): %s", save_root, strerror(errno));
            save_root[0] = 0;
            free(c);
            return NULL;
        }
    }
    hle_verbose = af_debug_env("LB_VERBOSE") != NULL;
    memset(&pending, 0, sizeof pending);
    pending.gun_x[0] = pending.gun_x[1] = pending.gun_y[0] = pending.gun_y[1] = 0.5f;
    last_buttons = 0;
    audio_head = audio_len = 0;
    frame_seq = 0;
    fresh = 0;
    stop_reason = STOP_NONE;
    go = 0;
    c->which = 0;
    the_core = c;
    pthread_create(&c->super, NULL, supervise, c);
    return c;
}

static void destroy(afcore *c)
{
    if (!c) return;
    pthread_mutex_lock(&lock);
    destroying = 1;
    request_stop(STOP_DESTROY);
    pthread_mutex_unlock(&lock);
    pthread_join(c->super, NULL);
    pthread_mutex_lock(&lock);
    the_core = NULL;
    stop_reason = STOP_NONE;
    destroying = 0;
    guest_stopping = 0;
    memset(&perf, 0, sizeof perf);   // the next game's perf: lines start afresh (its thread's CPU from zero)
    for (int i = 0; i < 2; i++) frames[i].w = frames[i].h = 0, frames[i].surface = NULL;
    fresh = 0;
    surfaces_on = 0;
    pthread_mutex_unlock(&lock);
    save_root[0] = elf_dir[0] = 0;
    free(c);
}

static void run_frame(afcore *c, const afcore_input *in)
{
    pthread_mutex_lock(&lock);
    if (c->error[0]) {
        pthread_mutex_unlock(&lock);
        return;
    }
    memset(&pending, 0, sizeof pending);
    if (in) memcpy(&pending, in, in->size < sizeof pending ? in->size : sizeof pending);
    pending.size = sizeof pending;
    unsigned seq = frame_seq;
    go = 1;
    pthread_cond_broadcast(&cv);
    // the next frame, or give up for now while the game is loading or
    // changing programs; the front end keeps showing the last one
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    while (frame_seq == seq && !c->error[0])
        if (pthread_cond_timedwait(&cv, &lock, &ts) == ETIMEDOUT) break;
    if (fresh) {
        framebuf t = frames[0];
        frames[0] = frames[1];
        frames[1] = t;
        fresh = 0;
    }
    pthread_mutex_unlock(&lock);
}

static const uint32_t *screen(afcore *c, int *w, int *h)
{
    static const uint32_t black[4] = {0xff000000u, 0xff000000u, 0xff000000u, 0xff000000u};
    (void)c;
    pthread_mutex_lock(&lock);
#ifdef LB_IOS
    framebuf *f = &frames[0];
    if (f->surface && !f->px_from_surface) {   // the front end takes surfaces, and now pixels too
        size_t n = (size_t)f->w * f->h;
        if (f->cap < n) {
            free(f->px);
            f->px = malloc(n * 4);
            f->cap = n;
        }
        host_gl_surface_read(f->surface, f->px, f->w, f->h);
        f->px_from_surface = 1;
    }
#endif
    const uint32_t *p = frames[0].px;
    if (p && frames[0].w) {
        *w = frames[0].w, *h = frames[0].h;
    } else {
        p = black;
        *w = *h = 2;
    }
    pthread_mutex_unlock(&lock);
    return p;
}

// iOS: the screen as an IOSurface-backed CVPixelBufferRef (afcore.h 5.1);
// from the first call the frames go to the front end that way, not read back
static void *screen_surface(afcore *c, int *w, int *h)
{
    (void)c;
#ifdef LB_IOS
    pthread_mutex_lock(&lock);
    surfaces_on = 1;
    void *s = frames[0].w ? frames[0].surface : NULL;
    if (s) *w = frames[0].w, *h = frames[0].h;
    pthread_mutex_unlock(&lock);
    return s;
#else
    (void)w, (void)h;
    return NULL;
#endif
}

static double audio_rate(afcore *c, int stream)
{
    (void)c;
    return stream == 0 ? AUDIO_RATE : 0;
}

static size_t audio_read(afcore *c, int stream, float *out, size_t max)
{
    (void)c;
    if (stream != 0) return 0;
    pthread_mutex_lock(&lock);
    size_t n = audio_len < max ? audio_len : max;
    for (size_t i = 0; i < n; i++) {
        size_t at = (audio_head + i) % AUDIO_CAP;
        out[2 * i] = audio[2 * at];
        out[2 * i + 1] = audio[2 * at + 1];
    }
    audio_head = (audio_head + n) % AUDIO_CAP;
    audio_len -= n;
    pthread_mutex_unlock(&lock);
    return n;
}

static void save(afcore *c)
{
    (void)c;
    hle_lindbergh_flush();
}

static int link_start(afcore *c, int port, const char *next, int framesync, char *err, size_t err_size)
{
    (void)c, (void)port, (void)next, (void)framesync;
    snprintf(err, err_size, "no link play for this game");
    return -1;
}

static int link_state(afcore *c, int *id, int *count)
{
    (void)c;
    if (id) *id = 0;
    if (count) *count = 0;
    return AFCORE_LINK_OFF;
}

static const char *error(afcore *c)
{
    pthread_mutex_lock(&lock);
    const char *e = c->error[0] ? c->error : NULL;
    pthread_mutex_unlock(&lock);
    return e;
}

#ifdef CORE_FILELIST
// afcore 5.2: the game folder against the list made from a known-good copy
// (tools/filelist in common/): every listed file, its size and CRC.
static int check_roms(const char *rom_path, char *report, size_t report_size)
{
    char root[1024];
    snprintf(root, sizeof root, "%s", rom_path ? rom_path : "");
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') root[--len] = 0;
    return afc_check_files(root, afc_filelist, AFC_FILELIST_COUNT, report, report_size);
}
#define CHECK_ROMS check_roms
#else
#define CHECK_ROMS NULL // no list for this game yet
#endif

// the settings the front end chooses (CORE_OPTIONS, from games/<game>/
// game.cmake's GAME_OPTIONS): the region, for the games whose region the
// runtime answers (hle_lindbergh.c)
#ifdef CORE_OPTIONS
extern int lb_region;
static int set_option(const char *name, const char *value)
{
    static const char *const regions[] = {"export", "usa", "japan"};
    if (strcmp(name, "region")) return -1;
    for (int i = 0; i < 3; i++)
        if (!strcmp(value, regions[i])) {
            lb_region = i;
            return 0;
        }
    return -1;
}
#define OPTIONS CORE_OPTIONS
#else
#define OPTIONS NULL
#define set_option NULL
#endif

const afcore_api *API_FN(CORE_ROMSET)(void)
{
    static const afcore_api api = {
        AFCORE_API_VERSION, sizeof(afcore_api), AFCORE_MAKE_VERSION(CORE_VERSION_MAJOR, CORE_VERSION_MINOR, CORE_VERSION_PATCH), STR(CORE_ROMSET), CORE_TITLE,
        60.0, CORE_WIDTH, CORE_HEIGHT, create, destroy, run_frame, screen, audio_rate, audio_read, save,
        link_start, link_state, error, NULL, NULL, NULL, 0.0f, screen_surface, CHECK_ROMS, OPTIONS, set_option,
    };
    return &api;
}
