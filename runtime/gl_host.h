// Host OpenGL access for the generated GL bridge.
#pragma once
#ifdef LB_IOS
// gl4es: desktop GL's declarations, then each name mapped to the gl4es_
// function that implements it (gl4es_names.h, generated)
#include <GL/gl.h>
#include <GL/glext.h>
#include "gl4es_names.h"
#else
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#endif
#include "hle.h"

#ifndef APIENTRY
#define APIENTRY
#endif

void *gl_host_proc(const char *name);
// a vertex-array pointer is an offset while a buffer object is bound
void *gl_array_ptr(uint32_t p);
void *gl_element_ptr(uint32_t p);
extern int gl_trace;
extern int gl_open;   // the program has its window and GL context
void gl_trace_call(const char *name, CPU *c);
extern const hle_entry gl_gen_table[];
