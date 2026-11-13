// OpenAL 1.1 (libopenal.so.0: 2 Spicy, Ghost Squad Evolution's CRI ADX) on the runtime's software mixer
// (hle_audio.c mixes these sources with the sound card's voices).
// Buffers are copied out of guest memory as alBufferData gives them; a source
// plays its queue of buffers in turn, with its gain, pitch and looping, and
// the distance attenuation and left-right pan of OpenAL's default model
// (inverse distance, clamped) from its position against the listener's.
// One device and one context; errors are not tracked (alGetError: none).
#include "hle.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 48000
#define MAX_BUFFERS 4096
#define MAX_SOURCES 256
#define MAX_QUEUE 64

enum {
    AL_SOURCE_RELATIVE = 0x202, AL_PITCH = 0x1003, AL_POSITION = 0x1004, AL_DIRECTION = 0x1005,
    AL_VELOCITY = 0x1006, AL_LOOPING = 0x1007, AL_BUFFER = 0x1009, AL_GAIN = 0x100a,
    AL_MIN_GAIN = 0x100d, AL_MAX_GAIN = 0x100e, AL_ORIENTATION = 0x100f, AL_SOURCE_STATE = 0x1010,
    AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013, AL_STOPPED = 0x1014,
    AL_BUFFERS_QUEUED = 0x1015, AL_BUFFERS_PROCESSED = 0x1016, AL_REFERENCE_DISTANCE = 0x1020,
    AL_ROLLOFF_FACTOR = 0x1021, AL_MAX_DISTANCE = 0x1023,
    AL_FORMAT_MONO8 = 0x1100, AL_FORMAT_MONO16 = 0x1101, AL_FORMAT_STEREO8 = 0x1102, AL_FORMAT_STEREO16 = 0x1103,
};

typedef struct {
    int used;
    float *data;   // interleaved, 1 or 2 channels
    uint32_t frames, chans, rate;
} al_buffer;

typedef struct {
    int used, state, looping, relative;
    uint32_t queue[MAX_QUEUE];
    int queued, current;        // queue length; the buffer playing (those before it are processed)
    double pos;                 // in the current buffer, frames
    float gain, pitch, ref, rolloff, max_dist, min_gain, max_gain;
    float p[3];
} al_source;

static al_buffer buffers[MAX_BUFFERS];
static al_source sources[MAX_SOURCES];
static float listener_p[3], listener_gain = 1, listener_at[3] = {0, 0, -1}, listener_up[3] = {0, 1, 0};
static pthread_mutex_t al_lock = PTHREAD_MUTEX_INITIALIZER;

static al_buffer *buf(uint32_t id) { return id && id < MAX_BUFFERS && buffers[id].used ? &buffers[id] : NULL; }
static al_source *src(uint32_t id) { return id && id < MAX_SOURCES && sources[id].used ? &sources[id] : NULL; }

// ---- the mixer (hle_audio.c, under its lock) -------------------------------------------------
// the source's gains to the left and right
static void source_pan(const al_source *s, float *gl, float *gr)
{
    float g = s->gain * listener_gain, pan = 0;
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = s->relative ? s->p[i] : s->p[i] - listener_p[i];
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist > 0) {
        // inverse distance, clamped (OpenAL's default)
        float dc = dist < s->ref ? s->ref : dist > s->max_dist ? s->max_dist : dist;
        if (s->ref > 0) g *= s->ref / (s->ref + s->rolloff * (dc - s->ref));
        // across the listener: right = at x up
        float r[3] = {listener_at[1] * listener_up[2] - listener_at[2] * listener_up[1],
                      listener_at[2] * listener_up[0] - listener_at[0] * listener_up[2],
                      listener_at[0] * listener_up[1] - listener_at[1] * listener_up[0]};
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl > 0) pan = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (rl * dist);
    }
    if (g < s->min_gain) g = s->min_gain;
    if (g > s->max_gain) g = s->max_gain;
    float a = (pan + 1) * 0.785398f;   // constant power
    *gl = g * cosf(a) * 1.41421f;
    *gr = g * sinf(a) * 1.41421f;
}

