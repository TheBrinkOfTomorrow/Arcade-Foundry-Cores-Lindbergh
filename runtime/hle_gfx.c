// X11 and GLX for a game that expects a single full-screen window, on the
// host's GL context (host.h). GLX pbuffers become framebuffer objects in it.
#include "af_debug.h"
#include "gl_host.h"
#include "host.h"
#include "input.h"
#include "boot_font.h"
#include "nv_vertex_program.h"
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>

int gl_trace;
int gl_open;
static int win_w = 640, win_h = 480;
static uint32_t display, visualinfo, visual, fbconfigs;
static uint32_t array_buffer, element_buffer;
static int frame;
static int shot_every;

// the host's function by its name, or by another name for it: an extension's
// function the host has in core (glGenFramebuffersEXT as glGenFramebuffers:
// gl4es on Metal), or a core one it has as the extension it came from
// (glGenQueries as glGenQueriesARB): 2 Spicy asks for every GL function by name
static void *host_proc_any(const char *name)
{
    void *p = host_gl_proc(name);
    size_t n = strlen(name);
    char alt[128];
    if (p || n >= sizeof alt - 4 || strncmp(name, "gl", 2)) return p;
    if (n > 5 && (!strcmp(name + n - 3, "EXT") || !strcmp(name + n - 3, "ARB"))) {
        snprintf(alt, sizeof alt, "%.*s", (int)(n - 3), name);
        if ((p = host_gl_proc(alt))) return p;
    }
    snprintf(alt, sizeof alt, "%sARB", name);
    if ((p = host_gl_proc(alt))) return p;
    snprintf(alt, sizeof alt, "%sEXT", name);
    return host_gl_proc(alt);
}

void *gl_host_proc(const char *name)
{
    void *p = host_proc_any(name);
    if (!p) hle_log("GL: host has no %s\n", name);
    return p;
}

void *gl_array_ptr(uint32_t p) { return array_buffer ? (void *)(uintptr_t)p : (p ? (void *)G(p) : NULL); }
void *gl_element_ptr(uint32_t p) { return element_buffer ? (void *)(uintptr_t)p : (p ? (void *)G(p) : NULL); }
static int trace_frame = -1, trace_last = -1;
void gl_trace_call(const char *name, CPU *c)
{
    hle_log("gl %s %x %x %x %x %x %x %x %x %x\n", name, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5),
            ARG(6), ARG(7), ARG(8));
}

