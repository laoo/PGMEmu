# 0013: The ARM7TDMI is our own, written from ARM's manual

**Status:** accepted

## Context

[0003](0003-cpu-cores.md) left the IGS027A's CPU open: our own ARMv4T interpreter, unless
SkyEmu's core (MIT) proved embeddable when M7 started. Two facts settled it.

- **SkyEmu's `arm7.h`** can be compiled into the core, but not as a unit of its own. It is
  2,279 lines of C that carry the ARM9's paths alongside the ARM7's. It fills global decode
  tables on first use, and needs SkyEmu's macros from elsewhere in that tree. Its CPU state holds
  debug rings and a `FILE*` beside the registers. Under [0006](0006-dependencies.md) it would be
  vendored unmodified, so every fix would be a patch to upstream code.
- **The RTL's core cannot be ported.** The MiSTer core's ARM7TDMI is Game Bub's
  (`rtl/gamebub-arm`), which is GPL-3.0-only. PGMEmu is GPL-2.0
  ([0004](0004-licence-gpl-2.md)), and a translation of it would be a derivative it cannot
  carry. Game Bub is the reference for what the RTL's CPU does, observed through the simulator
  and the suite, never read across into code.

## Decision

The ARM7TDMI is a core of our own, `cpu::Arm7`, written from ARM's ARM7TDMI Technical Reference
Manual. It covers ARM and Thumb, its three-stage pipeline held as two prefetched opcodes, and
every access and internal cycle counted as the manual counts them, one clock each.

- **It is right when it passes SingleStepTests' ARM7TDMI suite** (`scripts/fetch-arm7tdmi-tests.sh`).
  That covers 2,250,000 cases: every encoding from a random state, with every register bank, the
  pipeline, and each memory access in order with its kind, N or S, and its cycle. The suite was
  generated with NanoBoyAdvance. Where the manual leaves something open, such as R15 written by
  write-back, MRS into R15 or an empty register list, the core does what the suite does.
- **Where the suite and the RTL differ, the RTL wins**
  ([0002](0002-the-fpga-core-is-the-reference.md)). MULS and its kin leave C as it was, as Game
  Bub does; the suite has C as the real chip's Booth multiplier leaves it. That is 14,288 cases.
- **Where the suite and the manual differ in timing, the manual wins.** The suite runs an LDR
  that writes R15 back, and a Thumb LDMIA or POP of no registers, without the internal cycle the
  manual gives every load. Its own other loads of R15 have that cycle. That is 1,322 cases.
  None of these instructions occurs in a real program.
- **Memory is a bus interface**, `cpu::Arm7Bus`, given each access's kind, so that the IGS027A
  decodes it as `igs027a.sv` does.

## Consequences

- About 2,100 lines of our own code, the disassembler among them, formatted and tidied like the
  rest, its state a plain struct that a save state copies whole.
- The suite runs in about 2 seconds in a release build; `[cpu-suite]` runs it with the 68000's
  and the Z80's.
- Each access is one cycle. The RTL's core waits whenever its caches miss, and catches the time
  up afterwards, as the 68000 does after its ROM cache ([0010](0010-rom-timing.md)), so those
  waits are not modelled.
- The known disagreements are counted per file in `tests/cpu/Arm7SingleStepTest.cpp`; a change
  that moves any of them fails the suite.
- The core fetches its opcodes from code windows the IGS027A gives it, its ROMs, without the
  bus; the vectors stay on the bus, since type 3's FIQ vector fetch takes its FIQ down. Data
  accesses always go through the bus. The decode is a table of a byte an entry and the
  registers of the current mode are a copy, swapped when the mode changes. Measured on
  2026-11-01 (`scripts/benchmark.sh`), the ARM games gained seven to twenty-five percent in
  frames a second, killbldp and svg the most. The ARM runs a few thousand instructions at a
  time between the other chips and starts cold each time, so what it touches has to be small:
  a table of pointers, 64 KB, gave back everything the rest had gained.
