// The core library's OpenGL on iOS (iPadOS): an OpenGL ES context (EAGL) with no
// window, the game's window a framebuffer object the size the game asked for.
// The games' desktop OpenGL 2.x (immediate mode, fixed function, ARB programs,
// GLSL 1.20) goes through gl4es, which turns it into OpenGL ES; the runtime calls
// gl4es's functions by name (gl_host.h) and gl4es calls OpenGL ES.
#include "host.h"
#include "hle.h"
#include "gl_host.h"
#include <dlfcn.h>
#include <gl4esinit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#import <OpenGLES/EAGL.h>
#import <CoreVideo/CoreVideo.h>

static EAGLContext *ctx;
static int win_w, win_h;
static GLuint fbo, color_tex, depth_rb;
static __thread int current;      // the context is current on this thread
static int gl4es_ready;
// gl4es's own record of the context (what it has bound and enabled, its
// programs): a fresh one with each context, or a game started again in the
// same process skipped calls gl4es took for done and drew nothing
void *NewGLState(void *shared, int es2only);
void ActivateGLState(void *state);
void DeleteGLState(void *state);
static void *gl4es_state;

// gl4es asks for OpenGL ES's entry points by name: the system framework's
static void *gles_proc(const char *name) { return dlsym(RTLD_DEFAULT, name); }
static void main_fb_size(int *w, int *h) { *w = win_w, *h = win_h; }

void *host_gl_proc(const char *name) { return gl4es_GetProcAddress(name); }

static void make_current(int on)
{
    [EAGLContext setCurrentContext:on ? ctx : nil];
    current = on;
}

void host_gl_open(int w, int h)
{
    if (fbo) return;
    win_w = w, win_h = h;
    // OpenGL ES 3 where there is one: more varyings than ES 2 (HotD4's skinned
    // shaders need them); gl4es takes ES 3's core packed depth and stencil as
    // there (cmake/gl4es_fixes.cmake)
    ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    if (!ctx) ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!ctx) host_fatal("cannot create an OpenGL ES context");
    make_current(1);
    if (!gl4es_ready) {
        set_getprocaddress(gles_proc);
        set_getmainfbsize(main_fb_size);
        initialize_gl4es();
        gl4es_ready = 1;
    }
    gl4es_state = NewGLState(NULL, 0);
    ActivateGLState(gl4es_state);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    // colour in a texture: gl4es turns a colour renderbuffer into one anyway
    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, color_tex, 0);
    glGenRenderbuffersEXT(1, &depth_rb);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, depth_rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth_rb);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth_rb);
    GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status != GL_FRAMEBUFFER_COMPLETE_EXT) {
        hle_log("GL: framebuffer status 0x%x\n", status);
        host_fatal("the game's framebuffer is incomplete");
    }
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    hle_log("GL: OpenGL ES off-screen %dx%d (%s)\n", w, h, (const char *)glGetString(GL_RENDERER));
}

void host_gl_release(void) { if (ctx) make_current(0); }
void host_gl_current(void) { if (ctx) make_current(1); }
unsigned host_gl_framebuffer(void) { return fbo; }
void host_gl_size(int *w, int *h) { *w = win_w, *h = win_h; }

// a guest thread is ending: it must not take the context with it
void host_thread_exit(void)
{
    if (current) make_current(0);
}

