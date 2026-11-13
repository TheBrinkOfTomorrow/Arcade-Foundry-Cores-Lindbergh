# Release notes

Newest first. Each release is also on the repository's Releases page.

## 1.3.0 (2026-11-12)

**Arcade Foundry Cores: Lindbergh 1.3.0**

The Lindbergh cores as shipped with Insert Coin 1.3.0.

- **Video renders keep the game's sound:** with the afcore option
  `clock=frames`, which the app's `--video` sets, game time follows the
  frames (1/60 s each, 800 samples of sound a frame) however slow the render,
  so the sound stays in step with the picture (#95). Normal play is
  unchanged. Every set offers `clock=realtime|frames`; core versions: HotD4,
  Let's Go Jungle and OutRun 2 SP 1.1.0, Race TV 1.2.0.
- **afcore 5.4:** the cores build against the new interface. Lindbergh games
  have no save states (they run on host threads), so the save-state calls
  stay empty and the app offers none for them.
- **Not in the app yet:** first bring-ups of 2 Spicy (`2spicy`), Let's Go
  Jungle! Special (`letsgojusp`), Hummer and Hummer Extreme Edition
  (`hummer`, `hummerxt`), Primeval Hunt (`primevah`), Harley-Davidson: King of
  the Road (`hdkotr`), and Ghost Squad Evolution on Metal (`gsevo`) are in
  this repository for later releases (1.4.0 or later). Insert Coin 1.3.0
  doesn't list them.

## 1.2.0 (2026-11-06)

**Arcade Foundry Cores: Lindbergh 1.2.0**

The Lindbergh cores as shipped with Insert Coin 1.2.0, which carries the 1.1.0
and 1.2.0 work together (there was no separate 1.1.0 release).

- **Two more games:** Let's Go Jungle! Lost on the Island of Spice (`letsgoju`)
  and Sega Race TV (`segartv`), in the code since 1.0.0, are now in the app's
  library, on macOS, Linux and iPad, with The House of the Dead 4 and OutRun 2
  SP SDX. Let's Go Jungle's crosshair is off by default.
- **Metal on Apple devices:** on the Mac and the iPad the games now draw
  through Arcade-Foundry-Cores-Common's mtlgles (OpenGL ES on Metal) by
  default. `-DLB_MTLGLES=OFF` builds the old renderer (desktop OpenGL 2.1 on
  the Mac, Apple's OpenGL ES on the iPad) for this release only.
- **Region:** each game has a Region setting (Japan or Export; afcore 5.3)
  that the runtime writes into the cabinet's EEPROM record, which the game and
  its test menu read. The House of the Dead 4 and OutRun 2 SP follow it too.
- **Download check:** `check_roms` checks a game against a list of its files
  made from verified dumps, so the app's Downloads can say whether a game is
  complete and right.
- **Linux:** HotD4's stutter fixed (the sound clock) and a steady 60 fps in
  play (array copies cached); Let's Go Jungle's operator mode and board start
  no longer wait; Race TV's in-race sound (the sound API's voice settings) and
  its select-screen movies (a native Sofdec IDCT); OutRun 2 SP's depth and
  stencil textures; ARB program address offsets.
- **iPad:** HotD4's depth copy is a GPU blit; occlusion queries, cube maps,
  sampler state and DXT textures through gl4es; gl4es's shader cache; the
  game's per-frame objects are freed each frame.
- **Start-up:** frames during a game's boot no longer wait for the screen, so
  Race TV boots in about 16 to 18 seconds on the Mac (36 before).
- **Safety:** a game's file paths stay inside its game and save folders, and a
  fatal error in the game stops the core instead of the app.
- **Build:** gl4es is downloaded once per machine into a shared cache; the
  build's Python comes from one shared environment per machine, `~/venvs/afic`
  (Arcade-Foundry-Cores-Common's `tools/afic-venv/setup.sh`);
  `-DLB_GEN_OPT` sets the optimisation level of the translated C (`-O1` by
  default; `-O2` measured no faster).
- **Known issues:** Race TV's first start is slow on the iPad (several
  minutes on an M1 iPad, #57); the very first start of a game on Metal compiles
  its shaders and runs slowly, without sound, until they are built (#73).
- Sega Race TV on Metal (Mac and iPad): the cars are no longer invisible
  or painted with noise, and car select shows the right colours.
- Let's Go Jungle on Metal (Mac and iPad): its 3D no longer comes out
  blocky and zoomed in. The fix is in Cores-Common's Metal layer.
- Debug switches: the developer environment variables are now
  `AF_DEBUG_` plus their old name (`AF_DEBUG_LB_PERF`); the old
  names still work in this release. The list is in Arcade-Foundry-Cores-Common's
  docs/DEBUG-SWITCHES.md.
- Build: the core interface header and the x86 to C translator (x86recomp) now come from the shared Arcade-Foundry-Cores-Common repo (the `common/` submodule). Clone with `--recursive`, or run `git submodule update --init`.

## 1.0.0 (2026-11-02)

**Arcade Foundry Cores: Lindbergh 1.0.0**

First release of the Sega Lindbergh cores for Arcade Foundry: Insert Coin, as shipped with Insert Coin 1.0.0.

- **Games in 1.0.0:** The House of the Dead 4 (`hotd4a`) and OutRun 2 SP SDX (`outr2sdx`), on macOS (Apple silicon) and Linux. HotD4 plays a full game to Game Over, with sound and the test menu.
- **In the code, not in this release:** Let's Go Jungle! and Sega Race TV are in the repository but not in the app's library yet. Initial D Arcade Stage 4 hasn't been started.
- **How it works:** a Lindbergh cabinet is a Pentium 4 PC running Linux and each game is one 32-bit x86 program. The game's x86 code is translated to C ahead of time, so it also builds for ARM64 and where JIT isn't allowed. The libraries and hardware it uses (OpenGL, X11, pthreads, the baseboard, the dongle) are replaced with host code.
- **iPhone & iPad:** the core builds for iOS, but the game needs an entitlement that only paid Apple developer teams get. See the app's GAMES.md. Apple TV isn't supported.
- **Building:** see the README. It needs CMake, Python 3 with `capstone` & `pyelftools`, and SDL2.

No game code or data is in this repository. You supply your own dump, and the build translates it on your machine. BSD-3-Clause; third-party code is in `THIRD_PARTY.md`.

The release number is a snapshot label for "as shipped with Insert Coin 1.0.0"; each game core has its own version.
