// ALSA's PCM playback (libasound.so.2) on the runtime's software mixer
// (hle_audio.c mixes it with the sound card's voices and OpenAL's sources).
// CRI's sound output for Lindbergh (librnalindbergh_jr.so: Primeval Hunt)
// opens one PCM, sets its format, rate, channels (6) and buffer, and writes
// interleaved 16-bit frames as the buffer has room (snd_pcm_avail_update).
// The frames go into a ring the mixer plays at the PCM's rate; the room is
// what the ring has free, so the writer is paced by what is played.
#include "hle.h"
#include <pthread.h>
#include <string.h>

#define OUT_RATE 48000
#define RING 65536   // frames, stereo

enum { SND_PCM_FORMAT_S16_LE = 2, SND_PCM_STATE_PREPARED = 2, SND_PCM_STATE_RUNNING = 3 };

static struct {
    int open, running;
    uint32_t rate, chans, buffer;   // as set; buffer in frames
    float ring[RING][2];
    uint32_t head, tail;            // frames written, played (wrapping counters)
    double frac;                    // the next frame's position between two (resampling)
} pcm;
static pthread_mutex_t pcm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t pcm_handle;          // the guest's snd_pcm_t *

static uint32_t queued(void) { return pcm.head - pcm.tail; }
static uint32_t room(void)
{
    uint32_t b = pcm.buffer ? pcm.buffer : 4096, q = queued();
    return q < b ? b - q : 0;
}

// the mixer (hle_audio.c, under its lock): the PCM at its rate, into the output
void alsa_mix(float *out, int frames)
{
    pthread_mutex_lock(&pcm_lock);
    if (pcm.open && pcm.running && pcm.rate) {
        double step = (double)pcm.rate / OUT_RATE;
        for (int i = 0; i < frames && queued() > 1; i++) {
            uint32_t a = pcm.tail % RING, b = (pcm.tail + 1) % RING;
            float t = (float)pcm.frac;
            out[2 * i] += pcm.ring[a][0] + (pcm.ring[b][0] - pcm.ring[a][0]) * t;
            out[2 * i + 1] += pcm.ring[a][1] + (pcm.ring[b][1] - pcm.ring[a][1]) * t;
            pcm.frac += step;
            while (pcm.frac >= 1 && queued() > 1) {
                pcm.frac -= 1;
                pcm.tail++;
            }
        }
    }
    pthread_mutex_unlock(&pcm_lock);
}

void alsa_reset(void)
{
    pthread_mutex_lock(&pcm_lock);
    memset(&pcm, 0, sizeof pcm);
    pcm_handle = 0;
    pthread_mutex_unlock(&pcm_lock);
}

