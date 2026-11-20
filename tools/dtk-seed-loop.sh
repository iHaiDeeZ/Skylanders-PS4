#!/bin/bash
# Runs dtk dol split; when it fails on "Unpaired epilogue <sec>:<addr>", drops the nearest
# unsized seed at or before <addr> from symbols.txt (logging it) and tries again.
cd ~/ssa-build/dtk || exit 1
DTK=~/tools/dtk-1.8.4/dtk
: > config/dropped-seeds.txt
for i in $(seq 1 400); do
    out=$("$DTK" --no-color dol split config/config.yml build 2>&1)
    err=$(echo "$out" | grep -oE "Unpaired epilogue [0-9]+:0x[0-9A-F]+" | head -1)
    if ! echo "$out" | grep -q "Failed\|panicked"; then
        echo "dtk ok after $((i-1)) drops"
        echo "$out" | grep -E "found|wrote"
        exit 0
    fi
    addr=$(echo "$err" | grep -oE "0x[0-9A-F]+$")
    if [ -z "$addr" ]; then
        echo "unhandled error:"; echo "$out" | tail -5; exit 1
    fi
    seed=$(grep "type:function$" config/symbols.txt | grep -oE "0x[0-9A-F]{8}" | \
           awk -v a="$addr" 'BEGIN{t=strtonum(a)} {v=strtonum($1); if (v<=t && v>best) {best=v; s=$1}} END{print s}')
    if [ -z "$seed" ]; then
        echo "no seed before $addr; error: $err"; exit 1
    fi
    echo "$seed ($err)" >> config/dropped-seeds.txt
    grep -v ":$seed; // type:function$" config/symbols.txt > config/symbols.tmp && mv config/symbols.tmp config/symbols.txt
done
echo "gave up after 400 drops"; exit 1
