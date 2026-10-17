#!/bin/sh
# Runs the host test with a stand-in for ports/lwjgl's output (LWJGL_HOME):
# Minecraft's own LWJGL jars copied in, an empty liblwjgl.so. Checks the
# launcher swaps the jars and points the library path there.
cd "$(dirname "$0")/.." || exit 1
M=$HOME/.cache/mcl-test/home/.minecraft
F=$HOME/.cache/mcl-test/fake-lwjgl
mkdir -p "$F"
for a in lwjgl lwjgl-opengl lwjgl-glfw; do cp "$M/libraries/org/lwjgl/$a/3.3.2/$a-3.3.2.jar" "$F/$a.jar"; done
echo "not a real library" > "$F/liblwjgl.so"
export JAVA_HOME=$(sh test/get-jdk.sh | sed -n 's/^JAVA_HOME=//p')
LWJGL_HOME=$F sh test/run-host.sh 1.20.4 skip | tail -2
L=$M/logs/banana-launcher.log
grep -o -- '-Dos.name=[^ ]*\|-Dorg.lwjgl[^ ]*\|-Djava.library.path=[^ ]*' "$L" | sort -u
grep -o 'java.class.path=.*' "$L" | tr ':' '\n' | grep -i lwjgl
grep -m2 -o 'UnsatisfiedLinkError[^]<]*' "$L"
