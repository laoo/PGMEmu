// Moira's compile-time configuration, supplied by its client as Moira intends.
// Upstream's own MoiraConfig.h is deliberately not carried in libextern/Moira:
// its defaults suit Moira's test runner, not a machine. This file stands in its
// place on the include path, see docs/decisions/0009-moira-configuration.md.
//
// The file keeps upstream's name and extension because Moira includes it by
// that name.

#pragma once

// Proves which MoiraConfig.h was found. Moira includes the file with quotes, so
// a copy of upstream's beside Moira's sources would win over this one without a
// word; src/pgm_core/src/cpu/MoiraConfigCheck.cpp fails the build instead.
#define PGM_MOIRA_CONFIG 1

// `sync()` is called before every bus access rather than once per instruction.
// The RTL's fx68k is cycle-exact and the PGM bus inserts wait states per
// access (VRAM contention, protection stalls), so the machine has to be
// stepped up to each access to charge them where the core does.
#define MOIRA_PRECISE_TIMING true

// The client interface is a set of virtual functions. A non-virtual interface
// would be faster, but Moira then calls functions that one class in the
// program defines, and the 68000 single-step suite drives a client of its own
// beside M68k (tests/cpu/M68kSingleStepTest.cpp); it stays virtual until that
// suite can run through M68k.
#define MOIRA_VIRTUAL_API true

// A word or long access at an odd address raises an address error, as on the
// 68000 and in fx68k.
#define MOIRA_EMULATE_ADDRESS_ERROR true

// Function codes. Nothing on the PGM bus decodes them, but the 68000 stores them
// in the frame it stacks for an address error, so without them that frame is
// wrong (SingleStepTests 68000).
#define MOIRA_EMULATE_FC true

// The 68020 instruction cache does not exist on a 68000.
#define MOIRA_EMULATE_ICACHE false

// The debugger and the control API disassemble through Moira.
#define MOIRA_ENABLE_DASM true

// Musashi compatibility is for Moira's own test runner; the 68000 is wanted.
#define MOIRA_MIMIC_MUSASHI false

// The instructions whose execution the machine is told about, kept as upstream
// has them: STOP and TAS interact with the bus, BKPT with the debugger, and
// RESET asserts the reset line of the peripherals.
#define MOIRA_WILL_EXECUTE I == Instr::STOP || I == Instr::TAS || I == Instr::BKPT

#define MOIRA_DID_EXECUTE I == Instr::RESET
