#!/bin/bash
# (development script; run from the repository root)
# usage: walk.sh KEYS FRAMES EVERY FROM [extra args]: a headless --test run from a fresh user
# folder, the four legal screens skipped by Escape, then KEYS ("frame:KEY*hold,...") given in
# presented frames - the scripted-key clock also counts the legal screens' 227 waiting reads
# (main.c input_wait), which this adds. Frames dumped every EVERY into work/walk, a contact
# sheet of those from FROM on (30 at most) in work/walk.png. PAD=... passes TD6_PAD; SEED=DIR starts
# from a copy of DIR/PlayerData (a player who owns cars).
# Known points (fresh player): main menu from ~140, race menu after "220:DOWN,260:RETURN".
keys=$1; frames=$2; every=$3; from=$4; shift 4
K=$(echo "$keys" | tr ',' '\n' | awk -F: 'NF==2{printf ",%d:%s", $1+227, $2}')
rm -rf work/walk work/walk.user; mkdir -p work/walk work/walk.user
[ -n "$SEED" ] && cp -r "$SEED/PlayerData" work/walk.user/
TD6_DUMP_FRAMES=work/walk TD6_DUMP_EVERY=$every TD6_PAD="$PAD" \
  TD6_KEYS="60:ESCAPE*3,120:ESCAPE*3,180:ESCAPE*3,240:ESCAPE*3$K" \
  timeout 900 build/td6 --test --user work/walk.user --frames $frames --log input --logfile work/walk.log "$@" >/dev/null 2>&1
echo "exit $?"
w=$(grep -c 'waits on its devices' work/walk.log); [ "$w" != 227 ] && echo "note: $w waiting reads (movies or busy-waits after the legal screens shift later keys)"
files=$(ls work/walk/frame_*.ppm | awk -F'[_.]' -v f=$from '$(NF-1)+0>=f' | head -30)
python3 tools/dev/sheet.py work/walk.png 6 $files 2>/dev/null || .venv/bin/python tools/dev/sheet.py work/walk.png 6 $files
