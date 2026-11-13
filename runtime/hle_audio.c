// libsegaapi: the Lindbergh sound card's voice API, on a software mixer the
// host plays (host.h).
// A voice plays 8-bit unsigned or 16-bit signed PCM straight out of guest
// memory, with loop points, an end offset, per-channel volume, per-send
// routing to the cabinet's speaker outputs, and the attenuation and pitch
// synth parameters. Effects sends (FX slots) are not
// mixed. Games such as HotD4 also pace movie playback by a voice's position,
// so position and status advance in real time even without an audio device.
#include "af_debug.h"
#include "hle.h"
#include "host.h"
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define OUT_RATE 48000
#define MAX_VOICES 256
#define MAX_CH 6
#define MAX_SENDS 7
#define VOL_MAX 4294967295.0
#define UNUSED_SEND 0xffff0001u
#define SYNTH_PARAMS 32
enum { SYNTH_ATTENUATION = 0, SYNTH_PITCH = 1 };   // 0.1 dB of attenuation; cents

enum { STATUS_STOP = 0, STATUS_ACTIVE = 1, STATUS_PAUSE = 2, STATUS_INVALID = -1 };
// PlayWithSetup's voice settings
enum { VOICE_START_LOOP = 0x100, VOICE_END_LOOP, VOICE_END_OFFSET, VOICE_PLAY_POSITION, VOICE_LOOP_STATE };
enum { FMT_U8 = 0x0004, FMT_S16 = 0x0020 };

typedef struct {
    int used;
    uint32_t handle, data, size, user;
    uint32_t rate, format, chans;
    uint32_t start_loop, end_loop, end_offset;
    int loop, status;
    uint64_t pos;                  // in frames, 32.32 fixed point
    double chan_vol[MAX_CH];
    double send_level[MAX_CH][MAX_SENDS];
    uint32_t routing[MAX_CH][MAX_SENDS];
    double atten;                  // gain from the ATTENUATION synth parameter
    double pitch;                  // rate factor from the PITCH synth parameter
    int32_t synth[SYNTH_PARAMS];   // the synth parameters as set, for GetSynthParam
} voice;

static voice voices[MAX_VOICES];
static double io_vol[16];
static FILE *wav;
static uint32_t wav_bytes;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int opened;

static voice *find(uint32_t h)
{
    if (!h) return NULL;
    uint32_t i = rd32(h);
    return i < MAX_VOICES && voices[i].used && voices[i].handle == h ? &voices[i] : NULL;
}

static uint32_t frame_bytes(const voice *v) { return (v->format == FMT_U8 ? 1 : 2) * (v->chans ? v->chans : 1); }

static uint32_t end_frame(const voice *v)
{
    uint32_t end = v->end_offset && v->end_offset < v->size ? v->end_offset : v->size;
    return end / frame_bytes(v);
}

static float sample(const voice *v, uint32_t frame, uint32_t ch)
{
    uint32_t a = v->data + frame * frame_bytes(v) + ch * (v->format == FMT_U8 ? 1 : 2);
    if (v->format == FMT_U8) return (rd8(a) - 128) / 128.0f;
    return (int16_t)rd16(a) / 32768.0f;
}

// speaker ports: 0 front left, 1 front right, 2 centre, 3 LFE, 4 rear left, 5 rear right
static void port_gain(uint32_t port, double g, double *l, double *r)
{
    switch (port) {
    case 0: *l += g; break;
    case 1: *r += g; break;
    case 2: *l += g * 0.7071; *r += g * 0.7071; break;
    case 4: *l += g * 0.7071; break;
    case 5: *r += g * 0.7071; break;
    default: break;            // LFE, effects slots and unused sends
    }
}

