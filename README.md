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

> ⚠️ OpenUG contains **no game assets** and no EA code. You need a legally
> acquired copy of NFS: Underground 2 and point the engine at its data
> directory. Not affiliated with, authorized, or endorsed by Electronic Arts —
> see [Legal Notice & Disclaimer](#legal-notice--disclaimer).

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
make debug           # desktop build + Dear ImGui debug panel (dev only)
```

`make debug` adds a live tuning overlay (Dear ImGui, vendored under
`third_party/`): freecam, wheel placement, lighting, per-part visibility, paint,
and readouts. It's dev-only — plain `make` ships without it.

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
grip for drifts), `F` freecam (WASD move · hold right-mouse or arrows to look ·
`E`/`Q` up/down · `Shift` faster), `Esc` quit. Cars collide (they shove each
other) and you can't
drive through buildings — the car slides along wall footprints. `--shot out.png`
renders one frame to a PNG and exits.

## Layout

```
src/
  main.c      orchestrator: setup, game loop, input, race flow, HUD
  nfsu2.h     single-header asset parser (chunk formats — the ground truth)
  render.*    Renderer: GL objects, shaders, matrices, bitmap font, screenshot
  physics.*   car kinematics, wall + car-to-car collision
  ai.*        racing-line opponents, circuit loading
  audio.*     procedural engine/road/skid synth (no audio assets)
  resource.*  file mapping + track/car/circuit discovery
  debug.*     optional Dear ImGui dev overlay (`make debug`)
tools/   Python utilities used to reverse-engineer & inspect the data formats
docs/    format documentation + engine architecture brief (INIT.md)
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

Bundled dependency (dev builds only): **[Dear ImGui](https://github.com/ocornut/imgui)**
by Omar Cornut — MIT-licensed, vendored under `third_party/imgui/`.

## Legal Notice & Disclaimer

OpenUG is an open-source, non-profit game engine recreation project built from
scratch using OpenGL. It does not contain any copyrighted material, assets, or
original code from Electronic Arts (EA). To run this engine, users must possess
a legally acquired copy of Need for Speed Underground 2. "Need for Speed" and
"Underground" are registered trademarks of Electronic Arts. This project is not
affiliated with, authorized, or endorsed by Electronic Arts.

The engine reads the formats of an existing game you already own, in the spirit
of interoperability projects like OpenMW and OpenRW — it ships **no** game data
(models, textures, audio, maps, or `speed2.exe`); you supply your own. All such
files are excluded from the repository (see [`.gitignore`](.gitignore)).

### Licensing

- OpenUG engine code is **MIT-licensed** — see [`LICENSE`](LICENSE).
- Bundled dependency **Dear ImGui** (dev builds only) is MIT-licensed — see
  [`third_party/imgui/LICENSE.txt`](third_party/imgui/LICENSE.txt).
- The reverse-engineering references credited above are independent third-party
  projects; no code from them is copied here.