// The finished frame, top row first, as 0xAARRGGBB with alpha opaque.
// On OpenGL ES 3 the read is asynchronous: each frame is read into one of
// three pixel pack buffers with a fence, and the newest one the GPU has
// finished is copied out without waiting (one or two frames of latency); while
// none has, the front end keeps the last frame, so the game's CPU work and the
// GPU's overlap. This
// goes to OpenGL ES directly (gl4es's framebuffer names are OpenGL ES's) and
// reads BGRA where the driver offers it, which needs no swizzle. On OpenGL ES 2
// the read waits for the frame.
typedef void (*es_bind_fn)(GLenum, GLuint);
typedef void (*es_geti_fn)(GLenum, GLint *);
typedef void (*es_read_fn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (*es_genbuf_fn)(GLsizei, GLuint *);
typedef void (*es_bufdata_fn)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void *(*es_map_fn)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield);
typedef GLboolean (*es_unmap_fn)(GLenum);
typedef void *(*es_fence_fn)(GLenum, GLbitfield);
typedef GLenum (*es_wait_fn)(void *, GLbitfield, uint64_t);
typedef void (*es_delsync_fn)(void *);
typedef void (*es_delbuf_fn)(GLsizei, const GLuint *);
typedef void (*es_pixelstore_fn)(GLenum, GLint);
typedef void (*es_flush_fn)(void);
#define RING 3
static struct {
    int ready, async, bgra;
    GLuint pbo[RING];
    void *fence[RING];
    unsigned seq[RING], next, shown;   // frame numbers: in each buffer, last copied out
    double queued[RING];               // when each buffer's read was queued
    double gpu_total;                  // read queued -> found done, for the perf: line
    int gpu_n;
    // where the readback's time goes, summed over the frames since the last perf: line
    double t_flush, t_read, t_fence, t_poll, t_map, t_cpu;
    int t_n;
    es_bind_fn bind_fb, bind_buf;
    es_geti_fn geti;
    es_read_fn read;
    es_genbuf_fn gen_buf;
    es_bufdata_fn buf_data;
    es_map_fn map;
    es_unmap_fn unmap;
    es_fence_fn fence_sync;
    es_wait_fn wait_sync;
    es_delsync_fn del_sync;
    es_delbuf_fn del_buf;
    es_pixelstore_fn pixelstore;
    es_flush_fn flush;
} rb;

enum {
    ES_READ_FRAMEBUFFER = 0x8CA8, ES_READ_FRAMEBUFFER_BINDING = 0x8CAA, ES_PIXEL_PACK_BUFFER = 0x88EB,
    ES_PIXEL_PACK_BUFFER_BINDING = 0x88ED, ES_STREAM_READ = 0x88E1, ES_MAP_READ_BIT = 1,
    ES_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, ES_SYNC_FLUSH_COMMANDS_BIT = 1, ES_BGRA = 0x80E1,
    ES_ALREADY_SIGNALED = 0x911A, ES_CONDITION_SATISFIED = 0x911C,
    ES_IMPLEMENTATION_COLOR_READ_FORMAT = 0x8B9B, ES_IMPLEMENTATION_COLOR_READ_TYPE = 0x8B9A,
};

