# Sega Lindbergh cores

This document describes how the Sega Lindbergh games run in Arcade Foundry:
Insert Coin: how the work was planned, how each game becomes a core, what the
runtime stands in for, and how the graphics reach each platform. The
recompiler and the Metal layer are shared with other cores and have their
own documents in Arcade-Foundry-Cores-Common:

- [x86recomp](../../common/docs/design/X86RECOMP.md), the static x86
  recompiler (`common/docs/design/X86RECOMP.md`)
- [mtlgles](../../common/docs/design/MTLGLES.md), OpenGL ES on Metal
  (`common/docs/design/MTLGLES.md`)

This document links them rather than repeating them.

## Purpose and scope

A Lindbergh cabinet is a Pentium 4 PC running Linux with an NVIDIA graphics
card, a Sega baseboard (JVS I/O, EEPROM, a security dongle, DIP switches)
and a Sega sound card. Each game is one 32-bit x86 Linux program, sometimes
with a test-menu program and shared libraries next to it. The goal is to play
these games on Apple Silicon Macs, iPads and x86 Linux PCs from the user's own
copy of the game, with no x86 emulator.

In scope, one core per game:

| Game | Set | State |
|---|---|---|
| The House of the Dead 4 | `hotd4a` | Released in 1.0.0 |
| OutRun 2 SP SDX | `outr2sdx` | Released in 1.0.0 |
| Let's Go Jungle! | `letsgoju` | For 1.2.0 |
| Sega Race TV | `segartv` | For 1.2.0 |
| Ghost Squad Evolution | `gsevo` | Started, paused |

More Lindbergh games are planned for later releases.

Out of scope:

- the cabinet's system disk (its boot menus and font); the runtime supplies
  stand-ins
- network play between cabinets
- Apple TV for now (see Platforms)

## Requirements

1. Every game plays its attract mode, a full game to game over, and its test
   menu, on every platform it ships on.
2. The game runs at the speed the cabinet did: 60 frames a second, or the
   game's own 30 fps scenes where it asks for them.
3. Sound plays at the right speed and pitch, in step with the picture.
4. The cabinet's controls map to the app's: light guns on a mouse, touch or
   gamepad; wheel, pedals and gear lever for the driving games.
5. Saves (EEPROM and SRAM) carry over between runs and app versions.
6. The front end can set the cabinet's region (Japan, USA, Export), which
   picks the game's language where the game has one.
7. No game code or data is in any repository. The user supplies the game
   folder; the build translates it on the user's machine.
8. Every game is a core behind the app's `afcore.h` interface, the same as
   every other board.

## Design

### How the work was planned

The approach was settled before any game ran:

1. **Translate, don't emulate.** The game's x86 code is translated to C at
   build time by x86recomp. iOS does not allow a JIT, and translated C runs
   at native speed everywhere.
2. **Replace the operating system, not the hardware.** The game calls
   Linux's C library, pthreads, X11/GLX, OpenGL and Sega's libraries. The
   runtime implements those calls on the host (HLE), so no Linux kernel, no
   GPU emulation and no x86 libraries are needed.
3. **Start with one game, and grow the runtime with each one.** The House of
   the Dead 4 came first, then OutRun 2 SP SDX, then Let's Go Jungle! and
   Sega Race TV. Each new game adds what the earlier ones did not use.
4. **Per-game knowledge stays small.** Anything a game needs that the shared
   runtime cannot do in general goes in `games/<set>/` as lists of names and
   byte patches.
5. **Desktop OpenGL first.** The Mac and Linux run the games' OpenGL 2 on
   the system's OpenGL. Other graphics paths (OpenGL ES on the iPad, then
   Metal) come after a game plays.

### Where it sits

```
game folder (disk0/: the game's ELF, test ELF, libraries, data)
  -> x86recomp at build time -> generated C (gen/, gen-test/)
  -> + runtime/ (HLE: libc, pthreads, X11/GLX, GL, baseboard, sound)
  -> afcore_<set> core library (Linux .so, Apple framework)
  -> Arcade Foundry: Insert Coin
```

### The runtime

`runtime/` stands in for everything the game expects from the cabinet:

