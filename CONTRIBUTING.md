# Contributing to OpenUG2

Thank you for helping improve OpenUG2. This is a clean-room, from-scratch engine reimplementation: the repository contains no EA source code or game assets.

## Before you start

- Search open issues before beginning work. For a broad feature or a format interpretation, open an issue first so the approach can be discussed.
- Keep each pull request focused on one concern.
- Do not commit, attach, paste, or link redistributed game assets, executable code, decrypted game data, proprietary symbols, or copied disassembly/decompiler output.
- You may describe independently observed data layouts needed for interoperability. Clearly separate an observation from an inference, and cite public references where useful.

## Build and verify

OpenUG2 needs SDL2 and zlib. Build from a clean checkout:

```sh
make
make gles
```

Before opening a pull request:

- Build with no new warnings.
- Keep both desktop OpenGL and GLES builds working when your change touches shared rendering code.
- For rendering changes, include a reproducible before/after `--shot` comparison or equivalent evidence.
- For parser or format changes, include a small asset-free reproduction method and document confirmed format facts in `docs/FORMATS.md`.

You need your own legally acquired copy of the game to run the engine. Never include game files or personal installation paths in a commit, issue attachment, or pull request.

## Reporting format findings

A useful format report contains:

1. The file family and chunk/record identifier.
2. Exact offsets, field sizes, endianness, and sample count.
3. A repeatable measurement or minimal asset-free script.
4. Any negative result or competing interpretation that was tested.
5. Whether the result is directly observed or inferred.

Please avoid presenting an inferred schema as established fact. Format documentation should remain auditable and useful to independent implementations.

## Pull requests

Explain the problem, the solution, and how you verified it. Keep generated files and unrelated refactors out of the change. Maintainers may ask for a smaller PR, additional evidence, or a follow-up issue when a change combines unrelated work.

By contributing, you agree that your contribution may be distributed under this repository's MIT License.
