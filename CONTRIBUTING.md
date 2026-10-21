# Contributing to Angels95 / AngelTech Engine

Thanks for your interest! This is a clone-and-play battlefield of a game
engine — C++20, plain Makefiles (no CMake for the game itself), raylib 5.5 for
the client, and a standalone dedicated server. Before you dive in, check the
docs:

- [`AGENTS.md`](AGENTS.md) — quick developer reference (highly recommended).
- [`Wiki/Building.md`](Wiki/Building.md) — build prerequisites and usage.
- [`Wiki/Engine-Overview.md`](Wiki/Engine-Overview.md) — architecture and layout.
- [`Wiki/LightningScript.md`](Wiki/LightningScript.md) — the scripting language.
- [`Wiki/World-Format-OZONE.md`](Wiki/World-Format-OZONE.md) — WDL + OZONE formats.

## Getting started

### Prerequisites

| Platform | Toolchain |
|---|---|
| Linux / macOS | `g++` (or clang++), `make`, raylib 5.5 installed system-wide |
| Windows (w64devkit) | `C:\raylib\w64devkit` (GCC 15.2.0), run `build-native-win.ps1` |
| Windows (MSYS2) | `mingw-w64-x86_64-{gcc,make,raylib}`, run `build.ps1` |

The plain Makefile targets are `OTENGINE` (client `Angels95`), `AngelServ`
(server), `ozpack` (asset packer). `AngelEd` (editor) is built only on
Windows — its Makefile errors out on Linux by design.

### Build

```bash
make -j$(nproc)         # all three: Angels95, AngelServ, OzPack
make AngelServ          # server only (no raylib dependency)
make test               # build + run all test suites
```

On Windows use `build-native-win.ps1` (w64devkit) or `build.ps1` (MSYS2);
see the [Wiki](Wiki/Building.md).

## Development workflow

1. **Fork + branch.** Use a short descriptive branch name.
2. **Keep conventions:**
   - C++20; one `g++` invocation per Makefile target; objects in `build/`.
   - All network packet structs are `#pragma pack(push,1)`.
   - Include `Source/WindowsCompat.hpp` early in files mixing raylib and winsock.
   - Do not add comments unless needed; match the surrounding style.
3. **Add/update tests.** Tests live in `tests/*.test.cpp` (no framework —
   standalone executables). Run `make test`.
4. **Verify before opening a PR:**
   - `make test` passes (all suites).
   - `make OTENGINE AngelServ ozpack` builds cleanly.
   - If you changed the editor, also build `AngelEd` (Windows).
5. **CI.** GitHub Actions builds Linux (angelerv + client + ozpack) and
   Windows (all four targets + release assembly) on every push/PR. Keep it
   green — do not merge red.

## Release process (maintainers)

- Version bumps are tagged `b<N>` (e.g. `b55`).
- Tagging `b*` triggers CI to build and attach `System-<tag>.zip` as a GitHub
  Release. Bump the release just before or alongside the tag on `master`.
- Changelog entries go in `README.md`.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). Be
respectful, give constructive feedback, and report violations appropriately.

## Security

See [SECURITY.md](SECURITY.md) for reporting vulnerabilities.

## License

By contributing you agree that your contributions are licensed under the
[MIT License](LICENSE).