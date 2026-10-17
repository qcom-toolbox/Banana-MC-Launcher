#!/bin/sh
# What a downloaded version needs: its LWJGL jars, and the JDK modules
# its classes use (jdeps). Run after test/run-host.sh <version>.
V=${1:-1.20.4}
M=${OUT:-$HOME/.cache/mcl-test}/home/.minecraft
JH=$(sh "$(dirname "$0")/get-jdk.sh" | sed -n 's/^JAVA_HOME=//p')
python3 -c "
import json,sys
for l in json.load(open('$M/versions/$V/$V.json'))['libraries']:
    if 'lwjgl' in l['name']: print(l['name'])"
CP=$(find "$M/libraries" -name '*.jar' | grep -v natives | tr '\n' ':')
"$JH/bin/jdeps" --multi-release 17 --ignore-missing-deps --print-module-deps -cp "$CP" "$M/versions/$V/$V.jar" 2>&1 | tail -3
echo "-- the game's own classes:"
"$JH/bin/jdeps" --multi-release 17 --ignore-missing-deps --print-module-deps "$M/versions/$V/$V.jar" 2>&1 | tail -1
for mod in java.desktop java.management java.sql java.naming java.rmi java.compiler java.scripting jdk.jfr; do
    echo "-- $mod used by (jar: package):"
    for j in "$M/versions/$V/$V.jar" $(echo "$CP" | tr ':' ' '); do
        "$JH/bin/jdeps" --multi-release 17 --ignore-missing-deps -s "$j" 2>/dev/null | grep -q -- "-> $mod\$" && echo "   $(basename "$j")"
    done
done
