# File lists

`<set>.h`: every file of a known-good copy of the game (path, size, CRC-32;
nothing of the files themselves), which the core's `check_roms` checks a
game folder against. Made on the Mac from the DAT-checked copies:

    python3 common/tools/filelist/make_filelist.py ~/Games/ArcadeFoundry/Lindbergh/hotd4a.tar -o filelists/hotd4a.h

A set without a list builds without check_roms (the app says "Downloaded").
