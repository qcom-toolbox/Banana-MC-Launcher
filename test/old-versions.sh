#!/bin/sh
# Old versions are refused with a clear reason (1.0: launchwrapper, 1.12.2:
# LWJGL 2), and 1.20.4 still goes through install and launch.
cd "$(dirname "$0")/.." || exit 1
make 2>&1 | grep -E 'warning:|error|ok -' | grep -v 'Clock skew'
export JAVA_HOME=$(sh test/get-jdk.sh | sed -n 's/^JAVA_HOME=//p')
for v in 1.0 1.12.2 1.20.4; do
    echo "== $v"
    sh test/run-host.sh $v skip 2>&1 | grep -E "^install:|^installed|^game"
done
