// pthreads and POSIX semaphores. Guest objects are keyed by their guest
// address; each one gets a host record the first time it is used. Every
// guest wait is a wait on a host condition variable under one lock, so a
// core can wake and end all of a game's threads when it stops (guest_stop).
#include "hle.h"
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>

typedef struct obj {
    uint32_t addr;
    int kind;                // 1 mutex, 2 cond, 3 sem
    pthread_cond_t cv;
    uint32_t owner, depth;   // mutex: owning thread id, recursion depth
    uint32_t seq;            // cond: signals so far
    uint32_t count;          // sem
    struct obj *next;
} obj;

#define NB 4096
static obj *buckets[NB];
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;

// under sync_lock
static obj *lookup(uint32_t addr, int kind)
{
    unsigned b = (addr >> 2) % NB;
    for (obj *o = buckets[b]; o; o = o->next)
        if (o->addr == addr && o->kind == kind) return o;
    obj *o = calloc(1, sizeof *o);
    o->addr = addr;
    o->kind = kind;
    pthread_cond_init(&o->cv, NULL);
    o->next = buckets[b];
    buckets[b] = o;
    return o;
}

// ---- threads -------------------------------------------------------------------------
typedef struct {
    CPU *cpu;
    uint32_t fn, arg, ret;
    int used, done;
} gthread;

#define MAX_THREADS 128
static gthread threads[MAX_THREADS];
static pthread_cond_t threads_cv = PTHREAD_COND_INITIALIZER;   // a thread ended
static int live;                                               // threads started and not ended
static __thread uint32_t self_id;

// leaves the calling guest thread once the game is stopping
static void check_stop(void)
{
    if (guest_stopping) {
        pthread_mutex_unlock(&sync_lock);
        guest_exit_thread();
    }
}

static void wait_on(pthread_cond_t *cv)
{
    check_stop();   // guest_stop broadcasts once stopping is set: never wait after that
    pthread_cond_wait(cv, &sync_lock);
    check_stop();
}

static void thread_done(gthread *g)
{
    pthread_mutex_lock(&sync_lock);
    g->done = 1;
    live--;
    pthread_cond_broadcast(&threads_cv);
    pthread_mutex_unlock(&sync_lock);
}

static void thread_cleanup(void *p) { thread_done(p); }

static void *thread_main(void *p)
{
    gthread *g = p;
    cpu_set_current(g->cpu);
    self_id = (uint32_t)(g - threads) + 1;
    pthread_cleanup_push(thread_cleanup, g);
    g->ret = guest_call(g->cpu, g->fn, 1, &g->arg);
    pthread_cleanup_pop(1);
    return NULL;
}

static void h_pthread_create(CPU *c)
{
    uint32_t out = ARG(0), attr = ARG(1), fn = ARG(2), arg = ARG(3);
    pthread_mutex_lock(&sync_lock);
    int i;
    for (i = 1; i < MAX_THREADS && threads[i].used; i++);
    if (i == MAX_THREADS) {
        pthread_mutex_unlock(&sync_lock);
        RET(11);   // EAGAIN
    }
    gthread *g = &threads[i];
    memset(g, 0, sizeof *g);
    g->used = 1;
    live++;
    pthread_mutex_unlock(&sync_lock);
    uint32_t stack = 2 << 20;
    if (attr && rd32(attr + 32) > stack) stack = rd32(attr + 32);
    g->cpu = cpu_new();
    g->cpu->r[ESP] = stack_alloc(stack);
    g->fn = fn;
    g->arg = arg;
    if (out) wr32(out, (uint32_t)i + 1);
    hle_log("pthread_create %d fn 0x%08x arg 0x%08x\n", i + 1, fn, arg);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &at, thread_main, g);
    pthread_attr_destroy(&at);
    RET(0);
}

static void h_pthread_join(CPU *c)
{
    uint32_t id = ARG(0);
    if (id < 2 || id > MAX_THREADS) RET(3);
    gthread *g = &threads[id - 1];
    pthread_mutex_lock(&sync_lock);
    while (!g->done) wait_on(&threads_cv);
    pthread_mutex_unlock(&sync_lock);
    if (ARG(1)) wr32(ARG(1), g->ret);
    RET(0);
}

static void h_pthread_detach(CPU *c) { RET(0); }
static void h_pthread_cancel(CPU *c) { hle_log("pthread_cancel %d ignored\n", ARG(0)); RET(0); }
static void h_pthread_self(CPU *c) { RET(self_id ? self_id : 1); }
static void h_zero(CPU *c) { RET(0); }

static void h_pthread_getschedparam(CPU *c)
{
    if (ARG(1)) wr32(ARG(1), 0);
    if (ARG(2)) wr32(ARG(2), 0);
    RET(0);
}

static void h_pthread_attr_init(CPU *c)
{
    memset(G(ARG(0)), 0, 36);
    RET(0);
}

static void h_pthread_attr_setstacksize(CPU *c)
{
    wr32(ARG(0) + 32, ARG(1));
    RET(0);
}

