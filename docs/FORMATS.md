# NFS: Underground 2 — file format notes

Reverse-engineered from the retail PC data files. Enough to load geometry,
textures and racing lines. All integers are little-endian.

> These are independent, clean-room notes on the *layout* of a game's data
> files, produced for interoperability (reading data you already own), in the
> spirit of OpenMW/OpenRW. They contain no Electronic Arts code and no game
> assets. "Need for Speed" and "Underground" are trademarks of Electronic Arts;
> this project is unaffiliated with and unendorsed by EA. See the README's
> Legal Notice & Disclaimer.

## Chunk container (`.BIN` / `.BUN`)

Every file is a flat stream of chunks:

```
u32 magic
u32 size          // payload size, in bytes, right after this header
u8  payload[size]
```

- `magic == 0` → padding/alignment, skip.
- top nibble of `magic` == `0x8` (e.g. `0x80134000`, `0x8003b601`) → the payload
  is itself a sequence of nested chunks; recurse into `[payload, payload+size)`.
- otherwise → an opaque leaf chunk identified by its 32-bit id.

## Track / scenery geometry (`TRACKS/STREAM*.BUN`, family `0x134xxx`)

The big `STREAM*.BUN` files hold the visible world. The small per-region
`L4R*.BUN` files are mostly metadata/VFX/gameplay, **not** the render mesh.

```
0x80134000  object
  0x80134010  mesh   (one material)
    0x00134011   material (name string, e.g. TRN_ROADA_CHOP_*, TRN_TERRAINA_*)
    0x80134100  geometry
      0x00134900   header/counts
      0x00134B01   vertex buffer   (see below)
      0x00134B02   submesh table
      0x00134B03   index buffer    (u16 triangle list)
```

**Vertex buffer (`0x134B01`).** A run of `0x11` filler bytes prefixes the data
(scan until the byte value stops being `0x11`). Two vertex layouts:

| asset | stride | layout |
|-------|--------|--------|
| scenery (STREAM) | **24 B** | `pos: f32[3]`, `color: u32 ARGB`, `uv: f32[2]` |
| car parts        | **36 B** | `pos: f32[3]`, `normal: f32[3]`, `color: u32`, `uv: f32[2]` |

**Object placement matrix (`0x134011 +0x40`, verified 2026-07).** After the
header's own `0x11` filler, offset `+0x40` holds a 4x4 `f32` world transform.
Layout is the **D3D row-vector convention: basis rows at floats 0-2 / 4-6 /
8-10, translation at floats 12-14, `m[15] == 1`** — byte-identical to an
OpenGL column-major matrix, so the array is applied as-is
(`world = v.x*m[0..2] + v.y*m[4..6] + v.z*m[8..10] + m[12..14]`), no
transposition. Audited with `tools/props_audit.py` across L4RH + L4RB
(2817 objects): road/terrain chunks carry an **identity** matrix (their
vertices are already world-space); props/buildings (`XO_*`, `XB_*`, `XU_*`)
carry a real placement (all 1094 rotations orthonormal to 1e-3; reading the
transpose instead yields zero translation for every object, i.e. everything
would pile at the origin). Every object has exactly **one** `0x134011`
header, so one matrix per object is safe. Objects (props + skydome) can
appear **twice** in a file with identical data — harmless to draw (equal
depth), just redundant.

**Submesh table (`0x134B02`).** 60-byte records, the index buffer consumed in
order: `bbox_min: f32[3]`, `idx_count: u32`, `bbox_max: f32[3]`,
`mat_id: u32`, `flag: u32 (0xff)`, 24 B reserved.

**Material chain.** `submesh.mat_id` → `0x134003` hash list → a `0x134011`
material record carrying a texture name/hash.

## Textures

Two container variants, both DXT-compressed S3TC.

**Track TPK (uncompressed table).** Header block `0xb3310000` holds fixed
`0x7c`-byte records. The record is the NFSU2 texture header (per the Nikki
library, minus a `0x0C` padding prefix that this in-place variant omits):

