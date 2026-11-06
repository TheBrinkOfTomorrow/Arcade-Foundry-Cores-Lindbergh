# The House of the Dead 4 (Rev A): build defaults beyond CMakeLists.txt's
# CRI Sofdec's IDCT tables (DCT_IsseTrans, done natively: runtime/sofdec_idct.h):
# the row tables of rows 0-3, the row rounder, the column constants
set(GAME_SOFDEC_IDCT 0x84cfca0 0x84cfce0 0x84cfd20 0x84cfd60 0x84cfc68 0x84cfc98 0x84cfc90 0x84cfc88 0x84cfc80 0x84cfc78 0x84cfc70 0x84cfc60)
# the region the front end can choose (its language follows), Export first
set(GAME_OPTIONS "region=export|usa|japan\\n")
