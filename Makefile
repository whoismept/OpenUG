# OpenUG — build
#
#   make               desktop build (x86/ARM · Linux/macOS/Windows): SDL2 + OpenGL
#   make gles          OpenGL ES 2.0 build (embedded/mobile ARM)
#   make debug         desktop build + Dear ImGui debug overlay
#   make run DATA=DIR  build + run against your NFSU2 data directory
#   make clean
#
# Requires SDL2 (brew install sdl2 · apt install libsdl2-dev) and zlib.

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
# nfsu2.h is a static-function single-header parser shared by several modules;
# each module uses only part of it, so silence the per-TU unused copies.
CFLAGS  += -Wno-unused-function
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
DATA    ?= .

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  GL_LIBS := -framework OpenGL
else
  GL_LIBS := -lGL
endif

# engine modules (see docs/INIT.md): orchestrator + Renderer/Physics/AI/Audio/Resources
SRC  := src/main.c src/render.c src/physics.c src/ai.c src/audio.c src/resource.c
HDRS := src/nfsu2.h src/render.h src/physics.h src/ai.h src/audio.h src/resource.h src/debug.h

nfsu2: $(SRC) $(HDRS)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SRC) -o nfsu2 $(SDL_LIBS) $(GL_LIBS) -lz -lm

# Dev build with the Dear ImGui debug overlay + freecam controls (`make debug`).
# The engine stays C; ImGui + the wrapper compile as C++ and link together.
CXX      ?= c++
IMGUI_DIR := third_party/imgui
IMGUI_OBJ := build/imgui.o build/imgui_draw.o build/imgui_tables.o build/imgui_widgets.o \
             build/imgui_impl_sdl2.o build/imgui_impl_opengl2.o build/debugui.o

build/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p build
	$(CXX) -O2 $(SDL_CFLAGS) -I$(IMGUI_DIR) -c $< -o $@
build/%.o: $(IMGUI_DIR)/backends/%.cpp
	@mkdir -p build
	$(CXX) -O2 $(SDL_CFLAGS) -I$(IMGUI_DIR) -c $< -o $@
build/debugui.o: src/debugui.cpp src/debug.h
	@mkdir -p build
	$(CXX) -O2 $(SDL_CFLAGS) -I$(IMGUI_DIR) -c src/debugui.cpp -o $@

DBG_OBJ := $(SRC:src/%.c=build/%.dbg.o)
build/%.dbg.o: src/%.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DDEBUG_UI -c $< -o $@

debug: $(DBG_OBJ) $(IMGUI_OBJ)
	$(CXX) -O2 $(DBG_OBJ) $(IMGUI_OBJ) -o nfsu2 $(SDL_LIBS) $(GL_LIBS) -lz -lm

# OpenGL ES 2.0 (embedded/mobile). Cross-compile e.g.:
#   CC=aarch64-linux-gnu-gcc make gles
gles: $(SRC) $(HDRS)
	$(CC) $(CFLAGS) -DN2_GLES $(SDL_CFLAGS) $(SRC) -o nfsu2 $(SDL_LIBS) -lGLESv2 -lz -lm

run: nfsu2
	./nfsu2 $(DATA)

clean:
	rm -f nfsu2 *.png
	rm -rf build

.PHONY: run gles clean debug
