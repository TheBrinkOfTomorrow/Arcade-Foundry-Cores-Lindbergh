# Let's Go Jungle! Special: build defaults (CMakeLists.txt; title, controls and core version in games.toml).
# Its program, lgjsp_app, is Let's Go Jungle's engine with the same libraries: these are Let's Go Jungle's.
set(GAME_GEN_COUNT 120)            # gen_NNN.c files: the program and its two libraries
set(GAME_WIDTH 1024)               # the game, in the left half of the display it asks for
set(GAME_HEIGHT 768)
set(GAME_MODE_SIZE 2048 768)       # the display (_sSystem::userInitialize): the cabinet's two outputs side by side
# its guns' default calibration (acpBackup::GunDefaultSet) spans 0x6000 to
# 0xa000; on the screen the first channel of each gun is up and down and the
# second left and right, both from 0xa000 (Let's Go Jungle's, checked there on
# the Mac; not yet checked here)
set(GAME_GUN_RANGE 0xa000 0x6000 0xa000 0x6000)
set(GAME_GUN_SWAP_XY ON)
# the region the front end can choose (its language follows), Export first
set(GAME_OPTIONS "region=export|usa|japan\\n")
# CRI Sofdec's IDCT tables (DCT_IsseTrans, done natively: runtime/sofdec_idct.h):
# the row tables of rows 0-3, the row rounder, the column constants
set(GAME_SOFDEC_IDCT 0x8807ca8 0x8807ce8 0x8807d28 0x8807d68 0x8807c70 0x8807ca0 0x8807c98 0x8807c90 0x8807c88 0x8807c80 0x8807c78 0x8807c68)
