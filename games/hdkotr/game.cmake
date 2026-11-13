# Harley-Davidson: King of the Road: build defaults (CMakeLists.txt; title, controls and core version in games.toml)
set(GAME_GEN_COUNT 72)             # gen_NNN.c files: its program has no symbols, so more functions are cut
set(GAME_CWD /home/disk0/elf)      # game.sh starts elf/chopperM.elf from its folder
# The display (docs/design/LINDBERGH.md, "Harley-Davidson's cabinet"): one
# screen, 1280x768 at 60 Hz with the DIP switches the runtime reports (the
# default GAME_WIDTH and GAME_HEIGHT); the game also has 640x480 (VGA) assets
# for the other switch setting, not used here.
# The original cabinet's handlebar board on /dev/tts/0: Sega's drive board
# packets, status bytes until the first read, then 4-byte answers. Also the
# bike's controls on JVS: the throttle grip on analog 0 (the front end's
# accelerator), the handlebar on 1 (steering), the brake lever on 2
set(GAME_DRIVE_BOARD bike)
# its network: a stand-alone cabinet is its own master, at an address and port
# by region; the cabinet's address on 192.168.1.x for Export, USA and Japan
set(GAME_NET_HOST "9,5,1")
# TEST makes the game exit; its test menu is the same program started with -t
set(GAME_TEST_ON_EXIT ON)
# the region the front end can choose, Export first (Export and USA in
# English, Japan in Japanese)
set(GAME_OPTIONS "region=export|usa|japan\\n")
