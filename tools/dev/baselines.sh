#!/bin/bash
# (development script; run from the repository root)
# usage: baselines.sh OUTDIR [BINARY]: the port's checks recorded for a build (default build/td6),
# to compare another build against (tools/dev/compare_baselines.sh). Each scene is a --test run
# from a fresh or copied user folder; its per-frame memory hashes (--hash: frame, API calls,
# hash) go to OUTDIR/<scene>.hash, its log to OUTDIR/<scene>.log; the visual scenes also dump
# GPU frames (off-screen, 1280x800) into OUTDIR/<scene>/.
#   menus   fresh player, legal screens to the race menu and back round the front end (800)
#   race    quick race with throttle and steering, into an opponent and a wall (1800)
#   bet     a player who owns a car: single race, the bet raised by a scripted pad, OK (640;
#           TD6_ENHANCED: the port's controller code on)
#   wide    the race with widescreen 16:9 and resolution native (TD6_ENHANCED; 1700)
#   shots   frame dumps: menus, the options pages, at 1280x800 widescreen native
#   raceshots  frame dumps of a quick race (autopilot) from frame 1200, the same size: its
#           conditions (environment script, TD6.log) are the game's random pick, so compare a
#           build with another only where both logs name the same one
# The game folder is GAME (default ~/.local/share/td6-native/game); SEED a user folder whose
# PlayerData the bet and shots scenes copy (default ~/.local/share/td6-native).
set -u
out=${1:?usage: baselines.sh OUTDIR [BINARY]}
bin=${2:-build/td6}
GAME=${GAME:-$HOME/.local/share/td6-native/game}
SEED=${SEED:-$HOME/.local/share/td6-native}
mkdir -p "$out"
keys() { bash work/keys.sh "$1" 2>/dev/null || {   # (the legal screens' prefix, as walk.sh)
    echo -n "60:ESCAPE*3,120:ESCAPE*3,180:ESCAPE*3,240:ESCAPE*3"
    echo "$1" | tr ',' '\n' | awk -F: 'NF==2{printf ",%d:%s", $1+227, $2}'; }; }
user() { rm -rf "$out/$1.user"; mkdir -p "$out/$1.user"; [ "${2:-}" = seed ] && cp -r "$SEED/PlayerData" "$out/$1.user/"; }
run() {   # run NAME FRAMES KEYS [env and args...]
    local name=$1 frames=$2 k; k=$(keys "$3"); shift 3
    env TD6_KEYS="$k" "$@" --test --game "$GAME" --user "$out/$name.user" --frames "$frames" \
        --hash "$out/$name.hash" --logfile "$out/$name.log" >/dev/null 2>&1
    echo "$name: exit $?, $(wc -l < "$out/$name.hash") frames"
}

user menus
run menus 800 "40:RETURN,80:RETURN,220:DOWN,260:RETURN,360:DOWN,400:DOWN,440:DOWN,480:DOWN,520:DOWN,560:DOWN,600:DOWN,640:ESCAPE,700:DOWN,740:RETURN" \
    "$bin"
user race
run race 1800 "40:RETURN,80:RETURN,760:RETURN,900:RETURN,1150:UP*1600,1350:LEFT*40,1550:RIGHT*70" "$bin"
user bet seed
run bet 640 "40:RETURN,120:DOWN,160:RETURN,240:RETURN,340:RETURN" \
    TD6_ENHANCED=1 TD6_PAD="627:RIGHT*5,667:RIGHT*5,707:RIGHT*5,800:A*5" "$bin" --log input
user wide
run wide 1700 "40:RETURN,80:RETURN,760:RETURN,900:RETURN,1150:UP*1600,1350:LEFT*40,1550:RIGHT*70" \
    TD6_ENHANCED=1 "$bin" --option widescreen=16:9 --option resolution=native

user shots seed
rm -rf "$out/shots"; mkdir -p "$out/shots"
k=$(keys "40:RETURN,120:DOWN,150:DOWN,180:RETURN,260:RETURN,330:ESCAPE,400:UP,430:UP,460:RETURN,540:RETURN,640:UP*600")
TD6_GPU_OFFSCREEN=1 TD6_GPU_OFFSCREEN_SIZE=1280x800 TD6_DUMP_FRAMES="$out/shots" TD6_DUMP_EVERY=50 \
    TD6_KEYS="$k" TD6_ENHANCED=1 timeout 900 "$bin" --user "$out/shots.user" --test --frames 1100 \
    --logfile "$out/shots.log" --option widescreen=on --option resolution=native --game "$GAME" >/dev/null 2>&1
echo "shots: exit $?, $(ls "$out/shots" | wc -l) frames"

user raceshots
rm -rf "$out/raceshots"; mkdir -p "$out/raceshots"
k=$(keys "40:RETURN,80:RETURN,760:RETURN,900:RETURN")
TD6_AUTOPILOT=1 TD6_GPU_OFFSCREEN=1 TD6_GPU_OFFSCREEN_SIZE=1280x800 TD6_DUMP_FRAMES="$out/raceshots" TD6_DUMP_EVERY=50 \
    TD6_KEYS="$k" TD6_ENHANCED=1 timeout 900 "$bin" --user "$out/raceshots.user" --test --frames 1400 \
    --logfile "$out/raceshots.log" --option widescreen=on --option resolution=native --game "$GAME" >/dev/null 2>&1
st=$?
find "$out/raceshots" -name 'frame_0*.ppm' ! -name 'frame_01[2-4]*' -delete
echo "raceshots: exit $st, $(ls "$out/raceshots" | wc -l) frames, $(grep -o 'CM[0-9]*\.env' "$out/raceshots.user/TD6.log")"
