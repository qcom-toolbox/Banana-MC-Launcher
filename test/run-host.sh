#!/bin/sh
# Builds test/hosttest.c on Linux and runs it in a throwaway HOME.
#     test/run-host.sh                 hashes and names only
#     test/run-host.sh 1.20.4 [skip]   + install that version and start it
#                                      (JAVA_HOME: a JDK 17; skip: no asset files)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HOME/.cache/mcl-test}
mkdir -p "$OUT"
EXTRA=
[ "$2" = skip ] && EXTRA=-DMCL_TEST_NO_OBJECTS
gcc -O1 -g -Wall -Wextra -I"$HERE/host" -I"${JNI_HOME:=${JAVA_HOME:?set JAVA_HOME to a JDK 17}}/include" -I"$JNI_HOME/include/linux" $EXTRA \
    -o "$OUT/hosttest" "$HERE/hosttest.c" "$HERE/../src/mc.c" "$HERE/../src/json.c" "$HERE/../src/hash.c" -ldl -lpthread
HOME=$OUT/home "$OUT/hosttest" "$@"
