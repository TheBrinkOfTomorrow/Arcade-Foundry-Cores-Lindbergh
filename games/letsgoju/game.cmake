# Let's Go Jungle: build defaults (CMakeLists.txt; title, controls and core version in games.toml)
set(GAME_GEN_COUNT 120)            # gen_NNN.c files: the program and its two libraries
set(GAME_WIDTH 1360)               # its window; it renders 3D at 800x600 and scales up
set(GAME_HEIGHT 768)
# its guns' default calibration (acpBackup::GunDefaultSet) spans 0x6000 to
# 0xa000; on the screen the first channel of each gun is up and down and the
# second left and right, both from 0xa000 (checked on the Mac: the game's
# reticle where the front end aims)
set(GAME_GUN_RANGE 0xa000 0x6000 0xa000 0x6000)
set(GAME_GUN_SWAP_XY ON)
# the region the front end can choose (its language follows), Export first
set(GAME_OPTIONS "region=export|usa|japan\\n")
# CRI Sofdec's IDCT tables (DCT_IsseTrans, done natively: runtime/sofdec_idct.h):
# the row tables of rows 0-3, the row rounder, the column constants
set(GAME_SOFDEC_IDCT 0x87caf28 0x87caf68 0x87cafa8 0x87cafe8 0x87caef0 0x87caf20 0x87caf18 0x87caf10 0x87caf08 0x87caf00 0x87caef8 0x87caee8)
