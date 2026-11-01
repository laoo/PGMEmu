# Documentation map

This file is the index of every document in the project. **A document that is
not listed here must not exist.** If a new piece of writing cannot be classified
into one of the buckets below, that is the signal it should not be written. The
regime is NGA's, adopted whole: [0001](decisions/0001-documentation-regime.md).

## The rule

> **Documentation carries *why* and *contracts*. Code carries *how*.**

If a sentence can be read off a function signature, it does not belong in a
document. Non-obvious *reasoning* inside the code belongs in code comments, not
here.

Two supporting rules:

1. **One fact, one place.** Every other place links to it instead of repeating it.
2. **No status files.** No `PROGRESS.md`, `TODO.md`, `CHANGES.md`, session notes
   or similar. Git history covers that ground.

Documentation is updated in the **same commit** as the change that invalidated
it, never "at the end of the session".

## Buckets

| Location | Answers | Must not contain |
|---|---|---|
| `/README.md` | What PGMEmu is, how to build it, how to run it | architecture, internal APIs |
| `/THIRD_PARTY.md` | What PGMEmu is derived from and built with, and the terms that work asks to be carried | anything of ours beyond the credits |
| `/CLAUDE.md` | How to work in this repository: commands, conventions, where things live | domain knowledge; rules and pointers only |
| `docs/architecture.md` | What the components are, how time and data flow between them, **why** the boundaries sit where they do | class or function listings, signatures |
| `docs/decisions/NNNN-*.md` | Why a decision was made, and what it costs. Immutable once merged; a reversal is a new record that supersedes the old one | present-tense state of the system |
| `docs/spec/*.md` | External contracts: the `.pgm` file as read, the control protocol, save-state layout | implementation detail |
| `docs/hardware/*.md` | Where the emulator knowingly differs from the RTL, from PGMTech or from the board, and the evidence | anything the RTL or PGMTech already says; link to them |
| `docs/plans/*.md` | The order decided-but-unbuilt work is built in, one entry per session-sized task, each removed in the commit that lands it. A queue that only shrinks | status, progress, what was done, any decision the records do not hold |
| `docs/open-questions.md` | Questions deferred on purpose, each removed when a record answers it | anything decided |
| `libextern/README.md` | Third-party code carried in the tree: what, from which upstream commit, under which licence, and how it is updated | anything of ours; a local patch to upstream code |

## Index

### Overview
- [Architecture](architecture.md): the core, the control API, the frontends, and how emulated time is kept

### Specifications
- [The control protocol](spec/control-protocol.md): transports, requests, responses, error codes, transcripts, and every method
- [The `.pgm` cartridge image](spec/pgm-format.md): PGMBuilder's format as the emulator reads it, and what it refuses
- [Batch scripts](spec/batch.md): runs written down, the checkpoints they sum, and the regression suite made of them

### Decision records
- [0001: Documentation regime](decisions/0001-documentation-regime.md)
- [0002: The FPGA core is the hardware reference](decisions/0002-the-fpga-core-is-the-reference.md)
- [0003: CPU cores](decisions/0003-cpu-cores.md)
- [0004: Licence is GPL-2.0](decisions/0004-licence-gpl-2.md)
- [0005: One control API, spoken in the RTL simulator's protocol](decisions/0005-one-control-api.md)
- [0006: Code that decides emulated behaviour is vendored, tooling is fetched](decisions/0006-dependencies.md)
- [0007: Code style and toolchain are NGA's](decisions/0007-code-style-is-ngas.md)
- [0008: The renderer is SDL_GPU](decisions/0008-the-renderer-is-sdl-gpu.md)
- [0009: Moira's configuration is ours](decisions/0009-moira-configuration.md)
- [0010: The ROM cache's timing is not reproduced](decisions/0010-rom-timing.md)
- [0011: Layers are drawn a line at a time, sprites a frame at a time](decisions/0011-video-is-drawn-by-line-and-by-frame.md)
- [0012: MCP over HTTP is served by cpp-httplib, JSON-lines over TCP by sockets of our own](decisions/0012-network-transports.md)
- [0013: The ARM7TDMI is our own, written from ARM's manual](decisions/0013-arm7tdmi-core.md)
- [0014: A cartridge is its image, as RetroHQ's hardware runs it](decisions/0014-images-as-retrohq-runs-them.md)
- [0015: The screen's shaders are GLSL, compiled offline to SPIR-V and MSL](decisions/0015-shaders-are-compiled-offline.md)
- [0016: Vertical games are known by their set's name](decisions/0016-vertical-games-by-set-name.md) (superseded by 0017)
- [0017: Vertical games are known by their image's flag](decisions/0017-vertical-games-by-image-flag.md)
- [0018: Rewind keeps a whole state for every frame](decisions/0018-rewind-keeps-every-frame.md)
- [0019: Run-ahead is one setting for every game, off unless chosen](decisions/0019-run-ahead.md)
- [0020: The core stays on one thread until a machine cannot keep up](decisions/0020-the-core-stays-single-threaded.md)
- [0021: PGMBuilder's code is not carried into the emulator](decisions/0021-pgmbuilder-is-not-a-source-of-code.md)
- [0022: CI builds three platforms, and a tag makes a release](decisions/0022-ci-and-releases.md)
- [0023: Breakpoints on the Z80 and the ARM7 stop the 68000's run, not the CPU](decisions/0023-breakpoints-on-the-caught-up-cpus.md)
- [0024: The optimised configurations are -O3 with link-time optimisation](decisions/0024-release-builds-are-o3-with-lto.md)

### Hardware
- [Differences](hardware/differences.md): where the emulator departs from the RTL, and the RTL from the board

### Plans
- [Milestones](plans/milestones.md): what is still to be built; none is open

### Open questions
- [Open questions](open-questions.md)