```
+0x18 BinKey (u32)          // record hash — what meshes reference
+0x24 Offset (u32)          // texture data offset
+0x28 PaletteOffset (u32)   // palette offset (P8 only)
+0x2c Size (u32)
+0x30 PaletteSize (u32)     // 0 = not palettized; 1024 = 256-entry RGBA
+0x38 Width  (u16)
+0x3a Height (u16)
```

Pixels are in block `0xb3320000`, under its **`0x33320002`** sub-chunk payload
(a `0x33320001` info sub-chunk comes first), and **`Offset`/`PaletteOffset` are
relative to the data start after the `0x11` alignment filler** that prefixes it
(same quirk as vertex buffers). Three pixel formats occur, distinguished by the
record, not the (always-0) compression byte:

- **P8** (`PaletteSize >= 1024`) — a 256-entry RGBA palette at `PaletteOffset`
  then 8-bit indices at `Offset`. This is what the road-surface textures use
  (`RDP_AIRPORT_ROADPATCH_A` is 512×512 P8), which is why they looked like
  high-entropy noise until decoded through the palette — **not** a swizzle.
- **DXT1 / DXT3** (no palette) — inferred from `Size` (DXT3 base `= W·H`, DXT1
  `= W·H/2`).

Track meshes bind textures exactly like cars: the `0x134012` slot list's key →
a TPK record `BinKey`. Each region's own `STREAM*.BUN` TPK carries its
grass/road/prop textures (names vary per region: `TRN_GRASSC` vs
`ORG_GRASS_001`, `RDP_PARKING…` vs `RDP_AIRPORT_ROADPATCH_A`), and the shared
`TRACKS/LOC4DYNTEX.BIN` (a compressed offset-slot TPK, same format as car
TEXTURES.BIN) holds the sky + facade textures.

**Car TPK (`CARS/*/TEXTURES.BIN`, compressed).** Same outer container
(`0xb3300000` → `0xb3310000` header + `0xb3320000` pixels). The header block
has three sub-chunks: `0x33310001` (info), `0x33310002` (hash table),
`0x33310003` (**offset-slot table**, 24-byte records):

```
u32 Key            // texture hash
i32 AbsoluteOffset // FILE-absolute offset of the compressed block
i32 EncodedSize
i32 DecodedSize
i16 RefCount
i32 Unknown
```

Each block at `AbsoluteOffset` is **JDLZ**-compressed (EA's LZ; header
`"JDLZ"`, `u32 decodedSize @+8`, `u32 encodedSize @+12`). Decompress to get the
raw DXT pixels directly (no per-texture header) — **DXT1 or DXT3** (DXT3 =
16-byte blocks: 8-byte 4-bit explicit alpha + 8-byte DXT1 colour). The format
isn't stored, so infer it: the textures are **square**, and only one
`(side, format)` makes a full mip chain sum to `DecodedSize` (DXT1 base
`W·H/2`, DXT3 base `W·H`, each mip ≈ ¼ the last). E.g. a HUMMER body detail
atlas is `21980 → 128×128 DXT3`; its wheel is `11068 → 128×128 DXT1`.

## Car material → texture binding (`GEOMETRY.BIN`)

Each car mesh (`0x80134010`) carries, alongside its `0x134011` **material**
record (part name + a bounding box + a transform), a `0x134012` **texture-slot
list**: 8-byte entries `u32 key, u32 0`. A mesh lists several slot hashes
(diffuse / normal / spec / …); the one whose `key` is present in the car's TPK
offset-slot table is that mesh's **diffuse** — bind it and sample via the
mesh's UVs. (A HUMMER references only ~6 of its 58 TPK textures this way: one
body atlas across 105 body meshes, plus wheel / brake / engine.)

The hashes are the standard **NFS "bin" hash** of the asset name:

```
h = 0xFFFFFFFF;  for each byte c of the name:  h = h*0x21 + c   (mod 2^32)
```

Verified: the material's part-name hash at `0x134011 +0x10` equals
`binhash("HUMMER_BASE_A") = 0xEE913807`. The same hash keys the TPK slots, so a
material→texture link needs no name strings — just the `0x134012` slot list
intersected with the TPK keys.

## Racing lines & circuits (`TRACKS/ROUTES*/Paths*.bin`)

