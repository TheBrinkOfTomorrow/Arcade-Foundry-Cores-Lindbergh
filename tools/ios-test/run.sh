#!/bin/bash
# Lindbergh Test (main.m) on an iPhone or iPad over USB or the network.
#
#   run.sh build TEAM BUILD_DIR...
#       the cores of device builds (cmake -G Ninja -DCMAKE_SYSTEM_NAME=iOS
#       -DCMAKE_OSX_SYSROOT=iphoneos ...) into the app, built, signed for the
#       team (automatic signing registers the bundle ID) into build-ios-test/
#   run.sh install DEVICE
#   run.sh game DEVICE ROMSET GAME_FOLDER
#       the game folder (the one holding disk0) to Documents/ROMSET; files
#       already there unchanged are skipped
#   run.sh run DEVICE ROMSET SCRIPT LOCAL_OUT [PACE]
#       the script to Documents, the run (CORE_TEST_PACE=1 with PACE), and
#       Documents/out back to LOCAL_OUT: log.txt and the shots (.ppm). Shot
#       names in the script are relative to Documents: "2400 shot out/s_2400".
# DEVICE is a name, UDID or identifier from `xcrun devicectl list devices`.
set -e
cd "$(dirname "$0")/../.."
APP=com.thebrinkoftomorrow.lindberghtest
B=build-ios-test
dc() { xcrun devicectl "$@"; }
docs() { dc device copy "$1" --device "$2" --domain-type appDataContainer --domain-identifier $APP "${@:3}"; }

case "$1" in
build)
    team=$2; shift 2
    rm -rf $B/cores; mkdir -p $B/cores
    for d in "$@"; do cp -R "$d"/afcore_*.framework $B/cores/; done
    xcodegen --quiet --spec tools/ios-test/project.yml
    xcodebuild -quiet -project tools/ios-test/LindberghTest.xcodeproj -scheme LindberghTest -configuration Release \
        -destination generic/platform=iOS -derivedDataPath $B/dd -allowProvisioningUpdates \
        DEVELOPMENT_TEAM="$team" build
    echo "$B/dd/Build/Products/Release-iphoneos/LindberghTest.app"
    ;;
install)
    dc device install app --device "$2" $B/dd/Build/Products/Release-iphoneos/LindberghTest.app
    ;;
game)
    docs to "$2" --source "$4" --destination "Documents/$3"
    ;;
run)
    dev=$2 set=$3 script=$4 out=$5
    docs to "$dev" --source "$script" --destination "Documents/$(basename "$script")"
    # the app empties Documents/out and ends with out/done holding this run's token
    tmp=$(mktemp -d) run=$(date +%s)
    env="{\"LB_TEST_RUN\":\"$run\"${6:+,\"CORE_TEST_PACE\":\"1\"}}"
    dc device process launch --device "$dev" --terminate-existing --environment-variables "$env" \
        $APP "$set" "$(basename "$script")" out >/dev/null
    until docs from "$dev" --source Documents/out/done --destination "$tmp/done" >/dev/null 2>&1 &&
        [ "$(cut -d' ' -f1 "$tmp/done")" = "$run" ]; do sleep 20; done
    mkdir -p "$out"
    docs from "$dev" --source Documents/out --destination "$out" >/dev/null
    rm -rf "$tmp"
    tail -1 "$out/log.txt"
    ;;
*)
    sed -n 2,17p "$0"; exit 1 ;;
esac
