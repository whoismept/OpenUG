# OpenUG

An open, from-scratch reimplementation of the **Need for Speed: Underground**
engine. It reads the *original* game's data files directly — no Wine, no box64,
no x86 emulation — parses the geometry, textures and racing lines, and runs a
native racing scene: a textured car you drive around a real circuit against AI
opponents, with laps and standings.

Current focus is **Underground 2**; the format work and engine are meant to
generalise to the rest of the Underground series over time.

Portable across **x86 and ARM** (Linux, macOS, Windows; desktop OpenGL and
OpenGL ES). In the spirit of OpenMW / OpenRW / OpenRCT2: **the engine is open
source; you bring your own copy of the game.**

> ⚠️ OpenUG contains **no game assets**. You need an original installation of
> NFS: Underground 2 and point the engine at its data directory.

## Status

Early but real — the full asset→render→drive pipeline works end to end:

- **Track** — parses `STREAM*.BUN` scenery (chunk `0x134xxx`), assembles the
  world-space road/terrain, textures it (DXT1), and culls the skybox.
- **Car** — loads a car model (`GEOMETRY.BIN`, 36-byte vertices with normals)
  and its body texture (`TEXTURES.BIN`: JDLZ-decompressed + DXT1/DXT3).
- **Driving** — arcade car physics, ground-height follow, chase camera.
- **Racing** — AI opponents follow the game's own racing line
  (`ROUTES*/Paths*.bin`), lap counting on a closed circuit, and a font-free
  race-position HUD.

See [`docs/FORMATS.md`](docs/FORMATS.md) for the reverse-engineered file formats.

## Build

Needs **SDL2** and **zlib**.

```sh
# macOS:  brew install sdl2
# Debian/Ubuntu:  sudo apt install libsdl2-dev zlib1g-dev

make                 # desktop build -> ./nfsu2
make gles            # OpenGL ES 2.0 build (embedded/mobile ARM)
```

Cross-compiling for another ARM target is just the compiler swap, e.g.
`CC=aarch64-linux-gnu-gcc make gles`.

## Run

Point it at your NFS: Underground 2 data directory (the folder containing
`TRACKS/`, `CARS/`, …):

```sh
./nfsu2 /path/to/nfsu2/data
# or:
make run DATA=/path/to/nfsu2/data
```

**Controls:** `W`/`S` throttle/brake, `A`/`D` steer, `Esc` quit.
`--shot out.png` renders one frame to a PNG and exits (headless-friendly).

## Layout

```
src/     the engine — a single-header asset loader (nfsu2.h) + SDL2/GL app (main.c)
tools/   Python utilities used to reverse-engineer & inspect the data formats
docs/    format documentation
```

## Contributing

Reverse-engineering notes live in `docs/FORMATS.md`; the `tools/` scripts are
handy for poking at the data. Good next steps: per-mesh car material binding,
more tracks/cars, collision, a race start/finish flow, audio.

## Credits

Format reverse-engineering builds on prior community work, used as references
(all code here is independent):

- **[yugecin/nfsu2-re](https://github.com/yugecin/nfsu2-re)** — a superb, detailed
  NFSU2 reverse-engineering project; the chunk-container format was confirmed
  against its [documentation](https://yugecin.github.io/nfsu2-re/docs.html).
  Big thanks for the great work.
- **[Nikki](https://github.com/MaxHwoy/Nikki)** — TPK / texture header reference.
- **[OpenNFSTools](https://github.com/MWisBest/OpenNFSTools)** — JDLZ algorithm reference.

See [`docs/FORMATS.md`](docs/FORMATS.md) for details.

## Legal

Unofficial and not affiliated with Electronic Arts. Ships engine code only, no
assets. "Need for Speed: Underground 2" © Electronic Arts Inc. Engine code is
MIT-licensed — see [`LICENSE`](LICENSE).
