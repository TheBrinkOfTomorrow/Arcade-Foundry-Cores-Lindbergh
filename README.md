# Arcade Foundry Cores — Sega Lindbergh

Static recompilation of Sega Lindbergh arcade games for the Arcade Foundry: Insert Coin front
end. A Lindbergh cabinet is a Pentium 4 PC running Linux; each game is one
32-bit x86 ELF that talks to OpenGL 2, X11, pthreads, libc and a handful of Sega
devices. This project:

1. **translates the game's x86 code to C** ahead of time (`common/tools/x86recomp`, shared with Chihiro),
   so it builds for ARM64 and runs where JIT is not allowed (iOS, tvOS);
2. **replaces the libraries and hardware** the game uses with host code
   (`runtime/`): libc and the C++ runtime, pthreads, X11/GLX, an OpenGL bridge
   generated from the Khronos registry, the baseboard with its JVS I/O bus,
   the EEPROM, the security dongle and the DIP switches;
3. keeps per-game knowledge small and readable in `games/<game>/`:
   functions the runtime replaces and byte patches, by symbol name.

No game code or data is in this repository. You supply your own dump; the
build translates it on your machine.

## Status

| Game | State |
|---|---|
| The House of the Dead 4 (Rev A) | Plays on Linux and macOS (Apple Silicon): attract mode with movies and 3D demos, sound, a full game to Game Over and the rankings, and the test menu. iOS: the attract mode runs in the iPad simulator (OpenGL ES through gl4es); not yet on a device. |
| Let's Go Jungle! | Plays on macOS: attract mode, a scripted game, sound; also as a core. |
| Sega Race TV | Plays on macOS: attract mode, a race with steering, pedals and the view button, the test menu; also as a core with driving controls. |
| OutRun 2 SP SDX | Plays on macOS: attract mode, the transmission, mode and music selects, a race with steering, pedals, the view button and the gear lever, sound (the engine follows the revs), and its test program; also as a core with driving controls. |
| Initial D Arcade Stage 4 | Not started. |

## Building (Linux)

