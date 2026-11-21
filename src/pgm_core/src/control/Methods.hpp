#pragma once

// The methods of docs/spec/control-protocol.md, one group per file, and what
// they share for reading parameters. Private to the core: a transport sees only
// the Dispatcher.

#include "pgm/Emulator.hpp"
#include "pgm/control/Dispatcher.hpp"

#include <cstdint>
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>

namespace pgm::control
{

void addEmuMethods( Dispatcher& dispatcher, Emulator& emulator );
void addMemoryMethods( Dispatcher& dispatcher, Emulator& emulator );
void addRunMethods( Dispatcher& dispatcher, Emulator& emulator );
void addCpuMethods( Dispatcher& dispatcher, Emulator& emulator );
void addTraceMethods( Dispatcher& dispatcher, Emulator& emulator );
void addVideoMethods( Dispatcher& dispatcher, Emulator& emulator );
void addAudioMethods( Dispatcher& dispatcher, Emulator& emulator );
void addInputMethods( Dispatcher& dispatcher, Emulator& emulator );
void addStateMethods( Dispatcher& dispatcher, Emulator& emulator );
void addTestMethods( Dispatcher& dispatcher, Emulator& emulator );

Error badRequest( std::string message );

/// One parameter of a method, as its schema describes it: a JSON Schema type
/// ("string", "integer", "boolean", "object", "array"), and what it means.
struct Param
{
  std::string_view name;
  std::string_view type;
  std::string_view description;
  bool required{ true };
};

/// The JSON Schema of a method's `params`: an object of `params`.
Json paramsSchema( std::initializer_list<Param> params );

/// A method's description and the schema of its params.
MethodInfo info( std::string_view description, std::initializer_list<Param> params = {} );

/// The string `params[name]`, or the `bad_request` that says it is missing or
/// is not a string.
std::expected<std::string, Error> requireString( Json const& params, std::string_view name );

/// The non-negative integer `params[name]`, or the `bad_request` that says it
/// is missing or is not one. A string of hex digits after `0x` or `$` is one
/// too.
std::expected<std::uint64_t, Error> requireUnsigned( Json const& params, std::string_view name );

/// `params[name]` as requireUnsigned() reads it, or `fallback` when the
/// parameter is left out. A `bad_request` when it is outside `low` to `high`.
std::expected<std::uint64_t, Error> optionalUnsigned(
    Json const& params, std::string_view name, std::uint64_t fallback, std::uint64_t low, std::uint64_t high );

/// A run's answer, as `emu.run_frames` gives it: why it ended, how long it
/// took, and the breakpoint or the watchpoint it stopped at.
Json runJson( machine::RunResult const& run, machine::Machine const& machine );

/// A CPU's registers as `cpu.get_state` answers them, without what it adds of
/// the machine (`halted`, `disasm`).
Json z80Json( machine::Z80Registers const& registers );
Json arm7Json( machine::Arm7Registers const& registers );

/// Where the last run stopped, when it was at a breakpoint or a watchpoint: the
/// `breakpoint` or `watchpoint` object of a run's answer, under its name.
/// Empty otherwise.
Json debugStopOf( machine::Machine const& machine );

} // namespace pgm::control