// ---- window --------------------------------------------------------------------------
static void ensure_window(void)
{
    if (gl_open) return;
    host_gl_open(win_w, win_h);
    gl_open = 1;
    hle_log("GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

// ---- X11 ---------------------------------------------------------------------------------
static void h_XOpenDisplay(CPU *c)
{
    if (!display) {
        display = hostdata_alloc(512);
        uint32_t screen = hostdata_alloc(80);
        visual = hostdata_alloc(32);
        wr32(visual + 4, 0x21);         // visualid
        wr32(visual + 8, 4);            // TrueColor
        wr32(screen + 4, display);
        wr32(screen + 8, 0x1);          // root window
        wr32(screen + 12, 1360);
        wr32(screen + 16, 768);
        wr32(screen + 36, 24);
        wr32(screen + 40, visual);
        wr32(screen + 48, 0x20);        // default colormap
        wr32(screen + 52, 0xffffff);
        wr32(display + 8, 3);           // fd
        wr32(display + 132, 0);         // default_screen
        wr32(display + 136, 1);         // nscreens
        wr32(display + 140, screen);
    }
    RET(display);
}

typedef struct { uint32_t ev[24]; } xevent;
static xevent queue[64];
static int qhead, qtail;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;

static void push_event(int type, uint32_t window)
{
    pthread_mutex_lock(&qlock);
    xevent *e = &queue[qtail % 64];
    memset(e, 0, sizeof *e);
    e->ev[0] = type;
    e->ev[3] = display;
    e->ev[4] = window;
    e->ev[5] = window;
    qtail++;
    pthread_mutex_unlock(&qlock);
}

static void h_XCreateWindow(CPU *c)
{
    win_w = (int)ARG(4);
    win_h = (int)ARG(5);
#if LB_MODE_W != LB_SCREEN_W || LB_MODE_H != LB_SCREEN_H
    // a display larger than the game draws on (GAME_MODE_SIZE: Let's Go Jungle
    // Special's 2048x768, the game in its left 1024x768 and its other output
    // unused): the window is the screen, and the rest falls outside it
    if (win_w > LB_SCREEN_W) win_w = LB_SCREEN_W;
    if (win_h > LB_SCREEN_H) win_h = LB_SCREEN_H;
#endif
    hle_log("XCreateWindow %dx%d\n", win_w, win_h);
    RET(0x100);
}

static void h_XMapWindow(CPU *c)
{
    push_event(19, ARG(1));   // MapNotify
    push_event(12, ARG(1));   // Expose
    RET(0);
}

static void h_XPending(CPU *c)
{
    input_poll();
    pthread_mutex_lock(&qlock);
    int n = qtail - qhead;
    pthread_mutex_unlock(&qlock);
    RET(n);
}

static void h_XNextEvent(CPU *c)
{
    pthread_mutex_lock(&qlock);
    if (qhead == qtail) {
        pthread_mutex_unlock(&qlock);
        memset(G(ARG(1)), 0, 96);
        wr32(ARG(1), 14);   // NoExpose
        RET(0);
    }
    memcpy(G(ARG(1)), &queue[qhead % 64], 96);
    qhead++;
    pthread_mutex_unlock(&qlock);
    RET(0);
}

static void h_XTranslateCoordinates(CPU *c)
{
    if (ARG(5)) wr32(ARG(5), 0);
    if (ARG(6)) wr32(ARG(6), 0);
    if (ARG(7)) wr32(ARG(7), 0);
    RET(1);
}

static void h_XLookupString(CPU *c)
{
    if (ARG(3)) wr32(ARG(3), 0);
    RET(0);
}

static void h_ret0(CPU *c) { RET(0); }
static void h_ret1(CPU *c) { RET(1); }
static void h_retid(CPU *c) { RET(0x200); }

static void h_XInternAtom(CPU *c)
{
    static uint32_t next = 0x100;
    RET(next++);
}
static void h_XGetErrorText(CPU *c)
{
    if (ARG(2) && ARG(3)) G(ARG(2))[0] = 0;
    RET(0);
}
static void h_XQueryPointer(CPU *c)
{
    for (int i = 2; i <= 8; i++)
        if (ARG(i)) wr32(ARG(i), 0);
    RET(1);
}

// ---- GLX ---------------------------------------------------------------------------------
static uint32_t make_visualinfo(void)
{
    if (!visualinfo) {
        visualinfo = hostdata_alloc(40);
        wr32(visualinfo, visual);
        wr32(visualinfo + 4, 0x21);
        wr32(visualinfo + 12, 24);
        wr32(visualinfo + 16, 4);
        wr32(visualinfo + 20, 0xff0000);
        wr32(visualinfo + 24, 0xff00);
        wr32(visualinfo + 28, 0xff);
        wr32(visualinfo + 32, 256);
        wr32(visualinfo + 36, 8);
    }
    return visualinfo;
}

static void h_glXChooseVisual(CPU *c) { RET(make_visualinfo()); }
static void h_glXGetVisualFromFBConfig(CPU *c) { RET(make_visualinfo()); }

static void h_glXChooseFBConfig(CPU *c)
{
    if (!fbconfigs) {
        fbconfigs = hostdata_alloc(8);
        wr32(fbconfigs, 0x300);
    }
    if (ARG(3)) wr32(ARG(3), 1);
    RET(fbconfigs);
}

// pbuffers: an FBO with colour and depth-stencil attachments
typedef struct { uint32_t id; int w, h; GLuint fbo, color, depth; } pbuffer;
static pbuffer pbufs[16];
static int npbufs;
static uint32_t cur_drawable;

static uint32_t create_pbuffer(int w, int h)
{
    if (npbufs == 16) return 0;
    pbuffer *p = &pbufs[npbufs];
    p->id = 0x400 + npbufs++;
    p->w = w;
    p->h = h;
    hle_log("glXCreatePbuffer %dx%d -> 0x%x\n", w, h, p->id);
    return p->id;
}

static void h_glXCreatePbuffer(CPU *c)
{
    uint32_t attr = ARG(2);
    int w = 0, h = 0;
    for (int i = 0; attr && rd32(attr + 8 * i); i++) {
        uint32_t k = rd32(attr + 8 * i), v = rd32(attr + 8 * i + 4);
        if (k == 0x8041) w = (int)v;   // GLX_PBUFFER_WIDTH
        if (k == 0x8040) h = (int)v;   // GLX_PBUFFER_HEIGHT
    }
    RET(create_pbuffer(w, h));
}

// GLX_SGIX_fbconfig and GLX_SGIX_pbuffer (Let's Go Jungle)
static void h_glXCreateGLXPbufferSGIX(CPU *c) { RET(create_pbuffer((int)ARG(2), (int)ARG(3))); }
static void h_glXGetFBConfigAttribSGIX(CPU *c)
{
    uint32_t v = 0;
    switch (ARG(2)) {
    case 0x8010: v = 7; break;   // GLX_DRAWABLE_TYPE: window, pixmap, pbuffer
    case 0x8011: v = 1; break;   // GLX_RENDER_TYPE: RGBA
    case 0x8012: v = 1; break;   // GLX_X_RENDERABLE
    case 0x8013: v = 0x300; break; // GLX_FBCONFIG_ID
    case 0x8016: v = 4096; break;  // GLX_MAX_PBUFFER_WIDTH
    case 0x8017: v = 4096; break;  // GLX_MAX_PBUFFER_HEIGHT
    case 2: v = 32; break;       // GLX_BUFFER_SIZE
    case 5: v = 1; break;        // GLX_DOUBLEBUFFER
    case 8: case 9: case 10: case 11: v = 8; break;   // red, green, blue, alpha sizes
    case 12: v = 24; break;      // GLX_DEPTH_SIZE
    case 13: v = 8; break;       // GLX_STENCIL_SIZE
    }
    if (ARG(3)) wr32(ARG(3), v);
    RET(0);
}

static uint32_t cur_context;
static void h_glXGetCurrentContext(CPU *c) { RET(cur_context); }
static void h_glXGetCurrentDisplay(CPU *c) { RET(display); }
static void h_glXGetCurrentDrawable(CPU *c) { RET(cur_drawable); }

static uint32_t glx_string(int k)
{
    static uint32_t s[4];
    static const char *const text[4] = {
        "", "NVIDIA Corporation", "1.3",
        "GLX_ARB_get_proc_address GLX_SGIX_fbconfig GLX_SGIX_pbuffer GLX_SGI_swap_control"};
    if (k < 1 || k > 3) k = 3;
    if (!s[k]) s[k] = guest_strdup(text[k]);
    return s[k];
}
static void h_glXGetClientString(CPU *c) { RET(glx_string((int)ARG(1))); }
static void h_glXQueryServerString(CPU *c) { RET(glx_string((int)ARG(2))); }
static void h_glXQueryExtensionsString(CPU *c) { RET(glx_string(3)); }

// Cg profiles the host can run: the ARB assembly ones, and vp40/fp40 where
// the host has NVIDIA's program extensions (Apple's GL does). Let's Go Jungle
// compiles its skinning for 64 bones with vp40 and for 20 with arbvp1: with
// only arbvp1 the giant spider's bones past 20 indexed outside the palette.
static int host_has_extension(const char *name)
{
    const char *all = (const char *)glGetString(GL_EXTENSIONS);
    size_t n = strlen(name);
    for (const char *p = all; p && (p = strstr(p, name)); p += n)
        if ((p == all || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return 1;
    return 0;
}
// NVIDIA's options on ARB programs (OPTION NV_vertex_program3 and
// NV_fragment_program2), which the runtime's translations of NV programs
// carry: Apple's GL and NVIDIA's take them; Mesa rejects the whole program
// ("invalid ARB vertex program option": OutRun 2 SP's race on Linux drew
// with none of its vertex programs), so there arb_lower.c takes them out.
static int host_has_nv_options(void)
{
    static int has = -1;
    if (has < 0) has = host_has_extension("GL_NV_vertex_program3") && host_has_extension("GL_NV_fragment_program2");
    return has;
}
char *arb_lower_nv(char *src);

static void h_cgGLIsProfileSupported(CPU *c)
{
    switch (ARG(0)) {
    case 6150: case 7000: RET(1);                                     // arbvp1, arbfp1
    // gl4es (iOS) and Mesa (Linux) list no NVIDIA option, but arb_lower.c
    // lowers vp40's for them: with arbvp1 Let's Go Jungle compiled most of its
    // skinning for 20 bones (and three shaders not at all), while its heads'
    // vp40 program, made for 64, was given no bones (the characters drawn as
    // shards and streaks on iOS, slabs and bars on Linux)
    case 7001: RET(1);                                                // vp40
    case 6151: RET(host_has_extension("GL_NV_fragment_program2"));    // fp40
    }
    RET(0);
}

static void bind_drawable(uint32_t d)
{
    static PFNGLGENFRAMEBUFFERSEXTPROC gen;
    static PFNGLBINDFRAMEBUFFEREXTPROC bind;
    static PFNGLGENRENDERBUFFERSEXTPROC genrb;
    static PFNGLBINDRENDERBUFFEREXTPROC bindrb;
    static PFNGLRENDERBUFFERSTORAGEEXTPROC storage;
    static PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC attach;
    if (!gen) {
        gen = gl_host_proc("glGenFramebuffersEXT");
        bind = gl_host_proc("glBindFramebufferEXT");
        genrb = gl_host_proc("glGenRenderbuffersEXT");
        bindrb = gl_host_proc("glBindRenderbufferEXT");
        storage = gl_host_proc("glRenderbufferStorageEXT");
        attach = gl_host_proc("glFramebufferRenderbufferEXT");
    }
    cur_drawable = d;
    for (int i = 0; i < npbufs; i++)
        if (pbufs[i].id == d) {
            pbuffer *p = &pbufs[i];
            if (!p->fbo) {
                gen(1, &p->fbo);
                bind(GL_FRAMEBUFFER_EXT, p->fbo);
                genrb(1, &p->color);
                bindrb(GL_RENDERBUFFER_EXT, p->color);
                storage(GL_RENDERBUFFER_EXT, GL_RGBA8, p->w, p->h);
                attach(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, p->color);
                genrb(1, &p->depth);
                bindrb(GL_RENDERBUFFER_EXT, p->depth);
                storage(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, p->w, p->h);
                attach(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, p->depth);
                attach(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, p->depth);
            }
            bind(GL_FRAMEBUFFER_EXT, p->fbo);
            return;
        }
    if (bind) bind(GL_FRAMEBUFFER_EXT, host_gl_framebuffer());
}

static void h_glXCreateContext(CPU *c) { RET(0x500); }

static void h_glXMakeCurrent(CPU *c)
{
    ensure_window();
    cur_context = ARG(2);
    if (!ARG(2)) {
        host_gl_release();
        RET(1);
    }
    host_gl_current();
    bind_drawable(ARG(1));
    // what the runtime's own setup leaves in the error flag is not the game's
    // (Hummer reads glGetError when it makes its render targets and exits on any)
    while (glGetError() != GL_NO_ERROR) {}
    RET(1);
}

static void save_shot(void)
{
    int w, h;
    host_gl_size(&w, &h);
    unsigned char *px = malloc((size_t)w * h * 3);
    // LB_SHOT_FBO=N: the game's framebuffer object N instead of the screen
    static int fbo = -1;
    if (fbo < 0) fbo = af_debug_env("LB_SHOT_FBO") ? atoi(af_debug_env("LB_SHOT_FBO")) : 0;
    if (fbo) {
        GLint prev;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
        ((PFNGLBINDFRAMEBUFFEREXTPROC)gl_host_proc("glBindFramebufferEXT"))(GL_FRAMEBUFFER_EXT, (GLuint)fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
        ((PFNGLBINDFRAMEBUFFEREXTPROC)gl_host_proc("glBindFramebufferEXT"))(GL_FRAMEBUFFER_EXT, (GLuint)prev);
    } else {
        if (!host_gl_framebuffer()) glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    }
    char name[256];
    snprintf(name, sizeof name, "shot_%06d.ppm", frame);
    FILE *f = fopen(name, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
    }
    free(px);
}

static void h_glXSwapBuffers(CPU *c)
{
    frame++;
    // LB_GL_TRACE=N or N-M: log every library and GL call during those frames
    if (trace_frame >= 0) gl_trace = hle_trace = frame >= trace_frame && frame <= trace_last;
    if (gl_trace) hle_log("-- frame %d\n", frame);
    if (hle_verbose && frame % 600 == 1)
        for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) hle_log("GL error 0x%x before frame %d\n", e, frame);
    if (shot_every && frame % shot_every == 0) save_shot();
    host_present();
#ifdef LB_BOOT_TEXT
    // the boot's fast clock only after a frame its limiter held (one 1/60 s and
    // a margin at most: without the fast clock the limiter itself makes a frame
    // last 1/60 s), not after one whose work took longer (hle_libc.c)
    extern int lb_booting, lb_boot_clock;
    static double boot_last;
    struct timespec bt;
    clock_gettime(CLOCK_MONOTONIC, &bt);
    double boot_now = clk_render ? clk_mono_ns() * 1e-9 : bt.tv_sec + bt.tv_nsec * 1e-9;
    lb_boot_clock = boot_last > 0 && boot_now - boot_last < 1.25 / 60;
    boot_last = boot_now;
    if (lb_booting > 0) lb_booting--;
#endif
    audio_frame();
    input_poll();
    if (frame % 600 == 0) hle_log("frame %d\n", frame);
    RET(0);
}

// ---- GLUT (the test-mode programs use it instead of X11/GLX) ------------------------------
static uint32_t glut_display, glut_idle, glut_keyboard;
static int glut_redisplay = 1;

static void h_glutInitWindowSize(CPU *c)
{
    win_w = (int)ARG(0);
    win_h = (int)ARG(1);
    RET(0);
}
static void h_glutGameModeString(CPU *c)
{
    int w, h;
    if (sscanf(G(ARG(0)), "%dx%d", &w, &h) == 2) { win_w = w; win_h = h; }
    RET(0);
}
static void h_glutEnterGameMode(CPU *c) { ensure_window(); RET(1); }
static void h_glutCreateWindow(CPU *c) { ensure_window(); RET(1); }
static void h_glutDisplayFunc(CPU *c) { glut_display = ARG(0); RET(0); }
static void h_glutIdleFunc(CPU *c) { glut_idle = ARG(0); RET(0); }
static void h_glutKeyboardFunc(CPU *c) { glut_keyboard = ARG(0); RET(0); }
static void h_glutPostRedisplay(CPU *c) { glut_redisplay = 1; RET(0); }
// the test menu relies on the cabinet's vsync; the host holds every program to 60 Hz
static void h_glutSwapBuffers(CPU *c) { h_glXSwapBuffers(c); }
static void h_glutMainLoop(CPU *c)
{
    // never returns: the program leaves with exit()
    for (;;) {
        if (glut_idle) guest_call(c, glut_idle, 0, NULL);
        if (glut_display && (glut_redisplay || !glut_idle)) {
            glut_redisplay = 0;
            guest_call(c, glut_display, 0, NULL);
        }
    }
}

// OutRun 2 SP runs its own loop and asks GLUT only to handle events
static void h_glutMainLoopEvent(CPU *c)
{
    if (glut_display && glut_redisplay) {
        glut_redisplay = 0;
        guest_call(c, glut_display, 0, NULL);
    }
    RET(0);
}

static void h_glutGet(CPU *c)
{
    switch (ARG(0)) {
    case 102: case 200: RET((uint32_t)win_w);             // GLUT_WINDOW_WIDTH, GLUT_SCREEN_WIDTH
    case 103: case 201: RET((uint32_t)win_h);             // GLUT_WINDOW_HEIGHT, GLUT_SCREEN_HEIGHT
    case 700: {                                           // GLUT_ELAPSED_TIME
        static struct timespec t0;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (clk_render) {   // the frames' time (clock.c)
            uint64_t ns = clk_mono_ns();
            t.tv_sec = (time_t)(ns / 1000000000ull), t.tv_nsec = (long)(ns % 1000000000ull);
        }
        if (!t0.tv_sec) t0 = t;
        RET((uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000));
    }
    }
    RET(0);
}

// GLUT's 9x15 bitmap font (the only one the games use), drawn from the
// runtime's stand-in font, resampled from 10x20
#define GLYPH_W 9
#define GLYPH_H 15
static uint8_t glyph_bits[95][GLYPH_H * 2];   // rows bottom up, 2 bytes each, as glBitmap wants
static int glyphs_made;

static void make_glyphs(void)
{
    for (int g = 0; g < 95; g++)
        for (int y = 0; y < GLYPH_H; y++) {
            int sy = (GLYPH_H - 1 - y) * BOOT_FONT_H / GLYPH_H;   // top-down source row
            for (int x = 0; x < GLYPH_W; x++) {
                int sx = x * BOOT_FONT_W / GLYPH_W, i = sy * BOOT_FONT_W + sx;
                int v = (boot_font[g][i / 2] >> (i & 1 ? 0 : 4)) & 15;
                if (v >= 6) glyph_bits[g][y * 2 + x / 8] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
    glyphs_made = 1;
}

static void h_glutBitmapCharacter(CPU *c)
{
    int ch = (int)ARG(1);
    if (!glyphs_made) make_glyphs();
    GLint align;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const uint8_t *b = ch >= BOOT_FONT_FIRST && ch <= BOOT_FONT_LAST ? glyph_bits[ch - BOOT_FONT_FIRST] : glyph_bits[0];
    glBitmap(GLYPH_W, GLYPH_H, 0, 3, GLYPH_W, 0, b);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    RET(0);
}
static void h_glutBitmapWidth(CPU *c) { RET(GLYPH_W); }
static void h_glutSolidTeapot(CPU *c) { RET(0); }

// ---- GLU: the few matrix helpers, on the GL calls they stand for --------------------------
static void h_gluPerspective(CPU *c)
{
    double fovy = ARGD(0), aspect = ARGD(2), zn = ARGD(4), zf = ARGD(6);
    double t = zn * tan(fovy * M_PI / 360.0);
    glFrustum(-t * aspect, t * aspect, -t, t, zn, zf);
    RET(0);
}
static void h_gluOrtho2D(CPU *c) { glOrtho(ARGD(0), ARGD(2), ARGD(4), ARGD(6), -1, 1); RET(0); }
static void h_gluLookAt(CPU *c)
{
    double e[3] = {ARGD(0), ARGD(2), ARGD(4)}, t[3] = {ARGD(6), ARGD(8), ARGD(10)}, u[3] = {ARGD(12), ARGD(14), ARGD(16)};
    double f[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    double fl = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl > 0) f[0] /= fl, f[1] /= fl, f[2] /= fl;
    double s[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    double sl = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl > 0) s[0] /= sl, s[1] /= sl, s[2] /= sl;
    double v[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    double m[16] = {s[0], v[0], -f[0], 0, s[1], v[1], -f[1], 0, s[2], v[2], -f[2], 0, 0, 0, 0, 1};
    glMultMatrixd(m);
    glTranslated(-e[0], -e[1], -e[2]);
    RET(0);
}
static uint32_t glu_error;
static void h_gluErrorString(CPU *c)
{
    if (!glu_error) glu_error = hostdata_alloc(64);
    snprintf(G(glu_error), 64, "GL error 0x%x", ARG(0));
    RET(glu_error);
}

static void h_glXQueryExtension(CPU *c)
{
    if (ARG(1)) wr32(ARG(1), 0);
    if (ARG(2)) wr32(ARG(2), 0);
    RET(1);
}

// ---- GL calls that need more than argument conversion -------------------------------------
static uint32_t gl_strings[8];
static void h_glGetString(CPU *c)
{
    uint32_t e = ARG(0);
    int k = e - GL_VENDOR;
    if (k < 0 || k > 3) RET(0);
    if (!gl_strings[k]) {
        const char *s = (const char *)glGetString(e);
        if (!s) RET(0);
        if (e == GL_RENDERER) s = "GeForce 7800 GTX/PCI/SSE2";
        if (e == GL_VENDOR) s = "NVIDIA Corporation";
        // and the NVIDIA extensions the runtime builds on the host's (gl_emulated)
        char ext[64] = "";
        if (e == GL_EXTENSIONS) {
            if (strstr(s, "GL_ARB_occlusion_query")) strcat(ext, " GL_NV_occlusion_query");
#ifdef LB_IOS
            // Sega Race TV calls it for its lens flare whether listed or not; on
            // OpenGL ES it draws unconditionally (h_glBeginConditionalRenderNVX)
            strcat(ext, " GL_NVX_conditional_render");
#else
            if (strstr(s, "GL_NV_conditional_render")) strcat(ext, " GL_NVX_conditional_render");
#endif
        }
        gl_strings[k] = hostdata_alloc((uint32_t)(strlen(s) + strlen(ext)) + 1);
        strcpy(G(gl_strings[k]), s);
        strcat(G(gl_strings[k]), ext);
    }
    RET(gl_strings[k]);
}

static void bind_buffer(CPU *c, const char *name)
{
    typedef void (APIENTRY *fn)(GLenum, GLuint);
    static fn p, parb;
    fn *pp = strstr(name, "ARB") ? &parb : &p;
    if (!*pp) *pp = (fn)gl_host_proc(name);
    if (ARG(0) == GL_ARRAY_BUFFER) array_buffer = ARG(1);
    if (ARG(0) == GL_ELEMENT_ARRAY_BUFFER) element_buffer = ARG(1);
    (*pp)(ARG(0), ARG(1));
}
static void h_glBindBuffer(CPU *c) { bind_buffer(c, "glBindBuffer"); }
static void h_glBindBufferARB(CPU *c) { bind_buffer(c, "glBindBufferARB"); }

char *glsl_fix(const char *src, int fragment, const char *missing);

// the game's own source of each shader, kept so a program that fails to link can
// have its fragment shader rebuilt; and the shaders attached to each program
#define MAX_SHADERS 4096
static struct { char *src; int fragment; int arb; } shaders[MAX_SHADERS];
static struct { GLuint prog, sh[4]; } attached[1024];

// glsl_fix's output by its input, kept across runs: HotD4's shaders are 67 KB
// of source each, many shader objects share one, and fixing one took 3-8 ms on
// the iPad (a 25 ms hitch as attract mode started). The key covers the
// source, the stage, the varyings left out and this build (a new build starts
// a new cache); the file is in the user's caches folder, so the system may
// clear it. Past its size (below) it is started again when a game starts. On
// Linux and the Mac too: there the fixed text is as long as the source, but
// HotD4's first frames spent 6 ms a frame making it (glShaderSourceARB,
// Linux); its attract mode alone is 63 MB of it there (glsl_pp makes it a
// tenth of that on iOS).
#define GLSL_CACHE 2048
#ifdef LB_IOS
#define GLSL_CACHE_MAX (64 << 20)
#else
#define GLSL_CACHE_MAX (256 << 20)
#endif
static struct { uint64_t key; char *text; } glsl_cache[GLSL_CACHE];
static int glsl_cache_loaded;
static char glsl_cache_path[1100];

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void glsl_cache_put(uint64_t key, const char *text)
{
    for (unsigned i = 0, at = (unsigned)key % GLSL_CACHE; i < GLSL_CACHE; i++, at = (at + 1) % GLSL_CACHE)
        if (!glsl_cache[at].text || glsl_cache[at].key == key) {
            if (glsl_cache[at].text) return;
            glsl_cache[at].key = key;
            glsl_cache[at].text = strdup(text);
            return;
        }
}

static const char *glsl_cache_get(uint64_t key)
{
    for (unsigned i = 0, at = (unsigned)key % GLSL_CACHE; i < GLSL_CACHE && glsl_cache[at].text; i++, at = (at + 1) % GLSL_CACHE)
        if (glsl_cache[at].key == key) return glsl_cache[at].text;
    return NULL;
}

// records: the key, the text's length, the text
static void glsl_cache_load(void)
{
    glsl_cache_loaded = 1;
    const char *home = getenv("HOME");
#ifdef __APPLE__
    if (!home) return;
    snprintf(glsl_cache_path, sizeof glsl_cache_path, "%s/Library/Caches/lindbergh-glsl.bin", home);
#else
    // $XDG_CACHE_HOME, or ~/.cache
    const char *xdg = getenv("XDG_CACHE_HOME");
    char dir[1024];
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s", xdg);
    else if (home) snprintf(dir, sizeof dir, "%s/.cache", home);
    else return;
    mkdir(dir, 0755);
    snprintf(glsl_cache_path, sizeof glsl_cache_path, "%s/lindbergh-glsl.bin", dir);
#endif
    // past its size (builds come and go) it starts again
    struct stat st;
    if (stat(glsl_cache_path, &st) == 0 && st.st_size > GLSL_CACHE_MAX) remove(glsl_cache_path);
    FILE *f = fopen(glsl_cache_path, "rb");
    if (!f) return;
    uint64_t key;
    uint32_t n;
    while (fread(&key, sizeof key, 1, f) == 1 && fread(&n, sizeof n, 1, f) == 1 && n < (1u << 24)) {
        char *t = malloc(n + 1);
        if (fread(t, 1, n, f) != n) { free(t); break; }
        t[n] = 0;
        glsl_cache_put(key, t);
        free(t);
    }
    fclose(f);
}

static void glsl_cache_save(uint64_t key, const char *text)
{
    if (!*glsl_cache_path) return;
    FILE *f = fopen(glsl_cache_path, "ab");
    if (!f) return;
    uint32_t n = (uint32_t)strlen(text);
    fwrite(&key, sizeof key, 1, f);
    fwrite(&n, sizeof n, 1, f);
    fwrite(text, 1, n, f);
    fclose(f);
}

static char *glsl_fix_cached(const char *src, int fragment, const char *missing)
{
    // the build of this file and of glsl_fix.c's (a change to that alone
    // kept its old text: Race TV's shaders, cut short, from the cache)
    static const char build[] = __DATE__ " " __TIME__;
    extern const char glsl_fix_build[];
    if (!glsl_cache_loaded) glsl_cache_load();
    uint64_t key = fnv64(14695981039346656037ull, build, sizeof build);
    key = fnv64(key, glsl_fix_build, strlen(glsl_fix_build));
    key = fnv64(key, &fragment, sizeof fragment);
    key = fnv64(key, missing ? missing : "", missing ? strlen(missing) + 1 : 1);
    key = fnv64(key, src, strlen(src));
    const char *hit = glsl_cache_get(key);
    if (hit) return strdup(hit);
    char *fixed = glsl_fix(src, fragment, missing);
    glsl_cache_put(key, fixed);
    glsl_cache_save(key, fixed);
    return fixed;
}

static void set_shader_source(GLuint sh, const char *src, int fragment, int arb, const char *missing)
{
    typedef void (APIENTRY *fn)(GLuint, GLsizei, const GLchar **, const GLint *);
    fn p = (fn)gl_host_proc(arb ? "glShaderSourceARB" : "glShaderSource");
    char *fixed = glsl_fix_cached(src, fragment, missing);
    if (af_debug_env("LB_SHADER_DUMP")) {
        static int seq;
        char path[64];
        snprintf(path, sizeof path, "shader_%03d.glsl", seq++);
        FILE *f = fopen(path, "w");
        if (f) { fputs(fixed, f); fclose(f); }
    }
    const GLchar *one = fixed;
    p(sh, 1, &one, NULL);
    free(fixed);
}

static void shader_source(CPU *c, const char *name)
{
    // join the pieces, then rewrite NVIDIA-only GLSL for the host compiler
    int n = (int)ARG(1);
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        int32_t len = ARG(3) ? (int32_t)rd32(ARG(3) + 4 * i) : -1;
        total += len < 0 ? strlen(G(rd32(ARG(2) + 4 * i))) : (size_t)len;
    }
    char *all = malloc(total + 1), *q = all;
    for (int i = 0; i < n; i++) {
        const char *s = G(rd32(ARG(2) + 4 * i));
        int32_t len = ARG(3) ? (int32_t)rd32(ARG(3) + 4 * i) : -1;
        size_t l = len < 0 ? strlen(s) : (size_t)len;
        memcpy(q, s, l);
        q += l;
    }
    *q = 0;
    typedef void (APIENTRY *ivfn)(GLuint, GLenum, GLint *);
    ivfn iv = (ivfn)gl_host_proc(strstr(name, "ARB") ? "glGetObjectParameterivARB" : "glGetShaderiv");
    GLint type = 0;
    iv(ARG(0), 0x8b4f, &type);    // GL_SHADER_TYPE / GL_OBJECT_SUBTYPE_ARB
    int arb = strstr(name, "ARB") != NULL;
    set_shader_source(ARG(0), all, type == GL_FRAGMENT_SHADER, arb, NULL);
    if (ARG(0) < MAX_SHADERS) {
        free(shaders[ARG(0)].src);
        shaders[ARG(0)].src = all;
        shaders[ARG(0)].fragment = type == GL_FRAGMENT_SHADER;
        shaders[ARG(0)].arb = arb;
    } else free(all);
}
static void attach(CPU *c, const char *name)
{
    typedef void (APIENTRY *fn)(GLuint, GLuint);
    ((fn)gl_host_proc(name))(ARG(0), ARG(1));
    GLuint prog = ARG(0);
    int slot = -1;
    for (int i = 0; i < 1024; i++) {
        if (attached[i].prog == prog) { slot = i; break; }
        if (slot < 0 && !attached[i].prog) slot = i;
    }
    if (slot < 0) slot = (int)(prog % 1024);
    if (attached[slot].prog != prog) memset(&attached[slot], 0, sizeof attached[slot]);
    attached[slot].prog = prog;
    for (int k = 0; k < 4; k++)
        if (!attached[slot].sh[k] || attached[slot].sh[k] == ARG(1)) { attached[slot].sh[k] = ARG(1); break; }
}
static void h_glAttachObjectARB(CPU *c) { attach(c, "glAttachObjectARB"); }
static void h_glAttachShader(CPU *c) { attach(c, "glAttachShader"); }

#ifdef LB_IOS
char *glsl_float_literals(const char *src, const char *log);   // glsl_pp.c
#endif

// compile, and report a shader the host rejects with its source and log
static void compile_shader(CPU *c, int arb)
{
    typedef void (APIENTRY *compfn)(GLuint);
    typedef void (APIENTRY *ivfn)(GLuint, GLenum, GLint *);
    typedef void (APIENTRY *logfn)(GLuint, GLsizei, GLsizei *, GLchar *);
    compfn cp = (compfn)gl_host_proc(arb ? "glCompileShaderARB" : "glCompileShader");
    ivfn iv = (ivfn)gl_host_proc(arb ? "glGetObjectParameterivARB" : "glGetShaderiv");
    logfn lg = (logfn)gl_host_proc(arb ? "glGetInfoLogARB" : "glGetShaderInfoLog");
    logfn src = (logfn)gl_host_proc(arb ? "glGetShaderSourceARB" : "glGetShaderSource");
    GLuint sh = ARG(0);
    cp(sh);
    GLint ok = 1;
    iv(sh, GL_COMPILE_STATUS, &ok);
#ifdef LB_IOS
    // GLSL ES has no implicit int -> float (vec2 * 2): the lines the compiler
    // names get their integer constants as floats, and the shader compiles again
    typedef void (APIENTRY *srcfn)(GLuint, GLsizei, const GLchar **, const GLint *);
    srcfn set = (srcfn)gl_host_proc(arb ? "glShaderSourceARB" : "glShaderSource");
    for (int attempt = 0; !ok && attempt < 4; attempt++) {
        char *log = malloc(1 << 16), *text = malloc(1 << 20);
        lg(sh, 1 << 16, NULL, log);
        src(sh, 1 << 20, NULL, text);
        char *fixed = glsl_float_literals(text, log);
        free(log);
        free(text);
        if (!fixed) break;
        const GLchar *one = fixed;
        set(sh, 1, &one, NULL);
        free(fixed);
        cp(sh);
        iv(sh, GL_COMPILE_STATUS, &ok);
    }
#endif
    if (ok) return;
    static int failures;
    char *buf = malloc(1 << 20);
    lg(sh, 1 << 16, NULL, buf);
    hle_log("shader %u failed to compile:\n%s\n", sh, buf);
    // the first error's line (numbered from the #line 1 after the helpers)
    int line = 0;
    const char *e = strstr(buf, "0:");
    if (e) line = atoi(e + 2);
    char *text = malloc(1 << 20);
    src(sh, 1 << 20, NULL, text);
    const char *p = strstr(text, "#line 1\n");
    p = p ? p + 8 : text;
    for (int k = 1; p && k < line; k++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    if (line && p) hle_log("  line %d: %.*s\n", line, (int)strcspn(p, "\n"), p);
    free(text);
    if (failures++ < 20) {
        char path[64];
        snprintf(path, sizeof path, "shader_fail_%02d.glsl", failures);
        src(sh, 1 << 20, NULL, buf);
        FILE *f = fopen(path, "w");
        if (f) { fputs(buf, f); fclose(f); }
    }
    free(buf);
}
#ifdef LB_IOS
static void rect_forget(GLuint prog);

// the names of the varyings a shader declares, added to the space-separated list
static void varying_names(const char *src, char *list, size_t size)
{
    for (const char *p = src; (p = strstr(p, "varying")); p += 7) {
        if (p > src && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        const char *e = strchr(p, ';');
        if (!e) break;
        // the last identifier before ';' (or before '[')
        const char *b = memchr(p, '[', (size_t)(e - p));
        const char *end = b ? b : e;
        while (end > p && isspace((unsigned char)end[-1])) end--;
        const char *a = end;
        while (a > p && (isalnum((unsigned char)a[-1]) || a[-1] == '_')) a--;
        size_t n = (size_t)(end - a), used = strlen(list);
        if (n && used + n + 2 < size) snprintf(list + used, size - used, "%s%.*s", used ? " " : "", (int)n, a);
    }
}
#endif
static void link_program(CPU *c, int arb)
{
    typedef void (APIENTRY *linkfn)(GLuint);
    typedef void (APIENTRY *ivfn)(GLuint, GLenum, GLint *);
    typedef void (APIENTRY *logfn)(GLuint, GLsizei, GLsizei *, GLchar *);
    linkfn lk = (linkfn)gl_host_proc(arb ? "glLinkProgramARB" : "glLinkProgram");
    ivfn iv = (ivfn)gl_host_proc(arb ? "glGetObjectParameterivARB" : "glGetProgramiv");
    logfn lg = (logfn)gl_host_proc(arb ? "glGetInfoLogARB" : "glGetProgramInfoLog");
    typedef void (APIENTRY *compfn)(GLuint);
    compfn cp = (compfn)gl_host_proc(arb ? "glCompileShaderARB" : "glCompileShader");
    GLuint prog = ARG(0);
#ifdef LB_IOS
    rect_forget(prog);
#endif
    char buf[4096], missing[1024] = "";
#ifdef LB_IOS
    // A program with a fragment shader only: on the desktop its link fails over
    // the varyings no vertex shader writes and is retried without them (below);
    // gl4es links it with its own vertex shader at the first draw instead, so
    // the varyings it declares are taken out before the first link.
    int has_vertex = 0, has_fragment = 0;
    for (int i = 0; i < 1024; i++)
        if (attached[i].prog == prog)
            for (int k = 0; k < 4; k++) {
                GLuint sh = attached[i].sh[k];
                if (!sh || sh >= MAX_SHADERS || !shaders[sh].src) continue;
                if (shaders[sh].fragment) has_fragment = 1;
                else has_vertex = 1;
            }
    if (has_fragment && !has_vertex) {
        for (int i = 0; i < 1024; i++)
            if (attached[i].prog == prog)
                for (int k = 0; k < 4; k++) {
                    GLuint sh = attached[i].sh[k];
                    if (sh && sh < MAX_SHADERS && shaders[sh].src && shaders[sh].fragment)
                        varying_names(shaders[sh].src, missing, sizeof missing);
                }
        if (*missing)
            for (int i = 0; i < 1024; i++)
                if (attached[i].prog == prog)
                    for (int k = 0; k < 4; k++) {
                        GLuint sh = attached[i].sh[k];
                        if (sh && sh < MAX_SHADERS && shaders[sh].src && shaders[sh].fragment) {
                            set_shader_source(sh, shaders[sh].src, 1, shaders[sh].arb, missing);
                            cp(sh);
                        }
                    }
    }
#endif
    for (int attempt = 0; attempt < 16; attempt++) {
        lk(prog);
        GLint ok = 1;
        iv(prog, GL_LINK_STATUS, &ok);
        if (ok) {
#ifdef LB_IOS
            // gl4es can leave an error behind from the work around a link; HotD4
            // checks glGetError after linking and takes it for a failed link
            while (glGetError() != GL_NO_ERROR) {}
#endif
            return;
        }
        lg(prog, sizeof buf, NULL, buf);
        // Mesa: "fragment shader varying NAME not written by vertex shader"
        // Apple: "Input of fragment shader 'NAME' not written by vertex shader"
        int added = 0;
        for (const char *p = buf; (p = strstr(p, " not written")); p += 12) {
            const char *e = p > buf && p[-1] == '\'' ? p - 1 : p, *a = e;
            while (a > buf && (a[-1] == '_' || (a[-1] >= '0' && a[-1] <= '9') || (a[-1] >= 'a' && a[-1] <= 'z') ||
                               (a[-1] >= 'A' && a[-1] <= 'Z')))
                a--;
            if (e == a) continue;
            // a name already in the list is no progress
            int dup = 0;
            for (const char *q = missing; *q && !dup; q += strcspn(q, " "), q += *q == ' ')
                dup = strcspn(q, " ") == (size_t)(e - a) && !strncmp(q, a, (size_t)(e - a));
            if (dup) continue;
            size_t used = strlen(missing);
            if (used + (size_t)(e - a) + 2 < sizeof missing) {
                snprintf(missing + used, sizeof missing - used, "%s%.*s", used ? " " : "", (int)(e - a), a);
                added = 1;
            }
        }
        if (!added) break;
        if (hle_verbose) hle_log("program %u: relinking without inputs %s\n", prog, missing);
        if (af_debug_env("LB_SHADER_DUMP") && !attempt)
            for (int i = 0; i < 1024; i++)
                if (attached[i].prog == prog)
                    for (int k = 0; k < 4; k++) {
                        GLuint sh = attached[i].sh[k];
                        if (!sh || sh >= MAX_SHADERS || !shaders[sh].src) continue;
                        char path[64];
                        snprintf(path, sizeof path, "relink_%u_%u.glsl", prog, sh);
                        FILE *f = fopen(path, "w");
                        if (f) { fputs(shaders[sh].src, f); fclose(f); }
                    }
        for (int i = 0; i < 1024; i++)
            if (attached[i].prog == prog)
                for (int k = 0; k < 4; k++) {
                    GLuint sh = attached[i].sh[k];
                    if (sh && sh < MAX_SHADERS && shaders[sh].src && shaders[sh].fragment) {
                        set_shader_source(sh, shaders[sh].src, 1, shaders[sh].arb, missing);
                        cp(sh);
                    }
                }
    }
    static int failures;
    if (failures++ > 50) return;
    hle_log("program %u failed to link:\n%s\n", prog, buf);
}
// NVIDIA aliases the conventional vertex inputs onto generic attributes
// (position 0, weight 1, normal 2, colour 3 and 4, fog 5, texcoord n 8+n),
// and Let's Go Jungle feeds every input with glVertexAttribPointer: other
// hosts alias only the position. Vertex programs read the generic
// attributes instead, and the immediate-mode calls below set both.
static char *alias_vertex_inputs(const char *src, size_t n)
{
    static const struct { const char *from, *to; } map[] = {
        {"vertex.position", "vertex.attrib[0]"}, {"vertex.weight", "vertex.attrib[1]"},
        {"vertex.normal", "vertex.attrib[2]"}, {"vertex.color.secondary", "vertex.attrib[4]"},
        {"vertex.color.primary", "vertex.attrib[3]"}, {"vertex.color", "vertex.attrib[3]"},
        {"vertex.fogcoord", "vertex.attrib[5]"},
    };
    char *out = malloc(n * 2 + 64), *o = out;
    for (size_t i = 0; i < n;) {
        int done = 0;
        if (!strncmp(src + i, "vertex.texcoord", 15) && i + 15 <= n) {
            int unit = 0;
            size_t j = i + 15;
            if (j < n && src[j] == '[') {
                unit = atoi(src + j + 1);
                while (j < n && src[j] != ']') j++;
                j++;
            }
            o += sprintf(o, "vertex.attrib[%d]", 8 + unit);
            i = j;
            done = 1;
        }
        for (size_t k = 0; !done && k < sizeof map / sizeof map[0]; k++) {
            size_t l = strlen(map[k].from);
            if (i + l <= n && !strncmp(src + i, map[k].from, l)) {
                o += sprintf(o, "%s", map[k].to);
                i += l;
                done = 1;
            }
        }
        if (!done) *o++ = src[i++];
    }
    *o = 0;
    return out;
}

// NV_vertex_program3's address register has four components; Apple's driver
// ignores the component in c[A0.y + n] (it reads as index 0), so Let's Go
// Jungle's 64-bone skinning took its second bone from the wrong matrices.
// A0.y/z/w live in a temporary instead, and each instruction that indexes
// with one loads it into a second address register's x just before.
// (A vertex program indexes one parameter array per instruction.)
// ARB_vertex_program has one address register with one component, so on
// OpenGL ES (gl4es) and Mesa a program using A0.y/z/w or a second register
// (Let's Go Jungle's skinning has A0.xyzw and A1) is refused: "address
// registers limit exceeded", and what it draws (the characters' heads) was
// missing. Every address value is kept in a temporary instead (A0's in lbAI,
// A1's in lbAJ), and each instruction that indexes loads the one it uses into
// A0.x first. Mesa also refuses offsets past ARB's -64..63 (Let's Go Jungle's
// c[A0.x + 199]): those are added to the address before it is loaded.
static int far_offset(const char *src)
{
    for (const char *p = src; (p = strstr(p, "A")); p++)
        if ((p[1] == '0' || p[1] == '1') && p[2] == '.' && p[3] && strchr("xyzw", p[3])) {
            const char *q = p + 4;
            while (*q == ' ') q++;
            if ((*q == '+' || *q == '-') && atoi(q + 1) > (*q == '-' ? 64 : 63)) return 1;
        }
    return 0;
}

static char *one_address_register(char *src)
{
    if (!strstr(src, "OPTION NV_vertex_program") ||
        !(strstr(src, "A0.y") || strstr(src, "A0.z") || strstr(src, "A0.w") || strstr(src, "ADDRESS A1") ||
          far_offset(src)))
        return src;
    size_t n = strlen(src);
    char *out = malloc(n * 3 + 256), *o = out;
    for (char *line = src; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf ? len : sizeof buf - 1), line);
        line += len;
        char *op = buf + strspn(buf, " \t");
        if (strstr(buf, "ADDRESS A0;")) {
            o += sprintf(o, "%sTEMP lbAI, lbAJ, lbAO;\n", buf);
        } else if (strstr(buf, "ADDRESS A1;")) {
            continue;
        } else if (!strncmp(op, "ARL", 3) && (op[3] == ' ' || op[3] == '\t')) {
            // ARL A0.yz, R1: lbAI.y from R1.y, lbAI.z from R1.z (a one-component swizzle for each)
            char dst[64] = "", reg[128] = "";
            if (sscanf(op + 3, " %63[^, ] , %127[^;]", dst, reg) != 2) { o += sprintf(o, "%s", buf); continue; }
            const char *tmp = dst[1] == '1' ? "lbAJ" : "lbAI";
            const char *mask = strchr(dst, '.') ? strchr(dst, '.') + 1 : dst[1] == '1' ? "x" : "xyzw";
            char *dot = strrchr(reg, '.'), sw[8] = "";
            if (dot && !strchr(dot, ']')) { snprintf(sw, sizeof sw, "%s", dot + 1); *dot = 0; }
            for (const char *m = mask; *m && strchr("xyzw", *m); m++) {
                int k = *m == 'x' ? 0 : *m == 'y' ? 1 : *m == 'z' ? 2 : 3;
                size_t l = strlen(sw);
                char c = !l ? *m : l == 1 ? sw[0] : sw[k < (int)l ? k : (int)l - 1];
                o += sprintf(o, "MOV %s.%c, %s.%c;\n", tmp, *m, reg, c);
            }
        } else {
            // the address it indexes with, loaded into A0.x
            char *a = NULL;
            for (char *q = buf; *q && !a; q++)
                if ((q[0] == 'A' && (q[1] == '0' || q[1] == '1') && q[2] == '.' && strchr("xyzw", q[3])) && (q == buf || !isalnum((unsigned char)q[-1]))) a = q;
            if (!a) { o += sprintf(o, "%s", buf); continue; }
            const char *tmp = a[1] == '1' ? "lbAJ" : "lbAI";
            char *sign = a + 4;
            while (*sign == ' ') sign++;
            int off = *sign == '+' || *sign == '-' ? atoi(sign + 1) * (*sign == '-' ? -1 : 1) : 0;
            char *close = strchr(a, ']');
            if ((off < -64 || off > 63) && close) {
                o += sprintf(o, "ADD lbAO.x, %s.%c, {%d};\nARL A0.x, lbAO.x;\n", tmp, a[3], off);
                *a = 0;
                o += sprintf(o, "%sA0.x%s", buf, close);
                continue;
            }
            o += sprintf(o, "ARL A0.x, %s.%c;\n", tmp, a[3]);
            for (char *q = buf; *q; q++) {
                if (q == a) { o += sprintf(o, "A0.x"); q += 3; }
                else *o++ = *q;
            }
        }
    }
    *o = 0;
    free(src);
    if (hle_verbose) hle_log("vertex program: its address registers kept in temporaries\n");
    return out;
}

static char *split_address_components(char *src)
{
#ifdef LB_IOS   // gl4es has A0.x only
    return one_address_register(src);
#endif
    if (!host_has_nv_options()) return one_address_register(src);   // and so has Mesa's ARB
    if (!strstr(src, "OPTION NV_vertex_program") || strstr(src, "ADDRESS A1") ||
        !(strstr(src, "A0.y") || strstr(src, "A0.z") || strstr(src, "A0.w")))
        return src;
    size_t n = strlen(src);
    char *out = malloc(n * 3 + 256), *o = out;
    for (char *line = src; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(len < sizeof buf ? len : sizeof buf - 1), line);
        char *arl = strstr(buf, "ARL"), *a0 = strstr(buf, "A0."), *decl = strstr(buf, "ADDRESS A0;");
        if (decl) {
            o += sprintf(o, "%sADDRESS lbA1;\nTEMP lbAI, lbAO;\n", buf);
        } else if (arl && a0 && a0 > arl && strchr("yzw", a0[3])) {
            // ARL A0.k, src: keep the value of src's k-th component
            int k = a0[3] == 'y' ? 1 : a0[3] == 'z' ? 2 : 3;
            char *comma = strchr(a0, ','), reg[64] = "";
            if (comma) sscanf(comma + 1, " %63[^;]", reg);
            char *dot = strchr(reg, '.');
            char comp = "xyzw"[k];
            if (dot) {
                size_t sw = strlen(dot + 1);
                comp = sw == 1 ? dot[1] : sw >= 4 ? dot[1 + k] : comp;
                *dot = 0;
            }
            o += sprintf(o, "MOV lbAI.%c, %s.%c;\n", "xyzw"[k], reg, comp);
        } else if (a0 && strchr("yzw", a0[3])) {
            char k = a0[3];
            // an offset past ARB's -64..63 (vp40 allows more: Sega Race TV's
            // skinning reads c[A0.y + 96], and Mesa refused the program, its
            // scenery missing) is added to the address instead
            char *sign = a0 + 4;
            while (*sign == ' ') sign++;
            int off = *sign == '+' || *sign == '-' ? atoi(sign + 1) * (*sign == '-' ? -1 : 1) : 0;
            char *close = strchr(a0, ']');
            if ((off < -64 || off > 63) && close) {
                o += sprintf(o, "ADD lbAO.x, lbAI.%c, {%d};\nARL lbA1.x, lbAO.x;\n", k, off);
                *a0 = 0;
                o += sprintf(o, "%slbA1.x%s", buf, close);
            } else {
                o += sprintf(o, "ARL lbA1.x, lbAI.%c;\n", k);
                for (char *q = buf; *q; q++) {
                    if (!strncmp(q, "A0.", 3) && q[3] == k) {
                        o += sprintf(o, "lbA1.x");
                        q += 3;
                    } else *o++ = *q;
                }
            }
        } else {
            o += sprintf(o, "%s", buf);
        }
        line += len;
    }
    *o = 0;
    free(src);
    if (hle_verbose) hle_log("vertex program: A0.y/z/w moved to a second address register\n");
    return out;
}

#ifdef LB_IOS
char *arb_lower_nv(char *src);
char *arb_matrix_rows(char *src);
char *arb_inline_constants(char *src);
char *arb_address_offsets(char *src);
int arb_fog_env(char *src);
char *arb_shadow_lookups(char *src);
char *arb_program_matrices(char *src);
char *arb_rect_lookups(char *src);
char *arb_output_aliases(char *src);

static GLint rejected_pos = -1;
static char rejected_msg[256];

// a program gl4es rejected is given one that draws nothing, as a failed
// program does on the desktop (gl4es crashes drawing with a program whose
// source failed); the error position is kept for the report
static void arb_fallback(void (APIENTRY *p)(GLenum, GLenum, GLsizei, const void *), GLenum target, GLenum format)
{
    GLint pos = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
    if (pos < 0) return;
    const char *msg = (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
    snprintf(rejected_msg, sizeof rejected_msg, "%s", msg ? msg : "");
    // drawing with a program that failed draws nothing on the desktop: these
    // put every vertex outside the view and discard every fragment
    const char *plain = target == GL_VERTEX_PROGRAM_ARB
        ? "!!ARBvp1.0\nPARAM away = { 0, 0, 2, 1 };\nMOV result.position, away;\nEND\n"   // not "out": GLSL's
        : "!!ARBfp1.0\nPARAM no = { -1, -1, -1, -1 };\nKIL no;\nMOV result.color, fragment.color;\nEND\n";
    p(target, format, (GLsizei)strlen(plain), plain);
    rejected_pos = pos;
}

// the fog state as ARB programs see it, in environment parameters 22 and 23
// (arb_lower.c, arb_fog_env), for both program types
static void fog_env_update(void)
{
    typedef void (APIENTRY *envfn)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
    static envfn env;
    if (!env) env = (envfn)gl_host_proc("glProgramEnvParameter4fARB");
    if (!env) return;
    GLfloat color[4] = {0}, density = 1, start = 0, end = 1;
    glGetFloatv(GL_FOG_COLOR, color);
    glGetFloatv(GL_FOG_DENSITY, &density);
    glGetFloatv(GL_FOG_START, &start);
    glGetFloatv(GL_FOG_END, &end);
    GLfloat scale = end != start ? 1.0f / (end - start) : 0.0f;
    for (int t = 0; t < 2; t++) {
        GLenum target = t ? GL_FRAGMENT_PROGRAM_ARB : GL_VERTEX_PROGRAM_ARB;
        env(target, 22, color[0], color[1], color[2], color[3]);
        env(target, 23, density, start, end, scale);
    }
}
static void fog_call(CPU *c, const char *name)
{
    hle_fn f = NULL;
    for (const hle_entry *e = gl_gen_table; e->name && !f; e++)
        if (!strcmp(e->name, name)) f = e->fn;
    if (f) f(c);
    fog_env_update();
}
static void h_glFogf(CPU *c) { fog_call(c, "glFogf"); }
static void h_glFogfv(CPU *c) { fog_call(c, "glFogfv"); }
static void h_glFogi(CPU *c) { fog_call(c, "glFogi"); }
static void h_glFogiv(CPU *c) { fog_call(c, "glFogiv"); }
#endif

#if defined(__APPLE__) && !defined(LB_IOS)
char *arb_drop_back_colors(char *src);
static int two_side_on;
#define APPLE_BACK_COLORS(src) (two_side_on ? (src) : arb_drop_back_colors(src))
#else
#define APPLE_BACK_COLORS(src) (src)
#endif
#ifdef LB_IOS
char *arb_program_matrices(char *src);
char *arb_output_aliases(char *src);
// a vertex program's text as the host takes it (after arb_lower_nv)
#define HOST_VP_TEXT(src) arb_output_aliases(arb_program_matrices(arb_address_offsets(arb_matrix_rows(arb_inline_constants(src)))))
#else
#define HOST_VP_TEXT(src) APPLE_BACK_COLORS(src)
#endif
// Ghost Squad Evolution's lights loop over the lights of each kind
// (arb_lower.c, inline_calls), and unrolled for eight of each its programs
// ran in software on Apple's GL: 16 frames/s in a lit room. A vertex program
// with such loops is kept as written, and the program bound in its place is
// one unrolled for the counts in the environment parameters at the time
// (four point, two spot, seven directional lights at most, eight in all),
// made when first needed; it changes with the program bound or the counts.
char *arb_lower_counts(char *src, const int *counts, int n);
extern int arb_loop_env[8], arb_loop_comp[8], arb_nloops;
#define LOOP_PROGRAMS 1024   // Ghost Squad Evolution has a few hundred (past the first 64 its game froze: Metal never finished the eight-pass one)
static struct {
    GLuint id;
    char *src;   // before arb_lower.c
    int nloops, env[8], comp[8];
    struct { uint32_t key; GLuint id; } var[48];
    int nvar;
} loops[LOOP_PROGRAMS];
static int nloop_programs;
static float vp_env[256][4];   // the vertex programs' environment parameters
static GLuint vp_bound;        // the game's vertex program

static int loop_program(GLuint id)
{
    for (int i = 0; i < nloop_programs; i++)
        if (loops[i].id == id) return i;
    return -1;
}

// the program to bind for the game's program `id`
static GLuint vp_for(GLuint id)
{
    int i = loop_program(id);
    if (i < 0) return id;
    int counts[8];
    uint32_t key = 0;
    for (int k = 0; k < loops[i].nloops; k++) {
        float v = vp_env[loops[i].env[k]][loops[i].comp[k]];
        counts[k] = v <= 0 ? 0 : v >= 8 ? 8 : (int)ceilf(v);
        key = key * 9 + (uint32_t)counts[k];
    }
    for (int v = 0; v < loops[i].nvar; v++)
        if (loops[i].var[v].key == key) return loops[i].var[v].id;
    if (loops[i].nvar == 48) return id;   // the program for eight of each
    typedef void (APIENTRY *genfn)(GLsizei, GLuint *);
    typedef void (APIENTRY *bindfn)(GLenum, GLuint);
    typedef void (APIENTRY *strfn)(GLenum, GLenum, GLsizei, const void *);
    static genfn gen;
    static bindfn bind;
    static strfn str;
    if (!gen) {
        gen = (genfn)gl_host_proc("glGenProgramsARB");
        bind = (bindfn)gl_host_proc("glBindProgramARB");
        str = (strfn)gl_host_proc("glProgramStringARB");
    }
    GLuint var = 0;
    gen(1, &var);
    char *text = HOST_VP_TEXT(arb_lower_counts(strdup(loops[i].src), counts, loops[i].nloops));
    bind(GL_VERTEX_PROGRAM_ARB, var);
    str(GL_VERTEX_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(text), text);
    free(text);
    if (hle_verbose) hle_log("loops: program %u variant %u counts %d %d %d (%d loops) len %zu\n", id, var, counts[0], counts[1], counts[2], loops[i].nloops, strlen(loops[i].src));
    loops[i].var[loops[i].nvar].key = key;
    loops[i].var[loops[i].nvar++].id = var;
    return var;
}

static void bind_vp(void)
{
    typedef void (APIENTRY *bindfn)(GLenum, GLuint);
    static bindfn bind;
    if (!bind) bind = (bindfn)gl_host_proc("glBindProgramARB");
    bind(GL_VERTEX_PROGRAM_ARB, vp_for(vp_bound));
}

static void h_glBindProgramARB(CPU *c)
{
    typedef void (APIENTRY *fn)(GLenum, GLuint);
    static fn p;
    if (!gl_open) RET(0);   // before the window, as the generated calls (Let's Go Jungle Special's shader setup)
    if (!p) p = (fn)gl_host_proc("glBindProgramARB");
    if (!p) RET(0);
    if (gl_trace) gl_trace_call("glBindProgramARB", c);
    if (ARG(0) != GL_VERTEX_PROGRAM_ARB) {
        p(ARG(0), ARG(1));
        return;
    }
    vp_bound = ARG(1);
    bind_vp();
}

static void h_glProgramEnvParameter4fvARB(CPU *c)
{
    typedef void (APIENTRY *fn)(GLenum, GLuint, const GLfloat *);
    static fn p;
    if (!gl_open) RET(0);
    if (!p) p = (fn)gl_host_proc("glProgramEnvParameter4fvARB");
    if (!p) RET(0);
    if (gl_trace) gl_trace_call("glProgramEnvParameter4fvARB", c);
    const GLfloat *v = (const GLfloat *)G(ARG(2));
    p(ARG(0), ARG(1), v);
    if (ARG(0) != GL_VERTEX_PROGRAM_ARB || ARG(1) >= 256 || !memcmp(vp_env[ARG(1)], v, 16)) return;
    memcpy(vp_env[ARG(1)], v, 16);
    int i = loop_program(vp_bound);
    for (int k = 0; i >= 0 && k < loops[i].nloops; k++)
        if (loops[i].env[k] == (int)ARG(1)) { bind_vp(); break; }
}

// after the game's program `id` is given its text (`src`, not yet lowered)
static void vp_loops_check(GLuint id, const char *src)
{
    int i = loop_program(id);
    if (i >= 0) {   // a new text for it: its old variants go
        typedef void (APIENTRY *delfn)(GLsizei, const GLuint *);
        delfn del = (delfn)gl_host_proc("glDeleteProgramsARB");
        for (int v = 0; v < loops[i].nvar; v++) del(1, &loops[i].var[v].id);
        free(loops[i].src);
        loops[i] = loops[--nloop_programs];
    }
    int known = arb_nloops > 0;
    for (int k = 0; k < arb_nloops; k++) known &= arb_loop_env[k] >= 0 && arb_loop_env[k] < 256;
    if (!known || nloop_programs == LOOP_PROGRAMS) return;
    if (hle_verbose) hle_log("loops: program %u has %d loops env %d.%d\n", id, arb_nloops, arb_loop_env[0], arb_loop_comp[0]);
    i = nloop_programs++;
    memset(&loops[i], 0, sizeof loops[i]);
    loops[i].id = id;
    loops[i].src = strdup(src);
    loops[i].nloops = arb_nloops;
    memcpy(loops[i].env, arb_loop_env, sizeof arb_loop_env);
    memcpy(loops[i].comp, arb_loop_comp, sizeof arb_loop_comp);
}

// ARB assembly programs (Cg's arbvp1/arbfp1 output): report the ones the host rejects
static void h_glProgramStringARB(CPU *c)
{
    typedef void (APIENTRY *fn)(GLenum, GLenum, GLsizei, const void *);
    static fn p;
    if (!gl_open) RET(0);
    if (!p) p = (fn)gl_host_proc("glProgramStringARB");
    if (!p) RET(0);
    if (gl_trace) gl_trace_call("glProgramStringARB", c);
    if (ARG(0) == 0x8870) wr32(c->r[ESP] + 4, GL_FRAGMENT_PROGRAM_ARB);   // NV_fragment_program's target
    char *src = NULL;
    if (ARG(0) == GL_VERTEX_PROGRAM_ARB) src = split_address_components(alias_vertex_inputs(G(ARG(3)), ARG(2)));
    else if (ARG(2) > 4 && !strncmp(G(ARG(3)), "!!FP", 4)) src = nvfp_to_arb(G(ARG(3)), ARG(2));   // NVIDIA's own syntax
    else {
        src = malloc(ARG(2) + 1);
        memcpy(src, G(ARG(3)), ARG(2));
        src[ARG(2)] = 0;
    }
#ifdef LB_IOS
    char *as_written = NULL;
    if (ARG(0) == GL_VERTEX_PROGRAM_ARB) {   // as on Apple's GL below: the game's own program gets the text
        typedef void (APIENTRY *bindfn)(GLenum, GLuint);
        ((bindfn)gl_host_proc("glBindProgramARB"))(GL_VERTEX_PROGRAM_ARB, vp_bound);
        if (strstr(src, "CAL") && strstr(src, "RET")) as_written = strdup(src);
    }
    arb_nloops = 0;
    src = arb_address_offsets(arb_matrix_rows(arb_inline_constants(arb_lower_nv(src))));   // gl4es knows ARB only (arb_lower.c)
    src = arb_output_aliases(arb_rect_lookups(arb_shadow_lookups(arb_program_matrices(src))));
    if (arb_fog_env(src)) fog_env_update();
#else
    // Apple's GL takes NVIDIA's options but not their branches on Metal (it
    // drew such a program in software, wrongly: Ghost Squad Evolution's lights)
    char *as_written = NULL;
    if (ARG(0) == GL_VERTEX_PROGRAM_ARB) {
        // the game's own program is the one the text goes to
        typedef void (APIENTRY *bindfn)(GLenum, GLuint);
        ((bindfn)gl_host_proc("glBindProgramARB"))(GL_VERTEX_PROGRAM_ARB, vp_bound);
        if (strstr(src, "CAL") && strstr(src, "RET")) as_written = strdup(src);
    }
    arb_nloops = 0;
    if (!host_has_nv_options() || strstr(src, "BRA") || strstr(src, "CAL ")) src = arb_lower_nv(src);
    if (ARG(0) == GL_VERTEX_PROGRAM_ARB) src = APPLE_BACK_COLORS(src);
#endif
    if (af_debug_env("LB_SHADER_DUMP")) {   // before the host sees it, in case it crashes on it
        static int seq;
        char path[64];
        snprintf(path, sizeof path, "arbprog_%03d.txt", seq++);
        FILE *f = fopen(path, "w");
        if (f) { fputs(src, f); fclose(f); }
    }
    p(ARG(0), ARG(1), (GLsizei)strlen(src), src);
#ifdef LB_IOS
    arb_fallback(p, ARG(0), ARG(1));
#endif
    if (ARG(0) == GL_VERTEX_PROGRAM_ARB) {
        vp_loops_check(vp_bound, as_written ? as_written : "");
        free(as_written);
        bind_vp();
    }
    GLint pos = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
#ifdef LB_IOS
    const char *err_msg = rejected_pos >= 0 ? rejected_msg : (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
    if (rejected_pos >= 0) pos = rejected_pos, rejected_pos = -1;
#else
    const char *err_msg = (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
#endif
    static int failures;
    if (pos >= 0 && failures++ < 20) {
        hle_log("ARB program rejected at %d: %s\n", pos, err_msg);
        char path[64];
        snprintf(path, sizeof path, "arbprog_fail_%02d.txt", failures);
        FILE *f = fopen(path, "w");
        if (f) { fputs(src, f); fclose(f); }
    }
    free(src);
}

// framebuffer 0 is the game's window: in a core that is the host's own
// framebuffer object (Let's Go Jungle renders through FBOs and binds 0 at the end)
static void bind_framebuffer(CPU *c, const char *name)
{
    typedef void (APIENTRY *fn)(GLenum, GLuint);
    static fn p, pext;
    fn *pp = strstr(name, "EXT") ? &pext : &p;
    if (!*pp) *pp = (fn)gl_host_proc(name);
    if (gl_trace) gl_trace_call(name, c);
    (*pp)(ARG(0), ARG(1) ? ARG(1) : host_gl_framebuffer());
}
static void h_glBindFramebufferEXT(CPU *c) { bind_framebuffer(c, "glBindFramebufferEXT"); }
static void h_glBindFramebuffer(CPU *c) { bind_framebuffer(c, "glBindFramebuffer"); }

// immediate mode: the conventional input and its NVIDIA alias
static PFNGLVERTEXATTRIB4FARBPROC attrib4f;
static void set_alias(GLuint index, float x, float y, float z, float w)
{
#ifdef LB_IOS
    // gl4es with more than 8 attributes (mtlgles has 16) lays the conventional
    // inputs out as NVIDIA does (texcoord 0 is attribute 8): the call is the
    // alias already. A glVertexAttrib between glBegin and glEnd would end the
    // primitive there (gl4es flushes it), and each of Harley-Davidson's text
    // quads came out with one texcoord: black
    (void)index, (void)x, (void)y, (void)z, (void)w;
    return;
#endif
    if (!attrib4f) attrib4f = (PFNGLVERTEXATTRIB4FARBPROC)gl_host_proc("glVertexAttrib4fARB");
    attrib4f(index, x, y, z, w);
}
static void h_glColor4ub(CPU *c)
{
    glColor4ub((GLubyte)ARG(0), (GLubyte)ARG(1), (GLubyte)ARG(2), (GLubyte)ARG(3));
    set_alias(3, ARG(0) / 255.0f, ARG(1) / 255.0f, ARG(2) / 255.0f, ARG(3) / 255.0f);
}
static void h_glTexCoord2f(CPU *c)
{
    glTexCoord2f(ARGF(0), ARGF(1));
    set_alias(8, ARGF(0), ARGF(1), 0, 1);
}
static void h_glMultiTexCoord2fARB(CPU *c)
{
    static PFNGLMULTITEXCOORD2FARBPROC p;
    if (!p) p = (PFNGLMULTITEXCOORD2FARBPROC)gl_host_proc("glMultiTexCoord2fARB");
    p(ARG(0), ARGF(1), ARGF(2));
    set_alias(8 + (ARG(0) - GL_TEXTURE0), ARGF(1), ARGF(2), 0, 1);
}
static void h_glMultiTexCoord2fvARB(CPU *c)
{
    static PFNGLMULTITEXCOORD2FVARBPROC p;
    if (!p) p = (PFNGLMULTITEXCOORD2FVARBPROC)gl_host_proc("glMultiTexCoord2fvARB");
    p(ARG(0), (const GLfloat *)G(ARG(1)));
    set_alias(8 + (ARG(0) - GL_TEXTURE0), rdf32(ARG(1)), rdf32(ARG(1) + 4), 0, 1);
}

// framebuffer objects the host cannot render to
static void h_glCheckFramebufferStatusEXT(CPU *c)
{
    static PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC p;
    if (!p) p = (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)gl_host_proc("glCheckFramebufferStatusEXT");
    GLenum r = p(ARG(0));
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fb);
    if (r != GL_FRAMEBUFFER_COMPLETE_EXT) hle_log("framebuffer %d incomplete: 0x%x\n", fb, r);
    RET(r);
}

static void h_glLinkProgram(CPU *c) { link_program(c, 0); }
static void h_glLinkProgramARB(CPU *c) { link_program(c, 1); }
static void h_glCompileShader(CPU *c) { compile_shader(c, 0); }
static void h_glCompileShaderARB(CPU *c) { compile_shader(c, 1); }

static void h_glShaderSource(CPU *c) { shader_source(c, "glShaderSource"); }
static void h_glShaderSourceARB(CPU *c) { shader_source(c, "glShaderSourceARB"); }

// mapped buffers live in guest memory and are copied in on unmap
static struct { GLenum target; GLuint buf; uint32_t guest; uint32_t size; } maps[8];
static void map_buffer(CPU *c, int arb)
{
    typedef void (APIENTRY *getiv)(GLenum, GLenum, GLint *);
    typedef void *(APIENTRY *mapfn)(GLenum, GLenum);
    typedef GLboolean (APIENTRY *unmapfn)(GLenum);
    getiv gp = (getiv)gl_host_proc(arb ? "glGetBufferParameterivARB" : "glGetBufferParameteriv");
    GLint size = 0;
    gp(ARG(0), GL_BUFFER_SIZE, &size);
    uint32_t g = guest_malloc((uint32_t)size);
    mapfn mp = (mapfn)gl_host_proc(arb ? "glMapBufferARB" : "glMapBuffer");
    unmapfn up = (unmapfn)gl_host_proc(arb ? "glUnmapBufferARB" : "glUnmapBuffer");
    void *h = mp(ARG(0), GL_READ_ONLY);
    if (h) memcpy(G(g), h, (size_t)size);
    up(ARG(0));
    for (int i = 0; i < 8; i++)
        if (!maps[i].guest) {
            maps[i].target = ARG(0);
            maps[i].buf = ARG(0) == GL_ARRAY_BUFFER ? array_buffer : element_buffer;
            maps[i].guest = g;
            maps[i].size = (uint32_t)size;
            break;
        }
    RET(g);
}

static void unmap_buffer(CPU *c, int arb)
{
    typedef void (APIENTRY *subfn)(GLenum, GLintptr, GLsizeiptr, const void *);
    subfn sp = (subfn)gl_host_proc(arb ? "glBufferSubDataARB" : "glBufferSubData");
    uint32_t cur = ARG(0) == GL_ARRAY_BUFFER ? array_buffer : element_buffer;
    for (int i = 0; i < 8; i++)
        if (maps[i].guest && maps[i].target == ARG(0) && maps[i].buf == cur) {
            sp(ARG(0), 0, maps[i].size, G(maps[i].guest));
            guest_free(maps[i].guest);
            maps[i].guest = 0;
            RET(1);
        }
    RET(0);
}
static void h_glMapBuffer(CPU *c) { map_buffer(c, 0); }
static void h_glMapBufferARB(CPU *c) { map_buffer(c, 1); }
static void h_glUnmapBuffer(CPU *c) { unmap_buffer(c, 0); }
static void h_glUnmapBufferARB(CPU *c) { unmap_buffer(c, 1); }

// The arrays of counts and index pointers are guest memory; draw them one at a time.
static void h_glMultiDrawElements(CPU *c)
{
    typedef void (APIENTRY *fn)(GLenum, GLsizei, GLenum, const void *);
    static fn draw;
    if (!draw) draw = (fn)gl_host_proc("glDrawElements");
    if (gl_trace) gl_trace_call("glMultiDrawElements", c);
    uint32_t mode = ARG(0), counts = ARG(1), type = ARG(2), indices = ARG(3);
    for (int32_t i = 0; i < (int32_t)ARG(4); i++)
        draw(mode, (GLsizei)rd32(counts + 4 * i), type, gl_element_ptr(rd32(indices + 4 * i)));
    RET(0);
}
static void h_glMultiDrawArrays(CPU *c)
{
    typedef void (APIENTRY *fn)(GLenum, GLint, GLsizei);
    static fn draw;
    if (!draw) draw = (fn)gl_host_proc("glDrawArrays");
    if (gl_trace) gl_trace_call("glMultiDrawArrays", c);
    uint32_t mode = ARG(0), first = ARG(1), counts = ARG(2);
    for (int32_t i = 0; i < (int32_t)ARG(3); i++)
        draw(mode, (GLint)rd32(first + 4 * i), (GLsizei)rd32(counts + 4 * i));
    RET(0);
}

#ifdef LB_IOS
// Rectangle textures on OpenGL ES (glsl_pp.c, fix_rect_textures): each
// program's lbRectScale_<sampler> is 1 / the size of the rectangle texture on
// that sampler's unit, set when the program is used and when a rectangle
// texture is bound or given an image while it is in use.
#define MAX_RECT_PROGS 512
static struct { GLuint prog; int known, n; GLint scale[8], sampler[8]; } rect_progs[MAX_RECT_PROGS];
static GLuint rect_current;

static void rect_forget(GLuint prog)
{
    for (int i = 0; i < MAX_RECT_PROGS; i++)
        if (rect_progs[i].prog == prog) rect_progs[i].known = 0;
}

static void rect_update(void)
{
    typedef void (APIENTRY *ivfn)(GLuint, GLenum, GLint *);
    typedef void (APIENTRY *activefn)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
    typedef GLint (APIENTRY *locfn)(GLuint, const GLchar *);
    typedef void (APIENTRY *getufn)(GLuint, GLint, GLint *);
    typedef void (APIENTRY *u2fn)(GLint, GLfloat, GLfloat);
    typedef void (APIENTRY *texfn)(GLenum);
    typedef void (APIENTRY *levelfn)(GLenum, GLint, GLenum, GLint *);
    GLuint prog = rect_current;
    if (!prog) return;
    int slot = -1;
    for (int i = 0; i < MAX_RECT_PROGS && slot < 0; i++)
        if (rect_progs[i].prog == prog) slot = i;
    for (int i = 0; i < MAX_RECT_PROGS && slot < 0; i++)
        if (!rect_progs[i].prog) slot = i, rect_progs[i].prog = prog;
    if (slot < 0) return;
    if (!rect_progs[slot].known) {   // its lbRectScale_ uniforms and their samplers
        ivfn iv = (ivfn)gl_host_proc("glGetProgramiv");
        activefn active = (activefn)gl_host_proc("glGetActiveUniform");
        locfn loc = (locfn)gl_host_proc("glGetUniformLocation");
        GLint count = 0;
        iv(prog, GL_ACTIVE_UNIFORMS, &count);
        rect_progs[slot].n = 0;
        for (GLint u = 0; u < count && rect_progs[slot].n < 8; u++) {
            char name[128];
            GLint size;
            GLenum type;
            active(prog, (GLuint)u, sizeof name, NULL, &size, &type, name);
            if (strncmp(name, "lbRectScale_", 12)) continue;
            int k = rect_progs[slot].n++;
            rect_progs[slot].scale[k] = loc(prog, name);
            rect_progs[slot].sampler[k] = loc(prog, name + 12);
        }
        rect_progs[slot].known = 1;
    }
    if (!rect_progs[slot].n) return;
    getufn getu = (getufn)gl_host_proc("glGetUniformiv");
    u2fn u2 = (u2fn)gl_host_proc("glUniform2f");
    texfn activetex = (texfn)gl_host_proc("glActiveTexture");
    levelfn level = (levelfn)gl_host_proc("glGetTexLevelParameteriv");
    GLint was = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &was);
    for (int k = 0; k < rect_progs[slot].n; k++) {
        GLint unit = 0, w = 0, h = 0;
        if (rect_progs[slot].sampler[k] >= 0) getu(prog, rect_progs[slot].sampler[k], &unit);
        activetex(GL_TEXTURE0 + (GLenum)unit);
        level(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_WIDTH, &w);
        level(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_HEIGHT, &h);
        u2(rect_progs[slot].scale[k], w > 0 ? 1.0f / (float)w : 1.0f, h > 0 ? 1.0f / (float)h : 1.0f);
    }
    activetex((GLenum)was);
}

static void rect_call(CPU *c, const char *name)
{
    static struct { const char *name; hle_fn fn; } cache[8];
    hle_fn f = NULL;
    for (int i = 0; i < 8 && cache[i].name && !f; i++)
        if (!strcmp(cache[i].name, name)) f = cache[i].fn;
    if (!f) {
        for (const hle_entry *e = gl_gen_table; e->name && !f; e++)
            if (!strcmp(e->name, name)) f = e->fn;
        for (int i = 0; i < 8; i++)
            if (!cache[i].name) { cache[i].name = name, cache[i].fn = f; break; }
    }
    if (f) f(c);
}
static void rect_use(CPU *c, const char *name)
{
    GLuint prog = ARG(0);
    rect_call(c, name);
    rect_current = prog;
    rect_update();
}
static void rect_env_update(void);
static void rect_target(CPU *c, const char *name)
{
    uint32_t target = ARG(0);
    rect_call(c, name);
    if (target == GL_TEXTURE_RECTANGLE_ARB) rect_update(), rect_env_update();
}
static void h_glUseProgram(CPU *c) { rect_use(c, "glUseProgram"); }
static void h_glUseProgramObjectARB(CPU *c) { rect_use(c, "glUseProgramObjectARB"); }
// Depth+stencil textures (OutRun 2 SP's render targets and its car's shadow
// map) are gl4es's own, which on OpenGL ES 3 makes them DEPTH24_STENCIL8
// (host_core_ios.m sizes the format). They were once kept out of gl4es and
// attached as stand-in renderbuffers, which left the shadow map with no
// image to sample: the car was drawn all in shadow (#78).
static void depth_stencil_format(CPU *c);
static void h_glTexImage2D(CPU *c) { depth_stencil_format(c); rect_target(c, "glTexImage2D"); }
// The game attaches a depth+stencil texture to the depth attachment and then
// to the stencil attachment; gl4es's stencil path remakes the texture as a
// stencil one, which OpenGL ES 3 refuses (GL_INVALID_ENUM, the framebuffer
// incomplete and the frame black). The second is made one DEPTH_STENCIL
// attachment of it, which gl4es does.
static void framebuffer_texture(CPU *c, const char *name)
{
    static uint32_t depth_tex;
    uint32_t att = ARG(1), tex = ARG(3);
    if (att == GL_DEPTH_ATTACHMENT_EXT) depth_tex = tex;
    if (att == GL_STENCIL_ATTACHMENT_EXT && tex && tex == depth_tex) wr32(c->r[ESP] + 8, 0x821A);   // DEPTH_STENCIL_ATTACHMENT
    rect_call(c, name);
}
static void h_glFramebufferTexture2DEXT(CPU *c) { framebuffer_texture(c, "glFramebufferTexture2DEXT"); }
static void h_glFramebufferTexture2D(CPU *c) { framebuffer_texture(c, "glFramebufferTexture2D"); }

// Shadow textures (#78): a texture whose compare mode is on is sampled with
// no compare (OpenGL ES 3 compares only through a shadow sampler, and gl4es
// writes 2D lookups as texture2D), and the fragment programs make the
// comparison themselves (arb_lower.c, arb_shadow_lookups) from program
// environment parameter 18 + unit, which the runtime keeps for units 0-3:
// x 1 while a compare-mode texture is bound there, y its border depth, z and
// w 1 where its s and t wrap is CLAMP_TO_BORDER (which gl4es makes
// CLAMP_TO_EDGE: past OutRun 2 SP's shadow map the edge's depths were
// compared, and the scenery right of it was lit unlike the rest).
#define SHADOW_ENV 18
typedef struct { GLuint tex; int compare, border_s, border_t; GLfloat border; } shadow_info;
static shadow_info *shadow_tex;   // the textures given a compare mode, border wrap or colour
static int shadow_n, shadow_cap;
static GLuint unit_tex[4];        // the 2D texture bound on units 0-3
static int active_unit;
static shadow_info *shadow_get(GLuint t, int make)
{
    for (int i = 0; i < shadow_n; i++)
        if (shadow_tex[i].tex == t) return &shadow_tex[i];
    if (!make || !t) return NULL;
    if (shadow_n == shadow_cap) shadow_tex = realloc(shadow_tex, sizeof(shadow_info) * (size_t)(shadow_cap = shadow_cap ? shadow_cap * 2 : 16));
    shadow_tex[shadow_n] = (shadow_info){t, 0, 0, 0, 0};
    return &shadow_tex[shadow_n++];
}
static shadow_info *shadow_bound(int make)
{
    return active_unit < 4 ? shadow_get(unit_tex[active_unit], make) : NULL;
}
static void shadow_env_update(void)
{
    typedef void (APIENTRY *envfn)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
    static envfn env;
    static GLfloat set[4][4] = {{-1}, {-1}, {-1}, {-1}};
    if (!env) env = (envfn)gl_host_proc("glProgramEnvParameter4fARB");
    for (int u = 0; u < 4 && env; u++) {
        shadow_info *i = shadow_get(unit_tex[u], 0);
        GLfloat v[4] = {0, 0, 0, 0};
        if (i && i->compare) v[0] = 1, v[1] = i->border, v[2] = (GLfloat)i->border_s, v[3] = (GLfloat)i->border_t;
        if (memcmp(v, set[u], sizeof v)) env(GL_FRAGMENT_PROGRAM_ARB, SHADOW_ENV + u, v[0], v[1], v[2], v[3]), memcpy(set[u], v, sizeof v);
    }
}
// Rectangle textures in ARB fragment programs (arb_lower.c,
// arb_rect_lookups): program environment parameter 24 + unit is 1 / the size
// of the rectangle texture bound on that unit (units 0-7), set when one is
// bound or given an image.
#define RECT_ENV 24
static void rect_env_update(void)
{
    typedef void (APIENTRY *envfn)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
    typedef void (APIENTRY *levelfn)(GLenum, GLint, GLenum, GLint *);
    static envfn env;
    static levelfn level;
    if (!env) env = (envfn)gl_host_proc("glProgramEnvParameter4fARB"), level = (levelfn)gl_host_proc("glGetTexLevelParameteriv");
    if (!env || !level || active_unit < 0 || active_unit > 7) return;
    GLint w = 0, h = 0;
    level(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_WIDTH, &w);
    level(GL_TEXTURE_RECTANGLE_ARB, 0, GL_TEXTURE_HEIGHT, &h);
    env(GL_FRAGMENT_PROGRAM_ARB, RECT_ENV + (GLuint)active_unit, w > 0 ? 1.0f / (GLfloat)w : 1.0f, h > 0 ? 1.0f / (GLfloat)h : 1.0f, 1, 1);
}
static void h_glBindTexture(CPU *c)
{
    if (ARG(0) == GL_TEXTURE_2D && active_unit < 4) {
        unit_tex[active_unit] = ARG(1);
        shadow_env_update();
    }
    rect_target(c, "glBindTexture");
}
static void active_texture(CPU *c, const char *name)
{
    active_unit = (int)(ARG(0) - GL_TEXTURE0);
    rect_call(c, name);
}
static void h_glActiveTexture(CPU *c) { active_texture(c, "glActiveTexture"); }
static void h_glActiveTextureARB(CPU *c) { active_texture(c, "glActiveTextureARB"); }
// compare mode and function, and the depth texture mode (which OpenGL ES has
// not); a shadow texture filters NEAREST, as OpenGL ES 3 samples a depth
// texture with no compare only so (a LINEAR one is incomplete and reads 0:
// all in shadow). NVIDIA's linear filter averaged four comparisons. The wrap
// and the border colour are kept (and passed on).
static void shadow_filters(void)
{
    typedef void (APIENTRY *parfn)(GLenum, GLenum, GLint);
    static parfn par;
    if (!par) par = (parfn)gl_host_proc("glTexParameteri");
    par(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    par(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}
static int shadow_param(CPU *c, uint32_t value, float border)
{
    uint32_t pname = ARG(1);
    if (ARG(0) != GL_TEXTURE_2D) return 0;
    shadow_info *i = shadow_bound(0);
    if ((pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) && i && i->compare) {
        shadow_filters();
        c->r[EAX] = 0;
        return 1;
    }
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == 0x1004) {   // BORDER_COLOR
        if ((i = shadow_bound(value == 0x812D || pname == 0x1004))) {   // CLAMP_TO_BORDER
            if (pname == GL_TEXTURE_WRAP_S) i->border_s = value == 0x812D;
            else if (pname == GL_TEXTURE_WRAP_T) i->border_t = value == 0x812D;
            else i->border = border;
            shadow_env_update();
        }
        return 0;
    }
    if (pname != 0x884C && pname != 0x884D && pname != 0x884B) return 0;   // COMPARE_MODE, _FUNC, DEPTH_TEXTURE_MODE
    if (pname == 0x884C && (i = shadow_bound(1))) {
        int on = value == 0x884E;   // COMPARE_R_TO_TEXTURE
        if (on && !i->compare) shadow_filters();
        i->compare = on;
        shadow_env_update();
    }
    c->r[EAX] = 0;
    return 1;
}
static void h_glTexParameteri(CPU *c) { if (!shadow_param(c, ARG(2), 0)) rect_call(c, "glTexParameteri"); }
static void h_glTexParameterf(CPU *c) { if (!shadow_param(c, (uint32_t)ARGF(2), 0)) rect_call(c, "glTexParameterf"); }
static void h_glTexParameteriv(CPU *c)
{
    if (!shadow_param(c, rd32(ARG(2)), (float)(int32_t)rd32(ARG(2)) / 2147483647.0f)) rect_call(c, "glTexParameteriv");
}
static void h_glTexParameterfv(CPU *c)
{
    float v;
    uint32_t bits = rd32(ARG(2));
    memcpy(&v, &bits, 4);
    if (!shadow_param(c, (uint32_t)v, v)) rect_call(c, "glTexParameterfv");
}
// a deleted texture's name, reused, is no shadow texture (and unbound)
static void h_glDeleteTextures(CPU *c)
{
    for (uint32_t k = 0; k < ARG(0); k++) {
        GLuint t = rd32(ARG(1) + 4 * k);
        shadow_info *i = shadow_get(t, 0);
        if (i) *i = shadow_tex[--shadow_n];
        for (int u = 0; u < 4; u++)
            if (unit_tex[u] == t) unit_tex[u] = 0;
    }
    shadow_env_update();
    rect_call(c, "glDeleteTextures");
}
static void h_glCopyTexImage2D(CPU *c) { rect_target(c, "glCopyTexImage2D"); }
#endif

// A depth texture asked for as depth with depth+stencil data (format
// GL_DEPTH_STENCIL, type UNSIGNED_INT_24_8: Ghost Squad Evolution's shadow
// map and render targets), which NVIDIA takes; on Apple's GL the texture is
// left incomplete and samples as 0 (the whole scene in shadow), and gl4es
// refuses it: it is made DEPTH24_STENCIL8.
static void depth_stencil_format(CPU *c)
{
    uint32_t internal = ARG(2);
    if (ARG(6) == GL_DEPTH_STENCIL && (internal == GL_DEPTH_COMPONENT || internal == GL_DEPTH_COMPONENT16 ||
                                       internal == GL_DEPTH_COMPONENT24 || internal == GL_DEPTH_COMPONENT32))
        wr32(c->r[ESP] + 4 + 4 * 2, GL_DEPTH24_STENCIL8);
}
#ifndef LB_IOS
static void h_glTexImage2D_depth(CPU *c)
{
    static hle_fn gen;
    if (!gen)
        for (const hle_entry *e = gl_gen_table; e->name && !gen; e++)
            if (!strcmp(e->name, "glTexImage2D")) gen = e->fn;
    depth_stencil_format(c);
    gen(c);
}
#endif
// NVIDIA aliases the conventional arrays onto generic attributes (normal 2,
// colour 3 and 4, texcoord n 8+n), and the vertex programs read those
// (alias_vertex_inputs). Sega Race TV feeds its arrays the conventional way
// (glTexCoordPointer...): on Apple's GL and Mesa its scenery's lightmap
// coordinates (texcoord 1, attribute 9) read as 0, and the scenery was drawn
// black. Each conventional array is set as its generic attribute too.
static GLenum client_unit = GL_TEXTURE0;
typedef void (APIENTRY *attrib_ptr_fn)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (APIENTRY *attrib_on_fn)(GLuint);
// what each generic attribute was last set to, so a call that changes
// nothing is not made (HotD4 sets its arrays thousands of times a frame:
// on Linux the copies cost 3-5 ms a frame)
static struct { GLint size; GLenum type; GLboolean norm; GLsizei stride; const void *ptr; GLuint buffer; int on; } mirrored[16];
static void mirror_pointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, uint32_t ptr)
{
    static attrib_ptr_fn p;
    if (!p) p = (attrib_ptr_fn)gl_host_proc("glVertexAttribPointerARB");
    const void *a = gl_array_ptr(ptr);
    if (index < 16 && mirrored[index].ptr == a && mirrored[index].size == size && mirrored[index].type == type &&
        mirrored[index].norm == norm && mirrored[index].stride == stride && mirrored[index].buffer == array_buffer)
        return;
    if (index < 16) {
        mirrored[index].size = size, mirrored[index].type = type, mirrored[index].norm = norm;
        mirrored[index].stride = stride, mirrored[index].ptr = a, mirrored[index].buffer = array_buffer;
    }
    if (p) p(index, size, type, norm, stride, a);
}
static void mirror_array(GLenum array, int on)
{
    static attrib_on_fn en, dis;
    if (!en) {
        en = (attrib_on_fn)gl_host_proc("glEnableVertexAttribArrayARB");
        dis = (attrib_on_fn)gl_host_proc("glDisableVertexAttribArrayARB");
    }
    GLuint index = array == GL_NORMAL_ARRAY ? 2 : array == GL_COLOR_ARRAY ? 3 : array == GL_SECONDARY_COLOR_ARRAY ? 4
                 : array == GL_TEXTURE_COORD_ARRAY && client_unit - GL_TEXTURE0 < 8 ? 8 + client_unit - GL_TEXTURE0 : 0;
    if (!index || !en || !dis || mirrored[index].on == on + 1) return;
    mirrored[index].on = on + 1;
    (on ? en : dis)(index);
}
// the generated wrapper of a GL call, found once (*cache)
static void gen_call_cached(CPU *c, const char *name, hle_fn *cache)
{
    if (!*cache)
        for (const hle_entry *e = gl_gen_table; e->name && !*cache; e++)
            if (!strcmp(e->name, name)) *cache = e->fn;
    if (*cache) (*cache)(c);
}
#define gen_call(c, name) do { static hle_fn fn_; gen_call_cached(c, name, &fn_); } while (0)
static void h_glClientActiveTexture(CPU *c) { client_unit = ARG(0); gen_call(c, "glClientActiveTexture"); }
static void h_glClientActiveTextureARB(CPU *c) { client_unit = ARG(0); gen_call(c, "glClientActiveTextureARB"); }
static void h_glTexCoordPointer(CPU *c)
{
    if (client_unit - GL_TEXTURE0 < 8) mirror_pointer(8 + client_unit - GL_TEXTURE0, (GLint)ARG(0), ARG(1), GL_FALSE, (GLsizei)ARG(2), ARG(3));
    gen_call(c, "glTexCoordPointer");
}
static void h_glNormalPointer(CPU *c)
{
    mirror_pointer(2, 3, ARG(0), ARG(0) != GL_FLOAT && ARG(0) != GL_DOUBLE, (GLsizei)ARG(1), ARG(2));
    gen_call(c, "glNormalPointer");
}
static void h_glColorPointer(CPU *c)
{
    mirror_pointer(3, (GLint)ARG(0), ARG(1), ARG(1) != GL_FLOAT && ARG(1) != GL_DOUBLE, (GLsizei)ARG(2), ARG(3));
    gen_call(c, "glColorPointer");
}
// the game setting a generic attribute itself: the mirror's record of it no longer holds
static void forget(uint32_t index)
{
    if (index >= 16) return;
    mirrored[index].ptr = (const void *)~(uintptr_t)0;
    mirrored[index].on = 0;
}
static void h_glVertexAttribPointerARB(CPU *c) { forget(ARG(0)); gen_call(c, "glVertexAttribPointerARB"); }
static void h_glVertexAttribPointer(CPU *c) { forget(ARG(0)); gen_call(c, "glVertexAttribPointer"); }
static void h_glEnableVertexAttribArrayARB(CPU *c) { forget(ARG(0)); gen_call(c, "glEnableVertexAttribArrayARB"); }
static void h_glEnableVertexAttribArray(CPU *c) { forget(ARG(0)); gen_call(c, "glEnableVertexAttribArray"); }
static void h_glDisableVertexAttribArrayARB(CPU *c) { forget(ARG(0)); gen_call(c, "glDisableVertexAttribArrayARB"); }
static void h_glDisableVertexAttribArray(CPU *c) { forget(ARG(0)); gen_call(c, "glDisableVertexAttribArray"); }
static void h_glEnableClientState(CPU *c) { mirror_array(ARG(0), 1); gen_call(c, "glEnableClientState"); }
static void h_glDisableClientState(CPU *c) { mirror_array(ARG(0), 0); gen_call(c, "glDisableClientState"); }

static void h_glEnable(CPU *c), h_glDisable(CPU *c), h_glIsEnabled(CPU *c);
static void h_glLoadProgramNV(CPU *c), h_glTrackMatrixNV(CPU *c), h_glBindProgramNV(CPU *c), h_glGenProgramsNV(CPU *c),
    h_glDeleteProgramsNV(CPU *c), h_glIsProgramNV(CPU *c), h_glProgramParameter4fNV(CPU *c),
    h_glProgramParameter4fvNV(CPU *c), h_glProgramParameters4fvNV(CPU *c);
static const hle_entry gfx_special[] = {
    {"glEnable", h_glEnable}, {"glDisable", h_glDisable}, {"glIsEnabled", h_glIsEnabled},
    {"glLoadProgramNV", h_glLoadProgramNV}, {"glTrackMatrixNV", h_glTrackMatrixNV}, {"glBindProgramNV", h_glBindProgramNV},
    {"glGenProgramsNV", h_glGenProgramsNV}, {"glDeleteProgramsNV", h_glDeleteProgramsNV}, {"glIsProgramNV", h_glIsProgramNV},
    {"glProgramParameter4fNV", h_glProgramParameter4fNV}, {"glProgramParameter4fvNV", h_glProgramParameter4fvNV},
    {"glProgramParameters4fvNV", h_glProgramParameters4fvNV},
    {"glMultiDrawElements", h_glMultiDrawElements}, {"glMultiDrawElementsEXT", h_glMultiDrawElements},
    {"glMultiDrawArrays", h_glMultiDrawArrays}, {"glMultiDrawArraysEXT", h_glMultiDrawArrays},
    {"glClientActiveTexture", h_glClientActiveTexture}, {"glClientActiveTextureARB", h_glClientActiveTextureARB},
    {"glTexCoordPointer", h_glTexCoordPointer}, {"glNormalPointer", h_glNormalPointer}, {"glColorPointer", h_glColorPointer},
    {"glEnableClientState", h_glEnableClientState}, {"glDisableClientState", h_glDisableClientState},
    {"glVertexAttribPointerARB", h_glVertexAttribPointerARB}, {"glVertexAttribPointer", h_glVertexAttribPointer},
    {"glEnableVertexAttribArrayARB", h_glEnableVertexAttribArrayARB}, {"glEnableVertexAttribArray", h_glEnableVertexAttribArray},
    {"glDisableVertexAttribArrayARB", h_glDisableVertexAttribArrayARB}, {"glDisableVertexAttribArray", h_glDisableVertexAttribArray},
    {"glGetString", h_glGetString}, {"glBindBuffer", h_glBindBuffer}, {"glBindBufferARB", h_glBindBufferARB},
    {"glShaderSource", h_glShaderSource}, {"glShaderSourceARB", h_glShaderSourceARB},
    {"glCompileShader", h_glCompileShader}, {"glCompileShaderARB", h_glCompileShaderARB},
    {"glLinkProgram", h_glLinkProgram}, {"glLinkProgramARB", h_glLinkProgramARB},
    {"glProgramStringARB", h_glProgramStringARB},
    {"glCheckFramebufferStatusEXT", h_glCheckFramebufferStatusEXT}, {"glColor4ub", h_glColor4ub},
    {"glTexCoord2f", h_glTexCoord2f}, {"glBindFramebufferEXT", h_glBindFramebufferEXT},
    {"glBindFramebuffer", h_glBindFramebuffer}, {"glMultiTexCoord2fARB", h_glMultiTexCoord2fARB},
    {"glMultiTexCoord2fvARB", h_glMultiTexCoord2fvARB},
    {"glAttachShader", h_glAttachShader}, {"glAttachObjectARB", h_glAttachObjectARB},
    {"glMapBuffer", h_glMapBuffer}, {"glMapBufferARB", h_glMapBufferARB},
    {"glUnmapBuffer", h_glUnmapBuffer}, {"glUnmapBufferARB", h_glUnmapBufferARB},
#ifndef LB_IOS
    {"glTexImage2D", h_glTexImage2D_depth},
#endif
    {"glBindProgramARB", h_glBindProgramARB}, {"glProgramEnvParameter4fvARB", h_glProgramEnvParameter4fvARB},
#ifdef LB_IOS
    {"glUseProgram", h_glUseProgram}, {"glUseProgramObjectARB", h_glUseProgramObjectARB},
    {"glBindTexture", h_glBindTexture}, {"glTexImage2D", h_glTexImage2D},
    {"glActiveTexture", h_glActiveTexture}, {"glActiveTextureARB", h_glActiveTextureARB},
    {"glTexParameteri", h_glTexParameteri}, {"glTexParameterf", h_glTexParameterf},
    {"glTexParameteriv", h_glTexParameteriv}, {"glTexParameterfv", h_glTexParameterfv},
    {"glDeleteTextures", h_glDeleteTextures},
    {"glFramebufferTexture2DEXT", h_glFramebufferTexture2DEXT}, {"glFramebufferTexture2D", h_glFramebufferTexture2D},
    {"glCopyTexImage2D", h_glCopyTexImage2D},
    {"glFogf", h_glFogf}, {"glFogfv", h_glFogfv}, {"glFogi", h_glFogi}, {"glFogiv", h_glFogiv},
#endif
    {NULL, NULL},
};

// NV_occlusion_query on ARB_occlusion_query (Sega Race TV's lens flare calls
// it whether or not the extension is listed); the tokens are the same values
typedef void (APIENTRY *query_fn)(GLsizei, GLuint *);
static void nv_queries(CPU *c, const char *arb)
{
    query_fn p = (query_fn)gl_host_proc(arb);
    p((GLsizei)ARG(0), (GLuint *)G(ARG(1)));
}
static void h_glGenOcclusionQueriesNV(CPU *c) { nv_queries(c, "glGenQueriesARB"); }
static void h_glDeleteOcclusionQueriesNV(CPU *c) { nv_queries(c, "glDeleteQueriesARB"); }
static void h_glIsOcclusionQueryNV(CPU *c)
{
    static PFNGLISQUERYARBPROC p;
    if (!p) p = (PFNGLISQUERYARBPROC)gl_host_proc("glIsQueryARB");
    RET(p(ARG(0)));
}
static void h_glBeginOcclusionQueryNV(CPU *c)
{
    static PFNGLBEGINQUERYARBPROC p;
    if (!p) p = (PFNGLBEGINQUERYARBPROC)gl_host_proc("glBeginQueryARB");
    p(GL_SAMPLES_PASSED_ARB, ARG(0));
}
static void h_glEndOcclusionQueryNV(CPU *c)
{
    static PFNGLENDQUERYARBPROC p;
    if (!p) p = (PFNGLENDQUERYARBPROC)gl_host_proc("glEndQueryARB");
    p(GL_SAMPLES_PASSED_ARB);
}
static void h_glGetOcclusionQueryivNV(CPU *c)   // PIXEL_COUNT(_AVAILABLE)_NV == QUERY_RESULT(_AVAILABLE)_ARB
{
    static PFNGLGETQUERYOBJECTIVARBPROC p;
    if (!p) p = (PFNGLGETQUERYOBJECTIVARBPROC)gl_host_proc("glGetQueryObjectivARB");
    p(ARG(0), ARG(1), (GLint *)G(ARG(2)));
}
static void h_glGetOcclusionQueryuivNV(CPU *c)
{
    static PFNGLGETQUERYOBJECTUIVARBPROC p;
    if (!p) p = (PFNGLGETQUERYOBJECTUIVARBPROC)gl_host_proc("glGetQueryObjectuivARB");
    p(ARG(0), ARG(1), (GLuint *)G(ARG(2)));
}
// NVX_conditional_render (one argument) on NV_conditional_render
static void h_glBeginConditionalRenderNVX(CPU *c)
{
    typedef void (APIENTRY *fn)(GLuint, GLenum);
    static fn p;
    if (!p) p = (fn)gl_host_proc("glBeginConditionalRenderNV");
    if (p) p(ARG(0), 0x8E13);   // GL_QUERY_WAIT_NV
}
static void h_glEndConditionalRenderNVX(CPU *c)
{
    typedef void (APIENTRY *fn)(void);
    static fn p;
    if (!p) p = (fn)gl_host_proc("glEndConditionalRenderNV");
    if (p) p();
}
// NV_vertex_program (OutRun 2 SP) on ARB vertex programs: Apple has neither the NV
// entry points nor the VP2.0 syntax (nv_vertex_program.c). Program parameters are
// the vertex programs' environment parameters; the names are shared with ARB.
static nvvp_track nv_track[NVVP_TRACKS];
typedef struct { unsigned id; char *src; size_t len; } nv_program;
static nv_program nv_programs[512];
static int nv_program_count;
static PFNGLPROGRAMSTRINGARBPROC arb_string;
static PFNGLBINDPROGRAMARBPROC arb_bind;
static PFNGLPROGRAMENVPARAMETER4FARBPROC arb_env4f;
static PFNGLPROGRAMENVPARAMETER4FVARBPROC arb_env4fv;
static PFNGLGETPROGRAMIVARBPROC arb_getiv;
static PFNGLGENPROGRAMSARBPROC arb_gen;
static PFNGLDELETEPROGRAMSARBPROC arb_delete;
static PFNGLISPROGRAMARBPROC arb_is;

static void nv_procs(void)
{
    if (arb_string) return;
    arb_string = (PFNGLPROGRAMSTRINGARBPROC)gl_host_proc("glProgramStringARB");
    arb_bind = (PFNGLBINDPROGRAMARBPROC)gl_host_proc("glBindProgramARB");
    arb_env4f = (PFNGLPROGRAMENVPARAMETER4FARBPROC)gl_host_proc("glProgramEnvParameter4fARB");
    arb_env4fv = (PFNGLPROGRAMENVPARAMETER4FVARBPROC)gl_host_proc("glProgramEnvParameter4fvARB");
    arb_getiv = (PFNGLGETPROGRAMIVARBPROC)gl_host_proc("glGetProgramivARB");
    arb_gen = (PFNGLGENPROGRAMSARBPROC)gl_host_proc("glGenProgramsARB");
    arb_delete = (PFNGLDELETEPROGRAMSARBPROC)gl_host_proc("glDeleteProgramsARB");
    arb_is = (PFNGLISPROGRAMARBPROC)gl_host_proc("glIsProgramARB");
}

static void nv_load(GLenum target, unsigned id, const char *src, size_t len)
{
    char *arb = nvvp_to_arb(src, len, nv_track);
#ifdef LB_IOS
    arb = arb_address_offsets(arb_matrix_rows(arb_inline_constants(arb_lower_nv(arb))));   // gl4es knows ARB only (arb_lower.c)
    if (arb_fog_env(arb)) fog_env_update();
#else
    if (!host_has_nv_options()) arb = arb_lower_nv(arb);
#endif
    GLint was = 0;
    arb_getiv(target, GL_PROGRAM_BINDING_ARB, &was);
    arb_bind(target, id);
    arb_string(target, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(arb), arb);
#ifdef LB_IOS
    arb_fallback((void (APIENTRY *)(GLenum, GLenum, GLsizei, const void *))arb_string, target, GL_PROGRAM_FORMAT_ASCII_ARB);
#endif
    GLint pos = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
#ifdef LB_IOS
    const char *err_msg = rejected_pos >= 0 ? rejected_msg : (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
    if (rejected_pos >= 0) pos = rejected_pos, rejected_pos = -1;
#else
    const char *err_msg = (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
#endif
    static int failures;
    if (pos >= 0 && failures++ < 20) {
        hle_log("NV vertex program %u rejected at %d: %s\n", id, pos, err_msg);
        char path[64];
        snprintf(path, sizeof path, "nvprog_fail_%02d.txt", failures);
        FILE *f = fopen(path, "w");
        if (f) { fputs(arb, f); fclose(f); }
    }
    if (af_debug_env("LB_SHADER_DUMP")) {
        char path[64];
        snprintf(path, sizeof path, "nvprog_%03u.txt", id);
        FILE *f = fopen(path, "w");
        if (f) { fputs(arb, f); fclose(f); }
    }
    arb_bind(target, (GLuint)was);
    free(arb);
}

static void h_glLoadProgramNV(CPU *c)
{
    nv_procs();
    unsigned id = ARG(1);
    size_t len = ARG(2);
    nv_program *p = NULL;
    for (int i = 0; i < nv_program_count; i++)
        if (nv_programs[i].id == id) p = &nv_programs[i];
    if (!p && nv_program_count < 512) p = &nv_programs[nv_program_count++];
    if (p) {
        free(p->src);
        p->id = id;
        p->src = malloc(len);
        memcpy(p->src, G(ARG(3)), len);
        p->len = len;
    }
    nv_load(ARG(0), id, G(ARG(3)), len);
}

// the tracked matrices are bound into the programs: a change loads them again
static void h_glTrackMatrixNV(CPU *c)
{
    nv_procs();
    unsigned addr = ARG(1), m = ARG(2), t = ARG(3);
    int slot = -1, changed = 0;
    for (int i = 0; i < NVVP_TRACKS; i++)
        if (nv_track[i].matrix && nv_track[i].address == addr) slot = i;
    if (slot < 0 && m)
        for (int i = 0; i < NVVP_TRACKS && slot < 0; i++)
            if (!nv_track[i].matrix) slot = i;
    if (slot >= 0) {
        unsigned nm = m == GL_NONE ? 0 : m;
        changed = nv_track[slot].matrix != nm || nv_track[slot].transform != t;
        nv_track[slot] = (nvvp_track){addr, nm, t};
    }
    if (changed)
        for (int i = 0; i < nv_program_count; i++) nv_load(ARG(0), nv_programs[i].id, nv_programs[i].src, nv_programs[i].len);
}

// NV_fragment_program's target is ARB's on the host (its programs are translated)
static GLenum nv_target(GLenum t) { return t == 0x8870 ? GL_FRAGMENT_PROGRAM_ARB : t; }
// two-sided lighting for vertex programs: until a game turns it on, their
// back colours are taken out on Apple's GL (arb_lower.c, arb_drop_back_colors)
#if !(defined(__APPLE__) && !defined(LB_IOS))
static int two_side_on;
#endif
static void h_glEnable(CPU *c)
{
    if (ARG(0) == GL_VERTEX_PROGRAM_TWO_SIDE_ARB && !two_side_on) {
        two_side_on = 1;
        hle_log("vertex programs: two-sided lighting on; programs loaded before have no back colours\n");
    }
    glEnable(nv_target(ARG(0)));
}
static void h_glDisable(CPU *c) { glDisable(nv_target(ARG(0))); }
static void h_glIsEnabled(CPU *c) { RET(glIsEnabled(nv_target(ARG(0)))); }
static void h_glBindProgramNV(CPU *c) { nv_procs(); arb_bind(nv_target(ARG(0)), ARG(1)); }
static void h_glGenProgramsNV(CPU *c) { nv_procs(); arb_gen((GLsizei)ARG(0), (GLuint *)G(ARG(1))); }
static void h_glDeleteProgramsNV(CPU *c) { nv_procs(); arb_delete((GLsizei)ARG(0), (const GLuint *)G(ARG(1))); }
static void h_glIsProgramNV(CPU *c) { nv_procs(); RET(arb_is(ARG(0))); }
static void h_glProgramParameter4fNV(CPU *c)
{
    nv_procs();
    arb_env4f(ARG(0), ARG(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5));
}
static void h_glProgramParameter4fvNV(CPU *c)
{
    nv_procs();
    arb_env4fv(ARG(0), ARG(1), (const GLfloat *)G(ARG(2)));
}
static void h_glProgramParameters4fvNV(CPU *c)
{
    nv_procs();
    const GLfloat *v = (const GLfloat *)G(ARG(3));
    for (uint32_t i = 0; i < ARG(2); i++) arb_env4fv(ARG(0), ARG(1) + i, v + 4 * i);
}

// GL calls the runtime provides although the host has no such entry point
static const hle_entry gl_emulated[] = {
    {"glGenOcclusionQueriesNV", h_glGenOcclusionQueriesNV}, {"glDeleteOcclusionQueriesNV", h_glDeleteOcclusionQueriesNV},
    {"glIsOcclusionQueryNV", h_glIsOcclusionQueryNV}, {"glBeginOcclusionQueryNV", h_glBeginOcclusionQueryNV},
    {"glEndOcclusionQueryNV", h_glEndOcclusionQueryNV}, {"glGetOcclusionQueryivNV", h_glGetOcclusionQueryivNV},
    {"glGetOcclusionQueryuivNV", h_glGetOcclusionQueryuivNV},
    {"glBeginConditionalRenderNVX", h_glBeginConditionalRenderNVX},
    {"glEndConditionalRenderNVX", h_glEndConditionalRenderNVX},
    {NULL, NULL},
};

static uint32_t gl_proc(const char *name)
{
    hle_fn f = NULL;
    for (const hle_entry *e = gl_emulated; e->name; e++)
        if (!strcmp(e->name, name)) return magic_register(e->fn, name);
    for (const hle_entry *e = gfx_special; e->name && !f; e++)
        if (!strcmp(e->name, name)) f = e->fn;
    for (const hle_entry *e = gl_gen_table; e->name && !f; e++)
        if (!strcmp(e->name, name)) f = e->fn;
    // the host is asked only once it has a context: gl4es answers few names
    // before (2 Spicy asks for them all before it makes its window); the
    // wrapper looks the function up when it is first called
    return f && (!gl_open || host_proc_any(name)) ? magic_register(f, name) : 0;
}

static void h_glXGetProcAddressARB(CPU *c)
{
    const char *name = G(ARG(0));
    uint32_t p = gl_proc(name);
    if (!p && hle_verbose) hle_log("glXGetProcAddress %s: not provided\n", name);
    RET(p);
}

static const hle_entry glut_table[] = {
    {"glutInit", h_ret0}, {"glutFullScreen", h_ret0}, {"glutInitDisplayMode", h_ret0}, {"glutInitWindowPosition", h_ret0},
    {"glutInitWindowSize", h_glutInitWindowSize}, {"glutGameModeString", h_glutGameModeString},
    {"glutEnterGameMode", h_glutEnterGameMode}, {"glutLeaveGameMode", h_ret0},
    {"glutCreateWindow", h_glutCreateWindow}, {"glutDisplayFunc", h_glutDisplayFunc},
    {"glutIdleFunc", h_glutIdleFunc}, {"glutKeyboardFunc", h_glutKeyboardFunc},
    {"glutPostRedisplay", h_glutPostRedisplay}, {"glutSwapBuffers", h_glutSwapBuffers},
    {"glutMainLoop", h_glutMainLoop}, {"glutSetCursor", h_ret0},
    {"glutEntryFunc", h_ret0}, {"glutJoystickFunc", h_ret0}, {"glutGetModifiers", h_ret0},
    {"glutMainLoopEvent", h_glutMainLoopEvent}, {"glutGet", h_glutGet}, {"glutBitmapCharacter", h_glutBitmapCharacter},
    {"glutBitmapWidth", h_glutBitmapWidth}, {"glutSolidTeapot", h_glutSolidTeapot}, {"gluPerspective", h_gluPerspective},
    {"gluOrtho2D", h_gluOrtho2D}, {"gluLookAt", h_gluLookAt}, {"gluErrorString", h_gluErrorString},
    {"glutKeyboardUpFunc", h_ret0}, {"glutSpecialFunc", h_ret0}, {"glutSpecialUpFunc", h_ret0},
    {"glutMouseFunc", h_ret0}, {"glutMotionFunc", h_ret0}, {"glutPassiveMotionFunc", h_ret0},
    {"glutReshapeFunc", h_ret0},
    {NULL, NULL},
};

// ---- Hummer's further X11, GLX and GLU calls ----------------------------------------------------
static void h_XAllocWMHints(CPU *c) { RET(guest_calloc(1, 36)); }
static void h_XEventsQueued(CPU *c) { h_XPending(c); }
// XPeekEvent: the next event, left in the queue (none: a NoExpose)
static void h_XPeekEvent(CPU *c)
{
    pthread_mutex_lock(&qlock);
    if (qhead == qtail) {
        memset(G(ARG(1)), 0, 96);
        wr32(ARG(1), 14);
    } else
        memcpy(G(ARG(1)), &queue[qhead % 64], 96);
    pthread_mutex_unlock(&qlock);
    RET(0);
}
// XGetGeometry(display, drawable, root, x, y, width, height, border, depth): the window
static void h_XGetGeometry(CPU *c)
{
    static const int at[7] = {2, 3, 4, 5, 6, 7, 8};
    uint32_t v[7] = {0x100, 0, 0, (uint32_t)win_w, (uint32_t)win_h, 0, 24};
    for (int k = 0; k < 7; k++)
        if (ARG(at[k])) wr32(ARG(at[k]), v[k]);
    RET(1);
}
static void h_glXQueryVersion(CPU *c)
{
    if (ARG(1)) wr32(ARG(1), 1);
    if (ARG(2)) wr32(ARG(2), 4);
    RET(1);
}
// gluProject(objx, objy, objz, model, proj, view, winx, winy, winz) and gluUnProject,
// as GLU computes them (doubles on the stack)
static void mat_mul_vec(const double *m, const double *in, double *out)
{
    for (int i = 0; i < 4; i++)
        out[i] = m[i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2] + m[12 + i] * in[3];
}
static void read_mat(uint32_t a, double *m) { for (int i = 0; i < 16; i++) m[i] = u2d(rd64(a + 8 * i)); }
static void h_gluProject(CPU *c)
{
    double model[16], proj[16], in[4] = {ARGD(0), ARGD(2), ARGD(4), 1}, t[4], o[4];
    read_mat(ARG(6), model);
    read_mat(ARG(7), proj);
    uint32_t vp = ARG(8);
    mat_mul_vec(model, in, t);
    mat_mul_vec(proj, t, o);
    if (o[3] == 0) RET(0);
    for (int i = 0; i < 3; i++) o[i] = o[i] / o[3] * 0.5 + 0.5;
    wr64(ARG(9), d2u(o[0] * (int32_t)rd32(vp + 8) + (int32_t)rd32(vp)));
    wr64(ARG(10), d2u(o[1] * (int32_t)rd32(vp + 12) + (int32_t)rd32(vp + 4)));
    wr64(ARG(11), d2u(o[2]));
    RET(1);
}
static int invert4(const double *m, double *inv)
{
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[k * 4 + r] : (k - 4 == r);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++) if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (a[piv][col] == 0) return 0;
        for (int k = 0; k < 8; k++) { double x = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = x; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) { double f = a[r][col]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k]; }
    }
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) inv[k * 4 + r] = a[r][k + 4];
    return 1;
}
static void h_gluUnProject(CPU *c)
{
    double model[16], proj[16], mp[16], inv[16], o[4];
    read_mat(ARG(6), model);
    read_mat(ARG(7), proj);
    uint32_t vp = ARG(8);
    for (int col = 0; col < 4; col++) mat_mul_vec(proj, model + 4 * col, mp + 4 * col);
    if (!invert4(mp, inv)) RET(0);
    double in[4] = {(ARGD(0) - (int32_t)rd32(vp)) / (int32_t)rd32(vp + 8) * 2 - 1,
                    (ARGD(2) - (int32_t)rd32(vp + 4)) / (int32_t)rd32(vp + 12) * 2 - 1, ARGD(4) * 2 - 1, 1};
    mat_mul_vec(inv, in, o);
    if (o[3] == 0) RET(0);
    for (int i = 0; i < 3; i++) wr64(ARG(9 + i), d2u(o[i] / o[3]));
    RET(1);
}

static const hle_entry gfx_table[] = {
    {"XOpenDisplay", h_XOpenDisplay}, {"XCreateWindow", h_XCreateWindow}, {"XMapWindow", h_XMapWindow},
    {"XAllocWMHints", h_XAllocWMHints}, {"XEventsQueued", h_XEventsQueued}, {"XPeekEvent", h_XPeekEvent},
    {"XGetGeometry", h_XGetGeometry}, {"XChangeKeyboardControl", h_ret0}, {"XChangeProperty", h_ret0},
    {"XChangeWindowAttributes", h_ret0}, {"XConfigureWindow", h_ret0}, {"XFreeColormap", h_ret0},
    {"XLookupKeysym", h_ret0}, {"XRefreshKeyboardMapping", h_ret0}, {"XSelectInput", h_ret0},
    {"XSetWMNormalHints", h_ret0}, {"XUndefineCursor", h_ret0}, {"glXIsDirect", h_ret1},
    {"glXQueryVersion", h_glXQueryVersion}, {"glXWaitX", h_ret0}, {"gluProject", h_gluProject}, {"gluUnProject", h_gluUnProject},
    {"XPending", h_XPending}, {"XNextEvent", h_XNextEvent}, {"XTranslateCoordinates", h_XTranslateCoordinates},
    {"XLookupString", h_XLookupString}, {"XCreateColormap", h_retid}, {"XCreateBitmapFromData", h_retid},
    {"XCreatePixmapCursor", h_retid}, {"XCreateFontCursor", h_retid}, {"XDefineCursor", h_ret0},
    {"XFreePixmap", h_ret0}, {"XFree", h_ret0}, {"XFlush", h_ret0}, {"XSync", h_ret0},
    {"XMoveWindow", h_ret0}, {"XGrabPointer", h_ret0}, {"XWarpPointer", h_ret0},
    {"XSetInputFocus", h_ret0}, {"XSetStandardProperties", h_ret0}, {"XAutoRepeatOff", h_ret0},
    {"XKeysymToKeycode", h_ret0}, {"XMissingExtension", h_ret0}, {"XextAddDisplay", h_ret0},
    {"XextFindDisplay", h_ret0}, {"XextRemoveDisplay", h_ret0}, {"XextCreateExtension", h_ret0},
    {"_XEatData", h_ret0}, {"_XFlush", h_ret0}, {"_XRead", h_ret0}, {"_XReadPad", h_ret0},
    {"_XReply", h_ret0}, {"_XSend", h_ret0},
    {"XAutoRepeatOn", h_ret0}, {"XCloseDisplay", h_ret0}, {"XCreatePixmapFromBitmapData", h_retid},
    {"XDestroyWindow", h_ret0}, {"XFreeCursor", h_ret0}, {"XGetErrorText", h_XGetErrorText},
    {"XGrabKeyboard", h_ret0}, {"XInitThreads", h_ret1}, {"XInternAtom", h_XInternAtom},
    {"XLockDisplay", h_ret0}, {"XUnlockDisplay", h_ret0}, {"XParseColor", h_ret1},
    {"XQueryPointer", h_XQueryPointer}, {"XSetCloseDownMode", h_ret0}, {"XSetErrorHandler", h_ret0},
    {"XSetWMProperties", h_ret0}, {"XSetWMProtocols", h_ret1}, {"XStoreName", h_ret0},
    {"XStringListToTextProperty", h_ret1}, {"XUngrabKeyboard", h_ret0}, {"XUngrabPointer", h_ret0},
    {"glXChooseFBConfigSGIX", h_glXChooseFBConfig}, {"glXCreateGLXPbufferSGIX", h_glXCreateGLXPbufferSGIX},
    {"glXDestroyGLXPbufferSGIX", h_ret0}, {"glXCreateContextWithConfigSGIX", h_glXCreateContext},
    {"glXGetFBConfigAttribSGIX", h_glXGetFBConfigAttribSGIX}, {"glXGetCurrentContext", h_glXGetCurrentContext},
    {"glXGetCurrentDisplay", h_glXGetCurrentDisplay}, {"glXGetCurrentDrawable", h_glXGetCurrentDrawable},
    {"glXGetClientString", h_glXGetClientString}, {"glXQueryServerString", h_glXQueryServerString},
    {"glXQueryExtensionsString", h_glXQueryExtensionsString}, {"glXSwapIntervalSGI", h_ret0},
    {"cgGLIsProfileSupported", h_cgGLIsProfileSupported},
    {"glXChooseVisual", h_glXChooseVisual}, {"glXChooseFBConfig", h_glXChooseFBConfig},
    {"glXGetVisualFromFBConfig", h_glXGetVisualFromFBConfig}, {"glXCreatePbuffer", h_glXCreatePbuffer},
    {"glXDestroyPbuffer", h_ret0}, {"glXCreateContext", h_glXCreateContext},
    {"glXDestroyContext", h_ret0}, {"glXMakeCurrent", h_glXMakeCurrent},
    {"glXSwapBuffers", h_glXSwapBuffers}, {"glXQueryExtension", h_glXQueryExtension},
    {"glXGetProcAddressARB", h_glXGetProcAddressARB}, {"glXGetProcAddress", h_glXGetProcAddressARB},
    {NULL, NULL},
};

static void gl_check(const char *name)
{
    static int reported, in_begin;
    // glGetError is itself an error between glBegin and glEnd
    if (!strcmp(name, "glBegin")) in_begin = 1;
    if (!strcmp(name, "glEnd")) in_begin = 0;
    if (in_begin || !gl_open) return;   // nothing to ask before the window (OutRun 2 SP's first calls)
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;)
        if (reported < 50) {
            reported++;
            hle_log("GL error 0x%x after %s (frame %d)\n", e, name, frame);
        }
}

void hle_gfx_init(void)
{
    if (af_debug_env("LB_GL_CHECK")) gl_check_hook = gl_check;
    const char *s = af_debug_env("LB_SHOT_EVERY");
    if (s) shot_every = atoi(s);
    s = af_debug_env("LB_GL_TRACE");
    if (s) {
        // N, or N-M for a range of frames
        trace_frame = trace_last = atoi(s);
        if (strchr(s, '-')) trace_last = atoi(strchr(s, '-') + 1);
        // frame 0 is everything before the first swap
        if (trace_frame == 0) gl_trace = hle_trace = 1;
    }
    hle_register(gfx_table);
    hle_register(glut_table);
    hle_register(gfx_special);
    hle_register(gl_emulated);   // imported directly as well (OutRun 2 SP)
    hle_register(gl_gen_table);
}

// Back to the state before the first frame, for the next program (the GL
// context they used is gone with them).
void hle_gfx_reset(void)
{
    gl_open = 0;
    win_w = 640, win_h = 480;
    display = visualinfo = visual = fbconfigs = 0;
    array_buffer = element_buffer = 0;
    frame = 0;
    qhead = qtail = 0;
    memset(pbufs, 0, sizeof pbufs);
    npbufs = 0;
    cur_drawable = 0;
    glut_display = glut_idle = glut_keyboard = 0;
    glut_redisplay = 1;
    memset(gl_strings, 0, sizeof gl_strings);
    for (int i = 0; i < MAX_SHADERS; i++) free(shaders[i].src);
    memset(shaders, 0, sizeof shaders);
    memset(attached, 0, sizeof attached);
    memset(maps, 0, sizeof maps);
}