Container `0x80034147` → children `0x34148/49/4a/4c/4d`. Chunk **`0x34148`** is
the racing line: **24-byte records**, `x: f32 @+0`, `y: f32 @+4`, then unknown
fields (a 2D centreline; positions share the STREAM world coordinate system).

Routes come in two kinds: **open sprints** (first waypoint far from the last)
and **closed circuits** (first ≈ last) — the latter are what you lap.
`ROUTESL4RF/Paths4602.bin` is a small 33-waypoint circuit inside the
`STREAML4RH` region, used here for the lap demo.

## Engine sound banks (`SOUND/ENGINE/CAR_*_ENG_MB_SPU.abk`)

EA "Ginsu" engine audio: per-car banks of RPM-band loops. Header `ABKC`;
bank offset `u32 @+0x18`, bank size `u32 @+0x24` → a `BNKl` bank holding
**PT ("platform") headers**: `'P','T',u16 platform`, then a TLV stream of
`(tag u8, len u8, big-endian value)` — `0x84` sample rate (default 22050),
`0x85` sample count, `0x86/0x87` loop start/end, `0x88` data offset
(bank-relative). Engine banks carry **8 streams**: `[0]` idle, `[1..3]`
accel low/mid/high, `[4..6]` decel low/mid/high, `[7]` high/whine
(28 kHz). Verified across all 41 banks.

Stream data is **EA-XA mono ADPCM**, a mix of two frame kinds:

- **15-byte ADPCM frame** = 28 samples. Header byte: coef index in the high
  nibble, shift in the low. Per 4-bit nibble (high first):
  `s = ((nib << (20 - shift)) + prev*c1 + prev2*c2 + 0x80) >> 8`, clamped,
  with `c1 = {0,240,460,392}`, `c2 = {0,0,-208,-220}`.
- **`0xEE` frame (61 bytes)** = predictor reseed + 28 uncompressed samples:
  two `s16be` @+1/+3 are the encoder's exact `prev/prev2` seed for the
  following ADPCM run, then 28 `s16be` output samples @+5.

Both the frame grammar and the decode formula were pinned empirically: the
byte/sample accounting fits every stream exactly, decode is exact at every
`0xEE` resync, and the decoded loops come out near-perfectly periodic
(autocorrelation 0.94-1.00 — they're seamless engine loops).

## Native Ginsu sweeps (`SOUND/ENGINE/GIN_*.gin`) — SOLVED

The authentic per-car engine audio: one continuous recording of the engine
revving `rpm_min → rpm_max`, **named by car** (`GIN_Hummer`,
`GIN_Nissan_240SX`, …; `_DCL`/`Decel` variants for coast). There is no
car→bank table anywhere in the data — car identity lives in these
filenames (the `CAR_NN` .abk banks are the console-style fallback).

```
+0x00  "Gnsu20\0\0"
+0x08  f32 rpm_min, f32 rpm_max
+0x10  u32 n1 (=50), u32 n2 (=128)
+0x18  u32 total_samples, u32 sample_rate (24000-36000)
+0x20  u32 rpm_pos[n1+1]   // sample position at rpm_min + i*(range/n1);
                           // DESCENDING in _DCL files (they rev down)
       u32 grain[n2+1]     // ascending cycle-aligned loop points
       audio               // EA-XAS v0
```

**EA-XAS v0** (layout confirmed against vgmstream's `ea_xas_decoder.c`,
used as a format reference only): 0x13-byte frames of 32 samples. The
frame's leading `u32` (LE) packs coef index (bits 0-3), **hist2** (bits
4-15, read as `s16 & 0xFFF0`), shift (bits 16-19), **hist1** (bits 20-31,
same trick); both history samples are *output*, so every frame decodes
independently — which is what makes granular grain-jumping seamless. Then
15 bytes = 30 nibbles, high first:
`s = ((nib << 12) >> shift) + hist1*c1 + hist2*c2`, clamped, with the
CD-XA coefficient pairs `c1 = {0, 0.9375, 1.796875, 1.53125}`,
`c2 = {0, 0, -0.8125, -0.859375}`.