static void readback_setup(void)
{
    rb.ready = 1;
    rb.bind_fb = (es_bind_fn)dlsym(RTLD_DEFAULT, "glBindFramebuffer");
    rb.bind_buf = (es_bind_fn)dlsym(RTLD_DEFAULT, "glBindBuffer");
    rb.geti = (es_geti_fn)dlsym(RTLD_DEFAULT, "glGetIntegerv");
    rb.read = (es_read_fn)dlsym(RTLD_DEFAULT, "glReadPixels");
    rb.gen_buf = (es_genbuf_fn)dlsym(RTLD_DEFAULT, "glGenBuffers");
    rb.buf_data = (es_bufdata_fn)dlsym(RTLD_DEFAULT, "glBufferData");
    rb.map = (es_map_fn)dlsym(RTLD_DEFAULT, "glMapBufferRange");
    rb.unmap = (es_unmap_fn)dlsym(RTLD_DEFAULT, "glUnmapBuffer");
    rb.fence_sync = (es_fence_fn)dlsym(RTLD_DEFAULT, "glFenceSync");
    rb.wait_sync = (es_wait_fn)dlsym(RTLD_DEFAULT, "glClientWaitSync");
    rb.del_sync = (es_delsync_fn)dlsym(RTLD_DEFAULT, "glDeleteSync");
    rb.del_buf = (es_delbuf_fn)dlsym(RTLD_DEFAULT, "glDeleteBuffers");
    rb.pixelstore = (es_pixelstore_fn)dlsym(RTLD_DEFAULT, "glPixelStorei");
    rb.flush = (es_flush_fn)dlsym(RTLD_DEFAULT, "glFlush");
    rb.async = ctx.API == kEAGLRenderingAPIOpenGLES3 && rb.bind_fb && rb.bind_buf && rb.geti && rb.read &&
               rb.gen_buf && rb.buf_data && rb.map && rb.unmap && rb.fence_sync && rb.wait_sync && rb.del_sync;
    if (!rb.async) return;
    GLint prev_read = 0, fmt = 0, type = 0;
    rb.geti(ES_READ_FRAMEBUFFER_BINDING, &prev_read);
    rb.bind_fb(ES_READ_FRAMEBUFFER, fbo);
    rb.geti(ES_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
    rb.geti(ES_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    rb.bind_fb(ES_READ_FRAMEBUFFER, (GLuint)prev_read);
    rb.bgra = (fmt == ES_BGRA && type == GL_UNSIGNED_BYTE) || getenv("LB_READ_BGRA");   // the latter: to time it
    rb.gen_buf(RING, rb.pbo);
    GLint prev_pack = 0;
    rb.geti(ES_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
    for (int i = 0; i < RING; i++) {
        rb.bind_buf(ES_PIXEL_PACK_BUFFER, rb.pbo[i]);
        rb.buf_data(ES_PIXEL_PACK_BUFFER, (ptrdiff_t)win_w * win_h * 4, NULL, ES_STREAM_READ);
    }
    rb.bind_buf(ES_PIXEL_PACK_BUFFER, (GLuint)prev_pack);
    hle_log("GL: frames read back asynchronously (%s)\n", rb.bgra ? "BGRA" : "RGBA");
}

// rows bottom up -> top down, alpha opaque; RGBA bytes swizzled to 0xAARRGGBB
static void copy_frame(uint32_t *dst, const uint32_t *src, int bgra)
{
    size_t row = (size_t)win_w;
    for (int y = 0; y < win_h; y++) {
        const uint32_t *s = src + row * (size_t)(win_h - 1 - y);
        uint32_t *d = dst + row * (size_t)y;
        if (bgra) {
            for (size_t x = 0; x < row; x++) d[x] = s[x] | 0xff000000u;
        } else {
            for (size_t x = 0; x < row; x++) {
                uint32_t v = s[x];
                d[x] = 0xff000000u | (v & 0xff) << 16 | (v & 0xff00) | (v >> 16 & 0xff);
            }
        }
    }
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static double thread_cpu_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

void host_gl_readback(uint32_t *dst)
{
    if (!rb.ready) readback_setup();
    if (rb.async) {
        double t0 = now_s(), c0 = thread_cpu_s();
        // the frame's commands go to the driver first, so the read's own time shows
        if (rb.flush) rb.flush();
        double t1 = now_s();
        GLint prev_read = 0, prev_pack = 0;
        rb.geti(ES_READ_FRAMEBUFFER_BINDING, &prev_read);
        rb.geti(ES_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
        // this frame into the next buffer (a frame never copied out there is dropped)
        unsigned cur = rb.next % RING;
        rb.bind_fb(ES_READ_FRAMEBUFFER, fbo);
        rb.bind_buf(ES_PIXEL_PACK_BUFFER, rb.pbo[cur]);
        if (rb.pixelstore) rb.pixelstore(GL_PACK_ALIGNMENT, 4);
        rb.read(0, 0, win_w, win_h, rb.bgra ? ES_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        double t2 = now_s();
        if (rb.fence[cur]) rb.del_sync(rb.fence[cur]);
        rb.fence[cur] = rb.fence_sync(ES_SYNC_GPU_COMMANDS_COMPLETE, 0);
        rb.seq[cur] = ++rb.next;
        rb.queued[cur] = now_s();
        double t3 = rb.queued[cur];
        // the newest buffer the GPU has finished, without waiting for it
        int best = -1;
        for (int i = 0; i < RING; i++) {
            if (!rb.fence[i] || rb.seq[i] <= rb.shown) continue;
            GLenum r = rb.wait_sync(rb.fence[i], i == (int)cur ? ES_SYNC_FLUSH_COMMANDS_BIT : 0, 0);
            if (r != ES_ALREADY_SIGNALED && r != ES_CONDITION_SATISFIED) continue;
            rb.gpu_total += now_s() - rb.queued[i];
            rb.gpu_n++;
            rb.del_sync(rb.fence[i]);
            rb.fence[i] = NULL;
            if (best < 0 || rb.seq[i] > rb.seq[best]) best = i;
        }
        double t4 = now_s();
        if (best >= 0) {
            rb.bind_buf(ES_PIXEL_PACK_BUFFER, rb.pbo[best]);
            const uint32_t *p = rb.map(ES_PIXEL_PACK_BUFFER, 0, (ptrdiff_t)win_w * win_h * 4, ES_MAP_READ_BIT);
            if (p) copy_frame(dst, p, rb.bgra);
            rb.unmap(ES_PIXEL_PACK_BUFFER);
            rb.shown = rb.seq[best];
        }
        // otherwise dst still holds the last frame copied out
        rb.bind_buf(ES_PIXEL_PACK_BUFFER, (GLuint)prev_pack);
        rb.bind_fb(ES_READ_FRAMEBUFFER, (GLuint)prev_read);
        double t5 = now_s();
        rb.t_flush += t1 - t0, rb.t_read += t2 - t1, rb.t_fence += t3 - t2, rb.t_poll += t4 - t3;
        rb.t_map += t5 - t4, rb.t_cpu += thread_cpu_s() - c0;
        rb.t_n++;
        return;
    }
    GLint prev = 0, align = 4;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    static uint32_t *tmp;
    static size_t tmp_n;
    size_t n = (size_t)win_w * win_h;
    if (tmp_n < n) free(tmp), tmp = malloc(n * 4), tmp_n = n;
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)prev);
    copy_frame(dst, tmp, 0);
}

// The finished frame for a front end that shows it with Metal (afcore
// screen_surface): copied on the GPU, flipped top row first, into one of five
// IOSurface-backed BGRA pixel buffers (CVOpenGLESTextureCache), with a fence;
// the newest one the GPU has finished is handed over. No glReadPixels, which on
// the iPad converted the frame on the CPU (25-47 ms a frame in HotD4's stage 1).
// The last three buffers handed over are not drawn into: screen()'s, the next
// one's, and the one the front end's view may still be showing. Goes to OpenGL ES directly, putting back what it changes.
typedef void (*es_genfb_fn)(GLsizei, GLuint *);
typedef void (*es_fbtex_fn)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*es_fbstatus_fn)(GLenum);
typedef void (*es_blit_fn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef GLboolean (*es_isenabled_fn)(GLenum);
typedef void (*es_cap_fn)(GLenum);
typedef void (*es_bindtex_fn)(GLenum, GLuint);
#define SURFACES 5
enum { ES_DRAW_FRAMEBUFFER = 0x8CA9, ES_DRAW_FRAMEBUFFER_BINDING = 0x8CA6, ES_RASTERIZER_DISCARD = 0x8C89 };
static struct {
    int ready, ok;
    CVOpenGLESTextureCacheRef cache;
    CVPixelBufferRef buf[SURFACES];
    CVOpenGLESTextureRef tex[SURFACES];
    GLuint fb[SURFACES];
    void *fence[SURFACES];
    unsigned seq[SURFACES], next, shown;   // frame numbers: drawn into each, last handed over
    int done[SURFACES];                    // finished, not yet handed over
    int handed[3];                         // the last three handed over, newest first (-1: none)
    double t_blit, t_poll;
    int t_n;
    es_genfb_fn gen_fb, del_fb;
    es_fbtex_fn fb_tex;
    es_fbstatus_fn fb_status;
    es_blit_fn blit;
    es_isenabled_fn is_enabled;
    es_cap_fn enable, disable;
    es_bindtex_fn bind_tex;
} sf;

static void surface_setup(void)
{
    sf.ready = 1;
    if (!rb.ready) readback_setup();   // its entry points (fences, bindings)
    sf.gen_fb = (es_genfb_fn)dlsym(RTLD_DEFAULT, "glGenFramebuffers");
    sf.del_fb = (es_genfb_fn)dlsym(RTLD_DEFAULT, "glDeleteFramebuffers");
    sf.fb_tex = (es_fbtex_fn)dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
    sf.fb_status = (es_fbstatus_fn)dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
    sf.blit = (es_blit_fn)dlsym(RTLD_DEFAULT, "glBlitFramebuffer");
    sf.is_enabled = (es_isenabled_fn)dlsym(RTLD_DEFAULT, "glIsEnabled");
    sf.enable = (es_cap_fn)dlsym(RTLD_DEFAULT, "glEnable");
    sf.disable = (es_cap_fn)dlsym(RTLD_DEFAULT, "glDisable");
    sf.bind_tex = (es_bindtex_fn)dlsym(RTLD_DEFAULT, "glBindTexture");
    if (!rb.async) hle_log("GL: no OpenGL ES 3 readback: no IOSurface buffers\n");
    if (!rb.async || !sf.gen_fb || !sf.del_fb || !sf.fb_tex || !sf.fb_status || !sf.blit || !sf.is_enabled ||
        !sf.enable || !sf.disable || !sf.bind_tex)
        return;
    CVReturn r = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, NULL, ctx, NULL, &sf.cache);
    if (r != kCVReturnSuccess) {
        hle_log("GL: no texture cache (%d): frames read back\n", r);
        return;
    }
    NSDictionary *attrs = @{
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id)kCVPixelBufferOpenGLESCompatibilityKey: @YES,
        (id)kCVPixelBufferOpenGLESTextureCacheCompatibilityKey: @YES,
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    GLint prev_draw = 0, prev_tex = 0;
    rb.geti(ES_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    rb.geti(GL_TEXTURE_BINDING_2D, &prev_tex);
    int ok = 1;
    for (int i = 0; i < SURFACES && ok; i++) {
        r = CVPixelBufferCreate(kCFAllocatorDefault, (size_t)win_w, (size_t)win_h, kCVPixelFormatType_32BGRA,
                                (__bridge CFDictionaryRef)attrs, &sf.buf[i]);
        if (r == kCVReturnSuccess)
            r = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, sf.cache, sf.buf[i], NULL,
                                                             GL_TEXTURE_2D, GL_RGBA, win_w, win_h, ES_BGRA,
                                                             GL_UNSIGNED_BYTE, 0, &sf.tex[i]);
        if (r != kCVReturnSuccess) {
            hle_log("GL: no IOSurface buffer %d (%d)\n", i, r);
            ok = 0;
            break;
        }
        sf.gen_fb(1, &sf.fb[i]);
        rb.bind_fb(ES_DRAW_FRAMEBUFFER, sf.fb[i]);
        sf.fb_tex(ES_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, CVOpenGLESTextureGetTarget(sf.tex[i]),
                  CVOpenGLESTextureGetName(sf.tex[i]), 0);
        GLenum st = sf.fb_status(ES_DRAW_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) hle_log("GL: IOSurface framebuffer %d incomplete (0x%x)\n", i, st);
        ok = st == GL_FRAMEBUFFER_COMPLETE;
    }
    rb.bind_fb(ES_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
    sf.bind_tex(GL_TEXTURE_2D, (GLuint)prev_tex);
    sf.ok = ok;
    sf.handed[0] = sf.handed[1] = sf.handed[2] = -1;
    hle_log("GL: frames handed over as %s\n", ok ? "IOSurface pixel buffers" : "pixels (no IOSurface buffers)");
}

static int surface_held(int i) { return sf.handed[0] == i || sf.handed[1] == i || sf.handed[2] == i; }

// whether frames can go over as surfaces (else they are read back)
int host_gl_surfaces(void)
{
    if (!sf.ready) surface_setup();
    return sf.ok;
}

void *host_gl_surface_frame(void)
{
    if (!host_gl_surfaces()) return NULL;
    double t0 = now_s();
    // a buffer to draw into: not held, not still being drawn (else wait for the oldest)
    int slot = -1;
    for (int pass = 0; pass < 2 && slot < 0; pass++)
        for (int k = 0; k < SURFACES && slot < 0; k++) {
            int i = (int)((sf.next + k) % SURFACES);
            if (surface_held(i)) continue;
            if (sf.fence[i]) {
                if (!pass) continue;
                rb.wait_sync(sf.fence[i], ES_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
                rb.del_sync(sf.fence[i]);
                sf.fence[i] = NULL;
            }
            slot = i;
        }
    if (slot < 0) return NULL;
    GLint prev_read = 0, prev_draw = 0;
    rb.geti(ES_READ_FRAMEBUFFER_BINDING, &prev_read);
    rb.geti(ES_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    GLboolean scissor = sf.is_enabled(GL_SCISSOR_TEST), discard = sf.is_enabled(ES_RASTERIZER_DISCARD);
    if (scissor) sf.disable(GL_SCISSOR_TEST);
    if (discard) sf.disable(ES_RASTERIZER_DISCARD);
    rb.bind_fb(ES_READ_FRAMEBUFFER, fbo);
    rb.bind_fb(ES_DRAW_FRAMEBUFFER, sf.fb[slot]);
    sf.blit(0, 0, win_w, win_h, 0, win_h, win_w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);   // flipped: top row first
    if (scissor) sf.enable(GL_SCISSOR_TEST);
    if (discard) sf.enable(ES_RASTERIZER_DISCARD);
    rb.bind_fb(ES_READ_FRAMEBUFFER, (GLuint)prev_read);
    rb.bind_fb(ES_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
    sf.fence[slot] = rb.fence_sync(ES_SYNC_GPU_COMMANDS_COMPLETE, 0);
    sf.seq[slot] = ++sf.next;
    sf.done[slot] = 0;
    double t1 = now_s();
    // the newest buffer the GPU has finished, without waiting for it
    int best = -1;
    for (int i = 0; i < SURFACES; i++) {
        if (sf.fence[i]) {
            GLenum r = rb.wait_sync(sf.fence[i], i == slot ? ES_SYNC_FLUSH_COMMANDS_BIT : 0, 0);
            if (r != ES_ALREADY_SIGNALED && r != ES_CONDITION_SATISFIED) continue;
            rb.del_sync(sf.fence[i]);
            sf.fence[i] = NULL;
            sf.done[i] = 1;
        }
        if (sf.done[i] && sf.seq[i] > sf.shown && (best < 0 || sf.seq[i] > sf.seq[best])) best = i;
    }
    sf.t_blit += t1 - t0, sf.t_poll += now_s() - t1, sf.t_n++;
    if (best < 0) return NULL;
    sf.shown = sf.seq[best];
    sf.handed[2] = sf.handed[1], sf.handed[1] = sf.handed[0], sf.handed[0] = best;
    for (int i = 0; i < SURFACES; i++)
        if (sf.seq[i] <= sf.shown) sf.done[i] = 0;
    return sf.buf[best];
}

// a buffer from host_gl_surface_frame as 0xAARRGGBB pixels, for screen()
void host_gl_surface_read(void *surface, uint32_t *dst, int w, int h)
{
    CVPixelBufferRef b = surface;
    if (CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
    const uint8_t *src = CVPixelBufferGetBaseAddress(b);
    size_t stride = CVPixelBufferGetBytesPerRow(b);
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + stride * (size_t)y);
        uint32_t *d = dst + (size_t)w * y;
        for (int x = 0; x < w; x++) d[x] = s[x] | 0xff000000u;
    }
    CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
}

static void surface_close(void)
{
    for (int i = 0; i < SURFACES; i++) {
        if (sf.fence[i]) rb.del_sync(sf.fence[i]);
        if (sf.fb[i] && sf.del_fb) sf.del_fb(1, &sf.fb[i]);
        if (sf.tex[i]) CFRelease(sf.tex[i]);
        if (sf.buf[i]) CVPixelBufferRelease(sf.buf[i]);   // a front end's own retain keeps its buffer
    }
    if (sf.cache) CFRelease(sf.cache);
    memset(&sf, 0, sizeof sf);
}

// the GPU's time per frame (a read queued to its fence found done) and where
// the readback's time went, for the perf: line; empty without asynchronous reads
void host_gl_perf(char *buf, size_t size)
{
    *buf = 0;
    if (sf.t_n) {
        double n = sf.t_n * 1e-3;
        snprintf(buf, size, "; frame to the front end: copy on the GPU %.1f ms, poll %.1f ms", sf.t_blit / n,
                 sf.t_poll / n);
        sf.t_blit = sf.t_poll = 0, sf.t_n = 0;
        return;
    }
    if (!rb.async || !rb.t_n) return;
    double n = rb.t_n * 1e-3;
    snprintf(buf, size, ", GPU %.1f ms; readback: flush %.1f, read %.1f, fence %.1f, poll %.1f, map and copy %.1f ms "
             "(its CPU %.1f ms)", rb.gpu_n ? rb.gpu_total / rb.gpu_n * 1e3 : 0.0, rb.t_flush / n, rb.t_read / n,
             rb.t_fence / n, rb.t_poll / n, rb.t_map / n, rb.t_cpu / n);
    rb.gpu_total = 0, rb.gpu_n = 0;
    rb.t_flush = rb.t_read = rb.t_fence = rb.t_poll = rb.t_map = rb.t_cpu = 0;
    rb.t_n = 0;
}

// every guest thread has ended: drop the context and everything in it
void host_gl_close(void)
{
    if (ctx) make_current(1);   // on whichever thread closes
    if (ctx) surface_close();
    if (rb.async && ctx) {
        for (int i = 0; i < RING; i++)
            if (rb.fence[i]) rb.del_sync(rb.fence[i]);
        if (rb.del_buf) rb.del_buf(RING, rb.pbo);
    }
    memset(&rb, 0, sizeof rb);
    if (gl4es_state) {
        ActivateGLState(NULL);
        DeleteGLState(gl4es_state);
        gl4es_state = NULL;
    }
    if (ctx) [EAGLContext setCurrentContext:nil];
    ctx = nil;
    current = 0;
    fbo = color_tex = depth_rb = 0;
    win_w = win_h = 0;
}