Requirements: CMake 3.16+, a C compiler, SDL2, OpenGL, Python 3.11+ with
`capstone` and `pyelftools` (the shared venv: `common/tools/afic-venv/setup.sh`,
see [The Python venv](#the-python-venv)).

```sh
mkdir -p extern
curl -L -o extern/gl.xml \
  https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml
cmake -S . -B build -DGAME=hotd4a -DGAME_ELF=/path/to/hotd4a/disk0/elf/hod4M.elf \
  -DGAME_TEST_ELF=/path/to/hotd4a/disk0/elf/hod4testM.elf
cmake --build build -j
```

### macOS (Apple Silicon)

```sh
brew install cmake sdl2 e2fsprogs p7zip
git submodule update --init
common/tools/afic-venv/setup.sh      # the shared Python venv, ~/venvs/afic (once)
# then the same curl and cmake lines as above, adding -DCMAKE_BUILD_TYPE=Release
```

### The Python venv

The translator (`common/tools/x86recomp/recomp.py`) runs at build time and needs the
Python packages `capstone` and `pyelftools`. They come from the one shared
venv every Arcade Foundry repo uses, `~/venvs/afic` (`AFIC_VENV` to put it
elsewhere), never from the system's Python. Make it once per machine, and
again when Cores-Common's `tools/afic-venv/requirements.txt` changes:

```sh
common/tools/afic-venv/setup.sh
```

CMake uses it on its own when `-DPython3_EXECUTABLE` is not given and the
build folder is new (a build folder configured before keeps the Python it
found then). The app's `build_cores.py` uses it too. A `venv/` folder in this
checkout from before is no longer used and can be deleted. A build without
the packages stops with `ModuleNotFoundError: No module named 'capstone'`.

The runtime uses the system OpenGL framework's legacy 2.1 profile (GLSL 1.20).
A TeknoParrot-style dump (`hotd4.bin`, an ext2 image) unpacks without root:
`$(brew --prefix e2fsprogs)/sbin/debugfs -R "rdump / /path/to/hotd4" hotd4.bin`
(the "changing ownership" warnings are harmless).

Translation takes about a minute; compiling the generated C a few minutes.
`GAME_TEST_ELF` is optional and builds `lindbergh-run-test`, the cabinet's test
menu, which is a separate program. Building the font header again
(`python3 tools/mkfont.py > runtime/boot_font.h`) needs ImageMagick and is only
needed to change the font.

### The core library for Arcade Foundry: Insert Coin

Add `-DLB_CORE=ON` (needs `GAME_TEST_ELF`) to also build `afcore_hotd4a`, the
game as a core the Arcade Foundry: Insert Coin app loads: a shared library on Linux (EGL),
a framework on macOS. It exports one function, `afcore_api_hotd4a`, with the
interface in `common/include/afcore.h`. Its "ROM" is the game folder (the one that
holds `disk0/`). `core_test` drives it without a window:

```sh
build/core_test build/libafcore_hotd4a.so hotd4a /path/to/hotd4a /tmp/saves script.txt
```

where each script line is `<frame> coin|start|start2|test|service`,
`<frame> trigger on|off`, `<frame> aim X Y` (0..1), `<frame> steer V` (-1..1),
`<frame> gas|brake V` (0..1), `<frame> view|shiftup|shiftdown|boost`,
`<frame> shot NAME`, `<frame> restart` or `<frame> end`. With `CORE_TEST_PACE=1`
it runs at the core's frame rate, as the front end does (OutRun 2 SP times its
loading by the clock); otherwise as fast as it can.

The translated C is compiled at `-O1`; `-DLB_GEN_OPT=-O2` tries `-O2` for a
game (a longer build; `core_test` prints the frames a second to compare).
Measured on HotD4 (3600 frames of attract mode, 2026-11-03) it gained
nothing: 51.9 against 52.3 fps on an Apple M6, 49.1 against 49.6 on a
Ryzen 5600X with Mesa, inside run-to-run noise, for a build half again as
long. The game's frame-rate manager and the GPU set the pace, not the CPU.

### iOS (iPadOS)

The core builds for iOS as a framework (no `lindbergh-run`). The games'
desktop OpenGL 2 runs on OpenGL ES through gl4es, which CMake fetches at a
fixed commit (see `THIRD_PARTY.md`). Use Ninja:

```sh
cmake -S . -B build-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_BUILD_TYPE=Release \
  -DGAME=hotd4a -DGAME_ELF=... -DGAME_TEST_ELF=...
cmake --build build-ios -j
```

`iphonesimulator` instead of `iphoneos` builds for the simulator, where
`core_test` runs with `xcrun simctl spawn <device> build-ios/core_test.app/core_test ...`.
The app that loads the core needs the
`com.apple.developer.kernel.extended-virtual-addressing` entitlement (the
game's 4 GB address window).

## Running

```sh
cd /path/to/hotd4a/disk0/elf
/path/to/build/lindbergh-run ./hod4M.elf
```

Sega Race TV runs from `disk0` (`lindbergh-run ./drive.elf`; `-t` starts its
test menu). OutRun 2 SP SDX runs from `disk0/Jennifer` (`lindbergh-run ./Jennifer`);
its test menu is `disk0/JenTest/JenTest` (`lindbergh-run-test`). Like the cabinet, pressing Test ends the game program. To get the test menu
and return to the game afterwards, run both through the cabinet loop:

```sh
tools/cabinet.sh build/lindbergh-run /path/to/hod4M.elf \
                 build/lindbergh-run-test /path/to/hod4testM.elf
```

The test menu's font lives on the cabinet's system disk, which is not part of
the game; the runtime supplies a stand-in rendered from DejaVu Sans Mono.

The game directory is the cabinet's `/home` (it holds `disk0/`). Saves go to
`save/` next to `disk0/` (`eeprom.bin`, `sram.bin`), `/tmp` maps to `tmp/`.

Controls: mouse aims the player 1 gun, left button fires, right button
reloads; `1`/`2` start, `5`/`6` coin, `F2` test, `F1` service.

Environment variables (the cores' debug switches; every core's are listed in
Arcade-Foundry-Cores-Common's `docs/DEBUG-SWITCHES.md`). The old names without
`AF_DEBUG_` still work in 1.3.0 and go in 1.4.0:

| Variable | Effect |
|---|---|
| `AF_DEBUG_LB_VERBOSE=1` | Log file and device access. |
| `AF_DEBUG_LB_SHOT_EVERY=N` | Save a PPM of every Nth frame. |
| `AF_DEBUG_LB_GL_TRACE=N` or `N-M` | Log every GL and library call during frame N (or frames N to M). Frame 0 is everything before the first swap. |
| `AF_DEBUG_LB_GL_CHECK=1` | Check `glGetError` after every GL call and log the calls that raise errors. |
| `AF_DEBUG_LB_SHOT_FBO=N` | Screenshots read the game's framebuffer object N instead of the screen. |
| `AF_DEBUG_LB_SHADER_DUMP=1` | Write each shader, after the GLSL fixes, to `shader_NNN.glsl`, and each ARB assembly program to `arbprog_NNN.txt`. Shaders that still fail are written to `shader_fail_NN.glsl` (ARB: `arbprog_fail_NN.txt`) regardless. |
| `AF_DEBUG_LB_INPUT_SCRIPT=file` | Play scripted input. One command a line: `<frame> coin\|start\|service\|reload\|test [player]`, `<frame> aim <player> <x> <y>` (0-65535), `<frame> fire\|sweep <player> on\|off`, `<frame> analog <channel> <0..1>`, `<frame> button <player> up\|down\|left\|right\|b1..b4\|<bit> on\|off`, `<frame> press <player> <bit>`. |
| `AF_DEBUG_LB_AUDIO_WAV=file` | Also write the mixed sound to a WAV file. |
| `AF_DEBUG_LB_FRAME_CLOCK=1` | Mix 1/60 s of sound per video frame instead of using the audio device (for repeatable captures). |
| `AF_DEBUG_LB_DIPSW=0x60` | DIP switch byte (default 0x60: 1280x768, horizontal). |
| `AF_DEBUG_LB_SAVE_DIR=dir` | Where `eeprom.bin` and `sram.bin` live. |

## Licence

BSD-3-Clause. See `LICENSE` and `THIRD_PARTY.md`.

## Version numbers

The app and each game core are versioned separately. The release number on this
repository (for example v1.0.0) is a snapshot label: it means "as shipped with
Arcade Foundry: Insert Coin 1.0.0". Every game core also has its own x.y.z
version (shown in the app), which can be higher or lower than the release number.
A core's major number only changes when its saves stop carrying over.
