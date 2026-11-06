# OutRun 2 SP SDX: build defaults (CMakeLists.txt; title, controls and core version in games.toml)
set(GAME_WIDTH 1280)
set(GAME_HEIGHT 768)
set(GAME_DRIVE_BOARD status)     # answers with status bytes; homes on the JVS steering
# CRI Sofdec's IDCT tables (DCT_IsseTrans, done natively: runtime/sofdec_idct.h):
# the row tables of rows 0-3, the row rounder, the column constants
set(GAME_SOFDEC_IDCT 0x8252a90 0x8252ad0 0x8252b10 0x8252b50 0x8252a58 0x8252a88 0x8252a80 0x8252a78 0x8252a70 0x8252a68 0x8252a60 0x8252a50)
# the region the front end can choose (its language follows), Export first
set(GAME_OPTIONS "region=export|usa|japan\\n")