// ---- mutexes (recursive, as the game creates them) and condition variables -------------
static uint32_t me(void) { return self_id ? self_id : 1; }

static void lock_mutex(obj *m, uint32_t depth)
{
    while (m->owner && m->owner != me()) wait_on(&m->cv);
    m->owner = me();
    m->depth += depth;
}

static void h_mutex_init(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    lookup(ARG(0), 1);
    pthread_mutex_unlock(&sync_lock);
    RET(0);
}

static void h_mutex_lock(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    lock_mutex(lookup(ARG(0), 1), 1);
    pthread_mutex_unlock(&sync_lock);
    RET(0);
}

static void h_mutex_unlock(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    obj *m = lookup(ARG(0), 1);
    int r = 1;   // EPERM
    if (m->owner == me()) {
        r = 0;
        if (--m->depth == 0) {
            m->owner = 0;
            pthread_cond_broadcast(&m->cv);
        }
    }
    pthread_mutex_unlock(&sync_lock);
    RET(r);
}

static void h_cond_init(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    lookup(ARG(0), 2);
    pthread_mutex_unlock(&sync_lock);
    RET(0);
}

static void signal_cond(uint32_t addr, int all)
{
    pthread_mutex_lock(&sync_lock);
    obj *cv = lookup(addr, 2);
    cv->seq++;
    if (all) pthread_cond_broadcast(&cv->cv);
    else pthread_cond_signal(&cv->cv);
    pthread_mutex_unlock(&sync_lock);
}

static void h_cond_signal(CPU *c) { signal_cond(ARG(0), 0); RET(0); }
static void h_cond_broadcast(CPU *c) { signal_cond(ARG(0), 1); RET(0); }

static void h_cond_wait(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    obj *cv = lookup(ARG(0), 2), *m = lookup(ARG(1), 1);
    uint32_t depth = m->depth, seq = cv->seq;
    m->owner = 0;
    m->depth = 0;
    pthread_cond_broadcast(&m->cv);
    while (cv->seq == seq) wait_on(&cv->cv);
    lock_mutex(m, depth);
    pthread_mutex_unlock(&sync_lock);
    RET(0);
}

static void h_mutex_trylock(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    obj *m = lookup(ARG(0), 1);
    int r = 16;   // EBUSY
    if (!m->owner || m->owner == me()) {
        m->owner = me();
        m->depth++;
        r = 0;
    }
    pthread_mutex_unlock(&sync_lock);
    RET(r);
}

// abstime is CLOCK_REALTIME, as glibc's
static void h_cond_timedwait(CPU *c)
{
    struct timespec until = {(time_t)(int32_t)rd32(ARG(2)), (long)rd32(ARG(2) + 4)};
    pthread_mutex_lock(&sync_lock);
    obj *cv = lookup(ARG(0), 2), *m = lookup(ARG(1), 1);
    uint32_t depth = m->depth, seq = cv->seq;
    int r = 0;
    m->owner = 0;
    m->depth = 0;
    pthread_cond_broadcast(&m->cv);
    while (cv->seq == seq && !r) {
        check_stop();
        if (pthread_cond_timedwait(&cv->cv, &sync_lock, &until)) r = 110;   // ETIMEDOUT
        check_stop();
    }
    lock_mutex(m, depth);
    pthread_mutex_unlock(&sync_lock);
    RET(r);
}

// once_control: 0 not run, 1 running, 2 done
static void h_pthread_once(CPU *c)
{
    uint32_t ctl = ARG(0);
    pthread_mutex_lock(&sync_lock);
    if (rd32(ctl) == 0) {
        wr32(ctl, 1);
        pthread_mutex_unlock(&sync_lock);
        guest_call(c, ARG(1), 0, NULL);
        pthread_mutex_lock(&sync_lock);
        wr32(ctl, 2);
    }
    pthread_mutex_unlock(&sync_lock);
    while (rd32(ctl) != 2) {
        if (guest_stopping) guest_exit_thread();
        sched_yield();
    }
    RET(0);
}

// thread-specific data (destructors are not run)
#define MAX_KEYS 128
static uint32_t nkeys;
static __thread uint32_t key_vals[MAX_KEYS];

static void h_key_create(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    uint32_t k = nkeys < MAX_KEYS ? nkeys++ : MAX_KEYS;
    pthread_mutex_unlock(&sync_lock);
    if (k == MAX_KEYS) RET(11);   // EAGAIN
    wr32(ARG(0), k);
    RET(0);
}
static void h_getspecific(CPU *c) { RET(ARG(0) < MAX_KEYS ? key_vals[ARG(0)] : 0); }
static void h_setspecific(CPU *c)
{
    if (ARG(0) >= MAX_KEYS) RET(22);   // EINVAL
    key_vals[ARG(0)] = ARG(1);
    RET(0);
}
static void h_pthread_equal(CPU *c) { RET(ARG(0) == ARG(1)); }
static void h_prio_max(CPU *c) { RET(ARG(0) ? 99 : 0); }   // SCHED_FIFO/RR: 99, SCHED_OTHER: 0
static void h_prio_min(CPU *c) { RET(ARG(0) ? 1 : 0); }

