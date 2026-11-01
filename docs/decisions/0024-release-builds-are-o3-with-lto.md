# 0024: The optimised configurations are -O3 with link-time optimisation

**Status:** accepted

## Context

The `release` preset is CMake's RelWithDebInfo, which on GCC and clang is `-O2 -g`, and the
`windows` preset's Release configuration is MSVC's `/O2`. Neither optimises across translation
units, and the core is many of them: the 68000 reaches a device through `Bus68k`, the device
catches the Z80 or the ARM up, and each of those is its own file. Every bus cycle crosses those
boundaries.

Measured on 2026-11-01 with `scripts/benchmark.sh`, Apple clang, an M-series Mac, 3,600 frames
of each game from power-up, headless:

| Build | orlegend | kov2 | theglad | killbldp |
|---|---|---|---|---|
| `-O2 -g` (RelWithDebInfo as CMake defines it) | 7.6× | 5.7× | 4.8× | 4.0× |
| `-O3` | 8.1× | 5.9× | 5.0× | 4.3× |
| `-O3` with link-time optimisation | 8.9× | 6.5× | 5.3× | 4.5× |

The last build gave the same sums as the first on every script of the regression suite, as it
must: the core is deterministic whatever the optimiser does, and a difference would have been a
bug in the code.

## Decision

- `cmake/Optimisation.cmake` raises RelWithDebInfo from `-O2` to `-O3` on GCC and clang, keeping
  `-g`, and turns link-time optimisation on for the Release and RelWithDebInfo configurations
  wherever `CheckIPOSupported` says the toolchain has it. It applies to every target of the
  build, the fetched libraries included.
- Debug and the sanitizer preset are untouched.
- The flags are set in CMake, not in the presets, so that a build configured without a preset,
  and the Windows preset's Release configuration, get them too.

## Consequences

- Nine to twenty-one percent more frames a second over the thirty games, fourteen on average,
  for no change to the code. The games with an ARM gain the least: an interpreter's dispatch is
  what neither flag can inline away.
- A release build links longer, since the link is where the optimisation happens; the
  incremental build of a change to one file links the whole program again.
- A toolchain without link-time optimisation builds the same code, slower, and says so at
  configure time.
- The profile of a release build names inlined functions less exactly; a `sample` of it is
  still the right instrument ([0020](0020-the-core-stays-single-threaded.md)).
