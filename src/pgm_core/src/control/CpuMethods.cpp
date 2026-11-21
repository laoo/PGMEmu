#include "Methods.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <string>

namespace pgm::control
{

namespace
{

/// The most instructions one `cpu.disassemble` answers.
constexpr std::uint64_t MAX_INSTRUCTIONS = 1000;

std::expected<machine::Machine*, Error> loadedMachine( Emulator& emulator )
{
  machine::Machine* const machine = emulator.machine();
  if ( machine == nullptr )
  {
    return std::unexpected( Error{ .code = "not_loaded", .message = "No game is loaded" } );
  }
  return machine;
}

Json stateOf( machine::Machine const& machine )
{
  machine::M68kState const state = machine.m68kState();
  // `registers` is the simulator's: the 17 longs of fx68k's register file,
  // D0-D7, A0-A6, then the user and supervisor stack pointers.
  Json registers = Json::array();
  for ( std::uint32_t const value : state.d )
  {
    registers.push_back( value );
  }
  for ( std::size_t i = 0; i < 7; ++i )
  {
    registers.push_back( state.a.at( i ) );
  }
  registers.push_back( state.usp );
  registers.push_back( state.ssp );

  int length = 0;
  return Json{ { "pc", state.pc },
               { "registers", std::move( registers ) },
               { "disasm", machine.disassemble( state.pc, length ) },
               { "d", state.d },
               { "a", state.a },
               { "sr", state.sr },
               { "usp", state.usp },
               { "ssp", state.ssp },
               { "stopped", state.stopped },
               { "halted", state.halted } };
}

} // namespace

Json z80Json( machine::Z80Registers const& r )
{
  return Json{ { "pc", r.pc },   { "sp", r.sp },     { "af", r.af },    { "bc", r.bc },   { "de", r.de },
               { "hl", r.hl },   { "ix", r.ix },     { "iy", r.iy },    { "af_", r.af2 }, { "bc_", r.bc2 },
               { "de_", r.de2 }, { "hl_", r.hl2 },   { "wz", r.wz },    { "i", r.i },     { "r", r.r },
               { "im", r.im },   { "iff1", r.iff1 }, { "iff2", r.iff2 } };
}

namespace
{

Json z80StateOf( machine::Machine const& machine )
{
  Json state = z80Json( machine.z80Registers() );
  state["halted"] = machine.z80Halted();
  return state;
}

std::string_view modeName( std::uint32_t cpsr )
{
  switch ( cpsr & 0x1fU )
  {
  case 0x10:
    return "user";
  case 0x11:
    return "fiq";
  case 0x12:
    return "irq";
  case 0x13:
    return "supervisor";
  case 0x17:
    return "abort";
  case 0x1b:
    return "undefined";
  case 0x1f:
    return "system";
  default:
    return "invalid";
  }
}

Error noArm7()
{
  return Error{ .code = "no_cpu", .message = "The loaded board has no ARM7: it has no IGS027A" };
}

} // namespace

Json arm7Json( machine::Arm7Registers const& registers )
{
  return Json{ { "pc", registers.pc },
               { "r", registers.r },
               { "cpsr", registers.cpsr },
               { "spsr", registers.spsr },
               { "mode", modeName( registers.cpsr ) },
               { "thumb", ( registers.cpsr & 0x20U ) != 0 },
               { "fiq", registers.fiq },
               { "cycles", registers.cycles } };
}

namespace
{

std::expected<Json, Error> arm7StateOf( machine::Machine const& machine )
{
  auto const registers = machine.arm7Registers();
  if ( !registers )
  {
    return std::unexpected( noArm7() );
  }
  bool const thumb = ( registers->cpsr & 0x20U ) != 0;
  int length = 0;
  Json state = arm7Json( *registers );
  state["disasm"] = machine.disassembleArm7( registers->pc, thumb, length ).value_or( "" );
  return state;
}

/// Which CPU `params` name: m68k when they name none.
std::expected<std::string, Error> cpuOf( Json const& params )
{
  if ( !params.contains( "cpu" ) )
  {
    return std::string{ "m68k" };
  }
  auto cpu = requireString( params, "cpu" );
  if ( cpu && *cpu != "m68k" && *cpu != "z80" && *cpu != "arm7" )
  {
    return std::unexpected( badRequest( fmt::format( "cpu is m68k, z80 or arm7, not {}", *cpu ) ) );
  }
  return cpu;
}

/// The processor `params` name for a breakpoint, or the error that says it is
/// not one, or not on this board.
std::expected<machine::Cpu, Error> breakpointCpu( Json const& params, machine::Machine const& machine )
{
  auto const name = cpuOf( params );
  if ( !name )
  {
    return std::unexpected( name.error() );
  }
  if ( *name == "z80" )
  {
    return machine::Cpu::Z80;
  }
  if ( *name == "arm7" )
  {
    if ( !machine.arm7Registers() )
    {
      return std::unexpected( noArm7() );
    }
    return machine::Cpu::ARM7;
  }
  return machine::Cpu::M68K;
}

constexpr std::string_view BREAKPOINT_CPU = "m68k (the default), z80 or arm7 (the IGS027A's). A Z80 or ARM7 "
                                            "breakpoint stops the run after the 68000 instruction during which that "
                                            "processor reached it; the processor itself has run on.";

} // namespace

void addCpuMethods( Dispatcher& dispatcher, Emulator& emulator )
{
  dispatcher.add(
      "cpu.get_state",
      info( "A CPU's registers: the 68000's or the IGS027A's ARM7, with the instruction at its PC, or the Z80's.",
            { { .name = "cpu",
                .type = "string",
                .description = "m68k (the default), z80 or arm7.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const machine = loadedMachine( emulator );
        auto const cpu = cpuOf( params );
        if ( !machine || !cpu )
        {
          return std::unexpected( !machine ? machine.error() : cpu.error() );
        }
        if ( *cpu == "z80" )
        {
          return z80StateOf( **machine );
        }
        if ( *cpu == "arm7" )
        {
          return arm7StateOf( **machine );
        }
        return stateOf( **machine );
      } );

  dispatcher.add(
      "cpu.disassemble",
      info( "Disassembles 68000 or ARM7 instructions from an address.",
            { { .name = "address", .type = "integer", .description = "Where to start." },
              { .name = "count", .type = "integer", .description = "How many instructions, at most 1000." },
              { .name = "cpu", .type = "string", .description = "m68k (the default) or arm7.", .required = false },
              { .name = "thumb",
                .type = "boolean",
                .description = "ARM7: Thumb rather than ARM instructions; the ARM7's own state if left out.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const machine = loadedMachine( emulator );
        auto const address = requireUnsigned( params, "address" );
        auto const count = requireUnsigned( params, "count" );
        auto const cpu = cpuOf( params );
        if ( !machine )
        {
          return std::unexpected( machine.error() );
        }
        if ( !cpu )
        {
          return std::unexpected( cpu.error() );
        }
        if ( *cpu == "z80" )
        {
          return std::unexpected( badRequest( "The Z80 has no disassembler here; cpu is m68k or arm7" ) );
        }
        if ( !address )
        {
          return std::unexpected( address.error() );
        }
        if ( !count )
        {
          return std::unexpected( count.error() );
        }
        if ( *count > MAX_INSTRUCTIONS )
        {
          return std::unexpected( badRequest( fmt::format( "count is above the limit of {}", MAX_INSTRUCTIONS ) ) );
        }
        Json lines = Json::array();
        auto at = static_cast<std::uint32_t>( *address );
        bool thumb = false;
        if ( *cpu == "arm7" )
        {
          auto const registers = ( *machine )->arm7Registers();
          if ( !registers )
          {
            return std::unexpected( noArm7() );
          }
          thumb = params.contains( "thumb" ) ? params.at( "thumb" ) == true : ( registers->cpsr & 0x20U ) != 0;
        }
        for ( std::uint64_t i = 0; i < *count; ++i )
        {
          int length = 0;
          std::string text = *cpu == "arm7" ? ( *machine )->disassembleArm7( at, thumb, length ).value_or( "" )
                                            : ( *machine )->disassemble( at, length );
          lines.push_back( Json{ { "address", at }, { "length", length }, { "text", std::move( text ) } } );
          at += static_cast<std::uint32_t>( length > 0 ? length : 2 );
        }
        return lines;
      } );

  dispatcher.add( "debug.breakpoint.add",
                  info( "Stops runs where a processor executes the instruction at an address: the 68000 before it "
                        "executes it.",
                        { { .name = "address", .type = "integer", .description = "The instruction's address." },
                          { .name = "cpu", .type = "string", .description = BREAKPOINT_CPU, .required = false } } ),
                  [&emulator]( Json const& params ) -> Outcome
                  {
                    auto const machine = loadedMachine( emulator );
                    if ( !machine )
                    {
                      return std::unexpected( machine.error() );
                    }
                    auto const address = requireUnsigned( params, "address" );
                    auto const cpu = breakpointCpu( params, **machine );
                    if ( !address || !cpu )
                    {
                      return std::unexpected( !address ? address.error() : cpu.error() );
                    }
                    ( *machine )->addBreakpoint( static_cast<std::uint32_t>( *address ), *cpu );
                    return Json::object();
                  } );

  dispatcher.add( "debug.breakpoint.remove",
                  info( "Removes a breakpoint.",
                        { { .name = "address", .type = "integer", .description = "The breakpoint's address." },
                          { .name = "cpu",
                            .type = "string",
                            .description = "m68k (the default), z80 or arm7.",
                            .required = false } } ),
                  [&emulator]( Json const& params ) -> Outcome
                  {
                    auto const machine = loadedMachine( emulator );
                    if ( !machine )
                    {
                      return std::unexpected( machine.error() );
                    }
                    auto const address = requireUnsigned( params, "address" );
                    auto const cpu = breakpointCpu( params, **machine );
                    if ( !address || !cpu )
                    {
                      return std::unexpected( !address ? address.error() : cpu.error() );
                    }
                    ( *machine )->removeBreakpoint( static_cast<std::uint32_t>( *address ), *cpu );
                    return Json::object();
                  } );

  dispatcher.add(
      "debug.watchpoint.add",
      info( "Stops runs after a 68000 instruction that reads or writes data in a range of addresses. Instruction "
            "fetches do not count.",
            { { .name = "address", .type = "integer", .description = "The range's first address." },
              { .name = "size",
                .type = "integer",
                .description = "Bytes in the range; 1 if left out.",
                .required = false },
              { .name = "access",
                .type = "string",
                .description = "read, write (the default) or access, for both.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const machine = loadedMachine( emulator );
        auto const address = requireUnsigned( params, "address" );
        if ( !machine || !address )
        {
          return std::unexpected( !machine ? machine.error() : address.error() );
        }
        std::uint64_t size = 1;
        if ( params.contains( "size" ) )
        {
          auto const given = requireUnsigned( params, "size" );
          if ( !given || *given == 0 )
          {
            return std::unexpected( given ? badRequest( "size must be at least 1" ) : given.error() );
          }
          size = *given;
        }
        std::string access = "write";
        if ( params.contains( "access" ) )
        {
          auto const given = requireString( params, "access" );
          if ( !given || ( *given != "read" && *given != "write" && *given != "access" ) )
          {
            return std::unexpected( given ? badRequest( "access is read, write or access" ) : given.error() );
          }
          access = *given;
        }
        ( *machine )
            ->addWatchpoint( machine::Watchpoint{ .address = static_cast<std::uint32_t>( *address ),
                                                  .size = static_cast<std::uint32_t>( size ),
                                                  .read = access != "write",
                                                  .write = access != "read" } );
        return Json::object();
      } );

  dispatcher.add(
      "debug.watchpoint.remove",
      info( "Removes the watchpoint at an address.",
            { { .name = "address", .type = "integer", .description = "The watchpoint's first address." } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const machine = loadedMachine( emulator );
        auto const address = requireUnsigned( params, "address" );
        if ( !machine || !address )
        {
          return std::unexpected( !machine ? machine.error() : address.error() );
        }
        ( *machine )->removeWatchpoint( static_cast<std::uint32_t>( *address ) );
        return Json::object();
      } );

  dispatcher.add( "debug.watchpoint.list",
                  info( "The watchpoints." ),
                  [&emulator]( Json const& /*params*/ ) -> Outcome
                  {
                    auto const machine = loadedMachine( emulator );
                    if ( !machine )
                    {
                      return std::unexpected( machine.error() );
                    }
                    Json points = Json::array();
                    for ( machine::Watchpoint const& point : ( *machine )->watchpoints() )
                    {
                      std::string_view access = point.read ? "read" : "write";
                      if ( point.read && point.write )
                      {
                        access = "access";
                      }
                      points.push_back(
                          Json{ { "address", point.address }, { "size", point.size }, { "access", access } } );
                    }
                    return Json{ { "watchpoints", std::move( points ) } };
                  } );

  dispatcher.add( "debug.breakpoint.list",
                  info( "The addresses of a processor's breakpoints, ascending.",
                        { { .name = "cpu",
                            .type = "string",
                            .description = "m68k (the default), z80 or arm7.",
                            .required = false } } ),
                  [&emulator]( Json const& params ) -> Outcome
                  {
                    auto const machine = loadedMachine( emulator );
                    if ( !machine )
                    {
                      return std::unexpected( machine.error() );
                    }
                    auto const cpu = breakpointCpu( params, **machine );
                    if ( !cpu )
                    {
                      return std::unexpected( cpu.error() );
                    }
                    Json addresses = Json::array();
                    for ( std::uint32_t const address : ( *machine )->breakpoints( *cpu ) )
                    {
                      addresses.push_back( address );
                    }
                    return addresses;
                  } );
}

} // namespace pgm::control