| File | Stands in for |
|---|---|
| `core.c` | Guest memory (one 4 GB window), loading the program and its libraries, function dispatch, the CPU helpers the generated code calls |
| `hle_libc.c` | glibc 2.3, libm, libgcc and libstdc++, working on guest memory |
| `hle_thread.c` | pthreads and POSIX semaphores. Guest threads are host threads; every guest wait is a host condition variable, so the core can stop all of a game's threads |
| `hle_gfx.c` | X11 and GLX (one full-screen window; pbuffers become framebuffer objects), and the OpenGL calls that need more than passing through |
| generated `hle_gl_gen.c` | The OpenGL bridge: every GL call the game names, generated from the Khronos registry |
| `glsl_fix.c` | NVIDIA-only GLSL made acceptable to a strict compiler (`half` types, Cg-style initialisers, implicit vector resizes) |
| `nv_vertex_program.c` | NVIDIA's NV vertex and fragment programs as ARB programs |
| `arb_lower.c` | NVIDIA's options on ARB programs lowered to plain ARB, for gl4es |
| `hle_lindbergh.c` | The baseboard: JVS I/O, EEPROM, SRAM, the security dongle and keychip, DIP switches |
| `hle_audio.c` | The sound card's voice API on a software mixer |
| `input.c` | Cabinet controls from the host, or from a script for unattended runs |
| `core_api.c` | The game as an `afcore` core |
| `host_core.c`, `host_core_ios.m` | The OpenGL context: EGL on Linux, CGL on macOS, OpenGL ES on iOS |

### Graphics

The games use OpenGL 2 the way NVIDIA's driver allowed: immediate mode,
fixed function, NV and ARB assembly programs, GLSL with Cg habits, and shaders
compiled at run time by NVIDIA's Cg compiler (shipped with the game, and
translated with it).

- **macOS** uses the system OpenGL's 2.1 profile. NV programs become ARB
  programs with NVIDIA's options, which Apple's driver accepts.
- **Linux** uses Mesa. GLSL goes through `glsl_fix.c`; ARB programs are
  lowered where Mesa lacks NVIDIA's options.
- **iPad** has no desktop OpenGL. The game's OpenGL 2 goes through gl4es,
  which turns it into OpenGL ES 3. The runtime preprocesses GLSL
  (`glsl_pp.c`) and lowers ARB programs (`arb_lower.c`) first, because gl4es
  works on the text. Fixes to gl4es are applied at build time from
  `cmake/gl4es_fixes.cmake`; the configure fails if one no longer applies.
