#include "Methods.hpp"

#include <spdlog/fmt/fmt.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pgm::control
{

namespace
{

using Predicate = std::function<bool()>;

/// The run must not take longer than this without a timeout of its own: about
/// 20 seconds of emulated time, so that a request that will never be answered
/// is still answered.
constexpr std::uint64_t DEFAULT_TIMEOUT_TICKS = 1'000'000'000;

/// The most instructions one `emu.step` runs.
constexpr std::uint64_t MAX_STEPS = 1'000'000;

/// The 68000's address space, which `memory_equals` reads.
constexpr std::uint64_t ADDRESS_SPACE = 0x1000000;

std::string_view reasonName( machine::StopReason reason )
{
  switch ( reason )
  {
  case machine::StopReason::COMPLETED:
    return "completed";
  case machine::StopReason::BREAKPOINT:
    return "breakpoint";
  case machine::StopReason::WATCHPOINT:
    return "watchpoint";
  case machine::StopReason::CONDITION_MET:
    return "condition_met";
  case machine::StopReason::TIMEOUT:
    return "timeout";
  case machine::StopReason::HALTED:
    return "halted";
  }
  return "error";
}

std::string_view cpuName( machine::Cpu cpu )
{
  switch ( cpu )
  {
  case machine::Cpu::Z80:
    return "z80";
  case machine::Cpu::ARM7:
    return "arm7";
  case machine::Cpu::M68K:
    break;
  }
  return "m68k";
}

std::expected<machine::Machine*, Error> loadedMachine( Emulator& emulator )
{
  machine::Machine* const machine = emulator.machine();
  if ( machine == nullptr )
  {
    return std::unexpected( Error{ .code = "not_loaded", .message = "No game is loaded" } );
  }
  return machine;
}

/// The value of a signal the RTL simulator names, as the emulator has it.
std::optional<std::function<std::int64_t()>> signalOf( std::string const& name, machine::Machine const& machine )
{
  if ( name == "vblank" )
  {
    return [&machine] { return std::int64_t{ machine.vblank() ? 1 : 0 }; };
  }
  if ( name == "hblank" )
  {
    return [&machine] { return std::int64_t{ machine.hblank() ? 1 : 0 }; };
  }
  if ( name == "line" )
  {
    return [&machine] { return std::int64_t{ machine.line() }; };
  }
  if ( name == "frame" )
  {
    return [&machine] { return machine.frame(); };
  }
  return std::nullopt;
}

std::expected<Predicate, Error> parseCondition( Json const& condition, machine::Machine const& machine )
{
  if ( !condition.is_object() )
  {
    return std::unexpected( badRequest( "A condition must be an object" ) );
  }
  auto const type = requireString( condition, "type" );
  if ( !type )
  {
    return std::unexpected( type.error() );
  }

  if ( *type == "and" || *type == "or" || *type == "not" )
  {
    auto const children = condition.find( "children" );
    if ( children == condition.end() || !children->is_array() || children->empty() )
    {
      return std::unexpected( badRequest( fmt::format( "A {} condition needs children", *type ) ) );
    }
    std::vector<Predicate> parts;
    for ( Json const& child : *children )
    {
      auto part = parseCondition( child, machine );
      if ( !part )
      {
        return std::unexpected( part.error() );
      }
      parts.push_back( std::move( *part ) );
    }
    if ( *type == "not" )
    {
      return [first = std::move( parts.front() )] { return !first(); };
    }
    bool const all = *type == "and";
    return [all, operands = std::move( parts )]
    {
      for ( Predicate const& operand : operands )
      {
        if ( operand() != all )
        {
          return !all;
        }
      }
      return all;
    };
  }

  if ( type->starts_with( "cpu_pc_" ) )
  {
    auto const pc = [&machine] { return std::int64_t{ machine.m68kState().pc }; };
    if ( *type == "cpu_pc_equals" )
    {
      auto const target = requireUnsigned( condition, "value" );
      if ( !target )
      {
        return std::unexpected( target.error() );
      }
      return [pc, wanted = static_cast<std::int64_t>( *target )] { return pc() == wanted; };
    }
    // The simulator also accepts value/value2 for the bounds.
    bool const named = condition.contains( "start" );
    auto const start = requireUnsigned( condition, named ? "start" : "value" );
    auto const end = requireUnsigned( condition, named ? "end" : "value2" );
    if ( !start || !end )
    {
      return std::unexpected( !start ? start.error() : end.error() );
    }
    auto const low = static_cast<std::int64_t>( *start );
    auto const high = static_cast<std::int64_t>( *end );
    if ( *type == "cpu_pc_in_range" )
    {
      return [pc, low, high] { return pc() >= low && pc() < high; };
    }
    if ( *type == "cpu_pc_out_of_range" )
    {
      return [pc, low, high] { return pc() < low || pc() >= high; };
    }
  }

  if ( *type == "memory_equals" )
  {
    auto const address = requireUnsigned( condition, "address" );
    auto const wanted = requireUnsigned( condition, "value" );
    auto const size = optionalUnsigned( condition, "size", 1, 1, 4 );
    if ( !address || !wanted )
    {
      return std::unexpected( !address ? address.error() : wanted.error() );
    }
    if ( !size )
    {
      return std::unexpected( size.error() );
    }
    if ( *size == 3 || *address >= ADDRESS_SPACE || *size > ADDRESS_SPACE - *address )
    {
      return std::unexpected(
          badRequest( "memory_equals compares 1, 2 or 4 bytes within the 68000's 16 MB address space" ) );
    }
    return [&machine,
            at = static_cast<std::uint32_t>( *address ),
            bytes = static_cast<std::uint32_t>( *size ),
            value = *wanted]
    {
      // The bytes are taken in address order, the first the most significant,
      // as the 68000 reads a word or a long.
      std::uint64_t held = 0;
      for ( std::uint32_t i = 0; i < bytes; ++i )
      {
        std::uint16_t const word = machine.peek( at + i );
        held = ( held << 8U ) | ( ( ( at + i ) & 1U ) == 0 ? word >> 8U : word & 0xffU );
      }
      return held == value;
    };
  }

  if ( type->starts_with( "signal_" ) )
  {
    auto const name = requireString( condition, "signal" );
    if ( !name )
    {
      return std::unexpected( name.error() );
    }
    auto const signal = signalOf( *name, machine );
    if ( !signal )
    {
      return std::unexpected(
          Error{ .code = "invalid_signal", .message = fmt::format( "No signal {} in the emulator", *name ) } );
    }
    auto const field = condition.find( "value" );
    if ( field == condition.end() || !field->is_number_integer() )
    {
      return std::unexpected( badRequest( "Missing or invalid field: value" ) );
    }
    auto const wanted = field->get<std::int64_t>();
    auto const read = *signal;
    if ( *type == "signal_equals" )
    {
      return [read, wanted] { return read() == wanted; };
    }
    if ( *type == "signal_not_equals" )
    {
      return [read, wanted] { return read() != wanted; };
    }
    if ( *type == "signal_less_than" )
    {
      return [read, wanted] { return read() < wanted; };
    }
    if ( *type == "signal_less_equal" )
    {
      return [read, wanted] { return read() <= wanted; };
    }
    if ( *type == "signal_greater_than" )
    {
      return [read, wanted] { return read() > wanted; };
    }
    if ( *type == "signal_greater_equal" )
    {
      return [read, wanted] { return read() >= wanted; };
    }
  }

  return std::unexpected( badRequest( fmt::format( "Unknown condition type: {}", *type ) ) );
}

} // namespace

Json runJson( machine::RunResult const& run, machine::Machine const& machine )
{
  Json result{ { "reason", reasonName( run.reason ) },
               { "ticks_executed", run.ticks },
               { "frames_executed", run.frames } };
  result.update( debugStopOf( machine ) );
  return result;
}

Json debugStopOf( machine::Machine const& machine )
{
  Json stop = Json::object();
  if ( auto const& hit = machine.watchpointHit() )
  {
    stop["watchpoint"] = Json{ { "address", hit->address },
                               { "access", hit->write ? "write" : "read" },
                               { "value", hit->value },
                               { "bytes", hit->bytes },
                               { "pc", hit->pc } };
  }
  else if ( auto const& at = machine.breakpointHit() )
  {
    Json breakpoint{ { "cpu", cpuName( at->cpu ) }, { "address", at->address } };
    if ( at->z80 )
    {
      breakpoint["registers"] = z80Json( *at->z80 );
    }
    else if ( at->arm7 )
    {
      breakpoint["registers"] = arm7Json( *at->arm7 );
    }
    stop["breakpoint"] = std::move( breakpoint );
  }
  return stop;
}

void addRunMethods( Dispatcher& dispatcher, Emulator& emulator )
{
  dispatcher.add(
      "emu.reset",
      info( "Holds the board's reset line for some master ticks, the raster running on, then lets it go.",
            { { .name = "cycles",
                .type = "integer",
                .description =
                    "Master ticks (50 MHz) to hold reset for; 100 is what the RTL simulator's front end uses." } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto machine = loadedMachine( emulator );
        auto const ticks = requireUnsigned( params, "cycles" );
        if ( !machine || !ticks )
        {
          return std::unexpected( !machine ? machine.error() : ticks.error() );
        }
        ( *machine )->reset( static_cast<std::int64_t>( *ticks ) );
        return Json::object();
      } );

  dispatcher.add(
      "emu.run_frames",
      info( "Runs until some frame boundaries have passed, where vblank begins. Stops early at a breakpoint or a "
            "watchpoint; the answer then says which.",
            { { .name = "count", .type = "integer", .description = "Frames to run; 60 is about a second." } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto machine = loadedMachine( emulator );
        auto const count = requireUnsigned( params, "count" );
        if ( !machine || !count )
        {
          return std::unexpected( !machine ? machine.error() : count.error() );
        }
        return runJson( ( *machine )->runFrames( static_cast<std::int64_t>( *count ) ), **machine );
      } );

  dispatcher.add( "emu.run_cycles",
                  info( "Runs for at least some master ticks (50 MHz), stopping between 68000 instructions. Stops "
                        "early at a breakpoint or a watchpoint.",
                        { { .name = "count", .type = "integer", .description = "Master ticks to run." } } ),
                  [&emulator]( Json const& params ) -> Outcome
                  {
                    auto machine = loadedMachine( emulator );
                    auto const count = requireUnsigned( params, "count" );
                    if ( !machine || !count )
                    {
                      return std::unexpected( !machine ? machine.error() : count.error() );
                    }
                    return runJson( ( *machine )->runTicks( static_cast<std::int64_t>( *count ) ), **machine );
                  } );

  dispatcher.add(
      "emu.run_until",
      info( "Runs until a condition holds after a 68000 instruction, or a timeout passes. Conditions: {type: "
            "cpu_pc_equals, value}, {type: cpu_pc_in_range | cpu_pc_out_of_range, start, end}, {type: memory_equals, "
            "address, value, size}: the 1 (the default), 2 or 4 bytes at a 68000 address hold the value, {type: "
            "signal_equals | signal_not_equals | signal_less_than | signal_less_equal | signal_greater_than | "
            "signal_greater_equal, signal: vblank | hblank | line | frame, value}, and {type: and | or | not, "
            "children: [...]}.",
            { { .name = "condition", .type = "object", .description = "The condition, as described above." },
              { .name = "timeout_cycles",
                .type = "integer",
                .description = "Master ticks to give up after; about 20 s of emulated time if left out.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto machine = loadedMachine( emulator );
        if ( !machine )
        {
          return std::unexpected( machine.error() );
        }
        auto const condition = params.find( "condition" );
        if ( condition == params.end() )
        {
          return std::unexpected( badRequest( "Missing field: condition" ) );
        }
        auto predicate = parseCondition( *condition, **machine );
        if ( !predicate )
        {
          return std::unexpected( predicate.error() );
        }
        std::uint64_t timeout = DEFAULT_TIMEOUT_TICKS;
        if ( params.contains( "timeout_cycles" ) )
        {
          auto const given = requireUnsigned( params, "timeout_cycles" );
          if ( !given )
          {
            return std::unexpected( given.error() );
          }
          timeout = *given;
        }
        return runJson( ( *machine )->runUntil( *predicate, static_cast<std::int64_t>( timeout ) ), **machine );
      } );

  dispatcher.add(
      "emu.step",
      info( "Runs 68000 instructions one at a time, and answers as emu.run_frames does, with the instruction the "
            "68000 stands at after them as pc and disasm. Stops early at a breakpoint or a watchpoint.",
            { { .name = "count",
                .type = "integer",
                .description = "Instructions to run, 1 to 1000000; 1 if left out.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto machine = loadedMachine( emulator );
        auto const count = optionalUnsigned( params, "count", 1, 1, MAX_STEPS );
        if ( !machine || !count )
        {
          return std::unexpected( !machine ? machine.error() : count.error() );
        }
        Json result = runJson(
            ( *machine )
                ->step( static_cast<std::int64_t>( *count ), static_cast<std::int64_t>( DEFAULT_TIMEOUT_TICKS ) ),
            **machine );
        std::uint32_t const pc = ( *machine )->m68kState().pc;
        int length = 0;
        result["pc"] = pc;
        result["disasm"] = ( *machine )->disassemble( pc, length );
        return result;
      } );

  dispatcher.alias( "sim.reset", "emu.reset" );
  dispatcher.alias( "sim.run_frames", "emu.run_frames" );
  dispatcher.alias( "sim.run_cycles", "emu.run_cycles" );
  dispatcher.alias( "sim.run_until", "emu.run_until" );
}

} // namespace pgm::control
