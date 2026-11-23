# TD6 native: build the runtime and the C generated from the user's own TD6.exe.
#   make GAME=<folder with TD6.exe>      translate + build build/td6
# The translator and most of the runtime come from the AntiStatic toolkit, pinned as a git
# submodule (third_party/antistatic, a release tag); runtime/ holds what is TD6's own: port.h,
# the Bink and WMA stand-ins, the controller code, widescreen.
# Python with Capstone (and Unicorn, for the function check): the repository's .venv if there is one
PY ?= $(firstword $(wildcard .venv/bin/python) python3)
GAME ?= $(HOME)/.local/share/td6-native/game
BUILD_CFG ?= builds/td6_1999.py
TK ?= third_party/antistatic
# Parallel compiles: each takes ~300 MB; keep it modest so the machine stays usable.
JOBS ?= 8
MAKEFLAGS += -j$(JOBS)
CC = gcc
SDL3 ?= third_party/sdl3
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libswscale libswresample libavutil 2>/dev/null)
FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libswscale libswresample libavutil 2>/dev/null)
RT_INC = -Iruntime -I$(TK)/runtime/cpu -I$(TK)/runtime/win32 -I$(TK)/runtime/d3d7
CFLAGS_COMMON = -std=gnu11 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer $(RT_INC) -Igen -I$(SDL3)/include
CFLAGS_GEN = $(CFLAGS_COMMON) -O2 -w -DCPU_FAST_MEM
CFLAGS_RT = $(CFLAGS_COMMON) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-missing-field-initializers $(FFMPEG_CFLAGS)
LDFLAGS = -rdynamic $(SDL3)/lib/libSDL3.a $(FFMPEG_LIBS) -pthread -lm -ldl -lz -lvulkan

# the runtime: the toolkit's CPU, Windows and DirectX 7 folders, then the port's own files
TK_SRC = $(TK)/runtime/cpu/fpu.c $(wildcard $(TK)/runtime/win32/*.c) $(wildcard $(TK)/runtime/d3d7/*.c)
PORT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst %.c,build/obj/tk_%.o,$(notdir $(TK_SRC))) $(patsubst runtime/%.c,build/obj/rt_%.o,$(PORT_SRC))
RT_H = $(wildcard runtime/*.h $(TK)/runtime/*/*.h)
vpath %.c $(TK)/runtime/cpu $(TK)/runtime/win32 $(TK)/runtime/d3d7
# The code files recomp.py wrote, listed in gen/files.mk once it has run (a fresh tree has none
# yet: make writes the list after translating, then reads it again).
-include gen/files.mk
GEN_SRC = $(GEN_CODE) gen/table.c gen/stubs.c
GEN_OBJ = $(patsubst gen/%.c,build/obj/gen_%.o,$(GEN_SRC))

all: build/td6

gen/funcs.h: $(wildcard $(TK)/translator/*.py) $(BUILD_CFG)
	$(PY) $(TK)/translator/recomp.py $(GAME)/TD6.exe $(BUILD_CFG) gen
gen/table.c gen/stubs.c gen/image.h: gen/funcs.h ;
gen/files.mk: gen/funcs.h
	@echo 'GEN_CODE = $(sort $(wildcard gen/code_*.c))' > $@

build/obj/gen_%.o: gen/%.c gen/funcs.h runtime/port.h $(wildcard $(TK)/runtime/cpu/*.h) | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@

build/obj/tk_%.o: %.c $(RT_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

build/obj/rt_%.o: runtime/%.c $(RT_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

build/td6: $(GEN_OBJ) $(RT_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

build/obj:
	mkdir -p $@

# Function-level check against the original code in Unicorn (tests/fncheck.c)
UNICORN ?= $(firstword $(wildcard .venv/lib/python3*/site-packages/unicorn))
build/obj/tk_main_nomain.o: $(TK)/runtime/win32/main.c $(RT_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -DRUNTIME_NO_MAIN -c $< -o $@
build/obj/gen_x87_special.o: gen/x87_special.c | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@

build/fncheck: tests/fncheck.c $(filter-out build/obj/tk_main.o,$(RT_OBJ)) build/obj/tk_main_nomain.o $(GEN_OBJ) build/obj/gen_x87_special.o
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

# Reference build: the original code in Unicorn on the same runtime (ref/ref_cpu.c)
build/obj/ref_cpu.o: ref/ref_cpu.c $(RT_H) gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -c $< -o $@

build/td6_ref: $(RT_OBJ) $(GEN_OBJ) build/obj/gen_x87_special.o build/obj/ref_cpu.o
	$(CC) -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

check: build/fncheck
	build/fncheck --game $(GAME)

clean:
	rm -rf build gen

.PHONY: all clean check
