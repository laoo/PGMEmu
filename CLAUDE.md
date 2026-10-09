# Working in this repository

## Language

Discussion with the user is in Polish. **All code, comments, documentation,
commit messages and identifiers are in English.**

## Build and test

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --install build/release --component pgmemu --prefix build/stage   # what a release package holds; PGMEmu.app on macOS
```

Presets: `debug`, `release`, `asan`. Never create build directories by hand;
the presets own `build/<preset>`. The test presets run tests in parallel on
every core; the release suite takes about 70 s. `-DPGM_BUILD_APP=OFF` at configure leaves out
the desktop application and with it SDL3 and ImGui.

```sh
./scripts/make-pgm.sh                       # build roms/*.pgm from ../ROMS with ../PGMBuilder
build/debug/src/pgm_app/pgmemu --bios ../ROMS/pgm.zip --rom-dir roms orlegend   # desktop: arrows, Z X C V, 1 start, 5 coin; View > Input
build/debug/src/pgm_app/pgmemu --bios ../ROMS/pgm.zip --rom-dir roms --server tcp:7701 --mcp-http 7702 orlegend   # with agents attached
build/debug/src/pgm_cli/pgmemu-cli --info roms/orlegend.pgm
build/debug/src/pgm_cli/pgmemu-cli --server --bios ../ROMS/pgm.zip --rom-dir roms
build/debug/src/pgm_cli/pgmemu-cli --mcp --bios ../ROMS/pgm.zip --rom-dir roms   # MCP for an agent; --state-dir DIR for save states
```

An agent drives the emulator with the project skill, `.claude/skills/pgmemu/SKILL.md`: a headless
server that keeps its state, and a request per command.

```sh
build/release/src/pgm_cli/pgmemu-cli --server tcp:7701 --bios ../ROMS/pgm.zip --rom-dir roms &
scripts/pgmemu.py emu.load_game name=orlegend
scripts/pgmemu.py video.screenshot path=build/shot.png
```

```sh
./scripts/compare-with-rtl.py --frames 60 600 --pictures   # memory and pictures against the RTL simulation
./scripts/compare-with-rtl.py --frames 800 --regions AUDIO_RAM --audio   # the Z80's RAM and the sound
./scripts/make-pgmtest.sh system_basics             # a PGMTest page as a BIOS program, from the core's testroms/
./scripts/compare-with-rtl.py --program build/tools/pgmtest-system_basics/pgm/pgm_p02s.u20 --frames 30
./scripts/compare-with-rtl.py --frames 600 --bisect          # where memory first differs: frame, line, instruction
./scripts/make-pgmtest.sh --all                     # every PGMTest page, for the regression suite
ctest --preset release -L regression                # golden frames of the BIOS, every game and every page
build/release/src/pgm_cli/pgmemu-cli --batch tests/regression/orlegend.json --bios ../ROMS/pgm.zip --rom-dir roms --record   # re-record one
./scripts/benchmark.sh                              # headless speed of every game
./scripts/measure-lag.py                            # the frame each game first answers a control on, about 6 min
./scripts/compile-shaders.sh                        # the screen's shaders, after changing their GLSL (brew install glslang spirv-cross)
./scripts/fetch-680x0-tests.sh                      # the SingleStepTests 68000 suite
./scripts/fetch-z80-tests.sh                        # the SingleStepTests Z80 suite, 1.4 GB to fetch
./scripts/fetch-arm7tdmi-tests.sh                   # the SingleStepTests ARM7TDMI suite, 0.9 GB
build/release/tests/pgm_tests "[cpu-suite]"         # Moira, z80.h and our ARM7 against them, about 40 s
```

Tests tagged `[roms]` read `roms/` and `../ROMS`, and `[cpu-suite]` reads `build/tools/680x0/`,
`build/tools/z80/` and `build/tools/arm7tdmi/`; both skip when their data is absent. The comparison
with the RTL runs the simulator at about 1.4 frames per second. Do not use the PGMBuilder binary in
`../PGMBuilder/out`: it may predate the format version this emulator reads.

```sh
./scripts/format.sh          # format in place
./scripts/format.sh --check  # fail on deviation
./scripts/tidy.sh            # clang-tidy against build/debug
```

Homebrew keeps LLVM keg-only, so `clang-tidy` is not on `PATH` on macOS, and it
does not know where Apple's SDK keeps the standard library. `scripts/tidy.sh`
handles both; do not invoke `clang-tidy` directly.

What to build next is [docs/plans/milestones.md](docs/plans/milestones.md); a
session takes one milestone by name.

## Layout

| Path | Contents |
|---|---|
| `src/pgm_core/` | The emulated machine and the control dispatcher. All logic; no SDL, ImGui, threads or file dialogs; deterministic. `moira/MoiraConfig.h` is Moira's configuration ([0009](docs/decisions/0009-moira-configuration.md)). |
| `src/pgm_server/` | Transports onto the dispatcher: JSON-lines (stdio, TCP) and MCP. |
| `src/pgm_cli/` | `pgmemu-cli`: headless runs, `--server`, `--mcp`. Argument parsing and I/O only. |
| `src/pgm_app/` | `pgmemu`: the SDL3 + Dear ImGui desktop frontend. |
| `tests/` | Catch2 v3 tests, linked against `pgm_core`. |
| `libextern/` | Third-party code carried in the tree, unmodified. Not formatted, not tidied. `libextern/README.md` is the inventory. |
| `cmake/` | `Warnings.cmake`, `Dependencies.cmake`. |
| `docs/` | See `docs/README.md`; it is the index and the rulebook. |
| `.claude/skills/pgmemu/` | The project skill an agent drives the emulator with. |

## The workspace around it

The repository builds and tests on its own. These checkouts beside it are
optional; the scripts and tests that need one say so, and skip or stop
without it. They are read-only references, never edited from here:

- `../Arcade-IGSPGM_MiSTer` (https://github.com/MiSTer-devel/Arcade-IGSPGM_MiSTer),
  Martin Donlon's MiSTer core. Its `rtl/` is **the hardware reference**
  ([0002](docs/decisions/0002-the-fpga-core-is-the-reference.md)). Port from
  it, not from MAME. A module header cites the commit it was read at:
  `e898860` is https://github.com/wickerwaka/Arcade-IGSPGM_MiSTer, where the
  core was until June 2026, and is `ddd88ca` at MiSTer-devel, whose history
  the move rewrote, with the same RTL; any other commit is MiSTer-devel's.
  The Verilator simulation is in `sim/`:
  - Run `PGM_ROM_DIR=../../ROMS ./sim <game>` for the GUI.
  - Run `./sim --server` for the JSON-lines protocol in `docs/sim-server.md`.
  - It runs at about 1.4 frames per second.

  Its `testroms/` is **PGMTest**, a test ROM that replaces the BIOS and reports
  results at WRAM 0x81F000 and over the RFIF debug link;
  `scripts/make-pgmtest.sh` builds its pages from a copy.
- `../PGMTech` (https://github.com/laoo/PGMTech) documents the board: memory
  maps, registers, video and ICS2115.
- `../PGMBuilder` (https://github.com/laoo/PGMBuilder) converts MAME zips to
  `.pgm`; `pgm.hpp` is the format. `scripts/make-pgm.sh` builds it and runs it.
  The RTL simulator's own `.pgm` loader is outdated and is not a reference.
- `../ICS2115` (https://github.com/wickerwaka/ICS2115): its `docs/` is an
  ICS2115 specification.
- `../ROMS` holds your own MAME sets, including `pgm.zip`, the BIOS. They are
  not distributed with anything here.
- `../Gearlynx` (https://github.com/drhelius/Gearlynx) is a design reference
  only. It is GPL-3 and is never copied from
  ([0004](docs/decisions/0004-licence-gpl-2.md)).
- NGA is the owner's other project, whose style and regime this one adopts
  ([0007](docs/decisions/0007-code-style-is-ngas.md)); its rules are written
  out here and in `docs/`.

## Code style

Enforced by `.clang-format` and `.clang-tidy`; the summary below is for
orientation, and the config files are the authority.

- Allman braces, 2-space indent, no tabs, 120-column limit.
- Spaces inside parentheses, but not empty ones: `foo( a, b )`, `if ( x )`, `bar()`.
- East const, pointer on the left: `int* p`, `int const* p`.
- Types are `CamelCase`; functions and variables are `camelBack`; namespaces are
  `lower_case` (`pgm`, `pgm::video`, ...).
- Class members are `mMember` (private or protected); struct members are `member` (public).
- Constants are `UPPER_CASE` at namespace and class scope, and `camelBack` inside functions.
- Enum names follow types; enum constants follow constants.
- File names are `CamelCase.hpp` / `CamelCase.cpp`.
- Emulated registers, buses and bit fields are held in unsigned fixed-width
  types (`std::uint8_t` ... `std::uint64_t`).
- An RTL signal keeps its words in its C++ name: `sprite_dma_en` becomes
  `spriteDmaEn`.
- Every hardware module's header comment names the RTL file(s) it ports and
  the MiSTer core commit they were read at.

C++23, but only the subset Apple clang, GCC and MSVC all implement.
`src/pgm_core/src/PortabilityChecks.cpp` pins that subset down; extend it
rather than discovering a gap in CI. Prefer `fmt` (via spdlog) over
`std::format`, and avoid `std::print`, `<stacktrace>` and `std::flat_map`.

## Documentation discipline

The rules are in `docs/README.md` and they are not optional. In short:

- Documentation carries **why** and **contracts**; code carries **how**.
- One fact, one place; everywhere else links to it. Facts about the board stay
  in PGMTech and the RTL and are linked, not copied.
- No status, progress or session-note files.
- Documentation is updated **in the same commit** as the change that invalidated it.
- A new document requires an entry in `docs/README.md`, or it does not get written.

Non-obvious reasoning belongs in code comments; do not comment the obvious.

## Never

- Commit ROMs, BIOS images or `.pgm` files.
- Commit before the owner has reviewed.
