/*
 * Arcade Foundry's game core interface: one game (one ROM set) as a
 * library a front end drives a frame at a time. Each core is a shared
 * library (libafcore_<romset>.so), or a framework on Apple platforms
 * (afcore_<romset>.framework), that exports one symbol,
 * afcore_api_<romset>() (afcore_api_daytona, afcore_api_vr,
 * afcore_api_scud, afcore_api_hotd4, ...), and hides everything else, so
 * several games load into one process without clashing.
 *
 * The cores of every board (Model 1 and 2 here, built with CMake M2_CORE;
 * Model 3; Lindbergh) and the front ends share this one file: it lives in
 * Arcade-Foundry-Cores-Model12 (include/afcore.h) and the other repos carry
 * an identical copy. Change it here first, then copy it over unchanged.
 *
 * Threading: a core is used from one thread at a time. Nothing blocks:
 * run_frame returns once the board has composed the next screen.
 */
#ifndef AFCORE_H
#define AFCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Versions are x.y.z, packed as major << 16 | minor << 8 | patch.
 *
 * The interface (AFCORE_API_*): a front end loads a core whose major
 * matches its own and refuses any other (rebuild the core). A minor
 * version only appends: new fields at the end of afcore_input and
 * afcore_api, new buttons, new streams, never a change to what is already
 * there, so cores and front ends of different minors still work together.
 * afcore_input.size and afcore_api.size say how much of each the other side
 * knows: a field past the end reads as zero (core) or absent (front end).
 * Patch: comments and clarifications only.
 *
 * History: 1 the first; 2 gun_x/gun_y; 3 buttons2 and the fighting
 * buttons; 4.0.0 x.y.z versions, the size fields, core_version, error;
 * 4.1.0 stick_y, BUTTON2/3 and audio stream 2 (Star Wars Arcade);
 * 4.2.0 afcore_api_stcc (Sega Touring Car Championship); 4.3.0
 * afcore_api_von (Cyber Troopers Virtual-On: its two levers are the
 * fighting buttons, the left one in buttons, the right one in buttons2);
 * 4.4.0 afcore_api_hotdo (The House of the Dead: Virtua Cop 2's guns);
 * 4.5.0 afcore_api_gunblade (Gunblade NY: its turrets aim with gun_x/gun_y
 * and fire with TRIGGER1/2); 4.6.0 afcore_api_vstriker (Virtua Striker: the
 * fighting buttons, PUNCH long pass, KICK shoot, GUARD short pass); 4.7.0
 * afcore_api_zerogun (Zero Gunner: the fighting buttons, PUNCH shot, KICK
 * special, no GUARD); 4.8.0 afcore_api_pltkids (Pilot Kids: as Zero Gunner);
 * 4.9.0 afcore_api_motoraid (Motor Raid: Manx TT's lean, throttle and brake,
 * PUNCH and KICK for its shift buttons); 4.10.0 afcore_api_dynamcop (Dynamite
 * Cop: the fighting buttons, GUARD jump); 4.11.0 afcore_api_skytargt (Sky
 * Target: as Star Wars Arcade, steer and stick_y the flight stick, TRIGGER1
 * machine gun, BUTTON2 missile, BUTTON3 view change); 4.12.0 afcore_api_desert
 * (Desert Tank: steer, accel and brake the ADC's three channels, SHIFT_UP
 * machine gun, SHIFT_DOWN cannon, VIEW1-3 VR1-3, VIEW4 held the shift lever);
 * 4.13.0 afcore_api_lastbrnx (Last Bronx: Virtua Fighter 2's controls);
 * 4.14.0 afcore_api_schamp (Sonic Championship: Virtua Fighter 2's
 * controls, GUARD barrier); 4.15.0 stereo: render_eyes, eye_screen,
 * hud_screen, stereo_scale (afcore_stereo); 4.16.0 afcore_api_dynabb97
 * (Dynamite Baseball 97: two players' joystick and two buttons, GUARD the
 * bat swing); 5.0.0 renamed from m2core (m2core.h, m2core_*, M2CORE_*) to
 * afcore, and the libraries from m2core_<romset> to afcore_<romset>; one
 * header for every board's cores. Nothing else changed. 5.1.0
 * screen_surface (Apple: the screen as a GPU buffer, no copy to the CPU).
 *
 * Each game's core has its own version (afcore_api.core_version, from
 * core_versions.h in its repo): major when its saves no longer carry over,
 * minor for a change players see (fixes in the game, link play, new
 * controls), patch for anything else that changes its code. */
#define AFCORE_MAKE_VERSION(major, minor, patch) ((uint32_t)(major) << 16 | (uint32_t)(minor) << 8 | (uint32_t)(patch))
#define AFCORE_VERSION_MAJOR(v) ((v) >> 16)
#define AFCORE_VERSION_MINOR(v) (((v) >> 8) & 0xff)
#define AFCORE_VERSION_PATCH(v) ((v) & 0xff)

#define AFCORE_API_MAJOR 5
#define AFCORE_API_MINOR 1
#define AFCORE_API_PATCH 0
#define AFCORE_API_VERSION AFCORE_MAKE_VERSION(AFCORE_API_MAJOR, AFCORE_API_MINOR, AFCORE_API_PATCH)

typedef struct afcore afcore;

/* Buttons (afcore_input.buttons). */
enum {
    AFCORE_COIN = 1u << 0,
    AFCORE_START = 1u << 1,
    AFCORE_TEST = 1u << 2,    /* the board's test switch: test mode */
    AFCORE_SERVICE = 1u << 3, /* service: a credit, or the cursor in test mode */
    AFCORE_VIEW1 = 1u << 4,   /* Daytona VR1-4; Sega Rally VIEW (VIEW1 only); INDY 500 views 1-2; Model 3: Sega Rally 2, Dirt Devils, Harley VIEW */
    AFCORE_VIEW2 = 1u << 5,   /* Model 3: Harley-Davidson's music select */
    AFCORE_VIEW3 = 1u << 6,
    AFCORE_VIEW4 = 1u << 7,
    AFCORE_SHIFT_UP = 1u << 8,  /* Manx TT's and INDY 500's gear buttons, Virtua Racing's shifter (a change of gear also shifts in VR; the others use gear) */
    AFCORE_SHIFT_DOWN = 1u << 9,
    AFCORE_TRIGGER1 = 1u << 10, /* Virtua Cop 2's guns; aimed off screen, a shot reloads. Model 3's analog guns: each gun's left and right triggers, in buttons (1P) and buttons2 (2P) */
    AFCORE_TRIGGER2 = 1u << 11, /* Virtual-On (Model 3): TRIGGER1/2 the left and right sticks' shot triggers, BUTTON2/3 their turbo buttons */
    AFCORE_START2 = 1u << 12,   /* player 2's start (Virtua Cop 2, Virtua Fighter 2) */
    AFCORE_PUNCH = 1u << 13,    /* Virtua Fighter 2's buttons and joystick, in buttons */
    AFCORE_KICK = 1u << 14,     /* (player 1) and buttons2 (player 2) */
    AFCORE_GUARD = 1u << 15,    /* Model 3: Spikeout's beat, charge, shift (PUNCH, KICK, GUARD; jump is BUTTON2); Virtua Striker's short pass, long pass, shoot */
    AFCORE_UP = 1u << 16,
    AFCORE_DOWN = 1u << 17,
    AFCORE_LEFT = 1u << 18,
    AFCORE_RIGHT = 1u << 19,
    AFCORE_BUTTON2 = 1u << 20,  /* 4.1: Star Wars Arcade: TRIGGER1 fires, BUTTON2 and BUTTON3 are the stick's other buttons; Star Wars Trilogy: TRIGGER1/2 the triggers, BUTTON2/3 the event buttons */
    AFCORE_BUTTON3 = 1u << 21
};

typedef struct afcore_input {
    uint32_t size;    /* sizeof(afcore_input) as the front end knows it */
    float steer;      /* -1 full left .. +1 full right (Manx TT: the lean; Star Wars Arcade and Star Wars Trilogy: the stick) */
    float accel;      /* 0 .. 1 (Manx TT, Star Wars Arcade: the throttle) */
    float brake;      /* 0 .. 1 */
    float handbrake;  /* 0 .. 1 (Sega Rally; ignored by Daytona) */
    int gear;         /* 0 neutral, 1-4 */
    uint32_t buttons; /* AFCORE_* held this frame */
    float gun_x[2];   /* Virtua Cop 2: players 1 and 2's aim, 0 .. 1 across the screen, */
    float gun_y[2];   /* and 0 .. 1 down it; outside 0 .. 1, off screen (Model 3's analog guns have no off-screen sense: the edge) */
    uint32_t buttons2; /* player 2's AFCORE_PUNCH .. AFCORE_RIGHT (Virtua Fighter 2); Virtual-On's right stick (UP .. RIGHT) */
    float stick_y;    /* 4.1: Star Wars Arcade and Star Wars Trilogy: -1 stick pushed forward .. +1 pulled back */
} afcore_input;

/* Link play's state (link_state). */
enum {
    AFCORE_LINK_OFF = 0,     /* no link, or the game has not started it */
    AFCORE_LINK_WAITING = 1, /* looking for the other cabinets */
    AFCORE_LINK_UP = 2,
    AFCORE_LINK_LOST = 3     /* reset (destroy and create) to try again */
};

/* 4.15: stereo (render_eyes), for VR headsets and 3D displays. The last
 * screen's 3D scene drawn again for two eyes from the game's own polygons,
 * not a depth guess from the flat picture. */
enum {
    /* Two eyes either side of the game's camera, looking where it looks, with
     * no parallax at `convergence`: the original screen as a window into the
     * scene. Each eye is screen()-sized, 2D layers included (the HUD at
     * screen depth). */
    AFCORE_STEREO_PAIR = 0,
    /* Any view and field of view per eye (a headset's poses): the scene
     * re-projected into each eye, the HUD left out (hud_screen) for the front
     * end to place. Only the full-screen 3D view is drawn. */
    AFCORE_STEREO_HEAD = 1
};

typedef struct afcore_eye {
    /* Column-major 4x4 from the game camera's space to this eye's, in metres:
     * x right, y up, z forward (where the camera looks). */
    float view[16];
    float tan_left, tan_right, tan_down, tan_up; /* the eye's field of view: tangents of its edges, left and down negative */
    int width, height;                           /* the eye's image, at most 1024 x 512 */
} afcore_eye;

typedef struct afcore_stereo {
    uint32_t size;      /* sizeof(afcore_stereo) as the front end knows it */
    int mode;           /* AFCORE_STEREO_* */
    float world_scale;  /* metres per game unit; 0 = the core's stereo_scale */
    float ipd;          /* PAIR: the distance between the eyes, metres (0 = 0.063) */
    float convergence;  /* PAIR: the distance shown at screen depth, metres (0 = 10) */
    afcore_eye eye[2];  /* HEAD: left, right */
} afcore_stereo;

typedef struct afcore_api {
    uint32_t version;   /* AFCORE_API_VERSION the core was built with */
    uint32_t size;      /* sizeof(afcore_api) as the core knows it */
    uint32_t core_version; /* this game's core, AFCORE_MAKE_VERSION */
    const char *romset; /* "daytona93", "daytona", "srallyc", "manxttc", "vcop2", "vcop", "vf2", "indy500", "fvipers", "sgt24h", "overrevb", "stcc", "von", "hotdo", "gunblade", "vstriker", "zerogun", "pltkids", "motoraid", "dynamcop", "skytargt", "desert", "lastbrnx", "schamp", "dynabb97", "waverunr", "segawski", "skisuprg", "bel", "topskatr", "rchase2", "doab", "vr", "swa", ...; Model 3 "vf3", "scud", ...; Lindbergh "hotd4", ... */
    const char *title;  /* "Daytona USA", "Sega Rally Championship", "Virtua Racing", ... */
    double frame_hz;    /* the board's frame rate (57.52 Hz) */
    int width, height;  /* the original screen: 496 x 384 */

    /* rom_path: the user's MAME ROM set (.zip or .7z), or a folder of the
     * images m2import made from it. save_dir: where the
     * game's settings EEPROM and backup RAM are kept (created by the caller;
     * NULL keeps nothing). On failure returns NULL and writes why to err. */
    afcore *(*create)(const char *rom_path, const char *save_dir, char *err, size_t err_size);
    /* Saves the settings (as save does) and frees the core. */
    void (*destroy)(afcore *core);

    /* Run until the next screen is composed. input: this frame's controls. */
    void (*run_frame)(afcore *core, const afcore_input *input);

    /* The screen: *width x 384 pixels, 0xAARRGGBB words (BGRA bytes on a
     * little-endian host), rows packed. Valid until the next run_frame. */
    const uint32_t *(*screen)(afcore *core, int *width, int *height);

    /* Sound since the last call: the board's outputs as interleaved stereo
     * floats, each at its own rate (streams 0 and 1; Sega Rally's SCSP is
     * stream 0 and stream 1 is empty; Star Wars Arcade's Digital Sound Board,
     * its music and speech, is stream 2, whose rate is 0 in every other
     * game). Mix them at equal gain. Writes at
     * most max_frames frames and returns how many; the rest is kept for the
     * next call. */
    double (*audio_rate)(afcore *core, int stream);
    size_t (*audio_read)(afcore *core, int stream, float *out, size_t max_frames);

    /* Writes the settings EEPROM and backup RAM to save_dir now. */
    void (*save)(afcore *core);

    /* Link play over TCP: listen on listen_port for the cabinet before this
     * one, connect to next ("host:port"). Call before the first run_frame.
     * framesync holds every cabinet to the master's frame. Returns 0, or -1
     * with the reason in err. Which cabinet this is comes from the game's own
     * test-mode setting (Daytona: GAME SYSTEM > LINK ID; Sega Rally: GAME
     * ASSIGNMENTS > LINK TYPE). */
    int (*link_start)(afcore *core, int listen_port, const char *next, int framesync, char *err, size_t err_size);
    /* AFCORE_LINK_*; *id and *count (may be NULL) once it is up. */
    int (*link_state)(afcore *core, int *id, int *count);

    /* NULL while the game runs. Once the board hits something the core
     * cannot run (code the recompiler did not reach, say), the reason: the
     * core stops there (run_frame does nothing, the last screen stays) and
     * the front end should leave the game and show it. */
    const char *(*error)(afcore *core);

    /* 4.15, stereo; NULL in a core without it. Draws the last screen's 3D
     * scene for two eyes, left (0) and right (1): 1, or 0 when that screen has
     * no 3D scene to draw so (titles, menus, test mode: show screen() flat).
     * Call after run_frame, as often as wanted (HEAD: at the display's rate,
     * with fresh poses: the scene stays the last frame's). The normal screen
     * and the game are unchanged. */
    int (*render_eyes)(afcore *core, const afcore_stereo *stereo);
    /* An eye from the last render_eyes that returned 1: *width x *height,
     * 0xAARRGGBB, opaque, rows packed. Valid until the next run_frame or
     * render_eyes. */
    const uint32_t *(*eye_screen)(afcore *core, int eye, int *width, int *height);
    /* That screen's front 2D layers alone (HUD, text): 496 x 384, 0xAARRGGBB,
     * alpha 0 where see-through. */
    const uint32_t *(*hud_screen)(afcore *core, int *width, int *height);
    /* Metres per game unit, this game's best guess (0: not known; try 1). */
    float stereo_scale;

    /* 5.1, Apple platforms; NULL, or returning NULL, in a core without it:
     * use screen. The screen as a CVPixelBufferRef: IOSurface-backed,
     * kCVPixelFormatType_32BGRA, top row first, *width x *height, the GPU's
     * drawing of it finished, for a Metal view to show without a copy
     * (CVMetalTextureCache). Valid until the next run_frame, the core's
     * reference (retain it to hold on to it); the core does not draw again
     * into the last three it handed out, so a view may still be showing the
     * one before. From the first call the core stops copying each frame to
     * the CPU; screen() still works, copied out when called. */
    void *(*screen_surface)(afcore *core, int *width, int *height);
} afcore_api;

#if defined(_WIN32)
#ifdef AFCORE_BUILD
#define AFCORE_EXPORT __declspec(dllexport)
#else
#define AFCORE_EXPORT __declspec(dllimport)
#endif
#else
#define AFCORE_EXPORT __attribute__((visibility("default")))
#endif

/* Each core library defines only its own game's entry point. The Model 1
 * and 2 ones are declared here; the Model 3 and Lindbergh cores declare
 * their own (one per build). */

AFCORE_EXPORT const afcore_api *afcore_api_daytona93(void);
AFCORE_EXPORT const afcore_api *afcore_api_daytona(void);
AFCORE_EXPORT const afcore_api *afcore_api_srallyc(void);
AFCORE_EXPORT const afcore_api *afcore_api_manxttc(void);
AFCORE_EXPORT const afcore_api *afcore_api_vcop2(void);
AFCORE_EXPORT const afcore_api *afcore_api_vcop(void);
AFCORE_EXPORT const afcore_api *afcore_api_vf2(void);
AFCORE_EXPORT const afcore_api *afcore_api_indy500(void);
AFCORE_EXPORT const afcore_api *afcore_api_fvipers(void);
AFCORE_EXPORT const afcore_api *afcore_api_sgt24h(void);
AFCORE_EXPORT const afcore_api *afcore_api_overrevb(void);
AFCORE_EXPORT const afcore_api *afcore_api_stcc(void);
AFCORE_EXPORT const afcore_api *afcore_api_von(void);
AFCORE_EXPORT const afcore_api *afcore_api_hotdo(void);
AFCORE_EXPORT const afcore_api *afcore_api_gunblade(void);
AFCORE_EXPORT const afcore_api *afcore_api_vstriker(void);
AFCORE_EXPORT const afcore_api *afcore_api_zerogun(void);
AFCORE_EXPORT const afcore_api *afcore_api_pltkids(void);
AFCORE_EXPORT const afcore_api *afcore_api_motoraid(void);
AFCORE_EXPORT const afcore_api *afcore_api_dynamcop(void);
AFCORE_EXPORT const afcore_api *afcore_api_skytargt(void);
AFCORE_EXPORT const afcore_api *afcore_api_desert(void);
AFCORE_EXPORT const afcore_api *afcore_api_lastbrnx(void);
AFCORE_EXPORT const afcore_api *afcore_api_schamp(void);
AFCORE_EXPORT const afcore_api *afcore_api_dynabb97(void);
AFCORE_EXPORT const afcore_api *afcore_api_waverunr(void);
AFCORE_EXPORT const afcore_api *afcore_api_segawski(void);
AFCORE_EXPORT const afcore_api *afcore_api_skisuprg(void);
AFCORE_EXPORT const afcore_api *afcore_api_bel(void);
AFCORE_EXPORT const afcore_api *afcore_api_topskatr(void);
AFCORE_EXPORT const afcore_api *afcore_api_rchase2(void);
AFCORE_EXPORT const afcore_api *afcore_api_doab(void);
AFCORE_EXPORT const afcore_api *afcore_api_vr(void);
AFCORE_EXPORT const afcore_api *afcore_api_swa(void);
AFCORE_EXPORT const afcore_api *afcore_api_vf(void);
AFCORE_EXPORT const afcore_api *afcore_api_wingwar(void);

#ifdef __cplusplus
}
#endif

#endif /* AFCORE_H */
