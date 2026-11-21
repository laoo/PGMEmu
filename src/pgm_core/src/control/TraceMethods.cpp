#include "Methods.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pgm::control
{

namespace
{

/// The most instructions one `debug.trace` or `debug.trace.find` answers.
constexpr std::uint64_t MAX_ANSWERED = 4096;
constexpr std::uint64_t DEFAULT_ANSWERED = 32;

/// The 68000's address space: a PC is below this.
constexpr std::uint64_t ADDRESS_SPACE = 0x1000000;

/// The registers `debug.trace.find`'s `where` can name, as a trace entry
/// holds them: D0-D7, A0-A7, then the status register.
constexpr std::array<std::string_view, 17> REGISTERS{ "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0",
                                                      "a1", "a2", "a3", "a4", "a5", "a6", "a7", "sr" };

std::uint32_t registerOf( machine::TraceEntry const& entry, std::size_t index )
{
  if ( index < 8 )
  {
    return entry.d.at( index );
  }
  return index < 16 ? entry.a.at( index - 8 ) : entry.sr;
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

std::string lowered( std::string text )
{
  std::ranges::transform(
      text, text.begin(), []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
  return text;
}

/// An instruction of the trace as the protocol answers it; the disassembly is
/// of what memory holds at its address now.
Json instructionJson( machine::Machine const& machine,
                      machine::TraceEntry const& entry,
                      std::size_t index,
                      bool registers )
{
  int length = 0;
  Json instruction{ { "index", index },
                    { "pc", entry.pc },
                    { "ticks", entry.ticks },
                    { "disasm", machine.disassemble( entry.pc, length ) } };
  if ( registers )
  {
    instruction["d"] = entry.d;
    instruction["a"] = entry.a;
    instruction["sr"] = entry.sr;
  }
  return instruction;
}

/// What `debug.trace.find` looks for: every part given must hold.
struct Search
{
  /// The PC is at least `pcStart` and below `pcEnd`.
  std::uint32_t pcStart{};
  std::uint64_t pcEnd{ ADDRESS_SPACE };
  /// Registers by their place in REGISTERS, and the value each held.
  std::vector<std::pair<std::size_t, std::uint32_t>> registers;
  /// Lower case; empty for any.
  std::string disasmContains;
};

std::expected<Search, Error> searchOf( Json const& params )
{
  Search search;
  bool const range = params.contains( "pc_start" ) || params.contains( "pc_end" );
  if ( params.contains( "pc" ) )
  {
    auto const pc = requireUnsigned( params, "pc" );
    if ( !pc || range || *pc >= ADDRESS_SPACE )
    {
      return std::unexpected( pc ? badRequest( "pc is one address below 0x1000000, given without pc_start and pc_end" )
                                 : pc.error() );
    }
    search.pcStart = static_cast<std::uint32_t>( *pc );
    search.pcEnd = *pc + 1;
  }
  else if ( range )
  {
    auto const start = requireUnsigned( params, "pc_start" );
    auto const end = requireUnsigned( params, "pc_end" );
    if ( !start || !end )
    {
      return std::unexpected( badRequest( "pc_start and pc_end are given together" ) );
    }
    if ( *start >= *end || *end > ADDRESS_SPACE )
    {
      return std::unexpected( badRequest( "pc_start is below pc_end, which is at most 0x1000000" ) );
    }
    search.pcStart = static_cast<std::uint32_t>( *start );
    search.pcEnd = *end;
  }

  if ( auto const where = params.find( "where" ); where != params.end() )
  {
    if ( !where->is_object() )
    {
      return std::unexpected( badRequest( "where is an object of registers and the values they held" ) );
    }
    for ( auto const& [name, value] : where->items() )
    {
      auto const known = std::ranges::find( REGISTERS, std::string_view{ name } );
      auto const wanted = requireUnsigned( *where, name );
      if ( known == REGISTERS.end() || !wanted || *wanted > 0xffffffffU )
      {
        return std::unexpected( badRequest(
            fmt::format( "where names d0 to d7, a0 to a7 or sr, each with a 32-bit value; {} is not one", name ) ) );
      }
      search.registers.emplace_back( static_cast<std::size_t>( known - REGISTERS.begin() ),
                                     static_cast<std::uint32_t>( *wanted ) );
    }
  }

  if ( params.contains( "disasm_contains" ) )
  {
    auto text = requireString( params, "disasm_contains" );
    if ( !text )
    {
      return std::unexpected( text.error() );
    }
    search.disasmContains = lowered( std::move( *text ) );
  }
  return search;
}

constexpr std::string_view WITH_REGISTERS =
    "true to add each instruction's d, a and sr, as they were before it; false if left out. The trace keeps them "
    "only once debug.trace.configure has turned them on.";

/// What a request that needs the registers is answered while the trace keeps
/// none.
Error registersNotKept()
{
  return Error{ .code = "registers_not_kept",
                .message = "The trace keeps no registers: turn them on with debug.trace.configure {\"registers\": "
                           "true}, then run" };
}

} // namespace

void addTraceMethods( Dispatcher& dispatcher, Emulator& emulator )
{
  dispatcher.add(
      "debug.trace",
      info( "The last instructions the 68000 executed, oldest first, with when each began in master ticks. index "
            "counts back from the last one executed, which is 0; total is how many are kept.",
            { { .name = "count",
                .type = "integer",
                .description = "How many, at most 4096; 32 if left out.",
                .required = false },
              { .name = "from",
                .type = "integer",
                .description = "The index of the newest one to answer; 0, the last executed, if left out.",
                .required = false },
              { .name = "registers", .type = "boolean", .description = WITH_REGISTERS, .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const machine = loadedMachine( emulator );
        if ( !machine )
        {
          return std::unexpected( machine.error() );
        }
        std::uint64_t count = DEFAULT_ANSWERED;
        if ( params.contains( "count" ) )
        {
          auto const given = requireUnsigned( params, "count" );
          if ( !given )
          {
            return std::unexpected( given.error() );
          }
          count = std::min( *given, MAX_ANSWERED );
        }
        auto const from = optionalUnsigned( params, "from", 0, 0, machine::Machine::MAX_TRACE_SIZE );
        if ( !from )
        {
          return std::unexpected( from.error() );
        }
        bool const registers = params.contains( "registers" ) && params.at( "registers" ) == true;
        if ( registers && !( *machine )->traceRegisters() )
        {
          return std::unexpected( registersNotKept() );
        }

        auto const first = static_cast<std::size_t>( *from );
        auto const entries = ( *machine )->trace( static_cast<std::size_t>( count ), first );
        Json instructions = Json::array();
        for ( std::size_t i = 0; i < entries.size(); ++i )
        {
          instructions.push_back( instructionJson( **machine, entries[i], first + entries.size() - 1 - i, registers ) );
        }
        return Json{ { "total", ( *machine )->traceCount() }, { "instructions", std::move( instructions ) } };
      } );

  dispatcher.add(
      "debug.trace.find",
      info( "Searches the 68000's trace back from an index for the instructions that meet every condition given, "
            "and answers the newest of them, oldest first, with next_from to go on searching back from.",
            { { .name = "pc", .type = "integer", .description = "The instruction's address.", .required = false },
              { .name = "pc_start",
                .type = "integer",
                .description = "With pc_end, in place of pc: the address is at least this.",
                .required = false },
              { .name = "pc_end", .type = "integer", .description = "And below this.", .required = false },
              { .name = "where",
                .type = "object",
                .description = "Registers and the values they held before the instruction: d0 to d7, a0 to a7, sr; "
                               "such as {\"d0\": 5, \"a1\": \"0x810000\"}. Needs the registers kept, as "
                               "registers does.",
                .required = false },
              { .name = "disasm_contains",
                .type = "string",
                .description = "Text its disassembly holds, case aside: jsr, $c08000.",
                .required = false },
              { .name = "from",
                .type = "integer",
                .description = "The index to search back from; 0, the last executed, if left out.",
                .required = false },
              { .name = "limit",
                .type = "integer",
                .description = "The most matches to answer, 1 to 4096; 32 if left out.",
                .required = false },
              { .name = "registers", .type = "boolean", .description = WITH_REGISTERS, .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const machine = loadedMachine( emulator );
        if ( !machine )
        {
          return std::unexpected( machine.error() );
        }
        auto const search = searchOf( params );
        auto const from = optionalUnsigned( params, "from", 0, 0, machine::Machine::MAX_TRACE_SIZE );
        auto const limit = optionalUnsigned( params, "limit", DEFAULT_ANSWERED, 1, MAX_ANSWERED );
        if ( !search )
        {
          return std::unexpected( search.error() );
        }
        if ( !from || !limit )
        {
          return std::unexpected( !from ? from.error() : limit.error() );
        }
        bool const registers = params.contains( "registers" ) && params.at( "registers" ) == true;
        if ( ( registers || !search->registers.empty() ) && !( *machine )->traceRegisters() )
        {
          return std::unexpected( registersNotKept() );
        }

        // A loop is the same few addresses over and over, so whether an
        // address's disassembly holds the text is found once.
        std::unordered_map<std::uint32_t, bool> disasmHolds;
        auto const holdsText = [&]( std::uint32_t pc )
        {
          auto const known = disasmHolds.find( pc );
          if ( known != disasmHolds.end() )
          {
            return known->second;
          }
          int length = 0;
          bool const holds = lowered( ( *machine )->disassemble( pc, length ) ).contains( search->disasmContains );
          disasmHolds.emplace( pc, holds );
          return holds;
        };

        std::size_t const total = ( *machine )->traceCount();
        std::vector<Json> matches;
        Json nextFrom = nullptr;
        for ( auto index = static_cast<std::size_t>( *from ); index < total; ++index )
        {
          machine::TraceEntry const entry = ( *machine )->traceAt( index );
          if ( entry.pc < search->pcStart || entry.pc >= search->pcEnd )
          {
            continue;
          }
          if ( !std::ranges::all_of( search->registers,
                                     [&entry]( auto const& wanted )
                                     { return registerOf( entry, wanted.first ) == wanted.second; } ) )
          {
            continue;
          }
          if ( !search->disasmContains.empty() && !holdsText( entry.pc ) )
          {
            continue;
          }
          if ( matches.size() == *limit )
          {
            nextFrom = index;
            break;
          }
          matches.push_back( instructionJson( **machine, entry, index, registers ) );
        }
        std::ranges::reverse( matches );
        return Json{ { "total", total }, { "matches", std::move( matches ) }, { "next_from", std::move( nextFrom ) } };
      } );

  dispatcher.add(
      "debug.trace.configure",
      info( "What the 68000's trace keeps: how many of the last instructions, 4096 unless set (a frame is some tens "
            "of thousands), and whether their registers. It holds for the game loaded and for those loaded after "
            "it. Answers both settings.",
            { { .name = "instructions",
                .type = "integer",
                .description = "1 to 4194304, each 88 bytes of the host's memory; left out, it stays as it is.",
                .required = false },
              { .name = "registers",
                .type = "boolean",
                .description = "Whether every instruction is kept with its registers, which debug.trace's registers "
                               "and debug.trace.find's where need. It slows every run a few percent, so it is off "
                               "unless set. A change empties the trace.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        auto const instructions =
            optionalUnsigned( params, "instructions", emulator.traceSize(), 1, machine::Machine::MAX_TRACE_SIZE );
        if ( !instructions )
        {
          return std::unexpected( instructions.error() );
        }
        auto const registers = params.find( "registers" );
        if ( registers != params.end() && !registers->is_boolean() )
        {
          return std::unexpected( badRequest( "registers is true or false" ) );
        }
        if ( *instructions != emulator.traceSize() )
        {
          emulator.setTraceSize( static_cast<std::size_t>( *instructions ) );
        }
        if ( registers != params.end() )
        {
          emulator.setTraceRegisters( registers->get<bool>() );
        }
        return Json{ { "instructions", emulator.traceSize() }, { "registers", emulator.traceRegisters() } };
      } );
}

} // namespace pgm::control
