#!/bin/sh
# List the OpenGL command names game binaries mention: glgen.py wraps these
# beyond the GL 2.1 core, so extensions the game asks for by name are found.
# Each name also comes with ARB, EXT and NV suffixes: some games build those
# names at run time (Let's Go Jungle asks for "glCreateProgramObject" + "ARB").
# glgen.py wraps only names the registry has, so extra ones cost nothing.
# usage: glnames.sh out.txt game.elf [more.elf...]
out=$1
shift
strings -n 4 "$@" | grep -E '^gl[A-Z][A-Za-z0-9]+$' \
  | awk '{ print; print $0 "ARB"; print $0 "EXT"; print $0 "NV" }' | sort -u > "$out"
