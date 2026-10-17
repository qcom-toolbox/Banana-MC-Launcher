#!/bin/sh
# Which JDK classes outside java.base the game's own jar uses, per module.
V=${1:-1.20.4}
M=${OUT:-$HOME/.cache/mcl-test}/home/.minecraft
JH=$(sh "$(dirname "$0")/get-jdk.sh" | sed -n 's/^JAVA_HOME=//p')
"$JH/bin/jdeps" --multi-release 17 --ignore-missing-deps -verbose:class "$M/versions/$V/$V.jar" 2>/dev/null |
    grep -E '(java\.desktop|java\.management|java\.naming|jdk\.jfr|java\.logging)$' |
    awk '{print $NF, $(NF-1)}' | sort -u