static void mix(float *out, int frames)
{
    memset(out, 0, sizeof(float) * 2 * frames);
    pthread_mutex_lock(&lock);
    for (int n = 0; n < MAX_VOICES; n++) {
        voice *v = &voices[n];
        if (!v->used || v->status != STATUS_ACTIVE || !v->size || !v->rate) continue;
        uint32_t chans = v->chans ? v->chans : 1;
        if (chans > MAX_CH) chans = MAX_CH;
        double gl[MAX_CH] = {0}, gr[MAX_CH] = {0};
        for (uint32_t ch = 0; ch < chans; ch++)
            for (int s = 0; s < MAX_SENDS; s++)
                if (v->routing[ch][s] != UNUSED_SEND)
                    port_gain(v->routing[ch][s], v->send_level[ch][s] * v->chan_vol[ch] * v->atten *
                              (v->routing[ch][s] < 16 ? io_vol[v->routing[ch][s]] : 1.0), &gl[ch], &gr[ch]);
        uint64_t step = (uint64_t)((double)v->rate * v->pitch / OUT_RATE * 4294967296.0);
        uint32_t end = end_frame(v);
        uint32_t ls = v->start_loop / frame_bytes(v), le = v->end_loop / frame_bytes(v);
        if (!le || le > end) le = end;
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)(v->pos >> 32);
            uint32_t limit = v->loop ? le : end;
            if (f >= limit) {
                if (v->loop && le > ls) {
                    v->pos -= (uint64_t)(le - ls) << 32;
                    f = (uint32_t)(v->pos >> 32);
                } else {
                    v->status = STATUS_STOP;
                    break;
                }
            }
            for (uint32_t ch = 0; ch < chans; ch++) {
                float x = sample(v, f, ch);
                out[2 * i] += (float)(x * gl[ch]);
                out[2 * i + 1] += (float)(x * gr[ch]);
            }
            v->pos += step;
        }
    }
    al_mix(out, frames);   // OpenAL's sources (hle_openal.c)
    alsa_mix(out, frames);   // ALSA's PCM (hle_alsa.c)
    pthread_mutex_unlock(&lock);
}

void audio_mix(int16_t *o, int frames)
{
    float buf[4096 * 2];
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        mix(buf, n);
        // a 5.1 cabinet mix folded to stereo: leave headroom, then soft-limit
        for (int i = 0; i < 2 * n; i++) {
            float x = buf[i] * 0.6f;
            x = x > 0.5f ? 0.5f + 0.5f * tanhf((x - 0.5f) * 2) : x < -0.5f ? -0.5f + 0.5f * tanhf((x + 0.5f) * 2) : x;
            *o++ = (int16_t)(x * 32767);
        }
        if (wav) wav_bytes += (uint32_t)fwrite(o - 2 * n, 1, (size_t)n * 4, wav);
        frames -= n;
    }
}

static void wav_header(void)
{
    uint32_t hdr[11] = {0x46464952, 36 + wav_bytes, 0x45564157, 0x20746d66, 16, 0x00020001,
                        OUT_RATE, OUT_RATE * 4, 0x00100004, 0x61746164, wav_bytes};
    fseek(wav, 0, SEEK_SET);
    fwrite(hdr, 4, 11, wav);
    fflush(wav);
    fseek(wav, 0, SEEK_END);
}

// Without a host audio thread (a core, or LB_FRAME_CLOCK=1) sound is made
// once per video frame, as much as the time since the last frame: the sound
// board plays in real time whatever the game's frame rate. Before, a frame
// counted as a whole number of 60ths of a second (HotD4's 30 frames/s
// cutscenes as two): at 40 or 50 frames/s that made a third more or a sixth
// less sound than time passed, the front end's queue filled and was dropped,
// or ran dry (Race TV on Linux: stutter, sounds over each other). A gap of
// more than 1/10 s (a pause, the window dragged) counts as one frame.
static int frame_clock;
static double last;      // when the last frame's sound was made
static double owed;      // the fraction of an output frame carried to the next

void audio_frame(void)
{
    if (af_debug_env("LB_VOICELOG")) {   // the voices playing, once a second
        static int n;
        if (++n % 60 == 0) {
            int act = 0;
            pthread_mutex_lock(&lock);
            for (int i = 0; i < MAX_VOICES; i++) act += voices[i].used && voices[i].status == STATUS_ACTIVE;
            pthread_mutex_unlock(&lock);
            hle_log("voices: %d playing\n", act);
        }
    }
    if (!frame_clock || clk_render) return;   // render mode: audio_render_frame
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    double dt = last > 0 ? now - last : 1.0 / 60;
    last = now;
    if (dt < 0 || dt > 0.1) dt = 1.0 / 60;
    owed += dt * OUT_RATE;
    int n = (int)owed;
    owed -= n;
    static int16_t buf[OUT_RATE / 10 * 2 + 2];
    if (n > OUT_RATE / 10) n = OUT_RATE / 10;
    if (n > 0) {
        audio_mix(buf, n);
        host_audio_push(buf, n);
    }
}

