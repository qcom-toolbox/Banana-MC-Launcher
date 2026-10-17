#!/bin/sh
# A version with everything, assets too, into the Linux test's game folder
# (for test/banana's disk image)
cd "$(dirname "$0")/.." || exit 1
export JAVA_HOME=$(sh test/get-jdk.sh | sed -n 's/^JAVA_HOME=//p')
sh test/run-host.sh "${1:-1.20.1}" 2>&1 | grep -E "^installed|^install:"
du -sh ${OUT:-$HOME/.cache/mcl-test}/home/.minecraft/assets
