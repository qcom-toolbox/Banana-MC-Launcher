# mcplay - the launcher's logic (src/mc.c) as a console app, to test
# Minecraft on Banana OS from a shell (QEMU's serial console):
#     mcplay <version> [--dir <game folder>] [--name <player>] [--ram <MB>]
BANANA_OS   ?= ../../../Banana-OS

APP         = mcplay
TITLE       = mcplay
VERSION     = 0.1
TYPE        = console
DESCRIPTION = Minecraft launcher test: install and start a version from the shell
SRCS        = mcplay.c ../../src/mc.c ../../src/json.c ../../src/hash.c
CFLAGS      = -I$(BANANA_OS)/ports/openjdk/include -I../../src

LIBS_i686   = $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(shell gcc -m64 -print-libgcc-file-name)

include $(BANANA_OS)/sdk/banana.mk
