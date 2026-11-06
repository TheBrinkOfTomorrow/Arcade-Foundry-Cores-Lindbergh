#!/usr/bin/env python3
"""Write the files that list this repo's games from games.toml.

  gen_games.py          rewrite them
  gen_games.py --check  only compare (CI): exit 1 when one is out of date

Writes cmake/games.cmake: LB_GAMES and each game's title, controls (gun or
driving) and core version, which CMakeLists.txt uses over the defaults. A
game's `build` table has its program and test-menu program under the game
folder (the app's build_cores.py reads them). A new game: its [[game]] in
games.toml and its games/<set>/ folder, then this.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "common", "tools", "games"))
import afgames  # noqa: E402

GUN = {"gun", "turret", "analog-gun"}


def generate(out):
    games = afgames.load_core(os.path.join(ROOT, "games.toml"))
    cm = ["# Written by tools/gen_games.py from games.toml: edit that, not this.\n",
          f"set(LB_GAMES {afgames.cmake_list(g['set'] for g in games)})\n"]
    for g in games:
        if g["board"] != "lindbergh":
            raise afgames.ManifestError(f"{g['set']}: only Lindbergh games belong in this repo")
        if "program" not in g["build"]:
            raise afgames.ManifestError(f"{g['set']}: build.program (its main ELF under the game folder) is required")
        s = g["set"]
        cm.append(f"set(LB_TITLE_{s} {afgames.c_string(g['title'])})\n")
        cm.append(f"set(LB_CONTROLS_{s} {'gun' if g['controls'] in GUN else 'driving'})\n")
        cm.append(f"set(LB_CORE_VERSION_{s} {g['core_version']})\n")
    out.write("cmake/games.cmake", "".join(cm))


if __name__ == "__main__":
    afgames.main(generate, ROOT, __doc__)