// snd_pcm_open(snd_pcm_t **, name, stream, mode)
static void h_pcm_open(CPU *c)
{
    hle_audio_open();
    pthread_mutex_lock(&pcm_lock);
    memset(&pcm, 0, sizeof pcm);
    pcm.open = 1;
    pcm.rate = OUT_RATE;
    pcm.chans = 2;
    if (!pcm_handle) pcm_handle = hostdata_alloc(16);
    pthread_mutex_unlock(&pcm_lock);
    wr32(ARG(0), pcm_handle);
    if (hle_verbose) hle_log("alsa: open %s\n", ARG(1) ? G(ARG(1)) : "");
    RET(0);
}
static void h_pcm_close(CPU *c)
{
    pthread_mutex_lock(&pcm_lock);
    pcm.open = pcm.running = 0;
    pthread_mutex_unlock(&pcm_lock);
    RET(0);
}
// parameter objects: places the setters write to; what matters is kept here
static void h_params_malloc(CPU *c)
{
    wr32(ARG(0), guest_calloc(1, 256));
    RET(0);
}
static void h_params_free(CPU *c) { guest_free(ARG(0)); }
static void h_ret0(CPU *c) { RET(0); }
static void h_set_format(CPU *c) { RET(ARG(2) == SND_PCM_FORMAT_S16_LE ? 0 : (uint32_t)-22); }
// 1, 2, or a cabinet's 6 (5.1: front left and right, rear left and right,
// centre, LFE; Primeval Hunt's), folded to stereo as the mixer's voices are
static void h_set_channels(CPU *c)
{
    uint32_t n = ARG(2);
    if (n != 1 && n != 2 && n != 6) RET((uint32_t)-22);
    pthread_mutex_lock(&pcm_lock);
    pcm.chans = n;
    pthread_mutex_unlock(&pcm_lock);
    RET(0);
}
// set_rate_near(pcm, params, unsigned *rate, int *dir): taken as asked
static void h_set_rate_near(CPU *c)
{
    pthread_mutex_lock(&pcm_lock);
    pcm.rate = rd32(ARG(2));
    pthread_mutex_unlock(&pcm_lock);
    if (ARG(3)) wr32(ARG(3), 0);
    RET(0);
}
static void h_set_buffer_size(CPU *c)
{
    pthread_mutex_lock(&pcm_lock);
    pcm.buffer = ARG(2) < RING / 2 ? ARG(2) : RING / 2;
    pthread_mutex_unlock(&pcm_lock);
    RET(0);
}
static void h_format_width(CPU *c) { RET(ARG(0) == SND_PCM_FORMAT_S16_LE ? 16 : (uint32_t)-22); }
static void h_pcm_prepare(CPU *c) { RET(0); }
static void h_pcm_start(CPU *c)
{
    pthread_mutex_lock(&pcm_lock);
    pcm.running = 1;
    pthread_mutex_unlock(&pcm_lock);
    RET(0);
}
static void h_pcm_drop(CPU *c)
{
    pthread_mutex_lock(&pcm_lock);
    pcm.running = 0;
    pcm.tail = pcm.head;
    pthread_mutex_unlock(&pcm_lock);
    RET(0);
}
static void h_pcm_state(CPU *c) { RET(pcm.running ? SND_PCM_STATE_RUNNING : SND_PCM_STATE_PREPARED); }
static void h_pcm_avail(CPU *c)
{
    pthread_mutex_lock(&pcm_lock);
    uint32_t r = room();
    pthread_mutex_unlock(&pcm_lock);
    RET(r);
}
// writei(pcm, frames' data, count): as many as there is room for; the PCM
// starts with the first write (the start threshold)
static void h_pcm_writei(CPU *c)
{
    const int16_t *p = (const int16_t *)G(ARG(1));
    pthread_mutex_lock(&pcm_lock);
    uint32_t n = ARG(2), r = room();
    if (n > r) n = r;
    for (uint32_t i = 0; i < n; i++) {
        float *f = pcm.ring[(pcm.head + i) % RING];
        const int16_t *q = p + i * pcm.chans;
        if (pcm.chans == 1) f[0] = f[1] = q[0] / 32768.0f;
        else if (pcm.chans == 2) f[0] = q[0] / 32768.0f, f[1] = q[1] / 32768.0f;
        else {
            float centre = q[4] * 0.7071f;
            f[0] = (q[0] + centre + q[2] * 0.7071f) / 32768.0f;
            f[1] = (q[1] + centre + q[3] * 0.7071f) / 32768.0f;
        }
    }
    pcm.head += n;
    pcm.running = 1;
    pthread_mutex_unlock(&pcm_lock);
    RET(n ? n : (uint32_t)-11);   // -EAGAIN when full
}
static void h_strerror(CPU *c)
{
    static uint32_t s;
    if (!s) s = guest_strdup("ALSA error");
    RET(s);
}

static const hle_entry alsa_table[] = {
    {"snd_pcm_open", h_pcm_open}, {"snd_pcm_close", h_pcm_close},
    {"snd_pcm_hw_params_malloc", h_params_malloc}, {"snd_pcm_hw_params_free", h_params_free},
    {"snd_pcm_sw_params_malloc", h_params_malloc}, {"snd_pcm_sw_params_free", h_params_free},
    {"snd_pcm_hw_params_any", h_ret0}, {"snd_pcm_hw_params", h_ret0}, {"snd_pcm_sw_params", h_ret0},
    {"snd_pcm_sw_params_current", h_ret0}, {"snd_pcm_hw_params_set_access", h_ret0},
    {"snd_pcm_hw_params_set_format", h_set_format}, {"snd_pcm_hw_params_set_channels", h_set_channels},
    {"snd_pcm_hw_params_set_rate_near", h_set_rate_near}, {"snd_pcm_hw_params_set_buffer_size", h_set_buffer_size},
    {"snd_pcm_hw_params_set_periods", h_ret0}, {"snd_pcm_sw_params_set_avail_min", h_ret0},
    {"snd_pcm_sw_params_set_start_threshold", h_ret0}, {"snd_pcm_sw_params_set_xfer_align", h_ret0},
    {"snd_pcm_format_width", h_format_width}, {"snd_pcm_prepare", h_pcm_prepare}, {"snd_pcm_resume", h_pcm_prepare},
    {"snd_pcm_start", h_pcm_start}, {"snd_pcm_drop", h_pcm_drop}, {"snd_pcm_state", h_pcm_state},
    {"snd_pcm_avail_update", h_pcm_avail}, {"snd_pcm_writei", h_pcm_writei}, {"snd_strerror", h_strerror},
    {NULL, NULL},
};

void hle_alsa_init(void) { hle_register(alsa_table); }
