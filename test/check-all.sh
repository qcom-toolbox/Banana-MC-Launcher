#!/bin/sh
# Builds the .bpk and runs the Linux test with 1.20.4 (no asset files).
cd "$(dirname "$0")/.." || exit 1
make 2>&1 | grep -E 'warning:|error|ok -' | grep -v 'Clock skew'
export JAVA_HOME=$(sh test/get-jdk.sh | sed -n 's/^JAVA_HOME=//p')
sh test/run-host.sh "${1:-1.20.4}" skip
sh test/show-log.sh | tail -3
