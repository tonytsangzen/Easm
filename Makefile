# easm - embed WebAssembly runtime (aarch64-first)
CC      = clang
CFLAGS  = -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -fno-omit-frame-pointer
LDFLAGS = -ldl

SRC     = src/decode.c src/validate.c src/runtime.c src/interp.c src/arith.c \
          src/simd_exec.c src/util.c src/jit_a64.c src/jit_a64_rt.c src/wasi.c \
          src/wasi_posix.c \
          src/wasi_win.c src/wasi_ewok.c src/wast.c src/main.c
OBJ     = $(SRC:src/%.c=build/%.o)

all: build/easm

build:
	mkdir -p build

build/%.o: src/%.c src/easm.h src/opcodes.h src/a64_emit.h src/jit_a64.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/easm: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -rf build

test-smoke: build/easm
	build/easm run /tmp/smoke.wasm add 1 2

.PHONY: all clean test-smoke
