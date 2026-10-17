#!/bin/sh
# The host test's game log: the launch arguments and the first errors.
L=${OUT:-$HOME/.cache/mcl-test}/home/.minecraft/logs/banana-launcher.log
sed -n '/^game arguments/,/^----/p' "$L" | tr '\n' ' ' | cut -c1-900
echo
grep -m3 -o 'Exception[^]<]*\|Error:[^]<]*' "$L" | cut -c1-220
