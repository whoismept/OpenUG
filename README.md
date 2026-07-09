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
  world-space road/terrain, and **per-mesh textures** it: each mesh's `0x134012`
  key resolves to a texture in the region's own STREAM TPK or the shared
  `LOC4DYNTEX.BIN` (works across regions; textures that don't decode fall back).
- **Car** — loads a car model (`GEOMETRY.BIN`, 36-byte vertices with normals)
  with **per-mesh textures**: each part's `0x134012` slot list is matched
  against the car's TPK (`TEXTURES.BIN`: JDLZ-decompressed + DXT1/DXT3) so the
  body, wheels and details each wear their own decoded texture via real UVs.
- **Driving** — arcade car physics with grip/drift (tyre skid marks that fade
  over time + drift smoke when you slide), ground-height follow, chase camera, a speedometer,
  and a procedural engine sound (no audio assets — synthesised, pitch
  follows speed).
- **Racing** — a pre-race menu (orbiting-car preview; pick track + circuit),
  AI opponents that follow the game's own racing line (`ROUTES*/Paths*.bin`), a
  3-2-1 countdown start, lap counting on a closed circuit, finish + placing,
  and a font-free race-position HUD.

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

Pick the car, track and circuit (defaults shown):

```sh
./nfsu2 DATA --car HUMMER --track STREAML4RH --circuit ROUTESL4RF/Paths4602.bin
```

- `--car NAME` — a folder under `CARS/` (needs a `GEOMETRY.BIN`).
- `--track NAME` — a `STREAM*.BUN` under `TRACKS/` (e.g. `STREAML4RR`).
- `--circuit PATH` — a closed-loop `Paths*.bin` under `TRACKS/`.

It opens on a **pre-race menu**: the car orbits amid the city while you pick a
**car** (Left/Right — every drivable folder under `CARS/`), a **track** (Up/Down
— `STREAM*.BUN` under `TRACKS/`) and a **circuit** (`[` / `]` — closed loops
found on that track). `Enter` starts the 3-2-1. `--car`/`--track`/`--circuit`
above just preselect the menu. (Car and track changes re-launch the engine to
load fresh; circuit is an in-place reload.)

**Controls:** menu — `←`/`→` car, `↑`/`↓` track, `[`/`]` circuit, `Enter` race;
driving — `W`/`S` throttle/brake, `A`/`D` steer, `Space` handbrake (breaks rear
grip for drifts), `Esc` quit. Cars collide (they shove each other) and you can't
drive through buildings — the car slides along wall footprints. `--shot out.png`
renders one frame to a PNG and exits.

## Layout

```
src/     the engine — a single-header asset loader (nfsu2.h) + SDL2/GL app (main.c)
tools/   Python utilities used to reverse-engineer & inspect the data formats
docs/    format documentation
```

## Contributing

Reverse-engineering notes live in `docs/FORMATS.md`; the `tools/` scripts are
handy for poking at the data. Good next steps: decoding the global road-texture
pack (roads currently draw as flat asphalt), stitching multiple track regions,
and a proper front-end menu.

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
