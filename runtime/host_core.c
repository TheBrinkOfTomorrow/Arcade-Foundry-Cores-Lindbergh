// The core library's OpenGL: an off-screen context with no window, the game's
// window a framebuffer object the size the game asked for. EGL without a
// surface on Linux (Mesa), CGL on macOS.
#include "host.h"
#include "hle.h"
#include "gl_host.h"
#include <string.h>

#ifdef __APPLE__
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
static CGLContextObj ctx;
#else
#include <EGL/egl.h>
#include <EGL/eglext.h>
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
#endif

static int win_w, win_h;
static GLuint fbo, color_rb, depth_rb;
static __thread int current;      // the context is current on this thread

static PFNGLGENFRAMEBUFFERSEXTPROC gen_fb;
static PFNGLBINDFRAMEBUFFEREXTPROC bind_fb;
static PFNGLDELETEFRAMEBUFFERSEXTPROC del_fb;
static PFNGLGENRENDERBUFFERSEXTPROC gen_rb;
static PFNGLBINDRENDERBUFFEREXTPROC bind_rb;
static PFNGLDELETERENDERBUFFERSEXTPROC del_rb;
static PFNGLRENDERBUFFERSTORAGEEXTPROC rb_storage;
static PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC fb_rb;
static PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC fb_status;

void *host_gl_proc(const char *name)
{
#ifdef __APPLE__
    return dlsym(RTLD_DEFAULT, name);
#else
    return (void *)eglGetProcAddress(name);
#endif
}

static void make_current(int on)
{
#ifdef __APPLE__
    CGLSetCurrentContext(on ? ctx : NULL);
#else
    if (on) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    else eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#endif
    current = on;
}

static void create_context(void)
{
#ifdef __APPLE__
    // the legacy profile: OpenGL 2.1 with the fixed function pipeline the games use
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
        kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, kCGLPFADepthSize, (CGLPixelFormatAttribute)24,
        kCGLPFAStencilSize, (CGLPixelFormatAttribute)8, (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pf;
    GLint n;
    if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf) host_fatal("no OpenGL pixel format");
    CGLError e = CGLCreateContext(pf, NULL, &ctx);
    CGLReleasePixelFormat(pf);
    if (e) host_fatal("cannot create an OpenGL context");
#else
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_display) dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) host_fatal("no EGL display");
    if (!eglBindAPI(EGL_OPENGL_API)) host_fatal("EGL has no desktop OpenGL");
    EGLint attrs[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attrs, &cfg, 1, &n) || !n) host_fatal("no EGL config for desktop OpenGL");
    EGLint cattrs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE};
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattrs);
    if (ctx == EGL_NO_CONTEXT) host_fatal("cannot create an OpenGL context");
#endif
}

void host_gl_open(int w, int h)
{
    if (fbo) return;
    win_w = w, win_h = h;
    create_context();
    make_current(1);
    gen_fb = host_gl_proc("glGenFramebuffersEXT");
    bind_fb = host_gl_proc("glBindFramebufferEXT");
    del_fb = host_gl_proc("glDeleteFramebuffersEXT");
    gen_rb = host_gl_proc("glGenRenderbuffersEXT");
    bind_rb = host_gl_proc("glBindRenderbufferEXT");
    del_rb = host_gl_proc("glDeleteRenderbuffersEXT");
    rb_storage = host_gl_proc("glRenderbufferStorageEXT");
    fb_rb = host_gl_proc("glFramebufferRenderbufferEXT");
    fb_status = host_gl_proc("glCheckFramebufferStatusEXT");
    if (!gen_fb || !bind_fb || !gen_rb || !bind_rb || !rb_storage || !fb_rb || !fb_status)
        host_fatal("OpenGL has no framebuffer objects");
    gen_fb(1, &fbo);
    bind_fb(GL_FRAMEBUFFER_EXT, fbo);
    gen_rb(1, &color_rb);
    bind_rb(GL_RENDERBUFFER_EXT, color_rb);
    rb_storage(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
    fb_rb(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, color_rb);
    gen_rb(1, &depth_rb);
    bind_rb(GL_RENDERBUFFER_EXT, depth_rb);
    rb_storage(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
    fb_rb(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth_rb);
    fb_rb(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth_rb);
    if (fb_status(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) host_fatal("the game's framebuffer is incomplete");
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    hle_log("GL: off-screen %dx%d\n", w, h);
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

// the finished frame, top row first, as 0xAARRGGBB with alpha opaque
void host_gl_readback(uint32_t *dst)
{
    GLint prev = 0, prev_read = 0, align = 4;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
    glGetIntegerv(GL_READ_BUFFER, &prev_read);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    bind_fb(GL_FRAMEBUFFER_EXT, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glReadBuffer((GLenum)prev_read);
    bind_fb(GL_FRAMEBUFFER_EXT, (GLuint)prev);
    // GL's rows run bottom up
    size_t row = (size_t)win_w;
    for (int y = 0; y < win_h / 2; y++) {
        uint32_t *a = dst + row * y, *b = dst + row * (win_h - 1 - y);
        for (size_t x = 0; x < row; x++) {
            uint32_t t = a[x] | 0xff000000u;
            a[x] = b[x] | 0xff000000u;
            b[x] = t;
        }
    }
    if (win_h & 1)
        for (size_t x = 0; x < row; x++) dst[row * (win_h / 2) + x] |= 0xff000000u;
}

// every guest thread has ended: drop the context and everything in it
void host_gl_close(void)
{
#ifdef __APPLE__
    if (ctx) {
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(ctx);
    }
    ctx = NULL;
#else
    if (ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, ctx);
    }
    ctx = EGL_NO_CONTEXT;
#endif
    current = 0;
    fbo = color_rb = depth_rb = 0;
    win_w = win_h = 0;
}