Validated: the local fundamental measured at every `rpm_pos` marker equals
**rpm/60 Hz** exactly (up to autocorrelation octave picks) — the curve is
the RPM→position map, and engine fundamental = one cycle per rev. Playback
= loop the grain window containing `pos(rpm)`, pitch-ratio
`rpm / rpm_at(grain)`, crossfade accel vs `_DCL` sweep by throttle load.

## Sponsor vinyls (`CARS/*/VINYLS.BIN`) and EA "HUFF" compression — SOLVED

`VINYLS.BIN` is one big compressed TPK (same offset-slot container as
`TEXTURES.BIN`), but every slot's blob is **EA "HUFF"**: a 16-byte wrapper
(`"HUFF"`, u32 version 0x1001, u32 decSize LE, u32 encSize LE) around an
**EAC canonical-Huffman stream** (signature `30FBh`, part of the same EA
Canada codec family as RefPack `10FBh`). Some `TEXTURES.BIN` slots use the
same wrapper around raw DXT payloads.

**EAC Huffman** (per Martin Korth's spec at problemkaputt.de/psx-spx.htm,
"CDROM File Compression EA Methods (Huffman)"; implemented from the spec
as `n2_huff` in nfsu2.h): big-endian bitstream; u16 method 30FBh..35FBh
(+3 skip bytes if bit8), u24 decompressed size, u8 escape code; canonical
code widths read until the Kraft sum fills the 16-bit code space; symbol
values delta-assigned over not-yet-used bytes; decode loop = literal
symbols, with ESC + varint n meaning {n=0: EOS bit or raw 8-bit literal;
n>0: repeat previous byte n times} — the run mechanism that gives vinyls
their ~60:1 ratios. Methods 32FBh/34FBh add prefix-sum unfiltering.
Varint = count zero bits z, then read (z+2) bits + (1<<(z+2)) - 4.

**Vinyl payload** = P8-style 8-bit indices (w*h base + mip levels) + a
256-entry RGBA palette + a 144-byte trailing record: `name[24]` (e.g.
`GOLF_AEM_SCORPION`), key u32 @+0x18, palette offset/size @+0x2c/+0x30,
pixel size @+0x34, width/height u16 @+0x38. **The palette ships all-zero**
— vinyls are runtime-recolored from the player's chosen colours (the ~16
used indices are art shade levels; the most frequent index is the
transparent background). The engine synthesizes a dark cut-vinyl look and
composites the art under the badge atlas across the whole body (body
panels share one UV layout; most carry no texture key of their own).

## Scripted / dynamic objects (`ZCV_` / `ZCS_`) — companion `L4R*.BUN`

Each district has a small companion bundle (`TRACKS/L4RA.BUN` … `L4RR.BUN`,
0.1–0.4 MB) alongside its big `STREAM*.BUN`. It holds the **dynamic and
scripted set-dressing**: `ZCV_` names are moving vehicles (trains, trolleys,
drawbridges, warehouse doors, traffic semis) and `ZCS_` names are static props
(barrels, benches, boxes, signs). This is the format of the entity **definition
table**, decoded from `L4RA.BUN` (Phase 49). *Placement into the world and any
animation driver are NOT wired yet — see "Open" below.*

### Entity definition table

Container `0x80034020` holds **N named entities** (89 in L4RA). Each entity is a
**triple** of consecutive chunks in this order:

```
0x39200  header   (name, hash, type)        0x5c bytes
0x39201  hull verts (8-corner bounding box)  0x180 bytes (fixed)
0x39202  hull faces (12 triangles)           0x50 bytes (fixed)
```

**`0x39200` header** (0x5c body):

| off  | type      | value / meaning                                            |
|------|-----------|------------------------------------------------------------|
| 0x00 | u32       | 0                                                          |
| 0x04 | u32       | 1                                                          |
| 0x08 | u32       | 8 — class tag, constant across every entity                |
| 0x0c | u32       | **name hash** (FNV-32 of the name; e.g. DrawBridgeA = `0xca1d510d`) |
| 0x10 | char[32]  | **name**, NUL-padded (`ZCV_TrainEngineA`, `ZCS_Barrel_A`, …)|
| 0x30 | u32       | `0x24` — record type / sub-size, constant                  |
| 0x34 | u8[20]    | zero in this file — reserved / local-transform slot (unused)|

