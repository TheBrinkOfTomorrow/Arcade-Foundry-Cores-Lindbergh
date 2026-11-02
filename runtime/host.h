// What the runtime needs from the program hosting it: lindbergh-run's SDL
// window (host_sdl.c), or the core library a front end drives a frame at a
// time (core_api.c).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Graphics. The game's window is w x h; the host makes a desktop OpenGL 2.1
// (compatibility) context current on the calling thread.
void host_gl_open(int w, int h);
void host_gl_release(void);                  // no context current on this thread
void host_gl_current(void);                  // the context current again
unsigned host_gl_framebuffer(void);          // what the game's window draws to: 0 for a real window
void host_gl_size(int *w, int *h);           // that framebuffer's size
void *host_gl_proc(const char *name);
void host_thread_exit(void);                 // a guest thread ends: release the context if it has it
// The game has finished a frame: show it. A core hands it to the front end
// here and returns when the front end asks for the next one.
void host_present(void);

// Input: refresh jvs_in (input.h) from the host's devices. Once per frame.
void host_input(void);

// Sound: 1 if the host pulls the mixer (audio_mix) from its own audio thread;
// 0 to have it mixed once per video frame and handed over with host_audio_push.
int host_audio_open(int rate);
void host_audio_push(const int16_t *stereo, int frames);
void audio_mix(int16_t *stereo, int frames);    // the mixer, for the host to pull

// The guest has ended (exit, or main returned) with this status.
void host_exit(int status);
// The game hit something the runtime cannot run; why. Does not return.
void host_fatal(const char *why);

#ifdef __cplusplus
}
#endif