// Render mode (clock.c): run_frame makes each frame's sound, exactly 1/60 s
// (800 samples at 48 kHz), whether the game presented a picture in it or not
void audio_render_frame(void)
{
    if (!frame_clock) return;
    static int16_t buf[OUT_RATE / 60 * 2];
    audio_mix(buf, OUT_RATE / 60);
    host_audio_push(buf, OUT_RATE / 60);
}

static void audio_open(void)
{
    if (opened) return;
    opened = 1;
    for (int i = 0; i < 16; i++) io_vol[i] = 1.0;
    const char *w = af_debug_env("LB_AUDIO_WAV");
    if (w && (wav = fopen(w, "wb"))) wav_header();
    frame_clock = !host_audio_open(OUT_RATE);
    if (frame_clock) hle_log("audio: advancing with video frames\n");
}

void hle_audio_reset(void)
{
    if (wav) {
        wav_header();
        fclose(wav);
        wav = NULL;
    }
    wav_bytes = 0;
    memset(voices, 0, sizeof voices);
    opened = frame_clock = 0;
    last = owed = 0;
    al_reset();
    alsa_reset();
}

// OpenAL's device opening (hle_openal.c): the mixer starts as for the sound card
void hle_audio_open(void) { audio_open(); }

// ---- API -----------------------------------------------------------------------------
#define VOICE(h) voice *v = find(h); if (!v) RET(0x8000a012u)   // SEGA_ERROR_BAD_HANDLE

static void h_init(CPU *c) { audio_open(); RET(0); }
static void h_exit(CPU *c)
{
    if (wav) wav_header();
    RET(0);
}
static void h_reset(CPU *c)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].status = STATUS_STOP;
    pthread_mutex_unlock(&lock);
    RET(0);
}

// CreateBuffer(BufferConfig *cfg, callback, flags, void **out)
// BufferConfig: priority, rate, format, chans, reserved, user, {size, offset, data}
static void h_create(CPU *c)
{
    uint32_t cfg = ARG(0), flags = ARG(2), out = ARG(3);
    audio_open();
    pthread_mutex_lock(&lock);
    int i;
    for (i = 0; i < MAX_VOICES && voices[i].used; i++);
    if (i == MAX_VOICES) {
        pthread_mutex_unlock(&lock);
        RET(0x8000a00du);      // SEGA_ERROR_NO_RESOURCES
    }
    voice *v = &voices[i];
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->handle = hostdata_alloc(16);
    wr32(v->handle, (uint32_t)i);
    v->rate = rd32(cfg + 4);
    v->format = rd32(cfg + 8);
    v->chans = rd32(cfg + 12);
    v->user = rd32(cfg + 20);
    v->size = rd32(cfg + 24);
    uint32_t data = rd32(cfg + 32);
    if (!((flags & 6) && data)) data = guest_calloc(1, v->size ? v->size : 1);
    v->data = data;
    wr32(cfg + 28, 0);
    wr32(cfg + 32, data);
    v->end_offset = v->size;
    v->atten = 1.0;
    v->pitch = 1.0;
    for (int ch = 0; ch < MAX_CH; ch++) {
        v->chan_vol[ch] = 1.0;
        for (int s = 0; s < MAX_SENDS; s++) v->routing[ch][s] = UNUSED_SEND;
    }
    // the card's default routing: send 0 to the front left, send 1 to the front right
    // (OutRun 2 SP pans its engine with those two sends and routes only the others);
    // until the game sets levels, mono sits in the middle and stereo left/right
    for (int ch = 0; ch < MAX_CH; ch++) {
        v->routing[ch][0] = 0;
        v->routing[ch][1] = 1;
    }
    if (v->chans == 1) v->send_level[0][0] = v->send_level[0][1] = 0.7071;
    else v->send_level[0][0] = v->send_level[1][1] = 1.0;
    pthread_mutex_unlock(&lock);
    if (out) wr32(out, v->handle);
    RET(0);
}

static void h_destroy(CPU *c)
{
    VOICE(ARG(0));
    pthread_mutex_lock(&lock);
    v->used = 0;
    pthread_mutex_unlock(&lock);
    RET(0);
}