**`0x39201` hull vertices** (always 0x180 = 384 B):

| off  | type        | meaning                                                  |
|------|-------------|----------------------------------------------------------|
| 0x00 | u32, u32    | `3, 3` — constant (matrix/row dims)                       |
| 0x08 | u16, u16    | linked index pair, e.g. DrawBridgeA `(5,13)`; role TBD (grid cell or link id) |
| 0x0c | u32         | 0                                                        |
| 0x10 | 8 × 48 B    | **8 OBB corners**; each slot = `vec3 position` + 9 pad floats (0). **Local space** (centred near origin). |

Decoded corner boxes match real proportions: **DrawBridgeA** 20.4 × 37.2 × 2.1,
**TrainEngineA** 2.7 × 15.25 × 3.4, **TrolleyA** 14.3 × 5.9 × 1.5 (W×L×H units).

**`0x39202` hull faces** (always 0x50 = 80 B):

| off  | type        | meaning                                                  |
|------|-------------|----------------------------------------------------------|
| 0x00 | u32, u32    | `0x11111111 0x11111111` — the standard 8-byte `0x11` filler prefix (as everywhere in this format) |
| 0x08 | u16 × 36    | **36 indices, range 0–7 = 12 triangles** = the 6 faces of the corner box (2 tris/face) |

So a `ZCV_`/`ZCS_` entity is `name + hash + a local box hull`. There is **no
world position, orientation, or animation channel inside the triple.**

### World placement (partly decoded, next phase)

Placements live in a separate family in the same file, **linked to the entity
defs by name hash**, not decoded to completion here:

- `0x37090` (45 in L4RA, 168 B each): `0x11` filler, a count, a **name hash**
  (e.g. `0xb8a18038`), a 3×3 orientation, then **world-scale positions**
  (e.g. `(-712.0, -873.1, +17.7)`). This is the instance/placement record.
- `0x34250` (824 B, one per file): looks like the placement grid header —
  leading `93, 93` (cell dims) and a world origin/extent (`-1949, 1445, …`).
- `0x8003b601` groups (13, with `b602`–`b608` children) and the `0x39200/1/2`
  triples are cross-referenced by the same hashes.

**Open:** confirm the `0x37090` field layout (rotation vs. path-node list — a
train would carry a spline), resolve the `0x39201 +0x08` u16 pair, and map the
hash → def linkage before writing any runtime entity update loop.

## The retail executable

`speed2.exe` is packed with **SafeDisc** DRM (section names `stxt774`/`stxt371`,
`.text` entropy ≈ 8.0). Static disassembly reads encrypted bytes, so the formats
above were recovered from the *data* files, not the exe.

## Credits / references

These format notes stand on the shoulders of prior community reverse-engineering.
Used only as references to understand the formats — all code here is an
independent implementation.

- **yugecin — [`nfsu2-re`](https://github.com/yugecin/nfsu2-re)**
  ([docs](https://yugecin.github.io/nfsu2-re/docs.html),
  [functions](https://yugecin.github.io/nfsu2-re/funcs.html),
  [structs](https://yugecin.github.io/nfsu2-re/structs.html),
  [enums](https://yugecin.github.io/nfsu2-re/enums.html),
  [vars](https://yugecin.github.io/nfsu2-re/vars.html)) — an excellent, detailed
  reverse-engineering project of NFSU2. The chunk-container structure (the
  8-byte `magic`+`size` header and the `0x8`-nibble nesting rule) was confirmed
  against yugecin's documentation. Huge thanks — great work.
- **[Nikki](https://github.com/MaxHwoy/Nikki)** — NFS modding library; the
  reference for the TPK layout and the NFSU2 texture-header struct. Its field
  layout was what revealed the `PaletteOffset`/`PaletteSize` fields and cracked
  the P8 road-surface textures (which had otherwise looked like noise).
- **[OpenNFSTools](https://github.com/MWisBest/OpenNFSTools)** — reference for
  the JDLZ decompression algorithm.
- **[vgmstream](https://github.com/vgmstream/vgmstream)** — reference for the
  Gnsu20 table layout and the EA-XAS v0 frame format (`meta/gin.c`,
  `coding/ea_xas_decoder.c`).
