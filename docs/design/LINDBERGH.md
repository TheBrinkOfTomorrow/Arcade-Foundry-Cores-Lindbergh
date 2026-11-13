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
| Ghost Squad Evolution | `gsevo` | Started again on Metal, for 1.4.0 or later |
| 2 Spicy | `2spicy` | Started, for 1.4.0 or later |
| Let's Go Jungle! Special | `letsgojusp` | Started, for 1.4.0 or later |
| Hummer Extreme Edition | `hummerxt` | Started, for 1.4.0 or later |
| Hummer | `hummer` | Started, for 1.4.0 or later |
| Primeval Hunt | `primevah` | Started, for 1.4.0 or later |
| Harley-Davidson: King of the Road | `hdkotr` | Started, for 1.4.0 or later |

More Lindbergh games are planned for later releases.

A note on words: in this document and in the code's comments, "the cabinet"
or "the original cabinet" is Sega's arcade machine the game was made for (its
Lindbergh board, monitors, controls and drive or touch boards), never our
Game Cabinet app. Where the app is meant it is called the front end or the
Game Cabinet app.

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
| `hle_thread.c` | pthreads and POSIX semaphores. Guest threads are host threads; every guest wait is a host condition variable, so the core can stop all of a game's threads. A free mutex goes to whoever comes first, but after 8 takers have gone ahead of a waiting thread the waiters get it in turn (a ticket each), so a thread that unlocks and locks again at once cannot keep a waiting one out for long (Primeval Hunt's main thread did, with CRI's sound); turns on every lock cost HotD4 frames |
| `hle_gfx.c` | X11 and GLX (one full-screen window; pbuffers become framebuffer objects), and the OpenGL calls that need more than passing through |
| generated `hle_gl_gen.c` | The OpenGL bridge: every GL call the game names, generated from the Khronos registry |
| `glsl_fix.c` | NVIDIA-only GLSL made acceptable to a strict compiler (`half` types, Cg-style initialisers, implicit vector resizes) |
| `nv_vertex_program.c` | NVIDIA's NV vertex and fragment programs as ARB programs |
| `arb_lower.c` | NVIDIA's options on ARB programs lowered to plain ARB, for gl4es |
| `hle_lindbergh.c` | The baseboard: JVS I/O, EEPROM, SRAM, the security dongle and keychip, DIP switches |
| `hle_audio.c` | The sound card's voice API on a software mixer |
| `hle_openal.c` | OpenAL 1.1 (2 Spicy) on the same mixer: buffers, queued and looping sources, distance attenuation and panning |
| `input.c` | Cabinet controls from the host, or from a script for unattended runs |
| `clock.c` | The one time source the game sees: the host's clocks, or in a video render 1/60 s per frame |
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
- `arb_lower.c` turns NVIDIA's branches into straight-line code: an `if`
  becomes conditional writes, and a do-while loop (`BRA` back to a label,
  2 Spicy's lighting) is unrolled eight times, each pass's writes kept only
  while a running mask says the loop is still going. `NRM` becomes
  `DP3`/`RSQ`/`MUL`. Condition codes live in plain temporaries, because
  gl4es drops `RC`/`HC`. A `KIL` on a condition (`KIL LE.x`, Harley-Davidson's
  alpha tests) becomes a mask from the condition code and `KIL` of its
  negation.
- Immediate mode (`glBegin`/`glEnd`) with ARB programs: gl4es keeps a
  vertex's texture coordinate on OpenGL ES 2 and later whatever texturing is
  enabled (a gl4es fix: it kept it only for a unit with `GL_TEXTURE_2D`
  on), and the runtime does not also set the generic attribute a
  conventional input aliases on NVIDIA, which gl4es on Metal already lays
  out the same way (texcoord 0 is attribute 8); a `glVertexAttrib` between
  `glBegin` and `glEnd` ended the primitive in gl4es. Harley-Davidson's text
  came out black, one texture coordinate a quad.
- gl4es's ARB parser takes less than NVIDIA's, so `arb_lower.c` also
  rewrites what it refuses or mistranslates: OUTPUT aliases are written as
  their bindings, relative addressing as `[a.x + n]`, unused
  `state.matrix.program` declarations are dropped, and a RECT lookup becomes
  a 2D one scaled by 1 / the texture's size (environment parameters 24-31).
- Ghost Squad Evolution's lights loop over the lights of each kind. Unrolled
  behind a mask, an unused light's 1/0 times the mask's 0 gives NaN on Metal
  (NVIDIA's hardware gave 0), so each program is unrolled for the light
  counts in its environment at the time instead, a variant per count.
- `glXGetProcAddress` answers for a name with or without its `EXT`/`ARB`
  suffix, because gl4es on Metal knows some extension functions only by
  their core names (2 Spicy's `glGenFramebuffersEXT`).

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
- Every clock the game can read goes through `clock.c`: rdtsc, `time`,
  `gettimeofday`, `ftime`, the sleeps and `select`'s timeout,
  `pthread_cond_timedwait`, `GLUT_ELAPSED_TIME`, the boot's frame timing,
  the sound mixer and the gear lever's hold. Normal play reads the host's
  clocks, as before.
- **Video renders** (the afcore option `clock=frames`, which the app's
  `--video` sets) run far slower than real time, so there game time follows
  the frames instead: it runs at the real rate within a frame but stops 1/60 s
  after the last frame's end until the next `run_frame`, a sleep or timed
  wait past that point holds its thread, and `run_frame` mixes exactly 800
  samples (1/60 s at 48 kHz) of sound. A frame ends once its time has run and
  the game has presented, or the thread that presents waits for time past the
  frame (a 30 fps cut scene, the boot screen's limiter), or no picture has
  come yet (loading); a picture more than 1/4 s late is not waited for again
  until one comes. Between frames the game's threads are held at their next
  library call, so a load takes as many frames as in normal play whatever the
  render's speed (a render never runs faster than real time). The sound then
  stays with the picture however slow the render.

### The cabinet

- **JVS I/O**: the runtime is the I/O board on the JVS bus. Light guns,
  steering, pedals, gear lever, buttons, coins and the test and service
  switches come from the front end's input. Race TV's gear lever is
  sequential and springs back to neutral, as the cabinet's does: it is out
  (player 2 up or down) while the front end's shift button is held, and for
  at least 50 ms after a press so a one-frame paddle tap still counts. The
  front end's `gear` is ignored. (Until 1.2.0 it stayed where it was pushed,
  so the game saw one shift each way and only changed between 1st and 2nd.)
- **Let's Go Jungle! Special's motion seat** talks to its own board over
  the serial port /dev/ttyS0 (7-byte commands, 22-byte status replies). The
  runtime is that board: ready at once, its INITIALIZE button pressed when
  the game asks for it, the seat at its start position after the initialize
  command, every sensor (armrests, belts, obstacles) clear. The ride itself
  doesn't move. The game asks for a 2048x768 display (its cabinet's two
  outputs side by side) and draws in the left 1024x768: the X server offers
  that mode (`GAME_MODE_SIZE`) and the window is the 1024x768 the game uses.
- **Primeval Hunt's touch panel** is on /dev/tts/0 (10-byte frames); the
  runtime answers as the panel, with the front end's touch (below).
- **A twin cabinet's second wheel** (Hummer) is held at its centre on its
  JVS analog channel (`GAME_WHEEL2_ANALOG`); no player drives it.
- **Steering drive boards** on /dev/tts/0 take 4-byte packets (a command,
  two 7-bit data bytes, an XOR check). Race TV's answers with the wheel's
  position, OutRun 2 SP's and Hummer's with a status byte per wheel
  (`GAME_DRIVE_BOARD position`, `status` or `ready`): OutRun 2 SP's reports
  its motor check busy once, Hummer's is always ready. Harley-Davidson's
  handlebar board (`bike`) answers with status bytes until the game's first
  read, then always with 4 bytes (below). A device is always ready to be
  written to (Hummer selects before each write).
- **The network**: sockets are the host's, kept on the loopback (`hle_libc.c`),
  with Linux's behaviour where the host's differs: a non-blocking connect on
  the loopback is made by the time it returns (an accept right after finds
  it), connecting again then answers 0, and a failing call's errno is in the
  guest's errno cell for a game that took its address before the call. A
  stand-alone cabinet that is its own master (Harley-Davidson) gets the
  master's address for the region (`GAME_NET_HOST`), and its own connections
  come from that address, not 127.0.0.1. Network play between cabinets is
  not emulated.
- **EEPROM and SRAM** are files in the save folder. The region the front end
  picks is written into the EEPROM's system record (with its checksum) each
  time the game opens it. The game and the test menu read it there, as on a
  cabinet. Changing the region also changes the coin settings the game
  derives from it.
- **Security**: the dongle, keychip and DIP switches answer the way a
  cabinet's would. Game functions that only check them are replaced through
  `overrides.txt`. The keychip's encryption and amLib's security chip
  (`amsInit`, `amsUpdate`) are taken as passed; where a game compares what
  the keychip decrypted, the comparison is patched (Hummer Extreme).
- **A new cabinet's settings**: where a game refuses the factory defaults
  an operator would change in the test menu (a coin assignment, an
  uncalibrated pedal), the game's own defaults are patched in its
  `patches.txt`, so a new save folder starts playable.
- **Sound** through ALSA (Primeval Hunt's CRI output: 5.1 PCM) or OpenAL
  goes into the same mixer as the sound card's voices.
- **Sound**: the sound card's API plays PCM voices straight from guest
  memory with loop points, volume, routing to the cabinet's speakers, pitch
  and attenuation. Some games pace movies by a voice's position, so voices
  advance in real time even with no audio device.

### Primeval Hunt's display

On the original cabinet (Sega's hardware, not our Game Cabinet app), Primeval
Hunt goes full screen (`glutFullScreen`) on a 1280x1024 X screen and draws two
640x480 (4:3) views in its bottom 480 rows, each on its own monitor: the
touch/map view at x 0-639 and the gameplay view at x 640-1279. The touch
panel's coordinates are in the touch view's 640x480 (0-4095 each way as the
panel reports them). Off screen it draws a 1024x1024 shadow map into the same
framebuffer and copies it to a texture, and makes its blurred copies of the
scene in a 2048x2048 texture; neither is ever shown.

The core gives the game a 1280x1024 framebuffer (`GAME_FB_SIZE`) and sends
the front end only the bottom 1280x480: both views, side by side, at their
own size (chosen 12 November 2026). The guns aim at the gameplay
view (`GAME_GUN_VIEW_X0 0.5`: gun x 0.5-1 across the picture is that view's
0-1); a pointer (a mouse click, a finger) on the left half with the trigger
pulled becomes a touch on the panel there, and does not shoot. The front end
sends gun_x/gun_y across the whole picture and the trigger, as for any gun
game.

Later options: the gameplay view alone, the halves swapped, or the map as an
inset in the gameplay view. Each is a change to these settings and to the
part of the framebuffer read back (and the input mapping), not to the
game's bring-up, and any of them could become a per-game front-end setting
as the region is.

### Harley-Davidson's cabinet

On the original cabinet (Sega's arcade machine, not our Game Cabinet app) the
player sits on a motorbike: a handlebar that turns, with a motor behind it
for force feedback, a twist throttle grip, a brake lever and a start button.
The game draws one 1280x768 screen at 60 Hz (the DIP switches the runtime
reports; it also has 640x480 assets for the other setting). The core sends
the front end that 1280x768 picture. The front end's driving controls map
onto the bike's JVS analog channels in its own order (`GAME_DRIVE_BOARD bike`):
the accelerator is the throttle grip (channel 0: twisting it also makes the
choices on the select screens, "Turn The Grip To Decide"), steering is the
handlebar (channel 1, centred at the middle), the brake is the brake lever
(channel 2). The other driving games have steering on channel 0.

The handlebar board is on /dev/tts/0 (`GAME_DRIVE_BOARD bike`). It takes Sega's
drive board packets (a command with bit 7 set, two 7-bit data bytes, an XOR
check). It answers each with a status byte (0: ready) until the game's first
read (command 0x7a, its second data byte 0x10-0x17 saying what to read);
from then on the game takes every answer as 4 bytes: 0x80 | a code, two
7-bit data bytes, an XOR check. A read's answer is what it read (0x14: the
handlebar's 14-bit position, centred here); other commands get their own
code and data back. A status byte with bits set, or a 4-byte answer with bit
5 set, is an alarm (Errors 7301-7303); a malformed answer is Error 7304
("Serial DataError.").

Its network check runs even stand-alone: the cabinet is its own master at
192.168.1.9 (Export), .5 (USA) or .1 (Japan), port 50821, 50820 or 50819,
connects to it and exchanges packets with itself over TCP and UDP
(`GAME_NET_HOST "9,5,1"`); a new backup's network setting is stand-alone. Its
test menu is the same program started with `-t`: TEST makes the game exit,
and the core starts the other (`GAME_TEST_ON_EXIT`).

### Game and test menu

A cabinet runs the game program and, when Test is pressed, a separate test
menu program. Both are translated into the one core. When one exits, the core
stops all its threads, puts the runtime back to its power-on state and starts
the other, as the cabinet's start script does. Harley-Davidson has one
program: the game exits on Test and its menu is the program run with `-t`;
after either exits the core starts the other (`GAME_TEST_ON_EXIT`).

### libstdc++

Most games link their C++ library statically. Hummer links libstdc++.so.6,
which the cabinet's system disk has and the game folder does not, so the
runtime supplies it (`hle_cxx.c`): the string members and exception classes
it calls, and file and string streams. A program inlines much of the library's
templates and copies the library's vtables, VTTs, cout and cerr into its own
data; the runtime fills those copies at load, in libstdc++ 6's layouts, with
its own functions behind the virtual slots.

### Native replacements

Where translated code is much slower than it needs to be, the runtime
replaces it with native C that gives the same result to the bit. Example: the
movie decoder's 8x8 inverse DCT, written in MMX and SSE in the game, is done
natively (`sofdec_idct.h`), which made movie scenes smooth on slower CPUs.

### Programs without symbols

Most Lindbergh programs keep their symbol table, so x86recomp finds every
function from it and `overrides.txt` and `patches.txt` name functions. 2 Spicy's
programs are stripped. For those x86recomp finds functions from the program's
entry, `.init`/`.fini` and the constructor tables, the `.eh_frame` unwind
records, data words and immediates that point at a function prologue, and
vtables, then follows calls (Cores-Common's recomp.py; 22,354 functions in
2 Spicy's game program, 23,008 before the check below). A data word or a vtable entry is not taken for a
function when it lands inside a function `.eh_frame` describes, or in the
middle of an instruction decoding from the known function start before it
(Harley-Davidson's rodata has 0, 0, then a word that points into a
constructor; 532 such starts in its program). `overrides.txt` takes
`0xaddress name` lines for these programs. The AM library is linked
statically and a stripped program's copy of a build is the same code as in
a program with symbols that has it: `tools/match_functions.py` finds those
functions by their code (Harley-Davidson's are Race TV's). 2 Spicy's test
menu still stops at "Error 15 Game Program Not Found": its overrides by
address are not written yet.

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
Export), and on every set `clock=realtime|frames` (frames for video renders,
#95). The core is built against `afcore` 5.4 but has no save states: the
game runs on host threads, so a frame's state can't be written out.

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
  the core's frame rate or as fast as it can. For renders:
  `CORE_TEST_OPTION=clock=frames`, `CORE_TEST_SLOW=ms` (a slow renderer),
  `CORE_TEST_WAV` (the sound) and `CORE_TEST_HASH` (each frame's screen
  hash, sound so far and brightness).
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
4. Ghost Squad Evolution: running on the Metal path; a full mission, its test menu, aim and the iPad still to check.
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
