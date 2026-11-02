# Arcade Foundry Cores — Sega Lindbergh

Static recompilation of Sega Lindbergh arcade games for the Arcade Foundry: Insert Coin front
end. A Lindbergh cabinet is a Pentium 4 PC running Linux; each game is one
32-bit x86 ELF that talks to OpenGL 2, X11, pthreads, libc and a handful of Sega
devices. This project:

1. **translates the game's x86 code to C** ahead of time (`tools/x86recomp`),
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

Requirements: CMake 3.16+, a C compiler, SDL2, OpenGL, Python 3 with
`capstone` and `pyelftools`.

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
python3 -m venv venv && venv/bin/pip install capstone pyelftools
# then the same curl and cmake lines as above, adding
#   -DPython3_EXECUTABLE=$PWD/venv/bin/python -DCMAKE_BUILD_TYPE=Release
```

### The Python venv

The translator (`tools/x86recomp/recomp.py`) runs at build time and needs the
Python packages `capstone` and `pyelftools`. Homebrew's Python refuses
`pip install` into itself, so make a virtual environment once, in the root
of this checkout and named `venv` (it is git-ignored):

```sh
python3 -m venv venv
venv/bin/pip install capstone pyelftools
```

Then pass `-DPython3_EXECUTABLE=$PWD/venv/bin/python` to CMake. When this
repo is the Insert Coin app's submodule (`cores/arcade-foundry-cores-lindbergh`),
make the venv inside the submodule folder: the app's `build_cores.py` uses
`<this checkout>/venv/bin/python` when it exists and otherwise the Python it
runs under. A build without them stops with
`ModuleNotFoundError: No module named 'capstone'`.

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
interface in `include/afcore.h`. Its "ROM" is the game folder (the one that
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

### iOS (iPadOS)

The core builds for iOS as a framework (no `lindbergh-run`). The games'
desktop OpenGL 2 runs on OpenGL ES through gl4es, which CMake fetches at a
fixed commit (see `THIRD_PARTY.md`). Use Ninja:

```sh
cmake -S . -B build-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=$PWD/venv/bin/python \
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

Environment variables:

| Variable | Effect |
|---|---|
| `LB_VERBOSE=1` | Log file and device access. |
| `LB_SHOT_EVERY=N` | Save a PPM of every Nth frame. |
| `LB_GL_TRACE=N` or `N-M` | Log every GL and library call during frame N (or frames N to M). Frame 0 is everything before the first swap. |
| `LB_GL_CHECK=1` | Check `glGetError` after every GL call and log the calls that raise errors. |
| `LB_SHOT_FBO=N` | Screenshots read the game's framebuffer object N instead of the screen. |
| `LB_SHADER_DUMP=1` | Write each shader, after the GLSL fixes, to `shader_NNN.glsl`, and each ARB assembly program to `arbprog_NNN.txt`. Shaders that still fail are written to `shader_fail_NN.glsl` (ARB: `arbprog_fail_NN.txt`) regardless. |
| `LB_INPUT_SCRIPT=file` | Play scripted input. One command a line: `<frame> coin\|start\|service\|reload\|test [player]`, `<frame> aim <player> <x> <y>` (0-65535), `<frame> fire\|sweep <player> on\|off`, `<frame> analog <channel> <0..1>`, `<frame> button <player> up\|down\|left\|right\|b1..b4\|<bit> on\|off`, `<frame> press <player> <bit>`. |
| `LB_AUDIO_WAV=file` | Also write the mixed sound to a WAV file. |
| `LB_FRAME_CLOCK=1` | Mix 1/60 s of sound per video frame instead of using the audio device (for repeatable captures). |
| `LB_DIPSW=0x60` | DIP switch byte (default 0x60: 1280x768, horizontal). |
| `LB_SAVE_DIR=dir` | Where `eeprom.bin` and `sram.bin` live. |

## Licence

BSD-3-Clause. See `LICENSE` and `THIRD_PARTY.md`.
