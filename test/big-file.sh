#!/bin/sh
# The game's jar of 1.20.4 (23 MB) again: over Banana OS's 16 MiB per HTTP
# answer, so it must come in Range pieces (the shim keeps that limit).
cd "$(dirname "$0")/.." || exit 1
make 2>&1 | grep -E 'warning:|error|ok -' | grep -v 'Clock skew'
export JAVA_HOME=$(sh test/get-jdk.sh | sed -n 's/^JAVA_HOME=//p')
J=${OUT:-$HOME/.cache/mcl-test}/home/.minecraft/versions/1.20.4/1.20.4.jar
rm -f "$J"
sh test/run-host.sh 1.20.4 skip 2>&1 | grep -E "^ok|^FAIL|^install|^installed"
ls -l "$J" && sha1sum "$J"
