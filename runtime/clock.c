// The core's one time source (#95). Everything the game sees of time reads it:
// rdtsc (core.c), time, gettimeofday and ftime (hle_libc.c), the sleeps and
// select's timeout (guest_sleep_us), pthread_cond_timedwait (hle_thread.c),
// GLUT_ELAPSED_TIME and the boot's frame timing (hle_gfx.c), the sound
// board's samples (hle_audio.c) and the gear lever's hold (core_api.c).
//
// Normal play: the host's clocks, as before.
//
// Render mode (the front end's "clock=frames" option, for video renders that
// run far slower than real time): time runs at the real rate within a frame
// but stops at its end, 1/60 s after the last, until the front end asks for
// the next one (run_frame). A sleep or timed wait past that point holds its
// thread until enough frames have been asked for. The game, its frame limiter
// and the sound board then see 1/60 s per frame however long the front end
// takes, and each frame gives exactly 800 samples at 48 kHz.
//
// run_frame has a frame once the game presents one and the frame's time has
// run. The thread that presents may instead wait for time past the frame's end
// (HotD4's 30 frames a second cut-scenes present every other frame; Race TV's
// boot frames wait for its limiter): the frame is then over without a picture,
// the last one stays. Between frames, while the front end works on the last
// one, the game's threads are held at their next library call (clk_hold), so
// a load takes as many frames as it takes 60ths of a second in normal play,
// however fast or slow the render. Host-side timing (perf: lines, run_frame's
// own waits) stays on the host clock.
#include "hle.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>

int clk_render;   // render mode, set at create for the whole run

static uint64_t host_mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t host_real(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// Render time is origin + vt, vt = min(limit, base_vt + host time since
// base_host). clk_frame moves limit a frame on and sets base again, so vt
// carries on from where it stopped. Written under m, read without it through
// the sequence count (odd while being written).
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint64_t origin, real_origin;
static volatile uint64_t base_vt, base_host, limit;
static volatile uint32_t seq;
static uint64_t frames;   // frames the front end has asked for
static volatile int frame_open;   // between run_frame's start and its end: the game may run

// the thread that presents the game's frames, and whether it waits for time
// past the frame's end: spinning on the clock (reads that find it stopped) or
// in a sleep or timed wait
static pthread_t presenter;
static volatile int presenter_known, presenter_blocked;
static volatile uint32_t presenter_spins;
#define SPINS 64   // reads of a stopped clock that make a wait

static uint64_t frame_end(uint64_t n) { return (n * 1000000000ull + 59) / 60; }

void clk_init(int render)
{
    pthread_mutex_lock(&m);
    clk_render = render;
    origin = host_mono();
    real_origin = host_real();
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    base_vt = base_host = limit = 0;
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    frames = 0;
    frame_open = 0;
    presenter_known = presenter_blocked = 0;
    presenter_spins = 0;
    pthread_mutex_unlock(&m);
}

// render time since the origin, and whether it stands at the frame's end
static uint64_t vt_now(int *at_limit)
{
    for (;;) {
        uint32_t s = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
        if (s & 1) continue;
        uint64_t bv = base_vt, bh = base_host, lim = limit;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&seq, __ATOMIC_RELAXED) != s) continue;
        uint64_t h = host_mono();
        uint64_t v = bv + (h > bh ? h - bh : 0);
        if (at_limit) *at_limit = v >= lim;
        return v < lim ? v : lim;
    }
}

static int is_presenter(void) { return presenter_known && pthread_equal(presenter, pthread_self()); }

// a read of a stopped clock: the presenter spinning on it waits for the next
// frame; any thread doing so gives the CPU up a little (nothing changes before
// run_frame)
static void stopped_read(void)
{
    uint32_t n = 0;
    if (is_presenter()) {
        n = __atomic_add_fetch(&presenter_spins, 1, __ATOMIC_RELAXED);
        if (n > SPINS) presenter_blocked = 1;
    }
    static _Thread_local uint32_t mine;
    if (++mine > SPINS) usleep(100);
}

static uint64_t render_now(void)
{
    int at;
    uint64_t v = vt_now(&at);
    if (at) stopped_read();
    return v;
}

uint64_t clk_mono_ns(void) { return clk_render ? origin + render_now() : host_mono(); }
uint64_t clk_real_ns(void) { return clk_render ? real_origin + render_now() : host_real(); }

// ---- waits -------------------------------------------------------------------------------------
// render mode: until core time reaches `at` (clk_mono_ns time) or the game stops
void clk_sleep_until(uint64_t at)
{
    while (!guest_stopping) {
        int stopped;
        uint64_t v = vt_now(&stopped);
        if (origin + v >= at) return;
        if (stopped) {
            // held until run_frame moves time on
            pthread_mutex_lock(&m);
            if (is_presenter()) presenter_blocked = 1;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000;
            if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
            if (vt_now(NULL) >= limit) pthread_cond_timedwait(&cv, &m, &ts);
            pthread_mutex_unlock(&m);
            continue;
        }
        uint64_t left = at - origin - v, to_end = limit - v;
        if (left > to_end) left = to_end;
        if (left > 10000000) left = 10000000;
        usleep((useconds_t)(left / 1000 + 1));
    }
}

// render mode, a timed wait until `until` (clk_real_ns time): 0 once it has
// passed, else the host time to wait this time round (a slice: while time
// stands at a frame's end, until run_frame moves it on)
int clk_wait_slice(uint64_t until, struct timespec *host_deadline)
{
    int stopped;
    uint64_t now = real_origin + vt_now(&stopped);
    if (now >= until) return 0;
    uint64_t left = stopped ? 2000000 : until - now;
    if (stopped && is_presenter()) presenter_blocked = 1;
    if (left > 2000000) left = 2000000;
    clock_gettime(CLOCK_REALTIME, host_deadline);
    host_deadline->tv_nsec += (long)left;
    while (host_deadline->tv_nsec >= 1000000000) host_deadline->tv_sec++, host_deadline->tv_nsec -= 1000000000;
    return 1;
}

// ---- the front end -------------------------------------------------------------------------------
// run_frame: one more frame of time. It starts where the last one ended, also
// when the front end asked for it sooner (the app's --skip frames run faster
// than real time): game time is n/60 s at frame n's start however fast or
// slow the front end
void clk_frame(void)
{
    pthread_mutex_lock(&m);
    uint64_t v = vt_now(NULL), h = host_mono();
    if (v < limit) v = limit;
    frames++;
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    base_vt = v;
    base_host = h;
    limit = frame_end(frames);
    __atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
    presenter_blocked = 0;
    presenter_spins = 0;
    frame_open = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&m);
}

// run_frame: the frame is over; the game's threads wait for the next
void clk_frame_done(void) { frame_open = 0; }

// every library call the game makes (hle_call), in render mode: held while
// no frame is open
void clk_hold(void)
{
    if (frame_open || guest_stopping) return;
    pthread_mutex_lock(&m);
    while (!frame_open && !guest_stopping) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&cv, &m, &ts);
    }
    pthread_mutex_unlock(&m);
}

// host_present: this thread presents the game's frames
void clk_presenting(void)
{
    if (!clk_render) return;
    presenter = pthread_self();
    presenter_known = 1;
}

// run_frame: the frame's time has run
int clk_at_limit(void)
{
    int at;
    vt_now(&at);
    return at;
}

// run_frame: no picture will come for this frame: the presenter waits for time
// past its end (or none is known yet, the game loading, and its time has run)
int clk_frame_over(void)
{
    if (!clk_at_limit()) return 0;
    return !presenter_known || presenter_blocked;
}
