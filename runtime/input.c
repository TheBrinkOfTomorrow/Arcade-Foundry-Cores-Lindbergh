// Cabinet controls: the host's devices (host_input) and, for unattended runs,
// LB_INPUT_SCRIPT=file, which drives the cabinet from a script; each line is
// "<frame> <command> [args]":
//   coin P | start P | test | service       press for 6 frames
//   aim P X Y                                 gun position, 0..1 across the screen
//   fire P on|off                             pull the trigger every 8 frames
//   sweep P on|off                            move the aim around the screen
//   reload P                                  press the reload button for 6 frames
//   analog N V                                analog channel N (0-7) at V, 0..1 (driving: steering,
//                                             gas, brake)
//   button P up|down|left|right|b1|b2|b3|b4|BIT on|off   hold a player switch (gear shift, view;
//                                             BIT 1-15, as in press)
//   press P BIT                               press switch bit BIT (0-15) for 6 frames
#include "input.h"
#include "hle.h"
#include "host.h"
#include <math.h>
#include <string.h>

jvs_inputs jvs_in;

// ---- scripted input ----------------------------------------------------------------
typedef struct { int frame; char cmd[16]; int player, num; char name[16]; float x, y; int on; } script_line;
static script_line *script;
static int script_n, script_pos, script_loaded, frame_no;
static int fire[2], sweep[2], hold[2][16];   // hold: frames left per button bit
static uint16_t held[2], was_held[2];         // switches the script holds down

static void load_script(void)
{
    script_loaded = 1;
    const char *path = getenv("LB_INPUT_SCRIPT");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        script_line l = {0};
        char a[16] = "", b[16] = "";
        int n = sscanf(line, "%d %15s %d %15s %15s", &l.frame, l.cmd, &l.player, a, b);
        if (n < 2 || line[0] == '#') continue;
        l.num = l.player;
        l.player = l.player > 0 ? l.player - 1 : 0;
        l.on = !strcmp(a, "on") || !strcmp(b, "on");
        snprintf(l.name, sizeof l.name, "%s", a);
        l.x = (float)atof(a);
        l.y = (float)atof(b);
        script = realloc(script, sizeof *script * (script_n + 1));
        script[script_n++] = l;
    }
    fclose(f);
    hle_log("input script: %d lines\n", script_n);
}

static void press(int p, int bit) { hold[p][bit] = 6; }

static void run_script(void)
{
    if (!script_loaded) load_script();
    frame_no++;
    for (; script_pos < script_n && script[script_pos].frame <= frame_no; script_pos++) {
        script_line *l = &script[script_pos];
        int p = l->player & 1;
        if (!strcmp(l->cmd, "coin")) jvs_in.coins[p]++;
        else if (!strcmp(l->cmd, "start")) press(p, 15);
        else if (!strcmp(l->cmd, "service")) press(p, 14);
        else if (!strcmp(l->cmd, "reload")) press(p, 8);
        else if (!strcmp(l->cmd, "test")) press(0, 0);
        else if (!strcmp(l->cmd, "aim")) {
            jvs_in.analog[2 * p] = (uint16_t)(l->x * 65535);
            jvs_in.analog[2 * p + 1] = (uint16_t)(l->y * 65535);
        } else if (!strcmp(l->cmd, "press")) {
            int bit = atoi(l->name);
            if (bit >= 1 && bit < 16) press(p, bit);
        } else if (!strcmp(l->cmd, "analog")) {
            if (l->num >= 0 && l->num < 8) jvs_in.analog[l->num] = (uint16_t)(l->x * 65535);
        } else if (!strcmp(l->cmd, "button")) {
            static const struct { const char *name; uint16_t bit; } sw[] = {
                {"up", SW_UP}, {"down", SW_DOWN}, {"left", SW_LEFT}, {"right", SW_RIGHT},
                {"b1", SW_1}, {"b2", SW_2}, {"b3", SW_3}, {"b4", SW_4}};
            uint16_t bit = 0;
            for (size_t k = 0; k < sizeof sw / sizeof sw[0]; k++)
                if (!strcmp(l->name, sw[k].name)) bit = sw[k].bit;
            if (l->name[0] >= '1' && l->name[0] <= '9' && atoi(l->name) < 16) bit = (uint16_t)(1u << atoi(l->name));
            held[p] = l->on ? held[p] | bit : held[p] & ~bit;
        } else if (!strcmp(l->cmd, "fire")) fire[p] = l->on;
        else if (!strcmp(l->cmd, "sweep")) sweep[p] = l->on;
    }
    for (int p = 0; p < 2; p++) {
        jvs_in.player[p] = (uint16_t)((jvs_in.player[p] & ~(was_held[p] & ~held[p])) | held[p]);
        was_held[p] = held[p];
        if (fire[p] && frame_no % 8 == 0) press(p, 9);
        if (sweep[p]) {
            double t = frame_no / 60.0;
            jvs_in.analog[2 * p] = (uint16_t)((0.5 + 0.4 * sin(t * 1.3 + p)) * 65535);
            jvs_in.analog[2 * p + 1] = (uint16_t)((0.5 + 0.35 * sin(t * 2.1 + p)) * 65535);
        }
        for (int b = 0; b < 16; b++) {
            if (!hold[p][b]) continue;
            if (p == 0 && b == 0) jvs_in.system |= 0x80;
            else jvs_in.player[p] |= (uint16_t)(1u << b);
            if (--hold[p][b] == 0) {
                if (p == 0 && b == 0) jvs_in.system &= ~0x80;
                else jvs_in.player[p] &= (uint16_t)~(1u << b);
            }
        }
    }
}

void input_poll(void)
{
    host_input();
    run_script();
}
