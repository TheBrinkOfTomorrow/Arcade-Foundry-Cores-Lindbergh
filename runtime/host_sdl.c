// lindbergh-run's host: an SDL window with the game's GL context, keyboard and
// mouse as the cabinet's controls, and an SDL audio device pulling the mixer.
// Controls: mouse = player 1 gun, left button trigger, right button reload,
// 1/2 start, 5/6 coin, F2 test, F1 service.
#include "af_debug.h"
#include "host.h"
#include "hle.h"
#include "input.h"
#include <SDL.h>
#include <unistd.h>

static SDL_Window *win;
static SDL_GLContext ctx;

void host_gl_open(int w, int h)
{
    if (win) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        hle_log("SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win = SDL_CreateWindow("Arcade Foundry - Lindbergh", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) {
        hle_log("SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        hle_log("SDL_GL_CreateContext: %s\n", SDL_GetError());
        exit(1);
    }
}

void host_gl_release(void) { SDL_GL_MakeCurrent(win, NULL); }
void host_gl_current(void) { SDL_GL_MakeCurrent(win, ctx); }
unsigned host_gl_framebuffer(void) { return 0; }
void host_thread_exit(void) {}
void host_gl_size(int *w, int *h) { SDL_GL_GetDrawableSize(win, w, h); }
void *host_gl_proc(const char *name) { return SDL_GL_GetProcAddress(name); }

void host_present(void)
{
    SDL_GL_SwapWindow(win);
    // the game paces itself, the test menu relies on the cabinet's vsync: hold 60 Hz
    static uint64_t next;
    uint64_t f = SDL_GetPerformanceFrequency(), now = SDL_GetPerformanceCounter();
    if (next > now) SDL_Delay((uint32_t)((next - now) * 1000 / f));
    next = (next > now ? next : now) + f / 60;
}

static void key(SDL_Keycode k, int down)
{
    uint16_t *p1 = &jvs_in.player[0], *p2 = &jvs_in.player[1];
    uint16_t bit = 0, *dst = p1;
    switch (k) {
    case SDLK_1: bit = SW_START; break;
    case SDLK_2: bit = SW_START; dst = p2; break;
    case SDLK_UP: bit = SW_UP; break;
    case SDLK_DOWN: bit = SW_DOWN; break;
    case SDLK_LEFT: bit = SW_LEFT; break;
    case SDLK_RIGHT: bit = SW_RIGHT; break;
    case SDLK_F1: bit = SW_SERVICE; break;
    case SDLK_F2:
        if (down) jvs_in.system |= SYS_TEST; else jvs_in.system &= ~SYS_TEST;
        return;
    case SDLK_5: if (down) jvs_in.coins[0]++; return;
    case SDLK_6: if (down) jvs_in.coins[1]++; return;
    default: return;
    }
    if (down) *dst |= bit; else *dst &= ~bit;
}

void host_input(void)
{
    SDL_Event e;
    int w = 1, h = 1;
    if (win) SDL_GetWindowSize(win, &w, &h);
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: exit(INPUT_QUIT_STATUS);     // tells tools/cabinet.sh the player closed the window
        case SDL_KEYDOWN: if (!e.key.repeat) key(e.key.keysym.sym, 1); break;
        case SDL_KEYUP: key(e.key.keysym.sym, 0); break;
        case SDL_MOUSEMOTION:
            jvs_in.analog[0] = (uint16_t)(e.motion.x * 65535 / (w > 1 ? w - 1 : 1));
            jvs_in.analog[1] = (uint16_t)(e.motion.y * 65535 / (h > 1 ? h - 1 : 1));
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            uint16_t bit = e.button.button == SDL_BUTTON_LEFT ? SW_1 : e.button.button == SDL_BUTTON_RIGHT ? SW_2 : SW_3;
            if (e.type == SDL_MOUSEBUTTONDOWN) jvs_in.player[0] |= bit; else jvs_in.player[0] &= ~bit;
            break;
        }
        }
    }
}

static void callback(void *u, Uint8 *stream, int len)
{
    (void)u;
    audio_mix((int16_t *)stream, len / 4);
}

int host_audio_open(int rate)
{
    if (af_debug_env("LB_FRAME_CLOCK")) return 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        // no sound hardware: the dummy driver still runs the mixer in real time
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = callback;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    hle_log("audio: %s\n", dev ? SDL_GetCurrentAudioDriver() : SDL_GetError());
    if (!dev) return 0;
    SDL_PauseAudioDevice(dev, 0);
    return 1;
}

void host_audio_push(const int16_t *stereo, int frames) { (void)stereo, (void)frames; }

void host_exit(int status)
{
    fflush(NULL);
    _exit(status);
}

void host_fatal(const char *why)
{
    hle_log("fatal: %s\n", why);
    abort();
}