void al_mix(float *out, int frames)
{
    pthread_mutex_lock(&al_lock);
    for (int n = 1; n < MAX_SOURCES; n++) {
        al_source *s = &sources[n];
        if (!s->used || s->state != AL_PLAYING) continue;
        float gl, gr;
        source_pan(s, &gl, &gr);
        for (int i = 0; i < frames; i++) {
            al_buffer *b = s->current < s->queued ? buf(s->queue[s->current]) : NULL;
            while (b && s->pos >= b->frames) {
                s->pos -= b->frames;
                if (s->current + 1 < s->queued) s->current++;
                else if (s->looping) s->current = 0;
                else {
                    s->current = s->queued;   // all processed
                    break;
                }
                b = buf(s->queue[s->current]);
            }
            if (!b) {
                s->state = AL_STOPPED;
                break;
            }
            if (!b->frames) {
                s->current++;
                continue;
            }
            uint32_t f = (uint32_t)s->pos;
            float l = b->data[f * b->chans], r = b->chans > 1 ? b->data[f * b->chans + 1] : l;
            out[2 * i] += l * gl;
            out[2 * i + 1] += r * gr;
            s->pos += (double)b->rate * s->pitch / OUT_RATE;
        }
    }
    pthread_mutex_unlock(&al_lock);
}

void al_reset(void)
{
    pthread_mutex_lock(&al_lock);
    for (int i = 0; i < MAX_BUFFERS; i++) free(buffers[i].data);
    memset(buffers, 0, sizeof buffers);
    memset(sources, 0, sizeof sources);
    memset(listener_p, 0, sizeof listener_p);
    listener_gain = 1;
    pthread_mutex_unlock(&al_lock);
}

// ---- ALC: one device, one context ----------------------------------------------------------------
static void h_alcOpenDevice(CPU *c) { hle_audio_open(); RET(0x10); }
static void h_alcCloseDevice(CPU *c) { RET(1); }
static void h_alcCreateContext(CPU *c) { RET(0x20); }
static void h_alcMakeContextCurrent(CPU *c) { RET(1); }
static void h_alcNone(CPU *c) { RET(0); }

