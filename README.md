# Minecraft Launcher for Banana OS

A desktop app for [Banana OS](https://github.com/qcom-toolbox/Banana-OS) that
downloads Minecraft: Java Edition and starts it on Banana OS's OpenJDK 17 port.
Separate from the OS tree: it is built with the SDK and installed like any `.bpk`.

**Offline accounts only (for now):** type any player name (3-16 of `A-Z a-z 0-9 _`).
The game gets the UUID offline-mode servers use. This is meant for testing the
Java port; Microsoft sign-in comes later. Online servers will refuse offline players.

## What it does

- Lists versions from Mojang's manifest (a copy is kept for when you're offline);
  releases, plus snapshots if you tick the box. **1.7.2 to 1.20.4 are playable.**
  Versions that need Java 21 (1.20.5 and newer) are greyed out (Banana OS has
  Java 17), and so are 1.6.4 and older (launchwrapper, made for Java 8).
- 1.7.2 to 1.12.2 use LWJGL 2: they run on Banana OS's LWJGL 3 through
  `/usr/java/lwjgl/lwjgl2.jar` (Banana-OS `ports/lwjgl/lwjgl2`: LWJGL 2's
  `Display`, `Keyboard`, `Mouse`... and a class loader that rewrites the calls
  LWJGL 3 names otherwise). The launcher swaps Minecraft's LWJGL 2 jars for it.
  Checked: 1.8.9 on Banana OS (a world, moving, the inventory, sound).
- Downloads like the official launcher, into `~/.minecraft`: `versions/`,
  `libraries/`, `assets/`. Six downloads at once, every file checked against its
  SHA-1, and anything already there with the right size is skipped.
- Starts the JVM **inside the launcher** (`dlopen` of `libjvm.so`, as the `java` app
  does): `java` can't take arguments when another app starts it, and Java has no
  `ProcessBuilder` on Banana OS yet. So Java can only run once per launcher — open
  the launcher again to play again.
- Shows the game's log while it runs (`~/.minecraft/logs/banana-launcher.log`,
  which also lists the exact JVM options and game arguments it used).
- Remembers the name, version and memory in `~/.minecraft/banana-launcher.cfg`.

## Getting Java onto Banana OS

Java isn't in the Banana OS ISO. Each release on GitHub has a **`java.bpk`**
next to `Banana_OS.iso` (built by the `java` job in
`.github/workflows/build-iso.yml`). Download it on Banana OS and install it:

```
pkg install java.bpk        (or double-click it in Files)
```

It installs the `java` app, and puts Java's files in a folder of its own,
`/usr/java` (kept on an installed disk; `pkg remove java` deletes it):

```
/usr/java/jdk               OpenJDK 17 (ports/openjdk)
/usr/java/lib/libOSMesa.so  OpenGL 3.3 (ports/mesa)
/usr/java/lwjgl             LWJGL 3.3.3 (ports/lwjgl)
```

The launcher finds them by itself:

- **JDK:** `JAVA_HOME`, `/usr/java/jdk`, `/jdk`, `/mnt/jdk`, `/home/banana/jdk`,
  `/mnt/usb/jdk`, `/mnt/virtio/jdk`, `/mnt/sata/jdk`, `/mnt/nvme/jdk`, `~/.minecraft/jdk`.
- **LWJGL:** `LWJGL_HOME`, `~/.minecraft/natives`, the `lwjgl` folder beside the
  JDK (`/usr/java/lwjgl`), `/lwjgl`, `/mnt/<disk>/lwjgl`. Minecraft's own `lwjgl`, `lwjgl-opengl` and
  `lwjgl-glfw` jars are replaced by the ones there (they carry the checksums of
  Banana OS's native libraries), and the library path points there.
- **OpenGL:** the system Mesa runtime first — `libOSMesa.so` in `/lib`, `/usr/lib`,
  `/apps/mesa` or `/mnt/<disk>/lib` (the mesa package of the
  **Banana_OS-mesa.iso** prereleases) — and only then `libOSMesa.so` beside the
  JDK (`/usr/java/lib`, from java.bpk). The runtime one carries the GPU
  drivers: **crocus and iris** for Intel (used when `gpu3d on` has opened
  `/dev/dri/card0`) and **VirGL** in a VM; it falls back to softpipe itself.
  java.bpk's copy has no drivers and draws on the processor. The one the game
  got is in the log's header line, and `BGL3_LIBRARY` overrides the choice.

It also starts the game with `-Dos.name=Linux` (LWJGL doesn't know "Banana OS")
and `-Dorg.lwjgl.system.allocator=system` (no jemalloc build).

## What's still missing on Banana OS

1. **LWJGL's other modules:** Banana OS builds core, OpenGL and GLFW. Minecraft
   also loads **STB** (`liblwjgl_stb.so`: it decodes every texture with it),
   **OpenAL** (`libopenal.so`: sound) and tinyfd. Without STB the game stops
   while loading its resources.
2. **`libmanagement`** (java.management's native code) is built if it compiles;
   the CI log says if it was left out.
3. **The 64-bit Banana OS** (the Java port is 64-bit only). The `.bpk` contains
   a 32-bit build too, which says so.
4. Speed: Java on Banana OS is the Zero interpreter (no JIT). With an Intel GPU
   (`gpu3d on`) and the mesa package installed, the game draws with Mesa's
   crocus/iris; otherwise it is softpipe on the processor, so expect very low
   frame rates.

## Build

On Linux (or WSL) with the Banana OS SDK's tools (`gcc gcc-multilib binutils make python3`)
and the Banana OS source tree next to this folder:

```bash
make                                  # -> mclauncher.bpk
make BANANA_OS=/path/to/Banana-OS     # if it's somewhere else
```

Install it on Banana OS: `pkg install mclauncher.bpk` (or double-click it in Files).

## Testing on Linux

`test/` runs the launcher's logic (`src/mc.c`) on a normal Linux machine with a
desktop JDK 17: hashes, the offline UUID, the version list, a whole install,
and the game started in-process.

```bash
sh test/get-jdk.sh                                # Temurin 17 (checksum-checked), prints JAVA_HOME
JAVA_HOME=... sh test/run-host.sh                 # hashes and names only
JAVA_HOME=... sh test/run-host.sh 1.20.4 skip     # + install and start (skip: no asset files)
sh test/show-log.sh                               # the arguments used and the first error
```

On Linux the game gets just as far as on Banana OS: it stops at
`Failed to locate library: liblwjgl.so`, since the natives are skipped.

## Layout

```
src/main.c   the window: version list, name, memory, Play / Download, progress, log
src/mc.c     versions, downloads, rules, arguments, starting the JVM
src/json.c   a small JSON reader
src/hash.c   MD5 (offline UUIDs) and SHA-1 (download checks)
test/        the Linux test harness
```
