#!/bin/sh
# Run a game the way the cabinet does: pressing Test makes the game exit, the
# test menu takes over, and choosing EXIT there boots the game again.
# Closing the window (status 64) or any error ends the loop.
#   tools/cabinet.sh build/lindbergh-run disk0/elf/hod4M.elf build/lindbergh-run-test disk0/elf/hod4testM.elf
set -u
game_run=$1 game_elf=$2 test_run=$3 test_elf=$4
while :; do
    "$game_run" "$game_elf"; rc=$?
    [ $rc -eq 0 ] || break
    "$test_run" "$test_elf"; rc=$?
    [ $rc -eq 0 ] || break
done
[ $rc -eq 64 ] && rc=0
exit $rc
