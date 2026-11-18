#!/bin/bash
# (development script; run from the repository root)
# usage: compare_baselines.sh A B [scene...]: the first frame whose memory hash (or API call
# count) differs between two baselines.sh recordings, per scene. Lines are "frame calls hash
# [draw hash]" (the toolkit adds the draw hash); the columns both have are compared.
a=$1 b=$2; shift 2
for s in ${@:-menus race bet wide}; do
    [ -f "$a/$s.hash" ] && [ -f "$b/$s.hash" ] || { echo "$s: missing"; continue; }
    r=$(paste -d'|' "$a/$s.hash" "$b/$s.hash" | awk -F'|' '{
        n = split($1, x, " "); m = split($2, y, " "); k = n < m ? n : m
        for (i = 2; i <= k; i++) if (x[i] != y[i]) { print "first difference at frame " x[1] " (calls " x[2] " / " y[2] ")"; exit } }')
    echo "$s: ${r:-same on all $(wc -l < "$a/$s.hash") / $(wc -l < "$b/$s.hash") frames}"
done