// ---- buffers ---------------------------------------------------------------------------------------
static void h_alGenBuffers(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    for (uint32_t k = 0; k < ARG(0); k++) {
        uint32_t id = 0;
        for (uint32_t i = 1; i < MAX_BUFFERS; i++)
            if (!buffers[i].used) {
                buffers[i] = (al_buffer){.used = 1};
                id = i;
                break;
            }
        if (!id) hle_log("openal: out of buffers\n");
        wr32(ARG(1) + 4 * k, id);
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alDeleteBuffers(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    for (uint32_t k = 0; k < ARG(0); k++) {
        al_buffer *b = buf(rd32(ARG(1) + 4 * k));
        if (b) {
            free(b->data);
            memset(b, 0, sizeof *b);
        }
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alIsBuffer(CPU *c) { RET(buf(ARG(0)) != NULL); }
// (buffer, format, data, size, frequency)
static void h_alBufferData(CPU *c)
{
    uint32_t fmt = ARG(1), a = ARG(2), size = ARG(3), rate = ARG(4);
    uint32_t chans = fmt == AL_FORMAT_STEREO8 || fmt == AL_FORMAT_STEREO16 ? 2 : 1;
    uint32_t bytes = fmt == AL_FORMAT_MONO16 || fmt == AL_FORMAT_STEREO16 ? 2 : 1;
    uint32_t frames = size / (chans * bytes);
    float *d = malloc(sizeof(float) * chans * (frames ? frames : 1));
    for (uint32_t i = 0; i < frames * chans; i++)
        d[i] = bytes == 2 ? (int16_t)rd16(a + 2 * i) / 32768.0f : (rd8(a + i) - 128) / 128.0f;
    pthread_mutex_lock(&al_lock);
    al_buffer *b = buf(ARG(0));
    if (b) {
        free(b->data);
        b->data = d, b->frames = frames, b->chans = chans, b->rate = rate ? rate : OUT_RATE;
    } else {
        free(d);
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}

// ---- sources ---------------------------------------------------------------------------------------
static void source_init(al_source *s)
{
    memset(s, 0, sizeof *s);
    s->used = 1, s->state = AL_INITIAL;
    s->gain = s->pitch = s->ref = s->rolloff = s->max_gain = 1;
    s->max_dist = 3.4e38f;
}
static void h_alGenSources(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    for (uint32_t k = 0; k < ARG(0); k++) {
        uint32_t id = 0;
        for (uint32_t i = 1; i < MAX_SOURCES; i++)
            if (!sources[i].used) {
                source_init(&sources[i]);
                id = i;
                break;
            }
        if (!id) hle_log("openal: out of sources\n");
        wr32(ARG(1) + 4 * k, id);
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alDeleteSources(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    for (uint32_t k = 0; k < ARG(0); k++) {
        al_source *s = src(rd32(ARG(1) + 4 * k));
        if (s) s->used = 0;
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alIsSource(CPU *c) { RET(src(ARG(0)) != NULL); }

static void set_f(al_source *s, uint32_t p, const float *v)
{
    switch (p) {
    case AL_POSITION: memcpy(s->p, v, sizeof s->p); break;
    case AL_GAIN: s->gain = v[0]; break;
    case AL_PITCH: s->pitch = v[0] > 0 ? v[0] : 1; break;
    case AL_REFERENCE_DISTANCE: s->ref = v[0]; break;
    case AL_ROLLOFF_FACTOR: s->rolloff = v[0]; break;
    case AL_MAX_DISTANCE: s->max_dist = v[0]; break;
    case AL_MIN_GAIN: s->min_gain = v[0]; break;
    case AL_MAX_GAIN: s->max_gain = v[0]; break;
    default: break;   // velocity, direction: no doppler, no cones
    }
}
static void h_alSourcefv(CPU *c)
{
    float v[3] = {u2f(rd32(ARG(2))), u2f(rd32(ARG(2) + 4)), u2f(rd32(ARG(2) + 8))};
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s) set_f(s, ARG(1), v);
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSource3f(CPU *c)
{
    float v[3] = {ARGF(2), ARGF(3), ARGF(4)};
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s) set_f(s, ARG(1), v);
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSourcef(CPU *c)
{
    float v[3] = {ARGF(2), 0, 0};
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s) set_f(s, ARG(1), v);
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSourcei(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s) {
        switch (ARG(1)) {
        case AL_BUFFER:   // one buffer, or none
            s->queued = ARG(2) ? 1 : 0;
            s->queue[0] = ARG(2);
            s->current = 0, s->pos = 0;
            break;
        case AL_LOOPING: s->looping = ARG(2) != 0; break;
        case AL_SOURCE_RELATIVE: s->relative = ARG(2) != 0; break;
        default: break;
        }
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alGetSourcei(CPU *c)
{
    int32_t v = 0;
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s) {
        switch (ARG(1)) {
        case AL_SOURCE_STATE: v = s->state; break;
        case AL_BUFFERS_QUEUED: v = s->queued; break;
        case AL_BUFFERS_PROCESSED: v = s->state == AL_STOPPED ? s->queued : s->current; break;
        case AL_BUFFER: v = s->queued ? (int32_t)s->queue[s->current < s->queued ? s->current : 0] : 0; break;
        case AL_LOOPING: v = s->looping; break;
        case AL_SOURCE_RELATIVE: v = s->relative; break;
        default: break;
        }
    }
    pthread_mutex_unlock(&al_lock);
    if (ARG(2)) wr32(ARG(2), (uint32_t)v);
    RET(0);
}

static void play(al_source *s)
{
    if (s->state != AL_PAUSED) s->current = 0, s->pos = 0;
    s->state = AL_PLAYING;
}
static void h_alSourcePlay(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s) play(s);
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSourcePlayv(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    for (uint32_t k = 0; k < ARG(0); k++) {
        al_source *s = src(rd32(ARG(1) + 4 * k));
        if (s) play(s);
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSourcePause(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s && s->state == AL_PLAYING) s->state = AL_PAUSED;
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSourceStop(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    if (s) s->state = AL_STOPPED, s->current = s->queued, s->pos = 0;
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSourceQueueBuffers(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    for (uint32_t k = 0; s && k < ARG(1) && s->queued < MAX_QUEUE; k++) {
        // queued after a stop or a run to the end: what was played is processed
        s->queue[s->queued++] = rd32(ARG(2) + 4 * k);
        if (s->state == AL_STOPPED && s->current >= s->queued - 1) s->current = s->queued - 1;
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alSourceUnqueueBuffers(CPU *c)
{
    pthread_mutex_lock(&al_lock);
    al_source *s = src(ARG(0));
    uint32_t n = 0;
    if (s) {
        int processed = s->state == AL_STOPPED ? s->queued : s->current;
        n = ARG(1) < (uint32_t)processed ? ARG(1) : (uint32_t)processed;
        for (uint32_t k = 0; k < n; k++) wr32(ARG(2) + 4 * k, s->queue[k]);
        memmove(s->queue, s->queue + n, sizeof s->queue[0] * (size_t)(s->queued - (int)n));
        s->queued -= (int)n;
        s->current -= (int)n;
        if (s->current < 0) s->current = 0;
    }
    pthread_mutex_unlock(&al_lock);
    RET(0);
}

// ---- the listener ----------------------------------------------------------------------------------
static void listener_f(uint32_t p, const float *v)
{
    if (p == AL_POSITION) memcpy(listener_p, v, sizeof listener_p);
    else if (p == AL_GAIN) listener_gain = v[0];
    else if (p == AL_ORIENTATION) memcpy(listener_at, v, sizeof listener_at), memcpy(listener_up, v + 3, sizeof listener_up);
}
static void h_alListenerf(CPU *c)
{
    float v[3] = {ARGF(1), 0, 0};
    pthread_mutex_lock(&al_lock);
    listener_f(ARG(0), v);
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alListener3f(CPU *c)
{
    float v[3] = {ARGF(1), ARGF(2), ARGF(3)};
    pthread_mutex_lock(&al_lock);
    listener_f(ARG(0), v);
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alListenerfv(CPU *c)
{
    float v[6];
    for (int i = 0; i < 6; i++) v[i] = u2f(rd32(ARG(1) + 4 * (uint32_t)i));
    pthread_mutex_lock(&al_lock);
    listener_f(ARG(0), v);
    pthread_mutex_unlock(&al_lock);
    RET(0);
}
static void h_alGetError(CPU *c) { RET(0); }   // AL_NO_ERROR

static const hle_entry openal_table[] = {
    {"alcOpenDevice", h_alcOpenDevice}, {"alcCloseDevice", h_alcCloseDevice},
    {"alcCreateContext", h_alcCreateContext}, {"alcMakeContextCurrent", h_alcMakeContextCurrent},
    {"alcProcessContext", h_alcNone}, {"alcDestroyContext", h_alcNone}, {"alcGetError", h_alcNone},
    {"alcSuspendContext", h_alcNone},
    {"alGenBuffers", h_alGenBuffers}, {"alDeleteBuffers", h_alDeleteBuffers}, {"alIsBuffer", h_alIsBuffer},
    {"alBufferData", h_alBufferData},
    {"alGenSources", h_alGenSources}, {"alDeleteSources", h_alDeleteSources}, {"alIsSource", h_alIsSource},
    {"alSourcei", h_alSourcei}, {"alSourcef", h_alSourcef}, {"alSourcefv", h_alSourcefv}, {"alSource3f", h_alSource3f},
    {"alGetSourcei", h_alGetSourcei}, {"alSourcePlay", h_alSourcePlay}, {"alSourcePlayv", h_alSourcePlayv},
    {"alSourcePause", h_alSourcePause}, {"alSourceStop", h_alSourceStop},
    {"alSourceQueueBuffers", h_alSourceQueueBuffers}, {"alSourceUnqueueBuffers", h_alSourceUnqueueBuffers},
    {"alListenerf", h_alListenerf}, {"alListener3f", h_alListener3f}, {"alListenerfv", h_alListenerfv}, {"alGetError", h_alGetError},
    {NULL, NULL},
};

__attribute__((constructor)) static void reg(void) { hle_register(openal_table); }
