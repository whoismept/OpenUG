# OpenUG — build
#
#   make               desktop build (x86/ARM · Linux/macOS/Windows): SDL2 + OpenGL
#   make gles          OpenGL ES 2.0 build (embedded/mobile ARM)
#   make run DATA=DIR  build + run against your NFSU2 data directory
#   make clean
#
# Requires SDL2 (brew install sdl2 · apt install libsdl2-dev) and zlib.

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
DATA    ?= .

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  GL_LIBS := -framework OpenGL
else
  GL_LIBS := -lGL
endif

nfsu2: src/main.c src/nfsu2.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) src/main.c -o nfsu2 $(SDL_LIBS) $(GL_LIBS) -lz -lm

# OpenGL ES 2.0 (embedded/mobile). Cross-compile e.g.:
#   CC=aarch64-linux-gnu-gcc make gles
gles: src/main.c src/nfsu2.h
	$(CC) $(CFLAGS) -DN2_GLES $(SDL_CFLAGS) src/main.c -o nfsu2 $(SDL_LIBS) -lGLESv2 -lz -lm

run: nfsu2
	./nfsu2 $(DATA)

clean:
	rm -f nfsu2 *.png

.PHONY: run gles clean
