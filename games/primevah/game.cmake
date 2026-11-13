# Primeval Hunt: build defaults (CMakeLists.txt; title, controls and core version in games.toml)
set(GAME_GEN_COUNT 72)             # gen_NNN.c files: its program has no symbols, so more functions are cut
set(GAME_CWD /home/disk0/prog)     # prog/game starts prog/main.exe from its folder
# The display (docs/design/LINDBERGH.md, "Primeval Hunt's display"). On the
# original cabinet the game goes full screen (glutFullScreen) on a 1280x1024 X
# screen and draws two 640x480 (4:3) views, each on its own monitor: the
# touch/map view at x 0-639 and the gameplay view at x 640-1279, the bottom 480
# rows. Off screen it draws a 1024x1024 shadow map in that framebuffer (copied
# to a texture) and blurs in a 2048x2048 texture; neither is shown.
# The core gives it the 1280x1024 framebuffer and sends the front end the
# 1280x480 with both views side by side (chosen 12 Nov 2026). Only
# the gameplay view, the halves swapped or the map as an inset are later
# options (these settings and the readback's crop), possibly a per-game front
# end setting as the region is.
set(GAME_WIDTH 1280)               # what the front end gets: both views, side by side
set(GAME_HEIGHT 480)
set(GAME_FB_SIZE 1280 1024)        # the original cabinet's X screen: the game's framebuffer
# the guns aim at the gameplay view (the right half: gun x 0.5 .. 1 is its 0 .. 1);
# a pointer on the left half with the trigger pulled touches the touch panel,
# whose coordinates are 640x480 (0 .. 4095 each way)
set(GAME_GUN_VIEW_X0 0.5)
set(GAME_DRIVE_BOARD touch)        # /dev/tts/0 is its touch panel
# the region the front end can choose, Export first (the game shows English in each)
set(GAME_OPTIONS "region=export|usa|japan\\n")