static void h_play(CPU *c) { VOICE(ARG(0)); v->status = STATUS_ACTIVE; RET(0); }
static void h_pause(CPU *c) { VOICE(ARG(0)); if (v->status == STATUS_ACTIVE) v->status = STATUS_PAUSE; RET(0); }
static void h_stop(CPU *c)
{
    VOICE(ARG(0));
    pthread_mutex_lock(&lock);
    v->status = STATUS_STOP;
    v->pos = 0;
    pthread_mutex_unlock(&lock);
    RET(0);
}
static void h_status(CPU *c) { voice *v = find(ARG(0)); RET(v ? v->status : STATUS_INVALID); }

static void h_get_pos(CPU *c)
{
    voice *v = find(ARG(0));
    if (!v) RET(0);
    uint32_t p = (uint32_t)(v->pos >> 32) * frame_bytes(v);
    RET(v->size ? p % v->size : 0);
}
static void h_set_pos(CPU *c)
{
    VOICE(ARG(0));
    pthread_mutex_lock(&lock);
    v->pos = (uint64_t)(ARG(1) / frame_bytes(v)) << 32;
    pthread_mutex_unlock(&lock);
    RET(0);
}

static void h_rate(CPU *c) { VOICE(ARG(0)); v->rate = ARG(1); RET(0); }
static void h_chan_vol(CPU *c)
{
    VOICE(ARG(0));
    if (ARG(1) < MAX_CH) v->chan_vol[ARG(1)] = ARG(2) / VOL_MAX;
    RET(0);
}
static void h_send_level(CPU *c)
{
    VOICE(ARG(0));
    if (ARG(1) < MAX_CH && ARG(2) < MAX_SENDS) v->send_level[ARG(1)][ARG(2)] = ARG(3) / VOL_MAX;
    RET(0);
}
static void h_send_routing(CPU *c)
{
    VOICE(ARG(0));
    if (ARG(1) < MAX_CH && ARG(2) < MAX_SENDS) v->routing[ARG(1)][ARG(2)] = ARG(3);
    RET(0);
}
static void h_start_loop(CPU *c) { VOICE(ARG(0)); v->start_loop = ARG(1); RET(0); }
static void h_end_loop(CPU *c) { VOICE(ARG(0)); v->end_loop = ARG(1); RET(0); }
static void h_end_offset(CPU *c) { VOICE(ARG(0)); v->end_offset = ARG(1); RET(0); }
static void h_loop(CPU *c) { VOICE(ARG(0)); v->loop = ARG(1) != 0; RET(0); }
// release starts the voice's release envelope; without envelopes it ends the note
static void h_release(CPU *c) { VOICE(ARG(0)); if (ARG(1)) v->status = STATUS_STOP; RET(0); }
// the synth parameters the mixer uses: attenuation and pitch (OutRun 2 SP's engine
// moves its pitch every frame); the others are kept to be read back
static void set_synth(voice *v, uint32_t param, int32_t value)
{
    if (param < SYNTH_PARAMS) v->synth[param] = value;
    if (param == SYNTH_ATTENUATION) v->atten = pow(10.0, -value / 200.0);
    if (param == SYNTH_PITCH) v->pitch = pow(2.0, value / 1200.0);
}
static void h_synth(CPU *c)
{
    VOICE(ARG(0));
    set_synth(v, ARG(1), (int32_t)ARG(2));
    RET(0);
}
// (handle, count, {param, value}[count])
static void h_synth_multiple(CPU *c)
{
    VOICE(ARG(0));
    for (uint32_t i = 0; i < ARG(1); i++) set_synth(v, rd32(ARG(2) + 8 * i), (int32_t)rd32(ARG(2) + 8 * i + 4));
    RET(0);
}
// (handle, route count, routes, send-level count, levels, voice-parameter count,
// parameters, synth-parameter count, synth parameters): set up, then play
// routes and levels are {channel, send, destination or level}
static void h_play_with_setup(CPU *c)
{
    VOICE(ARG(0));
    for (uint32_t i = 0; i < ARG(1); i++) {
        uint32_t e = ARG(2) + 12 * i, ch = rd32(e), send = rd32(e + 4);
        if (ch < MAX_CH && send < MAX_SENDS) v->routing[ch][send] = rd32(e + 8);
    }
    for (uint32_t i = 0; i < ARG(3); i++) {
        uint32_t e = ARG(4) + 12 * i, ch = rd32(e), send = rd32(e + 4);
        if (ch < MAX_CH && send < MAX_SENDS) v->send_level[ch][send] = rd32(e + 8) / VOL_MAX;
    }
    // the voice's own settings, {ioctl, value, 0} each: Sega Race TV plays every
    // sound this way out of one 48 MB sample bank, and without them each sound
    // started at the bank's start and ran on through the others
    for (uint32_t i = 0; i < ARG(5); i++) {
        uint32_t e = ARG(6) + 12 * i, value = rd32(e + 4);
        pthread_mutex_lock(&lock);
        switch (rd32(e)) {
        case VOICE_START_LOOP: v->start_loop = value; break;
        case VOICE_END_LOOP: v->end_loop = value; break;
        case VOICE_END_OFFSET: v->end_offset = value; break;
        case VOICE_PLAY_POSITION: v->pos = (uint64_t)(value / frame_bytes(v)) << 32; break;
        case VOICE_LOOP_STATE: v->loop = value != 0; break;
        }
        pthread_mutex_unlock(&lock);
    }
    for (uint32_t i = 0; i < ARG(7); i++) set_synth(v, rd32(ARG(8) + 8 * i), (int32_t)rd32(ARG(8) + 8 * i + 4));
    v->status = STATUS_ACTIVE;
    RET(0);
}
// (handle, param): the value is the return value (there is no third argument;
// writing through one wrote the value to wherever the stack pointed)
static void h_get_synth(CPU *c)
{
    voice *v = find(ARG(0));
    RET(v && ARG(1) < SYNTH_PARAMS ? (uint32_t)v->synth[ARG(1)] : 0);
}
static void h_io_vol(CPU *c)
{
    if (ARG(0) < 16) io_vol[ARG(0)] = ARG(1) / VOL_MAX;
    RET(0);
}
static void h_update(CPU *c) { RET(0); }      // voices read guest memory as they play
static void h_get_user(CPU *c) { voice *v = find(ARG(0)); RET(v ? v->user : 0); }
static void h_set_user(CPU *c) { VOICE(ARG(0)); v->user = ARG(1); RET(0); }

