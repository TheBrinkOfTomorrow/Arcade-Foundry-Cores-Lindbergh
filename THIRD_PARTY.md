# Third-party components

| Component | Upstream | Licence | Use |
|---|---|---|---|
| OpenGL registry `gl.xml` | https://github.com/KhronosGroup/OpenGL-Registry (`xml/gl.xml`) | Apache-2.0 | Fetched into `extern/` at build time; `tools/glgen.py` generates the GL bridge from it. Not committed. |
| SDL2 | https://libsdl.org | zlib | Window, GL context and input on Linux. System package. |
| Capstone (Python) | https://www.capstone-engine.org | BSD-3-Clause | x86 disassembly in `tools/x86recomp`. pip package. |
| DejaVu Sans Mono | https://dejavu-fonts.github.io | Bitstream Vera / DejaVu (free, redistributable) | Glyph bitmaps in `runtime/boot_font.h`, rendered by `tools/mkfont.py`; stand-in for the cabinet's system font in the test menu. |
| gl4es | https://github.com/ptitSeb/gl4es (commit 2037865590d4) | MIT | Desktop OpenGL on OpenGL ES, iOS build only. Fetched by CMake at build time (pinned commit, SHA-256 checked) and linked statically into the iOS framework. Not committed. |
| pyelftools | https://github.com/eliben/pyelftools | Unlicense (public domain) | ELF parsing in `tools/x86recomp`. pip package. |

Reference only, no code copied:

| Project | Licence | How it is used |
|---|---|---|
| lindbergh-loader (https://github.com/lindbergh-loader/lindbergh-loader) | CC BY-NC-SA 4.0 | Run as the behavioural oracle; read for hardware facts. No code, tables or data are copied from it (its licence is incompatible with BSD-3-Clause). |
