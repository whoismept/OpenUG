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
`0x7c`-byte records: name @`+0`, data offset @`+0x24`, size @`+0x2c`,
`width/height` as a `u16` pair @`+0x38`; pixels are DXT1 in block `0xb3320000`.

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
raw DXT pixels — **DXT1 or DXT3** (DXT3 = 16-byte blocks: 8-byte 4-bit explicit
alpha + 8-byte DXT1 colour). Dimensions follow from `DecodedSize` (for DXT3,
`decoded ≈ W·H·4/3` including the mip chain, so e.g. `1398172 → 512×512`).

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

The car TPK layout and the NFSU2 texture header were cross-checked against the
open-source **Nikki** library and the JDLZ algorithm against **OpenNFSTools** —
used only as format references; all code here is an independent implementation.