static const hle_entry audio_table[] = {
    {"SEGAAPI_Init", h_init}, {"SEGAAPI_Exit", h_exit}, {"SEGAAPI_Reset", h_reset},
    {"SEGAAPI_CreateBuffer", h_create}, {"SEGAAPI_DestroyBuffer", h_destroy},
    {"SEGAAPI_Play", h_play}, {"SEGAAPI_Pause", h_pause}, {"SEGAAPI_Stop", h_stop},
    {"SEGAAPI_GetPlaybackStatus", h_status},
    {"SEGAAPI_GetPlaybackPosition", h_get_pos}, {"SEGAAPI_SetPlaybackPosition", h_set_pos},
    {"SEGAAPI_SetSampleRate", h_rate}, {"SEGAAPI_SetChannelVolume", h_chan_vol},
    {"SEGAAPI_SetSendLevel", h_send_level}, {"SEGAAPI_SetSendRouting", h_send_routing},
    {"SEGAAPI_SetStartLoopOffset", h_start_loop}, {"SEGAAPI_SetEndLoopOffset", h_end_loop},
    {"SEGAAPI_SetEndOffset", h_end_offset}, {"SEGAAPI_SetLoopState", h_loop},
    {"SEGAAPI_SetReleaseState", h_release}, {"SEGAAPI_SetSynthParam", h_synth},
    {"SEGAAPI_SetSynthParamMultiple", h_synth_multiple}, {"SEGAAPI_PlayWithSetup", h_play_with_setup},
    {"SEGAAPI_GetSynthParam", h_get_synth}, {"SEGAAPI_GetLastStatus", h_update}, {"SEGAAPI_SetGlobalEAXProperty", h_update},
    {"SEGAAPI_SetIOVolume", h_io_vol}, {"SEGAAPI_UpdateBuffer", h_update},
    {"SEGAAPI_GetUserData", h_get_user}, {"SEGAAPI_SetUserData", h_set_user},
    {NULL, NULL},
};

__attribute__((constructor)) static void reg(void) { hle_register(audio_table); }
