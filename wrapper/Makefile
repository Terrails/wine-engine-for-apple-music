# SPDX-License-Identifier: LGPL-2.1-or-later
# Wrapper programs (Windows executables built with winegcc):
#   build/amd.exe         the service (src/)
#   build/libampulse.so   Linux library preloaded by bin/amd: track name for the mixer (pulse/)
#   build/lab.exe         experiments (../dev/lab/lab.c + src/ minus main.c), `make lab` only
# `make` builds amd.exe and libampulse.so; only changed files are recompiled.
# `make lab` also generates build/iids_gen.h from the app's own metadata (tools/geniids.py) when it
# finds it: WINMD_DIR, else the installed app in AM_PREFIX, else $AM_CACHE_DIR/msix/<version>.
# Without it lab.exe is built with probe_iids() disabled.
CC      = winegcc
CFLAGS  = -b x86_64-windows -O1 -g -Wall -MMD -MP
LDFLAGS = -b x86_64-windows
LIBS    = -lole32 -lcombase -luuid -lpsapi -loleaut32 -lgdi32 -luser32 -lws2_32 -lbcrypt -lshell32

CORE := $(filter-out src/main.c,$(wildcard src/*.c))
CORE_OBJ := $(CORE:src/%.c=build/%.o)

all: build/amd.exe build/libampulse.so

lab: build/lab.exe

build/libampulse.so: pulse/ampulse.c | build
	cc -shared -fPIC -O2 -Wall -fvisibility=hidden $< -o $@ -ldl -lpthread

build/amd.exe: $(CORE_OBJ) build/main.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

build/lab.exe: $(CORE_OBJ) build/lab.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# build/iids.stamp changes only when build/iids_gen.h appears, changes or goes away
build/iids.stamp: FORCE | build
	@sh ../dev/lab/iids.sh build

build/lab.o: ../dev/lab/lab.c build/iids.stamp | build
	$(CC) $(CFLAGS) -Ibuild -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

FORCE:

-include $(wildcard build/*.d)
.PHONY: all lab clean FORCE
