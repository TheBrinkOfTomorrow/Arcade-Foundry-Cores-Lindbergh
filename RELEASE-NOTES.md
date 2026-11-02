# Release notes

Newest first. Each release is also on the repository's Releases page.

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