- **Metal** (the default on macOS and iOS, `LB_MTLGLES`): gl4es's OpenGL ES calls go to mtlgles
  instead of Apple's OpenGL ES, and mtlgles draws them with Metal. The old
  renderers (Apple's OpenGL ES on iOS, desktop OpenGL on macOS) stay
  selectable with `-DLB_MTLGLES=OFF` for one release. mtlgles has to
  offer every OpenGL ES extension gl4es checks for when Metal can do the
  feature. gl4es's fallbacks for missing extensions are only tested on
  drivers that lack them: before mtlgles offered
  `GL_EXT_color_buffer_half_float`, gl4es's float-attachment fallback
  resized Let's Go Jungle's 800x600 scene texture to 84x64, and its 3D came
  out blocky and zoomed in (Cores-Common #33).

The core draws into an off-screen framebuffer the size the game asked for.
On iOS the front end gets each frame as a shared surface, with no copy
through the CPU; on the Mac and Linux it reads the framebuffer.

### Frames and time

The game runs on threads of its own. When it finishes a frame (its buffer
swap), `host_present` hands the frame to the front end and waits for the
front end's next `run_frame`. So the front end sets the pace, and the game's
own frame limiter sees the time it expects.

- Sound is mixed by real elapsed time per frame, so a slow frame does not
  stretch the sound.
- Games that run some scenes at 30 fps (HotD4's cut scenes) do so on purpose;
  the core does not force 60.
- Some boot screens (Race TV's "INITIALIZE" steps) wait on the clock. While
  one is up, frames don't wait for the front end, and the game's clock runs
  faster only for frames that finished early, so a slow device is not
  penalised.

### The cabinet

- **JVS I/O**: the runtime is the I/O board on the JVS bus. Light guns,
  steering, pedals, gear lever, buttons, coins and the test and service
  switches come from the front end's input. Race TV's gear lever is
  sequential and springs back to neutral, as the cabinet's does: it is out
  (player 2 up or down) while the front end's shift button is held, and for
  at least 50 ms after a press so a one-frame paddle tap still counts. The
  front end's `gear` is ignored. (Until 1.2.0 it stayed where it was pushed,
  so the game saw one shift each way and only changed between 1st and 2nd.)
- **EEPROM and SRAM** are files in the save folder. The region the front end
  picks is written into the EEPROM's system record (with its checksum) each
  time the game opens it. The game and the test menu read it there, as on a
  cabinet. Changing the region also changes the coin settings the game
  derives from it.
- **Security**: the dongle, keychip and DIP switches answer the way a
  cabinet's would. Game functions that only check them are replaced through
  `overrides.txt`.
- **Sound**: the sound card's API plays PCM voices straight from guest
  memory with loop points, volume, routing to the cabinet's speakers, pitch
  and attenuation. Some games pace movies by a voice's position, so voices
  advance in real time even with no audio device.

### Game and test menu

A cabinet runs the game program and, when Test is pressed, a separate test
menu program. Both are translated into the one core. When one exits, the core
stops all its threads, puts the runtime back to its power-on state and starts
the other, as the cabinet's start script does.

### Native replacements

Where translated code is much slower than it needs to be, the runtime
replaces it with native C that gives the same result to the bit. Example: the
movie decoder's 8x8 inverse DCT, written in MMX and SSE in the game, is done
natively (`sofdec_idct.h`), which made movie scenes smooth on slower CPUs.

## Data and settings

Per game, in `games/<set>/` (our own names, addresses and byte values only):

| File | What |
|---|---|
| `game.cmake` | The ELF names, the number of generated files |
| `overrides.txt` | Game functions the runtime replaces (`ovr_<name>`) |
| `patches.txt` | Byte patches, by symbol plus offset |
| `libs.txt` | Shared libraries the game ships, translated with it, and their load addresses |
| `keychip.txt` | The keychip identity the game expects |

`games.toml` is the game list the app's manifest is generated from.
`filelists/<set>.h` lists each game folder's files with sizes and CRCs, so the
front end can check a user's copy (`check_roms`).

The game folder holds `disk0/`, or the whole folder as one `<set>.tar`.
Saves go to the front end's save folder (`eeprom.bin`, `sram.bin`).

Front-end options (`afcore` 5.3): `region=export|usa|japan` (default
Export).

Debug switches are `LB_*` environment variables (see README.md), for
example `AF_DEBUG_LB_PERF=1` for per-frame timings and `AF_DEBUG_LB_GL_CHECK=1` for GL errors.

## Platforms

| Platform | Graphics | State |
|---|---|---|
| macOS (Apple Silicon) | Metal through gl4es and mtlgles; OpenGL 2.1 with `-DLB_MTLGLES=OFF` | All four games |
| Linux (x86-64, Mesa) | OpenGL | All four games |
| iPadOS | Metal through gl4es and mtlgles; OpenGL ES 3 with `-DLB_MTLGLES=OFF` | All four games; needs the extended virtual addressing entitlement for the 4 GB guest window |
| tvOS | — | Not yet: tvOS has no extended addressing entitlement, so it needs a smaller guest window and the Metal path |

## Testing

- **`tools/core_test`** runs a core with no window from a script of timed
  inputs (coins, start, aim, trigger, steering, pedals, screenshots), at
  the core's frame rate or as fast as it can.
- **`tools/ios-test`** is a small iPad app that does the same on a device and
  saves screenshots.
- **`tools/cabinet.sh`** loops the game and test menu like a cabinet.
- **`tools/replayfix.py`** checks that every gl4es fix still applies to the
  pinned gl4es.
- **Every game** passes its test menu and a full game to game over on each
  platform before it ships, and is checked again after any change to the
  runtime or the graphics path.
- **CI** on every pull request: the release scan (no game data, no private
  paths) and a Linux build with no game.

## Open items

1. Metal is the default on Apple platforms. Left: hands-on play of all
   four games on the iPad, and removing the OpenGL ES path after one release.
2. Race TV's boot on the iPad: gl4es emulates its many small draws on the
   CPU. On Metal that costs little (about 30 s warm on the M4), so it isn't
   being fixed.
3. HotD4 on the iPad: frame rate and missing enemies in some scenes.
4. Ghost Squad Evolution: paused on a Metal shader compiler crash.
5. Apple TV: a smaller guest window.
6. More Lindbergh games in later releases.

## Decisions

- **Static translation to C** (x86recomp), not emulation: iOS forbids a JIT,
  and C runs at native speed.
- **HLE of the operating system and libraries**, not a Linux kernel or the
  cabinet's x86 libraries.
- **Per-game files hold names and patches, never game code.** The user's copy
  is translated on the user's machine.
- **The game sets its own pace**, held to the front end's frames at
  `host_present`; 30 fps scenes stay 30 fps.
- **gl4es for OpenGL ES, and mtlgles for Metal under it**, rather than ANGLE,
  which measured slower.
- **The region goes into the EEPROM's system record**, where the game and
  test menu already read it, rather than patching each game's language code.
- **Lindbergh projects under non-commercial licences are reference only.**
  No code is copied from them; this repository is BSD-3-Clause.
