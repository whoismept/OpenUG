# NFS: Underground 2 — file format notes

Reverse-engineered from the retail PC data files. Enough to load geometry,
textures and racing lines. All integers are little-endian.

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

Scenery positions are already world-space (no per-object transform needed).

**Submesh table (`0x134B02`).** 60-byte records, the index buffer consumed in
order: `bbox_min: f32[3]`, `idx_count: u32`, `bbox_max: f32[3]`,
`mat_id: u32`, `flag: u32 (0xff)`, 24 B reserved.

**Material chain.** `submesh.mat_id` → `0x134003` hash list → a `0x134011`
material record carrying a texture name/hash.

## Textures

Two container variants, both DXT-compressed S3TC.

**Track TPK (uncompressed table).** Header block `0xb3310000` holds fixed
`0x7c`-byte records: name @`+0`, **record hash @`+0x18`** (what meshes
reference — a precomputed value, *not* always `binhash(name)`), data offset
@`+0x24`, size @`+0x2c`, `width/height` as a `u16` pair @`+0x38`. Pixels are in
block `0xb3320000`, but relative to the payload of its **`0x33320002`
sub-chunk** (there's a `0x33320001` info sub-chunk first). Format is **DXT1 or
DXT3**, inferred from size (DXT3 base `= W·H`, DXT1 `= W·H/2`).

Track meshes bind textures exactly like cars: the `0x134012` slot list's key →
a TPK record hash. Each region's own `STREAM*.BUN` TPK carries its
grass/road/prop textures (names vary per region: `TRN_GRASSC` vs
`ORG_GRASS_001`, `RDP_PARKING…` vs `RDP_AIRPORT_ROADPATCH_A`), and the shared
`TRACKS/LOC4DYNTEX.BIN` (a compressed offset-slot TPK, same format as car
TEXTURES.BIN) holds the sky + facade textures. *Open:* some large road-surface
textures decode to noise (likely a swizzled/tiled layout) — not yet solved.

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
- **[Nikki](https://github.com/MaxHwoy/Nikki)** — NFS modding library; reference
  for the TPK layout and the NFSU2 texture header struct.
- **[OpenNFSTools](https://github.com/MWisBest/OpenNFSTools)** — reference for
  the JDLZ decompression algorithm.
