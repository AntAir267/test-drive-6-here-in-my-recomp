#!/bin/bash
# (development script; run from the repository root)
# usage: shot.sh OUTDIR FRAMES EVERY KEYS [extra args]: an off-screen --test run on the GPU
# (SIZE=1280x960 by default) with the presented frames dumped into OUTDIR
out=$1; frames=$2; every=$3; keys=$4; shift 4
rm -rf "$out" "$out.user"; mkdir -p "$out" "$out.user"
TD6_GPU_OFFSCREEN=1 TD6_GPU_OFFSCREEN_SIZE=${SIZE:-1280x960} TD6_DUMP_FRAMES=$out TD6_DUMP_EVERY=$every TD6_KEYS="$keys" \
  timeout 900 build/td6 --user "$out.user" --test --frames $frames --logfile "$out.log" "$@" >/dev/null 2>&1
echo "exit $?"; ls "$out" | wc -l