// ---- semaphores ------------------------------------------------------------------------
static void h_sem_init(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    lookup(ARG(0), 3)->count = ARG(2);
    pthread_mutex_unlock(&sync_lock);
    RET(0);
}

static void h_sem_wait(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    obj *s = lookup(ARG(0), 3);
    while (!s->count) wait_on(&s->cv);
    s->count--;
    pthread_mutex_unlock(&sync_lock);
    RET(0);
}

static void h_sem_post(CPU *c)
{
    pthread_mutex_lock(&sync_lock);
    obj *s = lookup(ARG(0), 3);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&sync_lock);
    RET(0);
}

// named semaphores: one guest sem_t per name
static struct { char name[64]; uint32_t addr; } named[32];
static int nnamed;

static void h_sem_open(CPU *c)
{
    const char *nm = G(ARG(0));
    for (int i = 0; i < nnamed; i++)
        if (!strcmp(named[i].name, nm)) RET(named[i].addr);
    uint32_t a = hostdata_alloc(16);
    pthread_mutex_lock(&sync_lock);
    lookup(a, 3)->count = (ARG(1) & 0100) ? ARG(3) : 0;
    pthread_mutex_unlock(&sync_lock);
    if (nnamed < 32) {
        snprintf(named[nnamed].name, sizeof named[nnamed].name, "%s", nm);
        named[nnamed++].addr = a;
    }
    RET(a);
}

static void h_sched_yield(CPU *c) { sched_yield(); RET(0); }

static const hle_entry thread_table[] = {
    {"pthread_create", h_pthread_create}, {"pthread_join", h_pthread_join},
    {"pthread_detach", h_pthread_detach}, {"pthread_cancel", h_pthread_cancel},
    {"pthread_self", h_pthread_self}, {"pthread_getschedparam", h_pthread_getschedparam},
    {"pthread_setschedparam", h_zero}, {"pthread_attr_init", h_pthread_attr_init},
    {"pthread_attr_setstacksize", h_pthread_attr_setstacksize},
    {"pthread_attr_setschedparam", h_zero}, {"pthread_attr_setschedpolicy", h_zero},
    {"pthread_mutexattr_init", h_zero}, {"pthread_mutex_init", h_mutex_init},
    {"pthread_mutex_destroy", h_zero}, {"pthread_mutex_lock", h_mutex_lock},
    {"pthread_mutex_unlock", h_mutex_unlock}, {"pthread_cond_init", h_cond_init},
    {"pthread_cond_destroy", h_zero}, {"pthread_cond_signal", h_cond_signal},
    {"pthread_cond_broadcast", h_cond_broadcast}, {"pthread_cond_wait", h_cond_wait},
    {"sem_init", h_sem_init}, {"sem_destroy", h_zero}, {"sem_wait", h_sem_wait},
    {"sem_post", h_sem_post}, {"sem_open", h_sem_open}, {"sem_close", h_zero},
    {"sched_yield", h_sched_yield}, {"pthread_mutex_trylock", h_mutex_trylock},
    {"pthread_cond_timedwait", h_cond_timedwait}, {"pthread_once", h_pthread_once},
    {"pthread_key_create", h_key_create}, {"pthread_getspecific", h_getspecific},
    {"pthread_setspecific", h_setspecific}, {"pthread_equal", h_pthread_equal},
    {"pthread_mutexattr_settype", h_zero}, {"pthread_mutexattr_destroy", h_zero},
    {"sched_get_priority_max", h_prio_max}, {"sched_get_priority_min", h_prio_min},
    {NULL, NULL},
};

// the calling host thread runs the game's main thread
void hle_thread_init(CPU *main)
{
    (void)main;
    threads[0].used = 1;
    self_id = 1;
}

// guest_stopping is set: wake every waiting guest thread so it leaves, and
// wait until all the threads the game started have ended
void hle_thread_stop(void)
{
    pthread_mutex_lock(&sync_lock);
    for (int b = 0; b < NB; b++)
        for (obj *o = buckets[b]; o; o = o->next) pthread_cond_broadcast(&o->cv);
    pthread_cond_broadcast(&threads_cv);
    while (live > 0) {
        // a thread may be about to wait: keep waking until all are gone
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&threads_cv, &sync_lock, &ts);
        for (int b = 0; b < NB; b++)
            for (obj *o = buckets[b]; o; o = o->next) pthread_cond_broadcast(&o->cv);
    }
    pthread_mutex_unlock(&sync_lock);
}

void hle_thread_reset(void)
{
    for (int b = 0; b < NB; b++)
        while (buckets[b]) {
            obj *o = buckets[b];
            buckets[b] = o->next;
            pthread_cond_destroy(&o->cv);
            free(o);
        }
    for (int i = 0; i < MAX_THREADS; i++) free(threads[i].cpu);
    memset(threads, 0, sizeof threads);
    live = 0;
    nnamed = 0;
    nkeys = 0;
    memset(key_vals, 0, sizeof key_vals);   // the host thread that runs the next game
}

__attribute__((constructor)) static void reg(void) { hle_register(thread_table); }
