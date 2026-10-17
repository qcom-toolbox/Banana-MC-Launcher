#!/bin/sh
# Minecraft's own log (logs/latest.log, a relative path) must land in
# ~/.minecraft: the game runs in its folder, like the official launcher.
cd "$(dirname "$0")/.." || exit 1
make 2>&1 | grep -E 'warning:|error|ok -' | grep -v 'Clock skew'
export JAVA_HOME=$(sh test/get-jdk.sh | sed -n 's/^JAVA_HOME=//p')
M=${OUT:-$HOME/.cache/mcl-test}/home/.minecraft
rm -f "$M/logs/latest.log"
sh test/run-host.sh ${1:-1.20.1} skip 2>&1 | grep -E "^installed"
ls -l "$M/logs/latest.log" && echo "OK: latest.log in ~/.minecraft/logs"
[ -d logs ] && echo "FAIL: logs/ in the launcher's folder" || echo "OK: nothing in the launcher's folder"
