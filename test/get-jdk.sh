#!/bin/sh
# A desktop JDK 17 for test/run-host.sh: the Temurin build ports/openjdk/build.sh uses.
set -e
D=${1:-$HOME/.cache/mcl-test/jdk}
mkdir -p "$D" && cd "$D"
T=temurin.tar.gz
[ -f $T ] || curl -fsSL -o $T "https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.20.1%2B1/OpenJDK17U-jdk_x64_linux_hotspot_17.0.20.1_1.tar.gz"
echo "3808d1d15e3ec6bd5b84057fb5d84c33d8a1536a258146bcea2e603fc726e08e  $T" | sha256sum -c -
[ -d jdk-17.0.20.1+1 ] || tar xzf $T
echo "JAVA_HOME=$D/jdk-17.0.20.1+1"
