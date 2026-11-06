# Sega Race TV: build defaults (CMakeLists.txt; title, controls and core version in games.toml)
set(GAME_WIDTH 640)                # drive.elf: the standard-definition cabinet
set(GAME_HEIGHT 480)
# the region the front end can choose (its language follows), Export first
set(GAME_OPTIONS "region=export|usa|japan\\n")
# CRI Sofdec's IDCT tables (DCT_IsseTrans, done natively: runtime/sofdec_idct.h):
# the row tables of rows 0-3, the row rounder, the column constants
set(GAME_SOFDEC_IDCT 0x884a7f0 0x884a7b0 0x884a770 0x884a730 0x884a860 0x884a830 0x884a838 0x884a840 0x884a848 0x884a850 0x884a858 0x884a868)
# "INITIALIZE STEP [ %d ]" (CInitSceneObj::DrawInit): the boot screen's frames
# don't wait (runtime/hle_libc.c, #57)
set(GAME_BOOT_TEXT 0x8565ec0)
