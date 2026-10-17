# Minecraft launcher for Banana OS - builds mclauncher.bpk (i686 + x86_64).
#
#     make                                  # SDK in ../Banana-OS/sdk
#     make BANANA_OS=/path/to/Banana-OS
#
# Needs the Banana OS source tree: its SDK, and the JNI headers of its
# OpenJDK port (ports/openjdk/include).

BANANA_OS   ?= ../Banana-OS

APP         = mclauncher
TITLE       = Minecraft Launcher
VERSION     = 0.1
TYPE        = gui
DESCRIPTION = Download and play Minecraft (Java Edition) - offline accounts, for testing
AUTHOR      = Banana OS community
CATEGORY    = Games
SRCS        = src/main.c src/mc.c src/json.c src/hash.c
CFLAGS      = -I$(BANANA_OS)/ports/openjdk/include

LIBS_i686   = $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(shell gcc -m64 -print-libgcc-file-name)

include $(BANANA_OS)/sdk/banana.mk
