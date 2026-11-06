// Drives a core library the way Arcade Foundry: Insert Coin does, without a window: for
// checking a core (boot, play, test menu, leaving and starting again).
// usage: core_test libafcore_<romset>.so romset game_folder save_dir script
// script lines: "<frame> <command> [args]", frames counted from the last create:
//   coin | start | start2 | test | service      held for 6 frames
//   trigger on|off                              player 1 fires every 8 frames
//   aim X Y                                     player 1's gun, 0..1 (outside: off screen)
//   steer V | gas V | brake V                   driving: steering -1..1, pedals 0..1
//   view | shiftup | shiftdown | boost          held for 6 frames
//   grenade                                     player 1's gun button (BUTTON3), 6 frames
//   shot NAME                                   the screen to NAME.ppm
//   restart                                     destroy and create again
//   surfaces                                    take the screen as surfaces from now on (5.1, Apple)
//   end                                         stop
// CORE_TEST_PACE=1 runs at the core's frame rate (CORE_TEST_PACE=40: at 40 a
// second, a slow host), as the front end does, for games
// that time their loading by the clock (OutRun 2 SP); otherwise as fast as it goes.
// CORE_TEST_EVERY=N prints the progress line every N frames (default 600).
// CORE_TEST_OPTION=name=value sets one of the core's options (afcore 5.3) first.
#include "afcore.h"
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void shot(const afcore_api *api, afcore *core, const char *name)
{
    int w, h;
    const uint32_t *p = api->screen(core, &w, &h);
    char path[300];
    snprintf(path, sizeof path, "%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char rgb[3] = {(unsigned char)(p[i] >> 16), (unsigned char)(p[i] >> 8), (unsigned char)p[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("shot %s (%dx%d)\n", path, w, h);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s core.so romset game_folder save_dir script\n", argv[0]);
        return 1;
    }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    char sym[64];
    snprintf(sym, sizeof sym, "afcore_api_%s", argv[2]);
    const afcore_api *(*get)(void) = (const afcore_api *(*)(void))dlsym(lib, sym);
    if (!get) {
        fprintf(stderr, "no %s\n", sym);
        return 1;
    }
    const afcore_api *api = get();
    printf("%s: %s, API %u.%u, %dx%d at %.2f Hz\n", api->romset, api->title, AFCORE_VERSION_MAJOR(api->version),
           AFCORE_VERSION_MINOR(api->version), api->width, api->height, api->frame_hz);

    FILE *sf = fopen(argv[5], "r");
    if (!sf) {
        perror(argv[5]);
        return 1;
    }
    const char *opt = getenv("CORE_TEST_OPTION");
    if (opt && strchr(opt, '=')) {
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(strchr(opt, '=') - opt), opt);
        int ok = api->size >= offsetof(afcore_api, set_option) + sizeof api->set_option && api->set_option
                     ? api->set_option(name, strchr(opt, '=') + 1) : -1;
        printf("option %s: %s\n", opt, ok == 0 ? "set" : "not taken");
    }
    char err[512];
    afcore *core = api->create(argv[3], argv[4], err, sizeof err);
    if (!core) {
        fprintf(stderr, "create: %s\n", err);
        return 1;
    }
    afcore_input in = {sizeof in};
    in.gun_x[0] = in.gun_y[0] = in.gun_x[1] = in.gun_y[1] = 0.5f;
    int hold[32] = {0}, fire = 0, frame = 0;
    char line[256];
    int next_frame = -1;
    char cmd[32] = "", arg1[200] = "", arg2[32] = "";
    double t0 = now(), audio_peak = 0;
    int pace = getenv("CORE_TEST_PACE") != NULL;
    double pace_hz = pace && atof(getenv("CORE_TEST_PACE")) > 1 ? atof(getenv("CORE_TEST_PACE")) : 0;
    int surfaces = 0, surfaces_seen = 0;
    size_t audio_frames = 0;
    static float buf[48000 * 2];
    for (;;) {
        while (next_frame < 0 || next_frame <= frame) {
            if (next_frame >= 0) {
                if (!strcmp(cmd, "end")) goto done;
                else if (!strcmp(cmd, "coin")) hold[0] = 6;
                else if (!strcmp(cmd, "start")) hold[1] = 6;
                else if (!strcmp(cmd, "test")) hold[2] = 6;
                else if (!strcmp(cmd, "service")) hold[3] = 6;
                else if (!strcmp(cmd, "start2")) hold[12] = 6;
                else if (!strcmp(cmd, "trigger")) fire = !strcmp(arg1, "on");
                else if (!strcmp(cmd, "aim")) in.gun_x[0] = (float)atof(arg1), in.gun_y[0] = (float)atof(arg2);
                else if (!strcmp(cmd, "steer")) in.steer = (float)atof(arg1);
                else if (!strcmp(cmd, "gas")) in.accel = (float)atof(arg1);
                else if (!strcmp(cmd, "brake")) in.brake = (float)atof(arg1);
                else if (!strcmp(cmd, "view")) hold[4] = 6;
                else if (!strcmp(cmd, "shiftup")) hold[8] = 6;
                else if (!strcmp(cmd, "shiftdown")) hold[9] = 6;
                else if (!strcmp(cmd, "boost")) hold[20] = 6;
                else if (!strcmp(cmd, "grenade")) hold[21] = 6;
                else if (!strcmp(cmd, "shot")) shot(api, core, arg1);
                else if (!strcmp(cmd, "surfaces")) surfaces = 1;
                else if (!strcmp(cmd, "restart")) {
                    printf("frame %d: restart (%.1f fps so far)\n", frame, frame / (now() - t0));
                    api->destroy(core);
                    core = api->create(argv[3], argv[4], err, sizeof err);
                    if (!core) {
                        fprintf(stderr, "create again: %s\n", err);
                        return 1;
                    }
                    frame = 0;
                    t0 = now();
                }
                next_frame = -1;
            }
            if (!fgets(line, sizeof line, sf)) {
                strcpy(cmd, "end");
                next_frame = frame;
                goto done;
            }
            if (line[0] == '#' || sscanf(line, "%d %31s %199s %31s", &next_frame, cmd, arg1, arg2) < 2) next_frame = -1;
        }
        in.buttons = 0;
        for (int b = 0; b < 32; b++)
            if (hold[b]) {
                in.buttons |= 1u << b;
                hold[b]--;
            }
        if (fire && frame % 8 < 4) in.buttons |= AFCORE_TRIGGER1;
        api->run_frame(core, &in);
        if (surfaces && api->size > offsetof(afcore_api, screen_surface) && api->screen_surface) {
            int sw, sh;
            if (api->screen_surface(core, &sw, &sh) && !surfaces_seen++) printf("frame %d: a surface %dx%d\n", frame, sw, sh);
        }
        if (pace) {
            double due = t0 + (frame + 1) / (pace_hz ? pace_hz : api->frame_hz);
            while (now() < due) {
                struct timespec ts = {0, 1000000};
                nanosleep(&ts, NULL);
            }
        }
        size_t n;
        while ((n = api->audio_read(core, 0, buf, 48000)) > 0) {
            audio_frames += n;
            for (size_t i = 0; i < 2 * n; i++)
                if (buf[i] > audio_peak) audio_peak = buf[i];
        }
        const char *e = api->error(core);
        if (e) {
            fprintf(stderr, "frame %d: core stopped: %s\n", frame, e);
            break;
        }
        frame++;
        if (frame % (getenv("CORE_TEST_EVERY") ? atoi(getenv("CORE_TEST_EVERY")) : 600) == 0)
            printf("frame %d: %.1f fps, audio %zu frames (%.0f a second), peak %.3f\n", frame, frame / (now() - t0), audio_frames,
                   audio_frames / (now() - t0), audio_peak);
    }
done:
    printf("end at frame %d: %.1f fps, audio %zu frames, peak %.3f\n", frame, frame / (now() - t0), audio_frames, audio_peak);
    api->save(core);
    api->destroy(core);
    return 0;
}
