# TD6 native: build the runtime and the C generated from the user's own TD6.exe.
#   make GAME=<folder with TD6.exe>      translate + build build/td6
# Python with Capstone (and Unicorn, for the function check): the repository's .venv if there is one
PY ?= $(firstword $(wildcard .venv/bin/python) python3)
GAME ?= $(HOME)/.local/share/td6-native/game
BUILD_CFG ?= builds/td6_1999.py
# Parallel compiles: each takes ~300 MB; keep it modest so the machine stays usable.
JOBS ?= 8
MAKEFLAGS += -j$(JOBS)
CC = gcc
SDL3 ?= third_party/sdl3
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libswscale libswresample libavutil 2>/dev/null)
FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libswscale libswresample libavutil 2>/dev/null)
CFLAGS_COMMON = -std=gnu11 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer -Iruntime -Igen -I$(SDL3)/include
CFLAGS_GEN = $(CFLAGS_COMMON) -O2 -w -DCPU_FAST_MEM
CFLAGS_RT = $(CFLAGS_COMMON) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation $(FFMPEG_CFLAGS)
LDFLAGS = -rdynamic $(SDL3)/lib/libSDL3.a $(FFMPEG_LIBS) -pthread -lm -ldl -lz -lvulkan

RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst runtime/%.c,build/obj/rt_%.o,$(RT_SRC))
# The code files recomp.py wrote, listed in gen/files.mk once it has run (a fresh tree has none
# yet: make writes the list after translating, then reads it again).
-include gen/files.mk
GEN_SRC = $(GEN_CODE) gen/table.c gen/stubs.c
GEN_OBJ = $(patsubst gen/%.c,build/obj/gen_%.o,$(GEN_SRC))

all: build/td6

gen/funcs.h: $(filter-out tools/profile.py,$(wildcard tools/*.py)) $(BUILD_CFG)
	$(PY) tools/recomp.py $(GAME)/TD6.exe $(BUILD_CFG) gen
gen/table.c gen/stubs.c: gen/funcs.h ;
gen/files.mk: gen/funcs.h
	@echo 'GEN_CODE = $(sort $(wildcard gen/code_*.c))' > $@

build/obj/gen_%.o: gen/%.c gen/funcs.h runtime/cpu.h | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@

build/obj/rt_%.o: runtime/%.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

# Shaders: SPIR-V headers are generated with glslang and kept in the repository, so building
# needs glslang only after a shader changed.
SHADERS = $(wildcard runtime/shaders/*.vert runtime/shaders/*.frag)
SPV_H = $(addsuffix .spv.h,$(SHADERS))
GLSLANG ?= glslangValidator
ifneq ($(shell command -v $(GLSLANG) 2>/dev/null),)
runtime/shaders/%.spv.h: runtime/shaders/%
	$(GLSLANG) --target-env vulkan1.3 --vn spv_$(subst .,_,$(notdir $<)) -o $@ $<
endif
build/obj/rt_vk.o: $(SPV_H)
shaders: $(SPV_H)

build/td6: $(GEN_OBJ) $(RT_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

build/obj:
	mkdir -p $@

# Function-level check against the original code in Unicorn (tests/fncheck.c)
UNICORN ?= $(firstword $(wildcard .venv/lib/python3*/site-packages/unicorn))
build/obj/rt_main_nomain.o: runtime/main.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -DRUNTIME_NO_MAIN -c $< -o $@

build/fncheck: tests/fncheck.c $(filter-out build/obj/rt_main.o,$(RT_OBJ)) build/obj/rt_main_nomain.o $(GEN_OBJ) build/obj/gen_x87_special.o
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

# Reference build: the original code in Unicorn on the same runtime (ref/ref_cpu.c)
build/obj/ref_cpu.o: ref/ref_cpu.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -c $< -o $@

build/td6_ref: $(RT_OBJ) $(GEN_OBJ) build/obj/gen_x87_special.o build/obj/ref_cpu.o
	$(CC) -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

check: build/fncheck
	build/fncheck --game $(GAME)

clean:
	rm -rf build gen

.PHONY: all clean shaders check
