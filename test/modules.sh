#!/bin/sh
# Runs versions on a desktop JDK cut down to Banana OS's modules (jlink of
# Temurin 17), to find the modules they need before the window opens:
#     test/modules.sh "<modules,...>" <version>...
# A version passes when it gets as far as LWJGL's natives (liblwjgl.so).
cd "$(dirname "$0")/.." || exit 1
MODS=$1; shift
FULL=$(sh test/get-jdk.sh | sed -n 's/^JAVA_HOME=//p')
IMG=$HOME/.cache/mcl-test/jdk-$(echo "$MODS" | md5sum | cut -c1-8)
[ -d "$IMG" ] || "$FULL/bin/jlink" --add-modules "$MODS" --output "$IMG" --strip-debug --no-header-files --no-man-pages
export JNI_HOME=$FULL JAVA_HOME=$IMG
L=${OUT:-$HOME/.cache/mcl-test}/home/.minecraft/logs/banana-launcher.log
for v in "$@"; do
    rm -f "$L"
    sh test/run-host.sh "$v" skip > /dev/null 2>&1
    if grep -q 'Failed to locate library: liblwjgl' "$L"; then echo "PASS $v (reached LWJGL)"
    else echo "FAIL $v:"; grep -m3 -o 'NoClassDefFoundError[^]<]*\|ClassNotFoundException[^]<]*\|IllegalStateException[^]<]*\|Exception: [^]<]*' "$L" | sort -u | head -4 | sed 's/^/    /'
    fi
done
